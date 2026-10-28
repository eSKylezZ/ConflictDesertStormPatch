// Several keyboards and mice in local split screen (2026-10-22; RE notes research/multi_kbm.md).
// Windows merges every keyboard and mouse into one, and so does the game: keys and mouse buttons come from window
// messages (window proc 0x411310 -> FUN_005393f0 -> key table 0x753940 [0x200 codes: scan code, +0x100 extended],
// mouse buttons 0x754C0C[3]; state values 0 up, 1 went down, 2 held, 3 went up, 4 down and up in one frame, aged by
// FUN_00538520 after each frame at 0x4116F6), mouse movement + wheel from one DirectInput mouse (GetDeviceState at
// 0x538701 into DIMOUSESTATE 0x754C24, then one sample into the look smoother 0x754BEC, FUN_005381b0).
// Raw Input tells the devices apart (WM_INPUT hDevice). Started when the co-op join screen opens (KbmStart):
//   - a message-only window on its own thread registers keyboards + mice (INPUTSINK). dinput8 registers Raw Input
//     itself when the game window gains the front and removes it when it loses it (seen with Frida 2026-10-22: usage
//     1/2 flags 0x100, 1/6 0x200 on its own window; through the delay-loaded ext-ms-win-mininput-inputhost, not a
//     user32 import of its own), and one registration per usage wins - ours is checked every 10 frames and taken
//     back. DirectInput's mouse then goes quiet: the game's mouse poll reads the sum of all mice from us instead
//     (call at 0x5386F9).
//   - every device keeps the game's own state values (keys, buttons, movement since the last poll, its own smoother).
// A co-op session with 2+ keyboard players maps each player to a keyboard and a mouse (KbmSetPlayers). The game reads a
// player's keys only where it passes that player's input block - the button evaluator FUN_00450770, the axis evaluator
// (splitscreen.cpp AxisValue), mouse look (splitscreen.cpp MouseDeltaStub) and the zoom wheel FUN_0044ee60 / 0044ee90:
// those set the "current player", and the six raw readers (key held / pressed / released 0x539DA0 / DD0 / E10, mouse
// button 0x539820 / 840 / 870) answer from that player's devices. Everything else (menus, cursor, debug keys, text
// entry) keeps reading the game's merged tables. One keyboard player = all devices merged, as before.
// Not online (lockstep reads one local player). Setting FeatMultiKbm (default on).
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
constexpr uint32_t kKeyHeld = 0x539DA0, kKeyPressed = 0x539DD0, kKeyReleased = 0x539E10;
constexpr uint32_t kButtonHeld = 0x539820, kButtonPressed = 0x539840, kButtonReleased = 0x539870;
constexpr uint32_t kButtonEval = 0x450770, kWheelUp = 0x44EE60, kWheelDown = 0x44EE90;
constexpr uint32_t kAgeCall = 0x4116F6, kAge = 0x538520;
constexpr uint32_t kSmootherAddCall = 0x53873C, kSmootherAdd = 0x5381B0, kSmootherReset = 0x538050;
constexpr uint32_t kMouseStateSite = 0x5386F9;   // push 0x754C24; push 0x10; push eax; call [ecx+0x24]
constexpr uint32_t kMouseState = 0x754C24, kCapture = 0x754C6C, kFrontEndState = 0x617C18;
constexpr uint32_t kBlocks = 0x60F5B8, kBlockSize = 0x478, kNoKey = 0x1146;
constexpr int kMaxPlayers = 4, kCodes = 0x200;

// The game's look smoother (ctor FUN_00537fd0): ring of {dx, dy, weight}, capacity 1.
struct Smoother {
    float* ring;
    float scale = 2.0f, weightScale = 30.0f;
    uint32_t pad = 0;
    int capacity = 1, count = 0, index = 0;
    float sample[3] = {};
};

struct Device {
    HANDLE handle = nullptr;
    bool mouse = false, gone = false;
    std::string name;
    uint8_t keys[kCodes] = {};
    uint8_t buttons[3] = {};
    long dx = 0, dy = 0, wheel = 0;     // since the last mouse poll
    long frameWheel = 0;                // the last poll's wheel (zoom reads it during the frame)
    bool clicked = false;               // left button went down this frame (join screen pairing)
    Smoother smoother;
};

struct Event {
    HANDLE handle;
    bool mouse;
    uint16_t code;      // keyboard: game code
    bool up;
    bool removed;
    long dx, dy;
    uint16_t buttonFlags;
    short wheel;
};

