// Followers the player can take over (2026-10-10): the Kuwaiti Emir (Mission 2, unit class 3 EMIR) and the missile
// expert Franklin (Missions 11A / 11B, class 4 MISSILE EXPERT). Unit class = unit +0x8C (UNITS.TXT column 2: 0 SOLDIER,
// 1 POW, 3 EMIR, 4 MISSILE EXPERT; units are shared templates). They are in the squad list with a HUD panel and the
// switch to them works, but the block's control mode stayed "AI drives":
//   FUN_004500b0(mode) (block +0x318: 0 player, 1 aim, 2 AI) returns unless soldier vtable +0x4C(5) = combat soldier
//   (class 0 / 5) - `mov eax,[ecx]; push 5; call [eax+0x4c]` at 0x4500BD -> call ControlStub (class 0 / 5, or a
//   follower on the squad's team). Also let through (user: a limited inventory, the weapon picture):
//   - opening the inventory: FUN_0044b6c0 `call 0x450b50` (block's soldier class 5) at 0x44B6C6 -> BlockControllable.
//     His inventory is only what he carries (Geiger counter / nothing, the pistol, a MediKit we add): pick-ups stay off.
//   - the HUD weapon display: panel callback FUN_0048d9e0 draws it for a class-5 soldier a player drives
//     (`mov eax,[ebx]; push 5; mov ecx,ebx; call [eax+0x4c]` at 0x48E2B5) -> mov ecx,ebx; call ControlStub.
//   Every other class-5 gate stays: give-take (+0x410 / +0x414 menus), context prompts (FUN_00450540: no pick-ups,
//   mounted guns, doors, vehicles), squad orders (FUN_0044c9e0 via FUN_00450b50).
// Each follower carries a pistol for the whole level (user: they wouldn't be out here unarmed; the Emir takes the
// Beretta of his dead bodyguard - the cutscene's CS_BODYGUARD_DEAD, object 188, carries one): handed out once he is
// in the squad list in plain play, again if a loaded save lost it (spawned weapons aren't in saves for non-squad
// roles). Pistol skill: no HSKILLS.TXT row = level 0, the worst (Bradley has PISTOL 2).
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <set>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kControlSite = 0x4500BD;
constexpr uint8_t kControlOrig[] = {0x8B, 0x01, 0x6A, 0x05, 0xFF, 0x50, 0x4C};  // mov eax,[ecx]; push 5; call [eax+0x4c]
constexpr uint32_t kInventorySite = 0x44B6C6, kBlockIsSoldier = 0x450B50;
constexpr uint32_t kWeaponHudSite = 0x48E2B5;
constexpr uint8_t kWeaponHudOrig[] = {0x8B, 0x03, 0x6A, 0x05, 0x8B, 0xCB, 0xFF, 0x50, 0x4C};
constexpr uint32_t kObjectManager = 0x63C9A0, kSquadTeam = 0x5F9B94;
constexpr uint32_t kFrontEndState = 0x617C18, kLoadPending = 0x60EC8C, kWorld = 0x63C948, kViewController = 0x63C98C;
constexpr int kClassEmir = 3, kClassExpert = 4;

struct Follower {
    int unitClass;
    const char* pistol;
    int mags;
    const char* message;
};
constexpr Follower kFollowers[] = {
    {kClassEmir, "US_WPN_Beretta92F", 3, "The Emir takes his bodyguard's Beretta"},
    {kClassExpert, "US_WPN_SIGP228", 3, "Franklin draws a SIG P228"},
};

int ClassOf(const uint8_t* soldier) {
    const auto* unit = *reinterpret_cast<const uint8_t* const*>(soldier + 0x24);
    return unit ? *reinterpret_cast<const int*>(unit + 0x8C) : -1;
}

const Follower* FollowerOf(const uint8_t* soldier) {
    if (!soldier || soldier[0x10] != *reinterpret_cast<const uint8_t*>(kSquadTeam)) return nullptr;
    const int cls = ClassOf(soldier);
    for (const Follower& f : kFollowers)
        if (f.unitClass == cls) return &f;
    return nullptr;
}

// Replaces the class-5 query in FUN_004500b0 (ecx = soldier, result in eax; the original call clobbered ecx / edx).
int __fastcall Controllable(uint8_t* soldier) {
    using Query = int(__thiscall*)(void*, int);
    if ((*reinterpret_cast<Query**>(soldier))[0x4C / 4](soldier, 5)) return 1;
    return FollowerOf(soldier) ? 1 : 0;
}

__declspec(naked) void ControlStub() {
    __asm {
        push ecx
        call Controllable  // fastcall: ecx = soldier
        pop ecx
        ret
    }
}

