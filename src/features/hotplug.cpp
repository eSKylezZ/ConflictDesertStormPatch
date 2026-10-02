// Pads plugged in / pulled out while the game runs, and the "controller disconnected" pause.
//
// The game enumerates its joysticks once at start-up (FUN_00538bb0: EnumDevices(GAMECTRL, ATTACHEDONLY) on its
// IDirectInput8 [0x754C40] with callback 0x538800, which opens the device into slot [0x754C34] of 0x754BC8[8] - data
// format, cooperative level, axis ranges, deadzone, force feedback - and bumps the count 0x754BE8). Every frame the poll
// loop 0x539340 reads all 8 slots and skips empty ones; a lost device just keeps its last button / stick state.
//
// Here: a device-interface notification (cfgmgr32 CM_Register_Notification, HID class) wakes a worker thread that
// enumerates with its own DirectInput object (EnumDevices takes ~140 ms - never on the game thread). On the game thread:
//   - a slot whose device is gone is emptied (device + force-feedback effect released, its input state reset to idle)
//     and remembered as "parked" with the device's instance GUID;
//   - a new device goes into: its own parked slot (same pad back), else a parked slot of a pad player in the running
//     game (their pad, even a different one), else a never-used slot, else any empty slot - opened by the game's own
//     callback, so it is set up exactly like the pads found at start-up. Player -> joystick numbers never change.
// In a mission a pad player losing their pad pauses the game (the PAUSE action's own path, FUN_0044b880) and a notice
// names the player until the pad is back.
#include <windows.h>
#include <cfgmgr32.h>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "core/proxy.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/overlay.h"

namespace {
constexpr uint32_t kJoysticks = 0x754BC8, kJoystickCount = 0x754BE8, kCurrentJoystick = 0x754C34;
constexpr uint32_t kGameDirectInput = 0x754C40, kOpenCallback = 0x538800;
constexpr uint32_t kRumbleTable = 0x753058, kRumbleStride = 0x1C;
constexpr uint32_t kInputState = 0x7541B8, kInputStride = 0x150;  // per joystick: 3 x 28 dwords around this
constexpr uint32_t kBlocks = 0x60F5B8, kBlockSize = 0x478, kBlockDevices = 0x38, kBlockJoystick = 0x3C8;
constexpr uint32_t kBlockPauseAction = 0x9C, kBlockPaused = 0x36C;  // action 6 (PAUSE) flags, pause toggle
constexpr uint32_t kFrontEndState = 0x617C18, kLevelName = 0x606880, kRenderer = 0x63C924;
constexpr uint32_t kStatePlay = 0xE, kStatePause = 0xF;
constexpr int kMax = 8;

void** Slots() { return reinterpret_cast<void**>(kJoysticks); }

// ---- worker: what is attached now ----
std::mutex g_lock;
std::vector<DIDEVICEINSTANCEA> g_found;
bool g_foundNew = false;
HANDLE g_wake = nullptr;
std::atomic<DWORD> g_lastChange{0};
bool g_notifications = false;

BOOL CALLBACK Collect(const DIDEVICEINSTANCEA* inst, void* out) {
    static_cast<std::vector<DIDEVICEINSTANCEA>*>(out)->push_back(*inst);
    return DIENUM_CONTINUE;
}

DWORD WINAPI ScanThread(void*) {
    using Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto create = reinterpret_cast<Create>(RealDInput8("DirectInput8Create"));
    IDirectInput8A* di = nullptr;
    if (!create || FAILED(create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
                                 reinterpret_cast<void**>(&di), nullptr)))
        return 0;
    for (;;) {
        // Without device notifications (older Windows): look every 4 s.
        WaitForSingleObject(g_wake, g_notifications ? INFINITE : 4000);
        // A pad arrives as several interfaces in a burst - wait until it has been quiet for a moment.
        while (GetTickCount() - g_lastChange < 400) Sleep(100);
        std::vector<DIDEVICEINSTANCEA> found;
        di->EnumDevices(DI8DEVCLASS_GAMECTRL, Collect, &found, DIEDFL_ATTACHEDONLY);
        std::lock_guard<std::mutex> guard(g_lock);
        g_found = std::move(found);
        g_foundNew = true;
    }
}

DWORD CALLBACK OnDeviceChange(HCMNOTIFICATION, void*, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA, DWORD) {
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL || action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
        g_lastChange = GetTickCount();
        SetEvent(g_wake);
    }
    return ERROR_SUCCESS;
}