CRITICAL_SECTION g_lock;
std::vector<Event> g_queue;
std::vector<Device*> g_devices;   // index = device number; never shrinks (players keep their numbers)
HWND g_window = nullptr;
bool g_started = false, g_failed = false;
bool g_ours = false;              // our registration is the process's (checked every 10 frames)
long g_mergedDx = 0, g_mergedDy = 0, g_mergedWheel = 0;
int g_playerKeyboard[kMaxPlayers] = {-1, -1, -1, -1}, g_playerMouse[kMaxPlayers] = {-1, -1, -1, -1};
bool g_mapped = false;            // 2+ keyboard players in the running session
int g_ctx = -1;                   // the player whose input the game is reading, -1 = none (merged tables)
bool g_wasFocused = true;

using RegisterFn = BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT);
RegisterFn g_register = nullptr;          // RegisterRawInputDevices

bool Active() { return g_mapped && settings::Get().featMultiKbm && !features::LockstepActive(); }

Device* Find(HANDLE h, bool mouse) {
    for (Device* d : g_devices)
        if (d->handle == h && d->mouse == mouse) return d;
    return nullptr;
}

std::string DeviceName(HANDLE h) {
    if (reinterpret_cast<uintptr_t>(h) >= 0x100 && reinterpret_cast<uintptr_t>(h) < 0x110) return "(dev: pretend device)";
    char name[512] = {};
    UINT size = sizeof name;
    if (h && GetRawInputDeviceInfoA(h, RIDI_DEVICENAME, name, &size) > 0) return name;
    return "?";
}

Device* Add(HANDLE h, bool mouse) {
    Device* d = new Device;
    d->handle = h;
    d->mouse = mouse;
    d->name = DeviceName(h);
    d->smoother.ring = d->smoother.sample;
    reinterpret_cast<void(__thiscall*)(void*)>(kSmootherReset)(&d->smoother);
    g_devices.push_back(d);
    dslog::Write("Several keyboards / mice: %s %d found - %s", mouse ? "mouse" : "keyboard", static_cast<int>(g_devices.size()) - 1,
                 d->name.c_str());
    return d;
}

// ---- Raw Input thread ----
bool RegisterOurs(DWORD extra = 0) {
    RAWINPUTDEVICE rid[2] = {{1, 6, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY | extra, g_window},
                             {1, 2, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, g_window}};
    return g_register(rid, 2, sizeof rid[0]) != FALSE;
}

void Queue(const Event& e) {
    EnterCriticalSection(&g_lock);
    if (g_queue.size() < 4096) g_queue.push_back(e);
    LeaveCriticalSection(&g_lock);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_INPUT) {
        alignas(8) uint8_t buffer[128];
        UINT size = sizeof buffer;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) !=
            static_cast<UINT>(-1)) {
            const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buffer);
            Event e = {};
            e.handle = raw->header.hDevice;
            if (raw->header.dwType == RIM_TYPEKEYBOARD) {
                const RAWKEYBOARD& k = raw->data.keyboard;
                // fake shifts (VKey 0xFF) and Pause's E1 prefix are left out, like the game's WM_KEYDOWN codes
                if (k.VKey != 0xFF && !(k.Flags & RI_KEY_E1) && k.MakeCode < 0x100) {
                    e.code = static_cast<uint16_t>(k.MakeCode | ((k.Flags & RI_KEY_E0) ? 0x100 : 0));
                    e.up = (k.Flags & RI_KEY_BREAK) != 0;
                    Queue(e);
                }
            } else if (raw->header.dwType == RIM_TYPEMOUSE) {
                const RAWMOUSE& m = raw->data.mouse;
                e.mouse = true;
                if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {   // tablets / remote desktop: no relative movement
                    e.dx = m.lLastX;
                    e.dy = m.lLastY;
                }
                e.buttonFlags = m.usButtonFlags;
                if (m.usButtonFlags & RI_MOUSE_WHEEL) e.wheel = static_cast<short>(m.usButtonData);
                Queue(e);
            }
        }
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    if (msg == WM_INPUT_DEVICE_CHANGE && wParam == GIDC_REMOVAL) {
        Event e = {};
        e.handle = reinterpret_cast<HANDLE>(lParam);
        e.removed = true;
        Queue(e);
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

DWORD WINAPI RawThread(LPVOID ready) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "DSFixRawInput";
    RegisterClassA(&wc);
    g_window = CreateWindowExA(0, wc.lpszClassName, "", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    // NOHOTKEYS keeps the Windows key off while the game is in front, as dinput's own registration did
    const bool ok = g_window && (RegisterOurs(RIDEV_NOHOTKEYS) || RegisterOurs());
    g_ours = ok;
    if (!ok) g_failed = true;
    SetEvent(static_cast<HANDLE>(ready));
    if (!ok) return 0;
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) DispatchMessageA(&msg);
    return 0;
}

