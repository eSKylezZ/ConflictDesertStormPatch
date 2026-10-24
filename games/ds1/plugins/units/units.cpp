// Unit tables: guards so a new unit row can't crash the game (always on). Background: research/custom_units.md.
//
// Units.txt column 14 (unit +0xD8) = number of starting-item slots; UnitWpns.txt (loader FUN_00492ac0, flag 1 = humans)
// puts each row into the first free slot of unit +0xDC[i] (item stat) / +0x140[i] (amount), at most 25. The soldier
// set-up hands out every slot 0..+0xD8-1 (0x4221E1..0x42228E; skipped only for the player's team in the campaign) and
// FUN_00436260 reads [item] at once -> a slot without a UnitWpns row = null = access violation at 0x436284 while the
// level's objects load. Patch: 0x42220F mov eax,[edx+edi*4+0xDC]; push eax; call 0x436260 -> SlotStub (empty slot =
// next slot; ecx = soldier is already set at 0x42220D, edx = unit stats).
//
// The three loaders read every text field with an unlimited %[^','] into fixed stack buffers (Units.txt 36 bytes,
// HStat.txt name 40, UnitWpns.txt / VehWpns.txt 36, Vehicle.txt 36 / connectors 35, VStat.txt name 40 / propulsion 36): a
// longer field overwrote the stack (Props.txt 36 too). Their format pushes (0x491F11, 0x4919A5, 0x492B09, 0x4923B3, 0x491C0B,
// 0x49430B) get copies
// with field widths.
// Vehicles have the same slot pattern: Vehicle.txt column 26 (vehicle +0x86) = gun slots, VehWpns.txt fills +0x88[i],
// and the vehicle set-up FUN_00437cd0 reads every slot unchecked (0x4381C7, 0x438297 ...). The game's data always has
// slots = rows; mods.cpp CheckVehicleTables sets every vehicle's count to its VehWpns rows, so no slot is empty. mods.cpp CheckUnitTables reports (and leaves out) such mod rows first;
// this is the safety net.
//
// Its own PSE plugin since 2026-10-23 (PSE\Plugins\dsfix.units.dll, on the dsfix kit - games/ds1/plugins/kit).
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <set>

#include "core/log.h"
#include "core/patch.h"
#include "kit/kit.h"

namespace {
constexpr uint32_t kSlotSite = 0x42220F, kSlotCont = 0x42221C, kSlotNext = 0x422278, kGiveItem = 0x436260;

// Field widths = buffer size - 1 (the game's own formats otherwise).
const char kUnitsFormat[] =
    "%35[^','],%35[^','],%35[^','],%35[^','],%35[^','],\t\t\t%35[^','],%35[^','],%35[^','],%d,%35[^','],%35[^','],"
    "%35[^','],%35[^','],%d";
const char kHStatFormat[] = "%39[^','],%d,%d,%d,%d,%d,%d,%f,%d,%f,%f,%f,%f,%d,%f,%d,%f,%f";
const char kUnitWpnsFormat[] = "%35[^','],%35[^','],%35[^','],%d";   // also VehWpns.txt (same loader, flag 0)
// Vehicle.txt: 5 names (36 bytes), 8 seat + 8 exit connectors (35 bytes each), wreck debris + 2 more names (36)
const char kVehicleFormat[] =
    "%35[^','],%35[^','],%35[^','],%35[^','],%35[^','],%d,			%34[^','],%34[^','],%34[^','],%34[^','],%34[^','],"
    "%34[^','],%34[^','],%34[^','],			%34[^','],%34[^','],%34[^','],%34[^','],%34[^','],%34[^','],%34[^','],"
    "%34[^','],			%35[^','],%35[^','],%35[^','],%d,%f";
const char kPropsFormat[] = "%35[^','],%35[^','],%35[^','],%*d";   // Props.txt: name, type, engine bank, (unused)
const char kVStatFormat[] =
    "%39[^','],%d,%d,%d,%d,%d,%f,%d,%f,%d,			%d,%d,%35[^','],%f,%f,%f,%d,%d,%d,%d,%f,%f,%f,%f,%f,%f,%f";

struct FormatSite {
    uint32_t push;      // push imm32 of the game's format string
    uint32_t original;  // its address
    const char* ours;
    const char* table;
};
const FormatSite kFormats[] = {
    {0x491F11, 0x5F3E40, kUnitsFormat, "Units.txt"},
    {0x4919A5, 0x5F3C68, kHStatFormat, "HStat.txt"},
    {0x492B09, 0x5F4324, kUnitWpnsFormat, "UnitWpns.txt / VehWpns.txt"},
    {0x4923B3, 0x5F3F08, kVehicleFormat, "Vehicle.txt"},
    {0x491C0B, 0x5F3D58, kVStatFormat, "VStat.txt"},
    {0x49430B, 0x5F4844, kPropsFormat, "Props.txt"},
};

void __cdecl EmptySlot(const uint8_t* unit) {
    static std::set<const uint8_t*> told;
    if (!told.insert(unit).second) return;
    const char* name = *reinterpret_cast<const char* const*>(unit + 4);
    dslog::Write("Units: %s has %u item slot(s) without a UnitWpns.txt row - empty slots skipped", name ? name : "?",
                 unit[0xD8]);
}

// At 0x42220F: ecx = soldier, edx = unit stats, edi = slot.
__declspec(naked) void SlotStub() {
    __asm {
        mov eax, dword ptr [edx + edi*4 + 0xDC]
        test eax, eax
        jz empty
        push eax
        push kSlotCont          // return address of the original call
        push kGiveItem
        ret
    empty:
        pushad
        push edx
        call EmptySlot
        add esp, 4
        popad
        push kSlotNext
        ret
    }
}
}  // namespace

void ApplyUnits() {
    static bool done = false;
    if (done) return;
    done = true;
    static const uint8_t slot[] = {0x8B, 0x84, 0xBA, 0xDC, 0x00, 0x00, 0x00, 0x50, 0xE8, 0x44, 0x40, 0x01, 0x00};
    if (patch::Matches(kSlotSite, slot, sizeof slot)) {
        // jmp SlotStub over the whole 13 bytes (the rest nop): one jmp@ code group.
        const bool ok = patch::WriteJump(kSlotSite, reinterpret_cast<const void*>(&SlotStub), sizeof slot, slot);
        dslog::Write(ok ? "Units: empty item slots skipped" : "[fail] Units: item slot guard not written");
    } else {
        dslog::Write("[fail] Units: soldier item set-up not recognised - item slot guard not applied");
    }
    int formats = 0;
    for (const FormatSite& f : kFormats) {
        uint8_t push[5] = {0x68};
        memcpy(push + 1, &f.original, 4);
        if (!patch::Matches(f.push, push, sizeof push)) {
            dslog::Write("[fail] Units: %s loader not recognised - field length guard not applied", f.table);
            continue;
        }
        if (patch::WriteValue(f.push + 1, reinterpret_cast<uint32_t>(f.ours))) ++formats;
    }
    dslog::Write("Units: field length guards on %d of %zu unit / vehicle table loaders", formats, sizeof kFormats / sizeof *kFormats);
}

DSFIX_KIT_PLUGIN("dsfix.units", "Unit / vehicle tables", PSE_PLUGIN_SIMULATION, [] {
    ApplyUnits();
    return true;
})