void StartWatching() {
    static bool started = false;
    if (started) return;
    started = true;
    g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!g_wake) return;
    // cfgmgr32 notifications: Windows 8+, looked up at run time.
    using Register = CONFIGRET(WINAPI*)(PCM_NOTIFY_FILTER, PVOID, PCM_NOTIFY_CALLBACK, PHCMNOTIFICATION);
    if (HMODULE cfg = LoadLibraryA("cfgmgr32.dll"))
        if (auto reg = reinterpret_cast<Register>(GetProcAddress(cfg, "CM_Register_Notification"))) {
            static const GUID kHid = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
            CM_NOTIFY_FILTER filter{};
            filter.cbSize = sizeof filter;
            filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
            filter.u.DeviceInterface.ClassGuid = kHid;
            HCMNOTIFICATION handle = nullptr;
            g_notifications = reg(&filter, nullptr, &OnDeviceChange, &handle) == CR_SUCCESS;
        }
    if (HANDLE t = CreateThread(nullptr, 0, &ScanThread, nullptr, 0, nullptr)) CloseHandle(t);
    dslog::Write("[ok]   Hot-plug: watching for pads (%s)", g_notifications ? "device notifications" : "every 4 s");
}

// ---- slots ----
struct SlotInfo {
    void* device = nullptr;  // device the GUID belongs to
    GUID guid{};
    bool parked = false;     // had a pad that was pulled out
    bool used = false;       // ever held a pad
} g_slots[kMax];

GUID InstanceOf(void* dev) {
    DIDEVICEINSTANCEA info{};
    info.dwSize = sizeof info;
    using GetInfo = HRESULT(__stdcall*)(void*, DIDEVICEINSTANCEA*);
    if (FAILED(reinterpret_cast<GetInfo>((*reinterpret_cast<void***>(dev))[15])(dev, &info))) return GUID{};
    return info.guidInstance;
}

// Slot j's GUID, learnt while its device is there.
void Learn(int j) {
    void* dev = Slots()[j];
    if (!dev || g_slots[j].device == dev) return;
    g_slots[j].device = dev;
    g_slots[j].guid = InstanceOf(dev);
    g_slots[j].used = true;
    g_slots[j].parked = false;
}

// Like the start-up initialisation (FUN_00538bb0): every button / axis of the joystick idle.
void ResetInput(int j) {
    auto* base = reinterpret_cast<uint32_t*>(kInputState + j * kInputStride);
    for (int i = 0; i < 28; ++i) {
        base[i - 28] = 0;
        base[i] = 1;
        base[i + 28] = 0;
    }
}

template <class Fn>
Fn Method(void* obj, int index) {
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(obj))[index]);
}

void ReleaseEffect(int j) {
    auto* e = reinterpret_cast<uint8_t*>(kRumbleTable + j * kRumbleStride);
    if (void* effect = *reinterpret_cast<void**>(e)) Method<ULONG(__stdcall*)(void*)>(effect, 2)(effect);
    *reinterpret_cast<void**>(e) = nullptr;
    *reinterpret_cast<DWORD*>(e + 4) = 0;
    *reinterpret_cast<DWORD*>(e + 0x10) = 0;
}

void Park(int j) {
    void* dev = Slots()[j];
    Slots()[j] = nullptr;
    ReleaseEffect(j);
    Method<HRESULT(__stdcall*)(void*)>(dev, 8)(dev);   // Unacquire
    Method<ULONG(__stdcall*)(void*)>(dev, 2)(dev);     // Release
    ResetInput(j);
    features::ForgetJoystick(j);
    g_slots[j].device = nullptr;
    g_slots[j].parked = true;
    dslog::Write("Hot-plug: joystick %d unplugged", j);
}

bool Open(int j, const DIDEVICEINSTANCEA& inst) {
    void* di = *reinterpret_cast<void**>(kGameDirectInput);
    if (!di || Slots()[j]) return false;
    auto& current = *reinterpret_cast<int*>(kCurrentJoystick);
    auto& count = *reinterpret_cast<int*>(kJoystickCount);
    const int savedCurrent = current, savedCount = count;
    ReleaseEffect(j);
    features::ForgetJoystick(j);
    current = j;
    reinterpret_cast<BOOL(__stdcall*)(const DIDEVICEINSTANCEA*, void*)>(kOpenCallback)(&inst, di);
    current = savedCurrent;
    count = Slots()[j] ? std::max(savedCount, j + 1) : savedCount;
    ResetInput(j);
    if (!Slots()[j]) {
        dslog::Write("[fail] Hot-plug: \"%s\" not opened", inst.tszProductName);
        return false;
    }
    Learn(j);
    // No pad at start-up: the game cleared player 1's pad bit (FUN_0044b220 while the count is 0) - give it back.
    if (savedCount == 0) *reinterpret_cast<uint32_t*>(kBlocks + kBlockDevices) |= 2;
    dslog::Write("Hot-plug: \"%s\" -> joystick %d", inst.tszProductName, j);
    return true;
}

