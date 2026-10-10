// Menu selection fixes (2026-10-10, user: SAVE GAME "went back like it had saved" - nothing written).
// - Mouse hover: a menu list (vtable 0x5DAC7C, at screen +8) moves its selection to the row under the pointer when
//   the pointer differs from the position it last saw (FUN_004e7750: list +0x98/+0x9C vs 0x754C18/0x754C1C). The
//   list constructor zeroes that position and the game re-centres the cursor while mouse-looking, so a menu opening
//   under a resting pointer jumped to the row under it (SAVE GAME: the set-up selects the new save, row 0, and the
//   pointer moved it to an old save). Both callers of the screen enter FUN_0047ee50 (0x47A1AE after a transition,
//   0x47EFAA from the state setter) are hooked: after the enter the list's position = the pointer's, so only a real
//   movement selects.
// - "OK to overwrite this file?" (state 0x17; the YES / NO screen, vtable 0x5D97B8, is shared with the quit
//   questions 0x18 / 0x19): its enter 0x4A6EA0 selects NO (list +0x28 = 1, +0x30 = id 0x14B6) -> YES (0, 0x14B5)
//   for the overwrite question only, so Enter on an existing save overwrites it.
#include <windows.h>

#include <cstdint>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kScreenEnter = 0x47EE50;        // cdecl (state)
constexpr uint32_t kEnterCalls[] = {0x47A1AE, 0x47EFAA};
constexpr uint32_t kScreenForState = 0x480930;     // cdecl (state) -> screen object (our coop.cpp stub included)
constexpr uint32_t kListVtable = 0x5DAC7C;
constexpr uint32_t kMouseX = 0x754C18, kMouseY = 0x754C1C;
constexpr uint32_t kFrontEndState = 0x617C18;
constexpr uint32_t kYesNoEnterSlot = 0x5D97B8 + 0x18, kYesNoEnter = 0x4A6EA0;
constexpr uint32_t kOverwriteState = 0x17;
constexpr uint16_t kYesId = 0x14B5;

template <typename T>
T& At(uintptr_t base, uint32_t off) { return *reinterpret_cast<T*>(base + off); }

uintptr_t ListOf(uintptr_t screen) {
    if (!screen) return 0;
    const uintptr_t list = At<uintptr_t>(screen, 8);
    return list && At<uint32_t>(list, 0) == kListVtable ? list : 0;
}

void __cdecl EnterScreen(uint32_t state) {
    reinterpret_cast<void(__cdecl*)(uint32_t)>(kScreenEnter)(state);
    const uintptr_t list = ListOf(reinterpret_cast<uintptr_t(__cdecl*)(uint32_t)>(kScreenForState)(state));
    if (!list) return;
    At<int32_t>(list, 0x98) = *reinterpret_cast<int32_t*>(kMouseX);
    At<int32_t>(list, 0x9C) = *reinterpret_cast<int32_t*>(kMouseY);
}

void __fastcall YesNoEnter(uintptr_t screen) {
    reinterpret_cast<void(__fastcall*)(uintptr_t)>(kYesNoEnter)(screen);
    if (*reinterpret_cast<uint32_t*>(kFrontEndState) != kOverwriteState) return;
    if (const uintptr_t list = ListOf(screen)) {
        At<int16_t>(list, 0x28) = 0;
        At<uint16_t>(list, 0x30) = kYesId;
    }
}
}  // namespace

void features::ApplyMenus() {
    bool ok = true;
    for (uint32_t site : kEnterCalls) ok = patch::HookCall(site, reinterpret_cast<const void*>(&EnterScreen), kScreenEnter) && ok;
    const uint32_t ours = reinterpret_cast<uint32_t>(&YesNoEnter);
    if (*reinterpret_cast<uint32_t*>(kYesNoEnterSlot) == kYesNoEnter) ok = patch::WriteValue(kYesNoEnterSlot, ours) && ok;
    else if (*reinterpret_cast<uint32_t*>(kYesNoEnterSlot) != ours) ok = false;
    dslog::Write(ok ? "[ok]   Menus: pointer selects only when moved, overwrite question defaults to YES"
                    : "[fail] Menus: screen enter / YES-NO screen not recognised");
}