bool OursRegistered() {
    RAWINPUTDEVICE list[16];
    UINT n = 16;
    const UINT got = GetRegisteredRawInputDevices(list, &n, sizeof list[0]);
    if (got == static_cast<UINT>(-1)) return false;
    int ours = 0;
    for (UINT i = 0; i < got; ++i)
        if (list[i].usUsagePage == 1 && (list[i].usUsage == 2 || list[i].usUsage == 6) && list[i].hwndTarget == g_window) ++ours;
    return ours == 2;
}

// ---- game thread: events -> device states ----
void KeyDown(uint8_t& s) {
    if (s == 0 || s == 3 || s == 4) s = 1;
}
void KeyUp(uint8_t& s) {
    if (s == 1) s = 4;
    else if (s == 2) s = 3;
}
void Age(uint8_t& s) {
    if (s == 1) s = 2;
    else if (s == 3 || s == 4) s = 0;
}
void ReleaseAll(Device& d) {
    for (uint8_t& s : d.keys) KeyUp(s);
    for (uint8_t& s : d.buttons) KeyUp(s);
    d.dx = d.dy = d.wheel = 0;
}

void Apply(const Event& e) {
    if (e.removed) {   // either kind
        for (Device* d : g_devices)
            if (d->handle == e.handle && !d->gone) {
                d->gone = true;
                ReleaseAll(*d);
                dslog::Write("Several keyboards / mice: %s %d removed", d->mouse ? "mouse" : "keyboard",
                             static_cast<int>(std::find(g_devices.begin(), g_devices.end(), d) - g_devices.begin()));
            }
        return;
    }
    Device* d = Find(e.handle, e.mouse);
    if (!d) d = Add(e.handle, e.mouse);
    d->gone = false;
    if (!e.mouse) {
        e.up ? KeyUp(d->keys[e.code]) : KeyDown(d->keys[e.code]);
        return;
    }
    d->dx += e.dx;
    d->dy += e.dy;
    d->wheel += e.wheel;
    g_mergedDx += e.dx;
    g_mergedDy += e.dy;
    g_mergedWheel += e.wheel;
    static const uint16_t kDown[3] = {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_DOWN};
    static const uint16_t kUp[3] = {RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP, RI_MOUSE_MIDDLE_BUTTON_UP};
    for (int b = 0; b < 3; ++b) {
        if (e.buttonFlags & kDown[b]) {
            KeyDown(d->buttons[b]);
            if (b == 0) d->clicked = true;
        }
        if (e.buttonFlags & kUp[b]) KeyUp(d->buttons[b]);
    }
}

// ---- the game's readers ----
int PlayerOfBlock(const void* block) {
    const int32_t off = static_cast<int32_t>(reinterpret_cast<uintptr_t>(block) - kBlocks);
    if (off < 0 || off % kBlockSize) return -1;
    const int p = off / static_cast<int32_t>(kBlockSize);
    return p < kMaxPlayers ? p : -1;
}

Device* KeyboardOf(int player) {
    const int k = player >= 0 ? g_playerKeyboard[player] : -1;
    return k >= 0 && k < static_cast<int>(g_devices.size()) ? g_devices[k] : nullptr;
}
Device* MouseOf(int player) {
    const int m = player >= 0 ? g_playerMouse[player] : -1;
    return m >= 0 && m < static_cast<int>(g_devices.size()) ? g_devices[m] : nullptr;
}

bool Context() { return g_ctx >= 0 && Active(); }

using Reader = int(__cdecl*)(uint32_t);
Reader g_next[6] = {};

