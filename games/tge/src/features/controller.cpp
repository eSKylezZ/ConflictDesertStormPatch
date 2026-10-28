// Joypads in game (tgefix.controller): port of DesertStormFix's controller.cpp (games/ds1/src/features/controller.cpp;
// DS1 addresses in brackets) - TGE's DirectInput joystick code is the same code at other addresses (mapped 2026-10-25).
//
// The game reads every joystick with DirectInput (poll FUN_004b98c0 [FUN_00538ed0]: Poll + GetDeviceState into a
// DIJOYSTATE2) and its default.key expects the layout of a PS2 pad on a USB adapter, as DS1's: buttons 0 Triangle
// 1 Circle 2 Cross 3 Square 4 L2 5 R2 6 L1 7 R1 8 Select 9 L3 10 R3 11 Start 12-15 D-pad up/right/down/left, left stick
// lX/lY and right stick lZ/lRz (range +-128 and deadzone 0xFA0 set by the enumeration callback FUN_004b9360
// [0x538800] on those four axes; deadzone immediate 0x4B9585 [0x538997]).
//
// Game state: joysticks IDirectInputDevice8A* 0x7A9D24[8] [0x754BC8], count [0x7AA888] [0x754BE8], current joystick
// [0x7AB0AC] [0x754C34] (the poll and the callback work on it), enumeration FUN_004b9680 [FUN_00538bb0], cooperative
// level push 5 at 0x4B9485, force-feedback table 0x7AB0B8 + j*0x1C [0x753058] (rumble - not driven yet).
//
// Logitech WingMan pads: the callback compares the upper-cased product name with "LOGITECH WINGMAN RUMBLEPAD"
// ([0x7AB1C4] = 1) / "LOGITECH WINGMAN" ([0x7AB1C0] = 1) and the key file loader (0x5D6A61) then takes wcurrent.key /
// rcurrent.key (from wingman.key / wingmanrumble.key) instead of current.key - those pads keep their own numbering here.
//
// Right after GetDeviceState (0x4B991A, `cmp [0x7AB1A8], esi` - 6 bytes -> call PadStub; nop) the state is rewritten
// per pad type, found once per device from its vendor id (DIDEVICEINSTANCE guidProduct.Data1 = MAKELONG(vid, pid)):
//   PlayStation (and any other pad) through DirectInput: 0 Square 1 Cross 2 Circle 3 Triangle 4 L1 5 R1 6 L2 7 R2
//     8 Create 9 Options 10 L3 11 R3 12 PS 13 touchpad (-> Select too); sticks already on X/Y and Z/Rz.
//   Xbox (vendor 0x045E): 0 A 1 B 2 X 3 Y 4 LB 5 RB 6 View 7 Menu 8 LS 9 RS; right stick on lRx/lRy (0..65535,
//     no range/deadzone from the game -> scaled and dead-zoned here), both triggers on lZ (LT +, RT -) -> L2/R2
//     buttons, lZ/lRz then carry the right stick.
// The POV hat becomes buttons 12-15 for every pad (WingMan too: the game reads no POV). Xbox pads and DualSense /
// DualShock 4 are read directly instead when padio.cpp can (XInput, HID: rumble, light bar, adaptive trigger).
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

#include "core/log.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "features/features.h"
#include "features/overlay.h"
#include "features/padio.h"
#include "loader.h"

