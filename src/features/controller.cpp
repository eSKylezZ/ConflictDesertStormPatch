// Joypads in game. The game reads every joystick with DirectInput (FUN_00538ed0: Poll + GetDeviceState into a
// DIJOYSTATE2) and expects the layout of a PS2 pad on a USB adapter: buttons 0 Triangle 1 Circle 2 Cross 3 Square
// 4 L2 5 R2 6 L1 7 R1 8 Select 9 L3 10 R3 11 Start 12-15 D-pad up/right/down/left, left stick lX/lY and right stick
// lZ/lRz (range +-128 and deadzone set by the game on those four axes only, 0x5388fb..0x5389ec).
//
// Right after GetDeviceState (0x538F2A, `cmp [0x754c6c], edi` - 6 bytes -> call PadStub; nop) the state is rewritten
// per pad type, found once per device from its vendor id (DIDEVICEINSTANCE guidProduct.Data1 = MAKELONG(vid, pid)):
//   Pads read directly by padio.cpp (XInput pads, DualSense / DualShock 4 over HID) are filled from there instead.
//   PlayStation (and any other pad) through DirectInput: 0 Square 1 Cross 2 Circle 3 Triangle 4 L1 5 R1 6 L2 7 R2
//     8 Create 9 Options 10 L3 11 R3 12 PS 13 touchpad (-> Select too); sticks already on X/Y and Z/Rz.
//   Xbox (vendor 0x045E): 0 A 1 B 2 X 3 Y 4 LB 5 RB 6 View 7 Menu 8 LS 9 RS; right stick on lRx/lRy (0..65535,
//     no range/deadzone from the game -> scaled and dead-zoned here), both triggers on lZ (LT +, RT -) -> L2/R2
//     buttons, lZ/lRz then carry the right stick.
// The POV hat becomes buttons 12-15 for every pad. Bindings (default.key, KEY ASSIGNMENT, per-player layouts)
// then work in the game's numbering whatever pad is used.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/padio.h"
#include "generated/patch_tables.h"

namespace {
constexpr uint32_t kDeadzoneVa = 0x538997;  // DIPROP_DEADZONE immediate, 0-10000
constexpr uint32_t kPollSite = 0x538F2A;    // cmp dword ptr [0x754c6c], edi
constexpr uint8_t kPollOriginal[] = {0x39, 0x3D, 0x6C, 0x4C, 0x75, 0x00};
constexpr uint32_t kJoysticks = 0x754BC8, kCurrentJoystick = 0x754C34, kJoystickCount = 0x754BE8;
constexpr int kMaxJoysticks = 8;

using padio::JoyState;

// Game button i <- DirectInput button (0xFF = none). PlayStation DirectInput: 4 L1 5 R1 6 L2 7 R2 (the game: 4 L2
// 5 R2 6 L1 7 R1 - an earlier table passed 4-7 straight through, which put fire on R2 while every prompt said R1).
constexpr uint8_t kPlayStationMap[12] = {3, 2, 1, 0, 6, 7, 4, 5, 8, 10, 11, 9};
constexpr uint8_t kXboxMap[12] = {3, 1, 0, 2, 0xFF, 0xFF, 4, 5, 6, 8, 9, 7};

struct Known {
    void* device;
    features::PadType type;
} g_known[kMaxJoysticks];

features::PadType TypeOf(void* dev) {
    struct Instance {
        DWORD size;
        GUID instance, product;
        DWORD devType;
        char instanceName[260], productName[260];
        GUID ffDriver;
        WORD usagePage, usage;
    } info{};
    info.size = sizeof info;
    using GetInfo = HRESULT(__stdcall*)(void*, Instance*);
    if (FAILED(reinterpret_cast<GetInfo>((*reinterpret_cast<void***>(dev))[15])(dev, &info)))
        return features::PadType::Generic;
    const WORD vid = LOWORD(info.product.Data1);
    const auto type = vid == 0x054C ? features::PadType::PlayStation
                      : vid == 0x045E ? features::PadType::Xbox
                                      : features::PadType::Generic;
    dslog::Write("Pad: \"%s\" (vendor %04X product %04X) -> %s layout", info.productName, vid,
                 HIWORD(info.product.Data1), type == features::PadType::Xbox ? "Xbox" : "PlayStation");
    return type;
}

// 0..65535 -> -128..128 with the game's deadzone (percent of the half range, like DIPROP_DEADZONE).
LONG ScaleAxis(LONG raw) {
    float v = (static_cast<float>(raw) - 32767.5f) / 32767.5f;
    const float dz = settings::Get().padDeadzone / 100.0f;
    const float mag = v < 0 ? -v : v;
    if (mag <= dz || dz >= 1.0f) return 0;
    v = (v < 0 ? -1.0f : 1.0f) * (mag - dz) / (1.0f - dz);
    return static_cast<LONG>(v * 128.0f);
}

void Fix(JoyState* s, features::PadType type) {
    BYTE raw[12];
    memcpy(raw, s->buttons, sizeof raw);
    const bool xbox = type == features::PadType::Xbox;
    const uint8_t* map = xbox ? kXboxMap : kPlayStationMap;
    const BYTE touchpad = xbox ? 0 : s->buttons[13];  // DualShock 4 / DualSense touchpad click
    for (int i = 0; i < 12; ++i) s->buttons[i] = map[i] == 0xFF ? 0 : raw[map[i]];
    s->buttons[8] |= touchpad;
    if (xbox) {
        constexpr LONG kTrigger = 48;  // of 128 (after the game's deadzone on lZ)
        s->buttons[4] = s->z > kTrigger ? 0x80 : 0;   // LT
        s->buttons[5] = s->z < -kTrigger ? 0x80 : 0;  // RT
        s->z = ScaleAxis(s->rx);
        s->rz = ScaleAxis(s->ry);
    }
    const DWORD pov = s->pov[0];
    bool dir[4] = {};
    if (LOWORD(pov) != 0xFFFF) {
        const int oct = static_cast<int>(((pov + 2250) / 4500) % 8);  // 0 up, 2 right, 4 down, 6 left
        dir[0] = oct == 7 || oct <= 1;
        dir[1] = oct >= 1 && oct <= 3;
        dir[2] = oct >= 3 && oct <= 5;
        dir[3] = oct >= 5 && oct <= 7;
    }
    for (int i = 0; i < 4; ++i) s->buttons[12 + i] = dir[i] ? 0x80 : 0;
}

// Pads read directly (XInput / HID, padio.cpp) replace the DirectInput state; the rest is remapped.
void __cdecl FixState(JoyState* s) {
    const int j = *reinterpret_cast<int*>(kCurrentJoystick);
    if (j < 0 || j >= kMaxJoysticks || !reinterpret_cast<void**>(kJoysticks)[j]) return;
    if (!padio::Fill(j, *s)) Fix(s, features::JoystickType(j));
}

// At 0x538F2A: the state is at [esp+8] of the poll function = [esp+0x2c] after our return address and pushad.
__declspec(naked) void PadStub() {
    __asm {
        pushad
        lea eax, [esp + 0x2C]
        push eax
        call FixState
        add esp, 4
        popad
        cmp dword ptr ds:[0x754C6C], edi
        ret
    }
}
}  // namespace