int Answer(int which, uint32_t code) {
    if (*reinterpret_cast<const uint32_t*>(kCapture)) return 0;   // KEY ASSIGNMENT capturing: all readers say no
    uint8_t s = 0;
    if (which < 3) {
        if (code == kNoKey || code >= kCodes) return 0;
        const Device* d = KeyboardOf(g_ctx);
        s = d ? d->keys[code] : 0;
    } else {
        if (code >= 3) return 0;
        const Device* d = MouseOf(g_ctx);
        s = d ? d->buttons[code] : 0;
    }
    switch (which % 3) {
        case 0: return s != 0;              // held (the release frame counts too, as in the game)
        case 1: return s == 1 || s == 4;    // went down
        default: return s == 3 || s == 4;   // went up
    }
}
#define READER(name, i) \
    int __cdecl name(uint32_t code) { return Context() ? Answer(i, code) : g_next[i](code); }
READER(KeyHeldHook, 0)
READER(KeyPressedHook, 1)
READER(KeyReleasedHook, 2)
READER(ButtonHeldHook, 3)
READER(ButtonPressedHook, 4)
READER(ButtonReleasedHook, 5)
#undef READER

struct Scope {
    int prev;
    explicit Scope(const void* block) : prev(g_ctx) { g_ctx = PlayerOfBlock(block); }
    ~Scope() { g_ctx = prev; }
};

using ButtonFn = int(__fastcall*)(void*, void*, int, uint32_t);
ButtonFn g_buttonNext = nullptr;
int __fastcall ButtonEval(void* block, void* edx, int action, uint32_t mask) {
    Scope scope(block);
    return g_buttonNext(block, edx, action, mask);
}

using WheelFn = int(__fastcall*)(void*);
WheelFn g_wheelUpNext = nullptr, g_wheelDownNext = nullptr;
int WheelAnswer(void* block, int sign) {
    const Device* d = MouseOf(PlayerOfBlock(block));
    if (*reinterpret_cast<const uint32_t*>(kFrontEndState) != 0xE || !d) return 0;
    return sign > 0 ? d->frameWheel > 0 : d->frameWheel < 0;
}
int __fastcall WheelUpHook(void* block) { return Active() && PlayerOfBlock(block) >= 0 ? WheelAnswer(block, 1) : g_wheelUpNext(block); }
int __fastcall WheelDownHook(void* block) {
    return Active() && PlayerOfBlock(block) >= 0 ? WheelAnswer(block, -1) : g_wheelDownNext(block);
}

// The game's mouse poll: GetDeviceState(device, 16, 0x754C24) -> the sum of all mice since the last poll.
using GetStateFn = HRESULT(__stdcall*)(void*, DWORD, void*);
HRESULT __stdcall MouseState(void* device) {
    if (!g_ours) {   // our registration was lost and couldn't be taken back: DirectInput's own reading
        using Fn = HRESULT(__stdcall*)(void*, DWORD, void*);
        return (*reinterpret_cast<Fn**>(device))[9](device, 16, reinterpret_cast<void*>(kMouseState));
    }
    long* out = reinterpret_cast<long*>(kMouseState);   // only applied events are in the sums (OnFrameMultiKbm)
    out[0] = g_mergedDx;
    out[1] = g_mergedDy;
    out[2] = g_mergedWheel;
    out[3] = 0;   // the DI buttons are never read (the game uses window messages)
    g_mergedDx = g_mergedDy = g_mergedWheel = 0;
    for (Device* d : g_devices)
        if (d->mouse) {
            d->frameWheel = d->wheel;
            d->wheel = 0;
        }
    return S_OK;
}
__declspec(naked) void MouseStateStub() {
    __asm {
        push eax          // the mouse device (the game's code had it in eax)
        call MouseState
        ret
    }
}

// After the game's smoother sample: one sample per mouse, the same dt.
using SmootherAddFn = void(__thiscall*)(void*, const long*, float);
SmootherAddFn g_smootherAddNext = nullptr;
void __fastcall SmootherAdd(void* smoother, void*, const long* lxy, float dt) {
    g_smootherAddNext(smoother, lxy, dt);
    for (Device* d : g_devices)
        if (d->mouse) {
            const long move[2] = {d->dx, d->dy};
            reinterpret_cast<SmootherAddFn>(kSmootherAdd)(&d->smoother, move, dt);
            d->dx = d->dy = 0;
        }
}

// After the frame, where the game ages its own tables.
using VoidFn = void(__cdecl*)();
VoidFn g_ageNext = nullptr;
void __cdecl AgeHook() {
    g_ageNext();
    for (Device* d : g_devices) {
        for (uint8_t& s : d->keys) Age(s);
        for (uint8_t& s : d->buttons) Age(s);
        d->clicked = false;
    }
}