namespace {
constexpr uint32_t kDeadzoneVa = 0x4B9585;  // DIPROP_DEADZONE immediate, 0-10000 (mov [esp+0x28], 0xFA0)
constexpr uint8_t kDeadzoneOriginal[] = {0xA0, 0x0F, 0x00, 0x00};
constexpr uint32_t kPollSite = 0x4B991A;    // cmp dword ptr [0x7AB1A8], esi
constexpr uint8_t kPollOriginal[] = {0x39, 0x35, 0xA8, 0xB1, 0x7A, 0x00};
constexpr uint32_t kJoysticks = 0x7A9D24, kCurrentJoystick = 0x7AB0AC, kJoystickCount = 0x7AA888;
// Input blocks (DS1 0x60F5B8 + i*0x478): block i = 0x7DCAB0 + i*0x3FC, +0x14 devices (bit 0 keyboard / mouse, bit 1 pad),
// +0x2E0 joystick (-1 = block off). Read by the binding evaluator FUN_005b89a0(block, action, ...) [FUN_00450770].
constexpr uint32_t kBlock0 = 0x7DCAB0;
// Per joystick (stride 0x150): button states 0x7A9DE0 + j*0x150 (u32 x 28, bit 2 = down) [0x7541B8], axes
// 0x7A9E98 + j*0x150 (float -1..1, every third: lY, lX, lRz, lZ) [0x754270].
constexpr uint32_t kButtonStates = 0x7A9DE0, kAxes = 0x7A9E98, kJoyStride = 0x150;
constexpr int kMaxJoysticks = 8;

using padio::JoyState;

enum class PadType { PlayStation, Xbox, Native };   // Native = WingMan: the game's own key files fit it

// Game button i <- DirectInput button (0xFF = none); as DS1.
constexpr uint8_t kPlayStationMap[12] = {3, 2, 1, 0, 6, 7, 4, 5, 8, 10, 11, 9};
constexpr uint8_t kXboxMap[12] = {3, 1, 0, 2, 0xFF, 0xFF, 4, 5, 6, 8, 9, 7};

struct Known {
    void* device;
    PadType type;
} g_known[kMaxJoysticks];

PadType TypeOf(void* dev) {
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
    if (FAILED(reinterpret_cast<GetInfo>((*reinterpret_cast<void***>(dev))[15])(dev, &info))) return PadType::PlayStation;
    const WORD vid = LOWORD(info.product.Data1);
    char upper[260];
    memcpy(upper, info.productName, sizeof upper);
    upper[sizeof upper - 1] = 0;
    CharUpperA(upper);
    const PadType type = strstr(upper, "LOGITECH WINGMAN") ? PadType::Native
                         : vid == 0x045E                    ? PadType::Xbox
                                                            : PadType::PlayStation;
    dslog::Write("Pad: \"%s\" (vendor %04X product %04X) -> %s", info.productName, vid, HIWORD(info.product.Data1),
                 type == PadType::Xbox ? "Xbox layout" : type == PadType::Native ? "the game's WingMan layout"
                                                                                 : "PlayStation layout");
    return type;
}

PadType JoystickType(int joystick, void* dev) {
    Known& k = g_known[joystick];
    if (k.device != dev) k = {dev, TypeOf(dev)};
    return k.type;
}

// 0..65535 -> -128..128 with the deadzone (percent of the half range, like DIPROP_DEADZONE).
LONG ScaleAxis(LONG raw) {
    float v = (static_cast<float>(raw) - 32767.5f) / 32767.5f;
    const float dz = features::Get().padDeadzone / 100.0f;
    const float mag = v < 0 ? -v : v;
    if (mag <= dz || dz >= 1.0f) return 0;
    v = (v < 0 ? -1.0f : 1.0f) * (mag - dz) / (1.0f - dz);
    return static_cast<LONG>(v * 128.0f);
}

void Fix(JoyState* s, PadType type) {
    if (type != PadType::Native) {
        BYTE raw[12];
        memcpy(raw, s->buttons, sizeof raw);
        const bool xbox = type == PadType::Xbox;
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
    }
    const DWORD pov = s->pov[0];
    if (LOWORD(pov) == 0xFFFF) {
        if (type != PadType::Native) for (int i = 12; i < 16; ++i) s->buttons[i] = 0;
        return;
    }
    const int oct = static_cast<int>(((pov + 2250) / 4500) % 8);  // 0 up, 2 right, 4 down, 6 left
    const bool dir[4] = {oct == 7 || oct <= 1, oct >= 1 && oct <= 3, oct >= 3 && oct <= 5, oct >= 5 && oct <= 7};
    for (int i = 0; i < 4; ++i)
        if (dir[i] || type != PadType::Native) s->buttons[12 + i] = dir[i] ? 0x80 : 0;
}

// Pads read directly (XInput / HID, padio.cpp) replace the DirectInput state; the rest is remapped. PadModernLayout
// then swaps the shoulder buttons and triggers (game 4 L2 <-> 6 L1, 5 R2 <-> 7 R1): FIRE (7) on R2 / RT.
void __cdecl FixState(JoyState* s) {
    const int j = *reinterpret_cast<int*>(kCurrentJoystick);
    if (j < 0 || j >= kMaxJoysticks) return;
    void* dev = reinterpret_cast<void**>(kJoysticks)[j];
    if (!dev) return;
    const PadType type = JoystickType(j, dev);
    if (type == PadType::Native || !padio::Fill(j, *s)) Fix(s, type);
    if (type != PadType::Native && features::Get().padModernLayout) {
        std::swap(s->buttons[4], s->buttons[6]);
        std::swap(s->buttons[5], s->buttons[7]);
    }
}

// At 0x4B991A: the state is at [esp+8] of the poll function = [esp+0x2c] after our return address and pushad. The
// replaced instruction compares with esi (DS1's: edi).
__declspec(naked) void PadStub() {
    __asm {
        pushad
        lea eax, [esp + 0x2C]
        push eax
        call FixState
        add esp, 4
        popad
        cmp dword ptr ds:[0x7AB1A8], esi
        ret
    }
}
// Player 1's input block reads one joystick (+0x2E0, 0 from the start), so with two pads only the first enumerated
// one worked. As DS1's FollowActivePad: player 1 follows the pad with a button held or a stick past half way.
int PSE_CALL FollowActivePad(PSEEventId, void*, void*) {
    features::OnFrameHotplug();
    padio::Update();
    auto* block0 = reinterpret_cast<uint8_t*>(kBlock0);
    auto& joystick0 = *reinterpret_cast<int32_t*>(block0 + 0x2E0);
    if (joystick0 < 0 || !(*reinterpret_cast<uint32_t*>(block0 + 0x14) & 2)) return 0;
    const int count = std::min(*reinterpret_cast<int*>(kJoystickCount), kMaxJoysticks);
    if (count < 2) return 0;
    for (int j = 0; j < count; ++j) {
        if (j == joystick0 || !reinterpret_cast<void**>(kJoysticks)[j]) continue;
        bool active = false;
        const auto* buttons = reinterpret_cast<const uint32_t*>(kButtonStates + j * kJoyStride);
        for (int b = 0; b < 16 && !active; ++b) active = (buttons[b] & 4) != 0;
        const auto* axes = reinterpret_cast<const float*>(kAxes + j * kJoyStride);
        for (int a = 0; a < 4 && !active; ++a) active = axes[a * 3] > 0.5f || axes[a * 3] < -0.5f;
        if (active) {
            joystick0 = j;
            dslog::Write("Pad: player 1 now uses joystick %d", j);
            return 0;
        }
    }
    return 0;
}
}  // namespace

