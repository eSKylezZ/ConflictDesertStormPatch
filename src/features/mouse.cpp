// Mouse acceleration off by default. The mouse settings live in each player's input block (0x60F5B8 + i*0x478):
// +0x43C/+0x440 sensitivity, +0x444 invert, +0x448 acceleration (read by the mouse look at 0x44FA69 / 0x44FB93).
// The block set-up gives every player acceleration: `mov dword ptr [esi+0x448], 1` at 0x44AAA6 (immediate at
// 0x44AAAC). MOUSE OPTIONS -> ACCELERATION (item 0xFEA, selection FUN_0045c7a0) edits player 1's (0x60FA00 =
// block 0 +0x448), saved in the game's settings data (0x415AD2) and applied when that is loaded (0x415F44).
// Our setting MouseAcceleration (default 0) owns the value: the set-up's immediate is it, and
//   0x415F44  mov [0x60fa00], ecx  -> call ApplyStub; nop   (loaded settings: our value instead)
//   0x45C862  mov [0x60fa00], edx  -> call ChangeStub; nop  (the player's choice in MOUSE OPTIONS: stored)
// so the game starts without acceleration and a player who turns it on keeps it.
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
constexpr uint32_t kAcceleration = 0x60FA00;
constexpr uint32_t kApplySite = 0x415F44, kChangeSite = 0x45C862;
constexpr uint32_t kDefaultImm = 0x44AAAC;  // mov dword ptr [esi+0x448], <1> (block set-up, at 0x44AAA6)
constexpr uint8_t kDefaultInstr[] = {0xC7, 0x86, 0x48, 0x04, 0x00, 0x00};
constexpr uint8_t kApplyOrig[] = {0x89, 0x0D, 0x00, 0xFA, 0x60, 0x00};
constexpr uint8_t kChangeOrig[] = {0x89, 0x15, 0x00, 0xFA, 0x60, 0x00};
uint32_t g_acceleration = 0;

void SetDefault() {
    if (patch::Matches(kDefaultImm - 6, kDefaultInstr, sizeof kDefaultInstr)) patch::WriteValue(kDefaultImm, g_acceleration);
}

void __cdecl Store(uint32_t on) {
    g_acceleration = on ? 1 : 0;
    SetDefault();
    settings::Values v = settings::Get();
    v.mouseAcceleration = g_acceleration != 0;
    settings::Save(v);
}

__declspec(naked) void ApplyStub() {
    __asm {
        push eax
        mov eax, g_acceleration
        mov dword ptr ds:[0x60FA00], eax  // literal: a constexpr name here would be its own address
        pop eax
        ret
    }
}

__declspec(naked) void ChangeStub() {
    __asm {
        mov dword ptr ds:[0x60FA00], edx
        pushad
        push edx
        call Store
        add esp, 4
        popad
        ret
    }
}

bool CallAt(uint32_t site, const void* fn, const uint8_t (&original)[6]) {
    uint8_t call[6] = {0xE8, 0, 0, 0, 0, 0x90};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(fn) - (site + 5));
    memcpy(call + 1, &rel, 4);
    if (patch::Matches(site, call, sizeof call)) return true;
    return patch::Matches(site, original, sizeof original) && patch::Write(site, call, sizeof call);
}
}  // namespace

void features::ApplyMouse() {
    g_acceleration = settings::Get().mouseAcceleration ? 1 : 0;
    const bool ok = patch::Matches(kDefaultImm - 6, kDefaultInstr, sizeof kDefaultInstr) && CallAt(kApplySite, reinterpret_cast<const void*>(&ApplyStub), kApplyOrig) &&
                    CallAt(kChangeSite, reinterpret_cast<const void*>(&ChangeStub), kChangeOrig);
    if (ok) SetDefault();
    dslog::Write(ok ? "[ok]   Mouse acceleration: %s (MOUSE OPTIONS)" : "[fail] Mouse acceleration: unexpected code (%s)",
                 g_acceleration ? "on" : "off");
}
