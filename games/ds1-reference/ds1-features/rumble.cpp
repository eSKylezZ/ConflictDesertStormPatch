// Rumble and adaptive triggers that follow what the player is actually using.
//
// All of the game's rumble goes through FUN_0044f320(block, effect, strength 0..1): an entry of the effect table
// 0x5EB540 (18 x 20 bytes: +0 strong motor i16, +2 weak motor i16, +4 duration ms u16, +8 priority, +0xC type 0
// constant / 1 fade out / 2 pulse, +0x10 period) is scaled and handed to FUN_005392d0 (the per-joystick table padio
// plays). Four callers:
//   0x4A1E38  weapon fire (FUN_004a1b80, esi = weapon, edi = the soldier firing; only a player's soldier) - effect =
//             the weapon definition's rumble class +0xC0 (0x5F5680 maps class -> effect 1:1): 0 AK rifles, 1 SMGs,
//             2 sniper rifles, 3 pistols, 4 machine guns incl. mounted M2HB and vehicle cannons, 5 rockets / missiles /
//             air strike, 6 C4 / mines, 7 frag grenade, 8 M203, 9 knife, 10 shotguns, 11 smoke grenade, 12 M16A2,
//             14 fist (13 unused)
//   0x425B84  15: the soldier thrown by a blast / impact     0x4246D7  16: damage taken (red flash)
//   0x40E4C0  17: explosion near the player's camera (white flash, strength by distance)
// The game's table rumbled for placing C4 or mines, throwing grenades, knife and fist, and gave pistols the same
// 300 ms buzz as SMGs - so rows 0-14 are replaced: short kicks sized by the gun, nothing for throwing or placing
// (their explosions still rumble through 17). Damage, impact and explosion rows stay.
//
// Adaptive triggers (DualSense) and Xbox trigger motors: by the weapon in the player's hands (soldier +0x26B0 = the
// held weapon object, +4 type 6, +0x24 definition: +0 category 5 = weapon, +0xC0 rumble class; object +0x5C current
// fire mode, 0x100 = M203) - or, for 1.5 s after it fired, the weapon that last fired for that player, which covers
// mounted guns and vehicle weapons (the fire call is hooked to note it).
#include <windows.h>

#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kEffectTable = 0x5EB540, kEffectSize = 20, kWeaponEffects = 15;
constexpr uint32_t kRumble = 0x44F320, kFireRumbleCall = 0x4A1E38;
constexpr uint32_t kInputBlocks = 0x60F5B8, kInputBlockSize = 0x478;
constexpr DWORD kFiredWindow = 1500;

#pragma pack(push, 1)
struct Effect {
    int16_t strong, weak;
    uint16_t duration, pad;
    uint32_t priority, type;
    uint16_t period, pad2;
};
#pragma pack(pop)
static_assert(sizeof(Effect) == kEffectSize);

// The game's rows 0-14 (checked before they are replaced).
const Effect kOriginal[kWeaponEffects] = {
    {0, 32767, 200, 0, 2, 0, 0, 0},      {0, 16384, 300, 0, 2, 0, 0, 0},    {16384, 0, 200, 0, 2, 0, 0, 0},
    {0, 16384, 300, 0, 2, 0, 0, 0},      {15000, 8000, 300, 0, 2, 0, 0, 0}, {32767, 0, 1000, 0, 2, 2, 2000, 0},
    {256, 0, 2000, 0, 2, 0, 0, 0},       {0, 1024, 1000, 0, 2, 2, 2000, 0}, {32767, 0, 500, 0, 2, 1, 0, 0},
    {0, 16384, 500, 0, 2, 1, 0, 0},      {32767, 0, 300, 0, 2, 0, 0, 0},    {0, 1024, 1000, 0, 2, 2, 2000, 0},
    {0, 32767, 200, 0, 2, 0, 0, 0},      {0, 1024, 1000, 0, 2, 2, 2000, 0}, {1024, 0, 200, 0, 2, 0, 0, 0},
};
// Ours: a fading kick per shot. Priority 0 rows are silent and never interrupt a running effect.
const Effect kTuned[kWeaponEffects] = {
    {20000, 14000, 110, 0, 2, 1, 0, 0},  // 0 AK assault rifles
    {12000, 10000, 80, 0, 2, 1, 0, 0},   // 1 sub-machine guns
    {32767, 16000, 180, 0, 2, 1, 0, 0},  // 2 sniper rifles
    {10000, 8000, 90, 0, 2, 1, 0, 0},    // 3 pistols
    {24000, 16000, 120, 0, 2, 1, 0, 0},  // 4 machine guns, mounted guns, vehicle cannons
    {32767, 32767, 450, 0, 2, 1, 0, 0},  // 5 rocket / missile launchers
    {},                                  // 6 C4, mines (placing)
    {},                                  // 7 frag grenade (throw)
    {26000, 12000, 250, 0, 2, 1, 0, 0},  // 8 M203 grenade launcher
    {5000, 4000, 60, 0, 2, 1, 0, 0},     // 9 combat knife
    {32767, 20000, 160, 0, 2, 1, 0, 0},  // 10 shotguns
    {},                                  // 11 smoke grenade (throw)
    {20000, 14000, 110, 0, 2, 1, 0, 0},  // 12 M16A2
    {},                                  // 13 (unused)
    {},                                  // 14 fist
};