#ifndef DS_DIST
// Dev\ShowPad = 1 (PS5) / 2 (Xbox): the controller picture over the game (overlay / callout checks).
static void DevPadPreview() {
    static DWORD checked = 0, value = 0;
    if (GetTickCount() - checked > 1000) {
        checked = GetTickCount();
        DWORD size = sizeof value;
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\GreatEscapeFix\\Dev", "ShowPad", RRF_RT_REG_DWORD, nullptr, &value,
                         &size) != ERROR_SUCCESS)
            value = 0;
    }
    if (!value) return;
    float w, h;
    overlay::ScreenSize(w, h);
    overlay::QueueRect(0, 0, w, h, 0xC0000000);
    features::DrawControllerPicture(w * 0.05f, h * 0.1f, w * 0.9f, h * 0.8f, value == 2);
}
#endif

void features::ApplyPadDeadzone() {
    const uint32_t dz = std::min<uint32_t>(Get().padDeadzone, 90) * 100;
    if (Get().controller) patch::WriteValue(kDeadzoneVa, static_cast<int32_t>(dz));   // pads opened from now on
    struct PropDword {
        DWORD size, headerSize, obj, how, data;
    };
    constexpr DWORD kDeadzoneAxes[] = {0, 4, 8, 20};   // DIJOFS_X, _Y, _Z, _RZ (the axes the game gives a deadzone)
    const int count = std::min(*reinterpret_cast<int*>(kJoystickCount), kMaxJoysticks);
    for (int j = 0; j < count; ++j) {
        void* dev = reinterpret_cast<void**>(kJoysticks)[j];
        if (!dev) continue;
        for (DWORD axis : kDeadzoneAxes) {
            PropDword p{sizeof p, 16, axis, 1 /* DIPH_BYOFFSET */, dz};
            using SetProperty = HRESULT(__stdcall*)(void*, uintptr_t, const PropDword*);
            reinterpret_cast<SetProperty>((*reinterpret_cast<void***>(dev))[6])(dev, 5 /* DIPROP_DEADZONE */, &p);
        }
    }
}