bool InMission() {
    const char* level = reinterpret_cast<const char*>(kLevelName);
    return level[0] && _strnicmp(level, "frontend", 8) != 0;
}

int Players() { return std::clamp(features::SplitScreenPlayers(), 1, 4); }

// The joystick player p plays with, -1 = keyboard & mouse / none.
int PlayerJoystick(int p) {
    if (features::CoopPlayers() >= 2) return features::CoopJoystick(p);
    const auto* block = reinterpret_cast<const uint8_t*>(kBlocks + p * kBlockSize);
    if (!(*reinterpret_cast<const uint32_t*>(block + kBlockDevices) & 2)) return -1;
    if (p == 0 && features::KeyboardInUse()) return -1;  // single player / P1 on keyboard & mouse
    return *reinterpret_cast<const int*>(block + kBlockJoystick);
}

// A parked slot a player of the running game is waiting on.
int WaitingSlot() {
    if (!InMission()) {
        for (int p = 0; p < features::CoopPlayers(); ++p) {
            const int j = features::CoopJoystick(p);
            if (j >= 0 && j < kMax && !Slots()[j] && g_slots[j].parked) return j;
        }
        return -1;
    }
    for (int p = 0; p < Players(); ++p) {
        const int j = PlayerJoystick(p);
        if (j >= 0 && j < kMax && !Slots()[j] && g_slots[j].parked) return j;
    }
    return -1;
}

void Reconcile(const std::vector<DIDEVICEINSTANCEA>& found) {
    auto attached = [&](const GUID& g) {
        for (const auto& d : found)
            if (IsEqualGUID(d.guidInstance, g)) return true;
        return false;
    };
    for (int j = 0; j < kMax; ++j) {
        if (!Slots()[j]) continue;
        Learn(j);
        if (!attached(g_slots[j].guid)) Park(j);
    }
    for (const auto& d : found) {
        bool open = false;
        for (int j = 0; j < kMax && !open; ++j) open = Slots()[j] && IsEqualGUID(g_slots[j].guid, d.guidInstance);
        if (open) continue;
        int target = -1;
        for (int j = 0; j < kMax && target < 0; ++j)
            if (!Slots()[j] && g_slots[j].parked && IsEqualGUID(g_slots[j].guid, d.guidInstance)) target = j;
        if (target < 0) target = WaitingSlot();
        for (int j = 0; j < kMax && target < 0; ++j)
            if (!Slots()[j] && !g_slots[j].used) target = j;
        for (int j = 0; j < kMax && target < 0; ++j)
            if (!Slots()[j]) target = j;
        if (target >= 0) Open(target, d);
    }
}

// ---- "controller disconnected" ----
struct Notice {
    bool lost = false;
    int joystick = -1;  // the slot that was lost
    DWORD back = 0;     // when the pad came back (the notice says so for a few seconds)
} g_notice[4];
bool g_pausePending = false;
constexpr DWORD kBackNotice = 3000;

// The PAUSE action's own path (FUN_0044b880: checks that pausing is allowed now, toggles block +0x36C, front-end
// state 0xF + sound), as if player 1 had pressed PAUSE - player 1's block owns the pause menu.
void Pause() {
    auto* block = reinterpret_cast<uint8_t*>(kBlocks);
    auto& flags = *reinterpret_cast<uint32_t*>(block + kBlockPauseAction);
    const uint32_t saved = flags;
    *reinterpret_cast<int*>(block + kBlockPaused) = 0;
    flags |= 2;  // pressed this frame
    reinterpret_cast<void(__fastcall*)(void*)>(0x44B880)(block);
    flags = saved;
}

#ifndef DS_DIST
// Dev: HKCU\Software\DesertStormFix\Dev "FakeLostPad" = n -> player n's pad counts as pulled out (read every 2 s).
int FakeLostPlayer() {
    static DWORD checked = 0, value = 0;
    if (GetTickCount() - checked > 2000) {
        checked = GetTickCount();
        DWORD size = sizeof value;
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "FakeLostPad", RRF_RT_REG_DWORD, nullptr,
                         &value, &size) != ERROR_SUCCESS)
            value = 0;
    }
    return static_cast<int>(value) - 1;
}
#endif

