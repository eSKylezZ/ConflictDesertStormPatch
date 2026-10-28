// Pads plugged in / pulled out while the game runs (part of tgefix.controller): port of DesertStormFix's hotplug.cpp
// (games/ds1/src/features/hotplug.cpp; DS1 addresses in brackets), without its "controller disconnected" pause.
//
// The game enumerates its joysticks once at start-up (FUN_004b9680 [FUN_00538bb0]: EnumDevices(GAMECTRL,
// ATTACHEDONLY) on its IDirectInput8 [0x7AB1C8] [0x754C40] with callback FUN_004b9360 [0x538800], which opens the
// device into slot [0x7AB0AC] of 0x7A9D24[8] - data format, cooperative level, axis ranges, deadzone, force feedback -
// and bumps the count [0x7AA888]). Every frame the poll loop 0x4B9C10 [0x539340] reads all 8 slots and skips empty
// ones; a lost device just keeps its last button / stick state.
//
// Here: a device-interface notification (cfgmgr32 CM_Register_Notification, HID class) wakes a worker thread that
// enumerates with its own DirectInput object (EnumDevices takes ~140 ms - never on the game thread). On the game thread:
//   - a slot whose device is gone is emptied (device + force-feedback effect released, its input state reset to idle)
//     and remembered as "parked" with the device's instance GUID;
//   - a new device goes into its own parked slot (same pad back), else player 1's parked slot, else a never-used slot,
//     else any empty slot - opened by the game's own callback, so it is set up exactly like the pads found at start-up.
#include <windows.h>
#include <cfgmgr32.h>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "features/features.h"

namespace {
constexpr uint32_t kJoysticks = 0x7A9D24, kJoystickCount = 0x7AA888, kCurrentJoystick = 0x7AB0AC;
constexpr uint32_t kGameDirectInput = 0x7AB1C8, kOpenCallback = 0x4B9360;
constexpr uint32_t kRumbleTable = 0x7AB0B8, kRumbleStride = 0x1C;
constexpr uint32_t kInputState = 0x7A9DE0, kInputStride = 0x150;  // per joystick: 3 x 28 dwords around this
constexpr uint32_t kBlock0 = 0x7DCAB0, kBlockDevices = 0x14, kBlockJoystick = 0x2E0;
constexpr int kMax = 8;

void** Slots() { return reinterpret_cast<void**>(kJoysticks); }

// ---- worker: what is attached now ----
std::mutex g_lock;
std::vector<DIDEVICEINSTANCEA> g_found;
bool g_foundNew = false;
HANDLE g_wake = nullptr;
volatile DWORD g_lastChange = 0;
bool g_notifications = false;

BOOL CALLBACK Collect(const DIDEVICEINSTANCEA* inst, void* out) {
    static_cast<std::vector<DIDEVICEINSTANCEA>*>(out)->push_back(*inst);
    return DIENUM_CONTINUE;
}

DWORD WINAPI ScanThread(void*) {
    // The system's dinput8.dll (the game folder's is the PSE shim).
    char path[MAX_PATH] = {};
    GetSystemDirectoryA(path, MAX_PATH);
    strcat_s(path, "\\dinput8.dll");
    HMODULE real = LoadLibraryA(path);
    using Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto create = real ? reinterpret_cast<Create>(GetProcAddress(real, "DirectInput8Create")) : nullptr;
    IDirectInput8A* di = nullptr;
    if (!create || FAILED(create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
                                 reinterpret_cast<void**>(&di), nullptr)))
        return 0;
    for (;;) {
        // A scan at once (a pad the game's start-up enumeration missed), then on device notifications - and every 5 s
        // anyway (no notifications on old Windows; a pad that was there all along never sends one).
        static bool first = true;
        if (!first) WaitForSingleObject(g_wake, 5000);
        first = false;
        while (GetTickCount() - g_lastChange < 400) Sleep(100);           // a pad arrives as a burst of interfaces
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

void Learn(int j) {
    void* dev = Slots()[j];
    if (!dev || g_slots[j].device == dev) return;
    g_slots[j].device = dev;
    g_slots[j].guid = InstanceOf(dev);
    g_slots[j].used = true;
    g_slots[j].parked = false;
}

// Like the start-up initialisation (FUN_004b9680): every button / axis of the joystick idle.
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
    features::PlayerPadChanged(j, false);
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
    // No pad at start-up: player 1's block may have no pad bit - give it one.
    if (savedCount == 0) *reinterpret_cast<uint32_t*>(kBlock0 + kBlockDevices) |= 2;
    dslog::Write("Hot-plug: \"%s\" -> joystick %d", inst.tszProductName, j);
    features::PlayerPadChanged(j, true);
    return true;
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
    const int player1 = *reinterpret_cast<int*>(kBlock0 + kBlockJoystick);
    for (const auto& d : found) {
        bool open = false;
        for (int j = 0; j < kMax && !open; ++j) open = Slots()[j] && IsEqualGUID(g_slots[j].guid, d.guidInstance);
        if (open) continue;
        int target = -1;
        for (int j = 0; j < kMax && target < 0; ++j)
            if (!Slots()[j] && g_slots[j].parked && IsEqualGUID(g_slots[j].guid, d.guidInstance)) target = j;
        if (target < 0 && player1 >= 0 && player1 < kMax && !Slots()[player1] && g_slots[player1].parked) target = player1;
        for (int j = 0; j < kMax && target < 0; ++j)
            if (!Slots()[j] && !g_slots[j].used) target = j;
        for (int j = 0; j < kMax && target < 0; ++j)
            if (!Slots()[j]) target = j;
        if (target >= 0) Open(target, d);
    }
}
}  // namespace

void features::OnFrameHotplug() {
    if (!Get().controller || !*reinterpret_cast<void**>(kGameDirectInput)) return;
    StartWatching();
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
    if (fresh) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            int open = 0;
            for (int j = 0; j < kMax; ++j) open += Slots()[j] != nullptr;
            dslog::Write("Hot-plug: the game opened %d pad(s) at start-up, %zu attached", open, found.size());
        }
        Reconcile(found);
    }
}