// Player 1's input block reads one joystick (block +0x3c8, 0 at every level load), so with two pads only the first
// enumerated one worked in the menus and in single player. Outside a co-op mission it follows the pad used last:
// any button held (game button states 0x7541B8 + j * 0x150, bit 4 = down) or a stick pushed past half way (axes
// 0x754270 + j * 0x150: lY, lX, lRz, lZ as -1..1).
// In a co-op mission each player keeps their pad; but the pause menu (and other menus: any front-end state other
// than 0xE) is read from player 1's block only, so while one is open player 1's block takes every pad and the
// keyboard, and gets its own joystick / devices back when play resumes.
void features::FollowActivePad() {
    auto* block0 = reinterpret_cast<uint8_t*>(0x60F5B8);
    auto& joystick0 = *reinterpret_cast<int32_t*>(block0 + 0x3C8);
    auto& devices0 = *reinterpret_cast<uint32_t*>(block0 + 0x38);
    static int32_t savedJoystick = -2;
    static uint32_t savedDevices = 0;
    if (features::CoopPlayers() >= 2 && features::SplitScreenPlayers() >= 2) {
        const bool menu = *reinterpret_cast<uint32_t*>(0x617C18) != 0xE;
        if (!menu) {
            if (savedJoystick != -2) joystick0 = savedJoystick, devices0 = savedDevices, savedJoystick = -2;
            return;  // playing: own pads
        }
        if (savedJoystick == -2) savedJoystick = joystick0, savedDevices = devices0;
        devices0 = 3;  // keyboard & mouse and pad
    } else if (savedJoystick != -2) {
        joystick0 = savedJoystick, devices0 = savedDevices, savedJoystick = -2;
    }
    if (!(devices0 & 2)) return;
    const int count = std::min(*reinterpret_cast<int*>(kJoystickCount), kMaxJoysticks);
    for (int j = 0; j < count; ++j) {
        if (!reinterpret_cast<void**>(kJoysticks)[j]) continue;
        bool active = false;
        const auto* buttons = reinterpret_cast<const uint32_t*>(0x7541B8 + j * 0x150);
        for (int b = 0; b < 16 && !active; ++b) active = (buttons[b] & 4) != 0;
        const auto* axes = reinterpret_cast<const float*>(0x754270 + j * 0x150);
        for (int a = 0; a < 4 && !active; ++a) active = axes[a * 3] > 0.5f || axes[a * 3] < -0.5f;
        if (active && joystick0 != j) {
            joystick0 = j;
            dslog::Write("Pad: player 1 now uses joystick %d", j);
            return;
        }
    }
}