bool features::PadInUseIsXbox() {
    const int j = *reinterpret_cast<const int32_t*>(kBlock0 + 0x2E0);
    if (j >= 0 && j < kMaxJoysticks && g_known[j].device) return g_known[j].type == PadType::Xbox;
    for (const Known& k : g_known)
        if (k.device) return k.type == PadType::Xbox;
    return false;
}

void features::ForgetJoystick(int joystick) {
    if (joystick >= 0 && joystick < kMaxJoysticks) g_known[joystick] = {};
    padio::Forget(joystick);
}

void features::ApplyController() {
    const Settings& s = Get();
    if (!s.controller) {
        dslog::Write("[off]  Controller: pads keep the game's DirectInput numbering (Controller)");
        return;
    }
    if (!patch::Matches(kPollSite, kPollOriginal, sizeof kPollOriginal) ||
        !patch::Matches(kDeadzoneVa, kDeadzoneOriginal, sizeof kDeadzoneOriginal)) {
        dslog::Write("[fail] Controller: unexpected code at 0x%08X / 0x%08X", kPollSite, kDeadzoneVa);
        return;
    }
    static const BytePatch kDeadzone[] = {
        {DS_PATCH_NAME("Pad: stick deadzone"), kDeadzoneVa, kDeadzoneOriginal, kDeadzoneOriginal, 4, true},
    };
    if (!patch::ApplyGroup("Controller", kDeadzone) ||
        !patch::WriteValue(kDeadzoneVa, static_cast<int32_t>(std::min<uint32_t>(s.padDeadzone, 90) * 100)) ||
        !patch::WriteCall(kPollSite, reinterpret_cast<const void*>(&PadStub), sizeof kPollOriginal, kPollOriginal)) {
        dslog::Write("[fail] Controller: poll hook at 0x%08X not written", kPollSite);
        return;
    }
    if (const PSEMessagingInterface* m = pse::MessagingInterface())
        m->Subscribe(pse_host::Find("tgefix.controller"), PSE_EVENT_FRAME, &FollowActivePad, nullptr, 0);
    overlay::Install();
    ApplyLauncherPad();
#ifndef DS_DIST
    overlay::AddLateDraw(&DevPadPreview);
#endif
    dslog::Write("[ok]   Controller: PlayStation / Xbox pads mapped to the game's layout, deadzone %u%%", s.padDeadzone);
}