bool InstallHooks() {
    bool ok = true;
    const uint8_t readerEntry[] = {0xA1, 0x6C, 0x4C, 0x75, 0x00};
    const struct { uint32_t at; Reader fn; } readers[] = {
        {kKeyHeld, &KeyHeldHook},       {kKeyPressed, &KeyPressedHook},       {kKeyReleased, &KeyReleasedHook},
        {kButtonHeld, &ButtonHeldHook}, {kButtonPressed, &ButtonPressedHook}, {kButtonReleased, &ButtonReleasedHook}};
    for (int i = 0; i < 6; ++i)
        ok = patch::HookFunction(readers[i].at, reinterpret_cast<const void*>(readers[i].fn),
                                 reinterpret_cast<void**>(&g_next[i]), readerEntry, sizeof readerEntry) && ok;
    const uint8_t evalEntry[] = {0x53, 0x55, 0x56, 0x8B, 0xF1};
    ok = patch::HookFunction(kButtonEval, reinterpret_cast<const void*>(&ButtonEval),
                             reinterpret_cast<void**>(&g_buttonNext), evalEntry, sizeof evalEntry) && ok;
    const uint8_t wheelEntry[] = {0x83, 0x3D, 0x18, 0x7C, 0x61, 0x00, 0x0E};
    ok = patch::HookFunction(kWheelUp, reinterpret_cast<const void*>(&WheelUpHook), reinterpret_cast<void**>(&g_wheelUpNext),
                             wheelEntry, sizeof wheelEntry) && ok;
    ok = patch::HookFunction(kWheelDown, reinterpret_cast<const void*>(&WheelDownHook),
                             reinterpret_cast<void**>(&g_wheelDownNext), wheelEntry, sizeof wheelEntry) && ok;
    ok = patch::SubscribeCall(kAgeCall, reinterpret_cast<const void*>(&AgeHook), kAge, 0, reinterpret_cast<void**>(&g_ageNext)) && ok;
    ok = patch::SubscribeCall(kSmootherAddCall, reinterpret_cast<const void*>(&SmootherAdd), kSmootherAdd, 0,
                              reinterpret_cast<void**>(&g_smootherAddNext)) && ok;
    const uint8_t pollBytes[] = {0x68, 0x24, 0x4C, 0x75, 0x00, 0x6A, 0x10, 0x50, 0xFF, 0x51, 0x24};
    ok = patch::WriteCall(kMouseStateSite, reinterpret_cast<const void*>(&MouseStateStub), sizeof pollBytes, pollBytes) && ok;
    return ok;
}
}  // namespace

void features::ApplyMultiKbm() {
    static bool init = false;
    if (!init) {
        init = true;
        InitializeCriticalSection(&g_lock);
    }
}

bool features::KbmStart() {
    if (g_started) return !g_failed;
    if (!settings::Get().featMultiKbm) return false;
    g_started = true;
    g_register = reinterpret_cast<RegisterFn>(GetProcAddress(GetModuleHandleA("user32.dll"), "RegisterRawInputDevices"));
    // the hooks first: once our registration is in, DirectInput's mouse is quiet and the poll must read ours
    if (!g_register || !InstallHooks()) {
        g_failed = true;
        dslog::Write("[fail] Several keyboards / mice: game input hooks not installed - off");
        return false;
    }
    HANDLE ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    CloseHandle(CreateThread(nullptr, 0, RawThread, ready, 0, nullptr));
    WaitForSingleObject(ready, 3000);
    CloseHandle(ready);
    dslog::Write(g_failed ? "[fail] Several keyboards / mice: Raw Input not registered"
                          : "[ok]   Several keyboards / mice: Raw Input running (keyboards and mice told apart)");
    return !g_failed;
}

void features::OnFrameMultiKbm() {
    if (!g_started || g_failed) return;
    std::vector<Event> events;
    EnterCriticalSection(&g_lock);
    events.swap(g_queue);
    LeaveCriticalSection(&g_lock);
    // Raw Input (INPUTSINK) arrives while the game is in the background too: not for the game then. Keys held when it
    // lost the front are let go (the game itself never does).
    // Background test runs (dev): only the pretend devices of dev.kbm_inject count, never the real ones.
    const bool front = GetForegroundWindow() == *reinterpret_cast<HWND*>(0x606A60);
    const bool background = !front && features::GameFocused();
    if (!front && g_wasFocused) {
        for (Device* d : g_devices) ReleaseAll(*d);
        g_mergedDx = g_mergedDy = g_mergedWheel = 0;
    }
    g_wasFocused = front;
    for (const Event& e : events) {
        const bool fake = reinterpret_cast<uintptr_t>(e.handle) >= 0x100 && reinterpret_cast<uintptr_t>(e.handle) < 0x110;
        if (front || (background && fake) || e.removed) Apply(e);
        else if (!Find(e.handle, e.mouse)) Add(e.handle, e.mouse);   // still learn the devices (join screen numbering)
    }
    static int check = 0;
    if (++check >= 10) {   // dinput8 took the registration (the window gained / lost the front): take it back
        check = 0;
        const bool was = g_ours;
        g_ours = OursRegistered() || RegisterOurs(RIDEV_NOHOTKEYS) || RegisterOurs();
        if (was && !g_ours) dslog::Write("[warn] Several keyboards / mice: Raw Input registration lost - DirectInput mouse");
    }
}

