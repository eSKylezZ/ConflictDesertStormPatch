#include "core/gamepad.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <xinput.h>

#include <initializer_list>

#include "core/log.h"
#include "core/proxy.h"

namespace gamepad {
namespace {
constexpr WORD kSonyVid = 0x054C;
constexpr LONG kStickThreshold = 500;  // of +-1000
constexpr int kMaxDevices = 4;

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_xinputGetState = nullptr;

IDirectInput8A* g_di = nullptr;
IDirectInputDevice8A* g_devices[kMaxDevices] = {};
int g_deviceCount = 0;
HWND g_owner = nullptr;
DWORD g_lastEnum = 0;
Type g_lastType = Type::None;

void LoadXInput() {
    if (g_xinputGetState) return;
    for (const char* dll : {"xinput1_4.dll", "xinput9_1_0.dll", "xinput1_3.dll"}) {
        if (HMODULE m = LoadLibraryA(dll)) {
            g_xinputGetState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"));
            if (g_xinputGetState) return;
        }
    }
}

BOOL CALLBACK OnDevice(const DIDEVICEINSTANCEA* inst, void*) {
    if (g_deviceCount >= kMaxDevices) return DIENUM_STOP;
    if (LOWORD(inst->guidProduct.Data1) != kSonyVid) return DIENUM_CONTINUE;  // everything else via XInput
    IDirectInputDevice8A* dev = nullptr;
    if (FAILED(g_di->CreateDevice(inst->guidInstance, &dev, nullptr))) return DIENUM_CONTINUE;
    DIPROPRANGE range{};
    range.diph.dwSize = sizeof range;
    range.diph.dwHeaderSize = sizeof range.diph;
    range.diph.dwHow = DIPH_BYOFFSET;
    range.lMin = -1000;
    range.lMax = 1000;
    if (FAILED(dev->SetDataFormat(&c_dfDIJoystick2)) ||
        FAILED(dev->SetCooperativeLevel(g_owner, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
        dev->Release();
        return DIENUM_CONTINUE;
    }
    for (DWORD axis : {DIJOFS_X, DIJOFS_Y}) {
        range.diph.dwObj = axis;
        dev->SetProperty(DIPROP_RANGE, &range.diph);
    }
    dev->Acquire();
    g_devices[g_deviceCount++] = dev;
    dslog::Write("Gamepad: DirectInput %s", inst->tszProductName);
    return DIENUM_CONTINUE;
}

void Enumerate() {
    g_lastEnum = GetTickCount();
    if (g_di && g_deviceCount == 0) g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, OnDevice, nullptr, DIEDFL_ATTACHEDONLY);
}

uint32_t Directions(bool up, bool down, bool left, bool right) {
    return (up ? kUp : 0) | (down ? kDown : 0) | (left ? kLeft : 0) | (right ? kRight : 0);
}

uint32_t PollXInput(bool& connected, bool& activity) {
    uint32_t out = 0;
    if (!g_xinputGetState) return 0;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE s{};
        if (g_xinputGetState(i, &s) != ERROR_SUCCESS) continue;
        connected = true;
        const XINPUT_GAMEPAD& g = s.Gamepad;
        const SHORT t = 16000;
        out |= Directions((g.wButtons & XINPUT_GAMEPAD_DPAD_UP) || g.sThumbLY > t,
                          (g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) || g.sThumbLY < -t,
                          (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) || g.sThumbLX < -t,
                          (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) || g.sThumbLX > t);
        if (g.wButtons & XINPUT_GAMEPAD_A) out |= kAccept;
        if (g.wButtons & XINPUT_GAMEPAD_B) out |= kBack;
        if (g.wButtons & XINPUT_GAMEPAD_START) out |= kStart;
        if (g.wButtons & XINPUT_GAMEPAD_Y) out |= kAlt;
        const SHORT dz = 8000;
        if (g.wButtons || g.bLeftTrigger > 30 || g.bRightTrigger > 30 || g.sThumbLX > dz || g.sThumbLX < -dz ||
            g.sThumbLY > dz || g.sThumbLY < -dz || g.sThumbRX > dz || g.sThumbRX < -dz || g.sThumbRY > dz ||
            g.sThumbRY < -dz)
            activity = true;
    }
    return out;
}

// DualShock 4 / DualSense DirectInput layout: 0 Square 1 Cross 2 Circle 3 Triangle ... 9 Options; D-pad = POV.
uint32_t PollDirectInput(bool& connected, bool& activity) {
    uint32_t out = 0;
    for (int i = 0; i < g_deviceCount; ++i) {
        IDirectInputDevice8A* dev = g_devices[i];
        DIJOYSTATE2 js{};
        if (FAILED(dev->Poll())) dev->Acquire();
        if (FAILED(dev->GetDeviceState(sizeof js, &js))) continue;
        connected = true;
        DWORD pov = js.rgdwPOV[0];
        bool hat = LOWORD(pov) != 0xFFFF;
        auto sector = [&](DWORD lo, DWORD hi) { return hat && ((lo < hi) ? (pov >= lo && pov <= hi) : (pov >= lo || pov <= hi)); };
        out |= Directions(sector(31500, 4500) || js.lY < -kStickThreshold, sector(13500, 22500) || js.lY > kStickThreshold,
                          sector(22500, 31500) || js.lX < -kStickThreshold, sector(4500, 13500) || js.lX > kStickThreshold);
        if (js.rgbButtons[1] & 0x80) out |= kAccept;
        if (js.rgbButtons[2] & 0x80) out |= kBack;
        if (js.rgbButtons[9] & 0x80) out |= kStart;
        if (js.rgbButtons[3] & 0x80) out |= kAlt;
        for (int b = 0; b < 16; ++b)
            if (js.rgbButtons[b] & 0x80) activity = true;
        if (hat || js.lX < -kStickThreshold || js.lX > kStickThreshold || js.lY < -kStickThreshold ||
            js.lY > kStickThreshold)
            activity = true;
    }
    return out;
}
}  // namespace

bool Open(HWND owner) {
    LoadXInput();
    if (!g_di) {
        using Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
        auto create = reinterpret_cast<Create>(RealDInput8("DirectInput8Create"));
        if (create) create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
                           reinterpret_cast<void**>(&g_di), nullptr);
    }
    g_owner = owner;
    Enumerate();
    return g_xinputGetState || g_di;
}