void WatchPlayers() {
    if (!InMission()) {
        for (Notice& n : g_notice) n = Notice{};
        g_pausePending = false;
        return;
    }
    const int players = Players();
    bool anyLost = false;
    for (int p = 0; p < 4; ++p) {
        Notice& n = g_notice[p];
        const int j = p < players ? PlayerJoystick(p) : -1;
        const bool gone = j >= 0 && j < kMax && !Slots()[j] && g_slots[j].parked;
#ifndef DS_DIST
        if (p == FakeLostPlayer()) {
            if (!n.lost) {
                n = Notice{true, -2, 0};
                g_pausePending = true;
                dslog::Write("[dev] Hot-plug: faking player %d's pad as unplugged", p + 1);
            }
            anyLost = true;
            continue;
        }
        if (n.joystick == -2) n = Notice{false, -1, GetTickCount()};  // fake pad "back"
#endif
        if (!n.lost) {
            if (gone) {
                n = Notice{true, j, 0};
                g_pausePending = true;
                dslog::Write("Hot-plug: player %d's controller (joystick %d) disconnected", p + 1, j);
            }
        } else if (Slots()[n.joystick]) {
            n = Notice{false, -1, GetTickCount()};
            dslog::Write("Hot-plug: player %d's controller is back", p + 1);
        } else if (j != n.joystick) {
            n = Notice{};  // plays on with the keyboard or another pad now
        }
        anyLost |= n.lost;
    }
    const uint32_t state = *reinterpret_cast<uint32_t*>(kFrontEndState);
    if (!anyLost || state == kStatePause) g_pausePending = false;
    // Again next frame if the game didn't take it yet; the loadout screen (its own freeze) first has to close.
    if (g_pausePending && state == kStatePlay && !features::CustomiseOpen()) Pause();
}

int ScreenW() { return *reinterpret_cast<int*>(*reinterpret_cast<uint8_t**>(kRenderer) + 0x40688); }
int ScreenH() { return *reinterpret_cast<int*>(*reinterpret_cast<uint8_t**>(kRenderer) + 0x4068C); }

// Notice lines this frame: {text, gold}.
struct Line {
    char text[96];
    bool lost;
};
int NoticeLines(Line (&out)[4]) {
    if (!InMission()) return 0;
    const bool split = Players() >= 2;
    int n = 0;
    for (int p = 0; p < 4; ++p) {
        const bool back = g_notice[p].back && GetTickCount() - g_notice[p].back < kBackNotice;
        if (!g_notice[p].lost && !back) continue;
        const char* what = g_notice[p].lost ? "CONTROLLER DISCONNECTED" : "CONTROLLER RECONNECTED";
        if (split) snprintf(out[n].text, sizeof out[n].text, "PLAYER %d - %s", p + 1, what);
        else snprintf(out[n].text, sizeof out[n].text, "%s", what);
        out[n++].lost = g_notice[p].lost;
    }
    return n;
}

// Top of the screen, above the pause menu's panel; box drawn right away (queued shapes would land under other
// screens' late draws, e.g. the loadout screen's panels).
void DrawNotice() {
    Line lines[4];
    const int n = NoticeLines(lines);
    void* font = *reinterpret_cast<void**>(0x60EDB8);
    if (!n || !font) return;
    const int w = ScreenW(), h = ScreenH();
    const float lineH = h * 0.055f, top = h * 0.015f;
    int widest = 0;
    for (int i = 0; i < n; ++i) widest = std::max(widest, overlay::TextWidth(font, lines[i].text));
    const float boxW = widest + h * 0.08f, boxH = lineH * n + h * 0.03f;
    const float box[1][4] = {{(w - boxW) / 2, top, boxW, boxH}};
    overlay::FillRects(box, 1, 0xC0000000);
    for (int i = 0; i < n; ++i) {
        const int x = (w - overlay::TextWidth(font, lines[i].text)) / 2;
        const int y = static_cast<int>(top + h * 0.015f + lineH * (i + 0.75f));
        overlay::DrawLabel(font, lines[i].text, x, y, lines[i].lost ? 0xFFFFC040 : 0xFF80FF80);
    }
}
}  // namespace

void features::OnFrameHotplug() {
    if (!settings::Get().controller || !*reinterpret_cast<void**>(kGameDirectInput)) return;
    StartWatching();
    overlay::AddLateDraw(&DrawNotice);
    for (int j = 0; j < kMax; ++j) Learn(j);
    std::vector<DIDEVICEINSTANCEA> found;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (g_foundNew) {
            found = std::move(g_found);
            g_foundNew = false;
            fresh = true;
        }
    }
    if (fresh) Reconcile(found);
    WatchPlayers();
}