int __fastcall BlockControllable(uint8_t* block) {
    auto* soldier = *reinterpret_cast<uint8_t**>(block + 0x310);
    return soldier ? Controllable(soldier) : 0;
}

bool CallPatch(uint32_t site, const uint8_t* orig, size_t len, size_t lead, const void* fn) {
    // `lead` bytes of the original kept / rewritten in front of the call (mov ecx,ebx), the rest nop
    uint8_t code[16] = {};
    memset(code, 0x90, len);
    if (lead) code[0] = 0x8B, code[1] = 0xCB;
    code[lead] = 0xE8;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(fn) - (site + lead + 5));
    memcpy(code + lead + 1, &rel, 4);
    if (patch::Matches(site, code, len)) return true;
    return patch::Matches(site, orig, len) && patch::Write(site, code, len);
}

template <class F>
void ForEachSquadMember(F f) {
    auto mgr = *reinterpret_cast<const uint8_t* const*>(kObjectManager);
    if (!mgr) return;
    const uint8_t team = *reinterpret_cast<const uint8_t*>(kSquadTeam);
    if (team >= mgr[0x10] || 1 >= mgr[0x11]) return;
    auto lists = *reinterpret_cast<const uintptr_t* const*>(mgr + 4);
    if (!lists) return;
    for (uintptr_t s = lists[team * mgr[0x11] + 1]; s; s = *reinterpret_cast<uintptr_t*>(s + 0x14)) f(reinterpret_cast<uint8_t*>(s));
}

bool Carries(uint8_t* soldier, const char* weapon) {
    auto* inv = *reinterpret_cast<uint8_t***>(soldier + 0x26B8);
    if (!inv) return false;
    for (int i = 0; i < 25; ++i) {
        uint8_t* it = inv[i];
        if (!it || *reinterpret_cast<int*>(it + 4) != 6) continue;
        auto* def = *reinterpret_cast<uint8_t**>(it + 0x24);
        const char* id = def ? *reinterpret_cast<const char**>(def + 4) : nullptr;
        if (id && _stricmp(id, weapon) == 0) return true;
    }
    return false;
}

std::set<uint8_t*> g_armed;  // followers handed their pistol in this level (a level load frees the soldiers)
bool g_hooked = false;
}  // namespace

void features::ApplyFollowers() {
    if (g_hooked) return;
    g_hooked = CallPatch(kControlSite, kControlOrig, sizeof kControlOrig, 0, reinterpret_cast<const void*>(&ControlStub));
    const bool extras = g_hooked &&
                        patch::HookCall(kInventorySite, reinterpret_cast<const void*>(&BlockControllable), kBlockIsSoldier) &&
                        CallPatch(kWeaponHudSite, kWeaponHudOrig, sizeof kWeaponHudOrig, 2, reinterpret_cast<const void*>(&ControlStub));
    dslog::Write(g_hooked ? "[ok]   Followers: the Emir / Franklin can be controlled (inventory, weapon display %s), pistol + MediKit"
                          : "[fail] Followers: control mode check not recognised (%s)", extras ? "ok" : "FAILED");
}

void features::OnFrameFollowers() {
    if (!g_hooked) return;
    if (*reinterpret_cast<uint8_t*>(kLoadPending) == 1) {
        g_armed.clear();
        return;
    }
    const auto* world = *reinterpret_cast<const uint8_t* const*>(kWorld);
    if (*reinterpret_cast<uint32_t*>(kFrontEndState) != 0xE || !world || (world[0x1B8C] & 1) || CustomiseOpen()) return;
    ForEachSquadMember([](uint8_t* soldier) {
        const Follower* f = FollowerOf(soldier);
        if (!f || *reinterpret_cast<float*>(soldier + 0x54) <= 0.0f || Carries(soldier, f->pistol)) return;
        if (!g_armed.insert(soldier).second) return;  // once per soldier and level (a loaded save = a new level)
        if (!GiveSpawnedWeapon(soldier, f->pistol, f->mags)) {
            dslog::Write("[fail] Followers: couldn't make %s for unit class %d", f->pistol, f->unitClass);
            return;
        }
        const bool medikit = GiveSpawnedMedikit(soldier);
        dslog::Write("Followers: %s%s for unit class %d", f->pistol, medikit ? " + MediKit" : " (no MediKit record)", f->unitClass);
        if (void* controller = *reinterpret_cast<void**>(kViewController))
            reinterpret_cast<void(__thiscall*)(void*, const char*, int)>(0x4D33A0)(controller, f->message, 4000);
    });
}
