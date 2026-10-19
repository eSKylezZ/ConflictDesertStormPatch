// Inventory tooltips: the game never tells the player that the "use" button works on the highlighted item without
// equipping it - a MediKit heals yourself, a weapon with several fire modes switches mode. The item name the
// inventory shows in the info bar (FUN_004544f0: name -> 0x617A30 -> FUN_00507a50 at 0x4545EB, edi = input block)
// gets a hint with the player's INVENTORY_USE button: "MediKit      [R2] Use on yourself"; night vision goggles:
// "[R2] Toggle". In the give menu (block +0x410) the item's name and "[GIVE_TAKE button] Give" instead.
// A weapon's text is "<name> - <fire mode>" (format 0x5EB700), but with more than one player ([0x610798] != 1,
// 0x454558 jne) only the fire mode - "Single Shot" instead of the sniper rifle; that branch is skipped.
//   list item = FUN_004e6f80(list block+0x3fc, list+0x30); item +4 type (6 = weapon), +0x24 definition:
//   +0x80 name hash, +0x280 available fire modes (1 single, 2 burst, 4 automatic, 0x100 grenade launcher;
//   FUN_004a4fb0 only switches to modes in that mask). "Change Fire Mode" is the game's own text (FIREMODE).
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kInfoBarSet = 0x507A50, kInventoryNameSite = 0x4545EB;  // thiscall (bar, text)
constexpr uint32_t kListItem = 0x4E6F80;                                   // list thiscall (id) -> item
constexpr uint32_t kInputBlocks = 0x60F5B8, kInputBlockSize = 0x478;
constexpr uint32_t kInventoryUse = 46, kGiveTake = 50;  // INVENTORY_USE, GIVE_TAKE_WEAPON_AMMO
constexpr uint32_t kHashMediKit = 0xB53B281C, kHashNightVision = 0xD56313D9, kHashFireMode = 0x0C531126;

int Bits(uint32_t v) {
    int n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}

void __stdcall SetInventoryName(void* bar, const char* text, const uint8_t* block) {
    using Set = void(__thiscall*)(void*, const char*);
    const char* hint = nullptr;
    char own[64] = "";
    const uint8_t* list = block ? *reinterpret_cast<const uint8_t* const*>(block + 0x3FC) : nullptr;
    uint32_t action = kInventoryUse;
    if (list && text && *reinterpret_cast<const uint32_t*>(block + 0x410)) {  // give menu (FUN_00451800)
        auto* item = reinterpret_cast<const uint8_t*>(reinterpret_cast<void*(__thiscall*)(const void*, int)>(kListItem)(
            list, *reinterpret_cast<const uint16_t*>(list + 0x30)));
        const uint8_t* def = item ? *reinterpret_cast<const uint8_t* const*>(item + 0x24) : nullptr;
        const char* name = def ? features::GameText(*reinterpret_cast<const uint32_t*>(def + 0x80)) : nullptr;
        if (name && *name) text = name;  // the item, not its fire mode
        strcpy_s(own, "Give");
        hint = own;
        action = kGiveTake;
        list = nullptr;
    }
    if (list && text) {
        auto* item = reinterpret_cast<const uint8_t*>(reinterpret_cast<void*(__thiscall*)(const void*, int)>(kListItem)(
            list, *reinterpret_cast<const uint16_t*>(list + 0x30)));
        const uint8_t* def = item ? *reinterpret_cast<const uint8_t* const*>(item + 0x24) : nullptr;
        if (def) {
            const uint32_t modes = *reinterpret_cast<const uint32_t*>(def + 0x280) & 0x107;
            const uint32_t name = *reinterpret_cast<const uint32_t*>(def + 0x80);
            if (name == kHashMediKit) {
                strcpy_s(own, "Use on yourself");
                hint = own;
            } else if (name == kHashNightVision) {
                strcpy_s(own, "Toggle");
                hint = own;
            } else if (*reinterpret_cast<const int32_t*>(item + 4) == 6 && Bits(modes) > 1) {
                hint = features::GameText(kHashFireMode);
            }
        }
    }
    const int player = static_cast<int>((reinterpret_cast<uintptr_t>(block) - kInputBlocks) / kInputBlockSize) & 3;
    const char* icon = hint ? features::ControlIconText(player, action) : nullptr;
    if (!hint || !icon) return reinterpret_cast<Set>(kInfoBarSet)(bar, text);
    static char line[192];
    snprintf(line, sizeof line, "%s     %s %s", text, icon, hint);
    reinterpret_cast<Set>(kInfoBarSet)(bar, line);
}

// At 0x4545EB: ecx = info bar, [esp] = text; edi = the input block.
__declspec(naked) void InventoryNameStub() {
    __asm {
        pop eax           // return address
        pop edx           // text
        push eax
        push edi          // block
        push edx          // text
        push ecx          // bar
        call SetInventoryName
        ret
    }
}
}  // namespace

void features::ApplyTooltips() {
    static bool done = false;
    if (done) return;
    done = patch::HookCall(kInventoryNameSite, reinterpret_cast<const void*>(&InventoryNameStub), kInfoBarSet);
    static const uint8_t onlyMode[] = {0x75, 0x43}, nameAndMode[] = {0x90, 0x90};  // 0x454558: jne (players != 1)
    if (patch::Matches(0x454558, onlyMode, 2)) patch::Write(0x454558, nameAndMode, 2);
    dslog::Write(done ? "[ok]   Inventory tooltips" : "[fail] Inventory tooltips: unexpected code at 0x%08X", kInventoryNameSite);
}