void Close() {
    for (int i = 0; i < g_deviceCount; ++i) {
        g_devices[i]->Unacquire();
        g_devices[i]->Release();
        g_devices[i] = nullptr;
    }
    g_deviceCount = 0;
    if (g_di) g_di->Release();
    g_di = nullptr;
    g_owner = nullptr;
}

State Poll() {
    if (g_deviceCount == 0 && GetTickCount() - g_lastEnum > 3000) Enumerate();  // hot-plug (Sony pads)
    bool xConnected = false, dConnected = false, xActivity = false, dActivity = false;
    uint32_t x = PollXInput(xConnected, xActivity);
    uint32_t d = PollDirectInput(dConnected, dActivity);
#ifndef DS_DIST
    // Dev builds only: test scripts inject presses through HKCU\Software\DesertStormFix\Dev "PadInject"
    // (Button bits; bit 31 set = act as an Xbox pad, else PlayStation).
    DWORD inject = 0, size = sizeof inject;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "PadInject", RRF_RT_REG_DWORD, nullptr,
                     &inject, &size) == ERROR_SUCCESS &&
        inject) {
        (inject >> 31 ? x : d) |= inject & 0xFF;
        (inject >> 31 ? xConnected : dConnected) = true;
    }
#endif
    if (x || xActivity) g_lastType = Type::Xbox;
    else if (d || dActivity) g_lastType = Type::PlayStation;
    else if (g_lastType == Type::None) g_lastType = dConnected ? Type::PlayStation : xConnected ? Type::Xbox : Type::None;
    if (!xConnected && !dConnected) g_lastType = Type::None;
    return State{x | d, g_lastType, xActivity || dActivity || (x | d) != 0};
}
}  // namespace gamepad