struct Fired {
    const uint8_t* def = nullptr;
    uint32_t mode = 0;
    DWORD time = 0;
};
Fired g_fired[4];

bool Readable(const void* p) { return reinterpret_cast<uintptr_t>(p) >= 0x10000; }

// Weapon fire: note which weapon fired for which player (mounted / vehicle guns aren't the held weapon).
void __stdcall NoteFire(const uint8_t* weapon, const uint8_t* block) {
    const uintptr_t b = reinterpret_cast<uintptr_t>(block);
    if (!Readable(weapon) || b < kInputBlocks || b >= kInputBlocks + 4 * kInputBlockSize) return;
    Fired& f = g_fired[(b - kInputBlocks) / kInputBlockSize];
    f.def = *reinterpret_cast<const uint8_t* const*>(weapon + 0x24);
    f.mode = *reinterpret_cast<const uint32_t*>(weapon + 0x5C);
    f.time = GetTickCount();
}

// At 0x4A1E38: ecx = the input block, esi = the weapon; then on to FUN_0044f320 as before.
__declspec(naked) void FireRumbleStub() {
    __asm {
        push ecx
        push ecx
        push esi
        call NoteFire
        pop ecx
        mov eax, kRumble
        jmp eax
    }
}

features::TriggerFeel FeelOf(const uint8_t* def, uint32_t mode) {
    using features::TriggerFeel;
    if (!Readable(def) || *reinterpret_cast<const int32_t*>(def) != 5) return TriggerFeel::Default;
    if (mode & 0x100) return TriggerFeel::Launcher;  // M203 selected
    switch (*reinterpret_cast<const int32_t*>(def + 0xC0)) {
        case 1: case 3: return TriggerFeel::Light;
        case 0: case 12: return TriggerFeel::Rifle;
        case 2: case 10: return TriggerFeel::Heavy;
        case 4: return TriggerFeel::MachineGun;
        case 5: case 8: return TriggerFeel::Launcher;
        case 6: case 7: case 9: case 11: case 13: case 14: return TriggerFeel::None;
        default: return TriggerFeel::Default;
    }
}
}  // namespace

features::TriggerFeel features::PlayerTriggerFeel(int player) {
    if (player < 0 || player > 3) return TriggerFeel::Default;
    const Fired& f = g_fired[player];
    if (f.def && GetTickCount() - f.time < kFiredWindow) return FeelOf(f.def, f.mode);
    const auto* block = reinterpret_cast<const uint8_t*>(kInputBlocks + player * kInputBlockSize);
    const auto* soldier = *reinterpret_cast<const uint8_t* const*>(block + 0x310);
    if (!Readable(soldier)) return TriggerFeel::Default;
    const auto* weapon = *reinterpret_cast<const uint8_t* const*>(soldier + 0x26B0);
    if (!Readable(weapon)) return TriggerFeel::None;  // empty hands
    if (*reinterpret_cast<const int32_t*>(weapon + 4) != 6) return TriggerFeel::None;
    return FeelOf(*reinterpret_cast<const uint8_t* const*>(weapon + 0x24), *reinterpret_cast<const uint32_t*>(weapon + 0x5C));
}

void features::ApplyRumble() {
    static bool done = false;
    if (done) return;
    done = true;
    if (patch::Matches(kEffectTable, kOriginal, sizeof kOriginal))
        patch::Write(kEffectTable, kTuned, sizeof kTuned);
    else if (!patch::Matches(kEffectTable, kTuned, sizeof kTuned))
        dslog::Write("[fail] Rumble: effect table not recognised at 0x%08X - kept", kEffectTable);
    if (patch::HookCall(kFireRumbleCall, reinterpret_cast<const void*>(&FireRumbleStub), kRumble))
        dslog::Write("[ok]   Rumble: weapon effects retuned, trigger feel per weapon");
    else
        dslog::Write("[fail] Rumble: weapon fire call not recognised at 0x%08X", kFireRumbleCall);
}