int features::KbmDeviceCount(bool mice) {
    int n = 0;
    for (const Device* d : g_devices) n += d->mouse == mice && !d->gone;
    return n;
}

int features::KbmKeyboardIndex(int k) {   // k-th present keyboard -> device number
    for (size_t i = 0; i < g_devices.size(); ++i)
        if (!g_devices[i]->mouse && !g_devices[i]->gone && k-- == 0) return static_cast<int>(i);
    return -1;
}

bool features::KbmKeyHeld(int device, uint32_t code) {
    return device >= 0 && device < static_cast<int>(g_devices.size()) && !g_devices[device]->mouse && code < kCodes &&
           g_devices[device]->keys[code] != 0;
}

bool features::KbmDevicePresent(int device) {
    return device >= 0 && device < static_cast<int>(g_devices.size()) && !g_devices[device]->gone;
}

int features::KbmMouseClicked() {
    for (size_t i = 0; i < g_devices.size(); ++i)
        if (g_devices[i]->mouse && g_devices[i]->clicked && !g_devices[i]->gone) return static_cast<int>(i);
    return -1;
}

void features::KbmSetPlayers(const int keyboard[4], const int mouse[4]) {
    int keyboards = 0;
    for (int p = 0; p < kMaxPlayers; ++p) {
        g_playerKeyboard[p] = keyboard ? keyboard[p] : -1;
        g_playerMouse[p] = mouse ? mouse[p] : -1;
        keyboards += g_playerKeyboard[p] >= 0;
    }
    const bool mapped = g_started && !g_failed && keyboards >= 2;
    if (mapped != g_mapped || mapped) {
        dslog::Write("Several keyboards / mice: %s", mapped ? "each keyboard player reads their own devices" : "merged");
        for (int p = 0; mapped && p < kMaxPlayers; ++p)
            if (g_playerKeyboard[p] >= 0)
                dslog::Write("  player %d: keyboard %d, mouse %d", p + 1, g_playerKeyboard[p], g_playerMouse[p]);
    }
    g_mapped = mapped;
}

void* features::KbmSmoother(const void* block) {
    if (!Active()) return nullptr;
    const int p = PlayerOfBlock(block);
    if (p < 0 || g_playerKeyboard[p] < 0) return nullptr;
    Device* d = MouseOf(p);
    static Smoother still;   // a keyboard player without a mouse: no movement
    if (!still.ring) {
        still.ring = still.sample;
        reinterpret_cast<void(__thiscall*)(void*)>(kSmootherReset)(&still);
    }
    return d ? static_cast<void*>(&d->smoother) : &still;
}

int features::KbmEnter(const void* block) {
    const int prev = g_ctx;
    g_ctx = PlayerOfBlock(block);
    return prev;
}
void features::KbmLeave(int prev) { g_ctx = prev; }

#ifndef DS_DIST
// dev (devserver dev.kbm_inject): an event from a pretend device (handle = 0x100 + n), for scripted tests on a PC with
// one keyboard and one mouse.
void features::KbmInject(int fake, bool mouse, int code, bool up, int dx, int dy, int buttonFlags, int wheel) {
    Event e = {};
    e.handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x100 + fake));
    e.mouse = mouse;
    e.code = static_cast<uint16_t>(code);
    e.up = up;
    e.dx = dx;
    e.dy = dy;
    e.buttonFlags = static_cast<uint16_t>(buttonFlags);
    e.wheel = static_cast<short>(wheel);
    if (!g_started) return;
    Queue(e);
}
#else
void features::KbmInject(int, bool, int, bool, int, int, int, int) {}   // (in dsfix.coop's table in every build)
#endif