void features::ForgetJoystick(int joystick) {
    if (joystick >= 0 && joystick < kMaxJoysticks) g_known[joystick] = {};
    padio::Forget(joystick);
}

features::PadType features::JoystickType(int joystick) {
    if (joystick < 0 || joystick >= kMaxJoysticks) return PadType::Generic;
    void* dev = reinterpret_cast<void**>(kJoysticks)[joystick];
    if (!dev) return PadType::Generic;
    Known& k = g_known[joystick];
    if (k.device != dev) k = {dev, TypeOf(dev)};
    return k.type;
}

uint32_t features::ReadJoystick(int joystick) {
    if (joystick < 0 || joystick >= kMaxJoysticks) return 0;
    void* dev = reinterpret_cast<void**>(kJoysticks)[joystick];
    if (!dev) return 0;
    JoyState s{};
    if (!padio::Fill(joystick, s)) {
        uint8_t buffer[0x110] = {};
        void** vt = *reinterpret_cast<void***>(dev);
        reinterpret_cast<HRESULT(__stdcall*)(void*)>(vt[25])(dev);  // Poll
        if (FAILED(reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, void*)>(vt[9])(dev, sizeof buffer, buffer))) return 0;
        memcpy(&s, buffer, sizeof s);
        Fix(&s, JoystickType(joystick));
    }
    uint32_t bits = 0;
    for (int i = 0; i < 16; ++i)
        if (s.buttons[i] & 0x80) bits |= 1u << i;
    constexpr LONG kPush = 64;  // of 128
    if (s.y < -kPush) bits |= 1u << 16;
    if (s.x > kPush) bits |= 1u << 17;
    if (s.y > kPush) bits |= 1u << 18;
    if (s.x < -kPush) bits |= 1u << 19;
    return bits;
}

// CONTROLLER screen: a new deadzone for every pad - saved, used by our own scaling (padio, Xbox right stick) and set
// on the joysticks already open (the game sets DIPROP_DEADZONE on X / Y / Z / Rz once, when it opens a device).
void features::SetPadDeadzone(uint32_t percent) {
    settings::Values v = settings::Get();
    v.padDeadzone = std::min<uint32_t>(percent, 90);
    if (!settings::Save(v)) return;
    if (patch::Matches(kPollSite, kPollOriginal, sizeof kPollOriginal)) return;  // controller support off
    const int32_t value = static_cast<int32_t>(v.padDeadzone * 100);
    patch::WriteValue(kDeadzoneVa, value);
    struct {
        DWORD size, headerSize, obj, how, data;
    } prop{sizeof prop, 16, 0, 1 /* DIPH_BYOFFSET */, static_cast<DWORD>(value)};
    for (int j = 0; j < kMaxJoysticks; ++j) {
        void* dev = reinterpret_cast<void**>(kJoysticks)[j];
        if (!dev) continue;
        for (DWORD axis : {0u, 4u, 8u, 0x14u}) {  // lX, lY, lZ, lRz
            prop.obj = axis;
            using SetProperty = HRESULT(__stdcall*)(void*, uintptr_t, void*);
            reinterpret_cast<SetProperty>((*reinterpret_cast<void***>(dev))[6])(dev, 5 /* DIPROP_DEADZONE */, &prop);
        }
    }
    dslog::Write("Pad: deadzone %u%%", v.padDeadzone);
}

void features::ApplyController() {
    const auto& s = settings::Get();
    uint8_t call[6] = {0xE8, 0, 0, 0, 0, 0x90};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&PadStub) - (kPollSite + 5));
    memcpy(call + 1, &rel, 4);
    if (!s.controller) {
        patch::RevertGroup("Controller", gen::kPad);
        if (patch::Matches(kPollSite, call, sizeof call)) patch::Write(kPollSite, kPollOriginal, sizeof kPollOriginal);
        return;
    }
    if (!patch::Matches(kPollSite, kPollOriginal, sizeof kPollOriginal) && !patch::Matches(kPollSite, call, sizeof call)) {
        dslog::Write("[fail] Controller: unexpected code at 0x%08X", kPollSite);
        return;
    }
    if (patch::ApplyGroup("Controller", gen::kPad)) {
        patch::WriteValue(kDeadzoneVa, static_cast<int32_t>(s.padDeadzone * 100));
        patch::Write(kPollSite, call, sizeof call);
    }
}
