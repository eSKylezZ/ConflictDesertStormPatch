// The mission scripts' co-op branches (2026-10-21). Script API slot 0x8A8 (FUN at 0x5ED238 + 0x8A8) is a stub on PC
// (`mov eax,1; ret`); on PS2 (retail 0x156A78) it answers the number of local players (2 in split screen). The PC
// mission DLLs still ask it and carry the co-op handling behind it (readable Lua of the shipped DLLs):
//   Mission 1  Foley's rifle / ammo / Medi-Kit stay hidden on the table, the rescued Foley joins team 0 under AI state 1
//              instead of team 2 + Bradley as the player soldier, Foley dying alone doesn't fail the mission, no
//              stretch / walk to the cell door, front-end states 14 / 28 after the cutscenes
//   Mission 2  the Emir follows the squad leader only when told to
//   Mission 10 more than 3 players -> next level Mission 12A (11A / 11B skipped: Franklin's escort)
// The hook answers the players of the running co-op game (split screen or online - CoopPlayers, else the split-screen
// count). Mission 10's 4-player skip only applies with PLAYABLE FOLLOWERS off (user decision: with followers a fourth
// player can take Franklin, so 11A / 11B are played): there the answer is capped at 3.
// Front-end state 28 (0x1C) has no screen on PC (FUN_00480930 answers null; PS2 has one): set_frontend_state (slot
// 0x474) ignores a state without a screen instead of switching to nothing.
// Mission 1 co-op hides Foley's rifle / ammo / Medi-Kit on the table at the rescue (hide_object, slot 0x2C4) - kept
// visible instead (user decision 2026-10-21: the kit stays on the table for Foley's player to pick up).
// Optional (settings featCoopScripts, default on), live; online = the host's value (hostsync `S`).
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
constexpr uint32_t kApi = 0x5ED238;
constexpr uint32_t kSlotPlayers = kApi + 0x8A8, kSlotFrontEndState = kApi + 0x474, kSlotHide = kApi + 0x2C4;
constexpr uint32_t kPlayersStub = 0x48A4C0;   // the PC body: mov eax,1; ret
constexpr uint32_t kSetState = 0x47EF20;       // the front-end state setter the slot points at
constexpr uint32_t kHideObject = 0x485230;
constexpr int kFoleyKit[] = {33, 245, 52};       // Mission 1: FOLEY_SNIPER_RIFLE, FOLEY_AMMO, FOLEY_MEDIKIT
constexpr uint32_t kSplitFlag = 0x606410, kSplitCount = 0x606414;
constexpr uint32_t kSceneName = 0x606880;     // "Mission10.env" while that level runs
constexpr uint32_t kScreenOf = 0x480930;      // state -> screen object (coop.cpp's hook answers our own states too)

using PlayersFn = int(__cdecl*)();
using StateFn = void(__cdecl*)(int);
PlayersFn g_nextPlayers = nullptr;
StateFn g_nextState = nullptr;
StateFn g_nextHide = nullptr;

bool On() {
    const features::HostSettings* host = features::HostSync();   // online: the host's choice on every PC
    if (host && host->coopScripts >= 0) return host->coopScripts != 0;
    return settings::Get().featCoopScripts;
}

bool Followers() {
    const features::HostSettings* host = features::HostSync();
    // PLAYABLE FOLLOWERS is plugin dsfix.followers' setting (no plugin: off)
    return host ? host->followers : features::PluginBool("dsfix.followers", "FeatFollowers", false);
}

int Players() {
    int n = features::CoopPlayers();
    if (n <= 0 && *reinterpret_cast<const uint32_t*>(kSplitFlag) != 0) n = *reinterpret_cast<const int*>(kSplitCount);
    return n < 1 ? 1 : n > 4 ? 4 : n;
}

int __cdecl PlayerCount() {
    if (!On()) return g_nextPlayers ? g_nextPlayers() : 1;
    int n = Players();
    if (n > 3 && Followers() && _strnicmp(reinterpret_cast<const char*>(kSceneName), "mission10", 9) == 0) n = 3;
    static int logged = 0;
    if (n != logged) {
        logged = n;
        dslog::Write("Co-op scripts: the mission asks for the player count - %d", n);
    }
    return n;
}

void __cdecl SetFrontEndState(int state) {
    using ScreenFn = void*(__cdecl*)(int);
    if (On() && reinterpret_cast<ScreenFn>(kScreenOf)(state) == nullptr) {
        dslog::Write("Co-op scripts: front-end state %d asked by the mission has no screen on PC - ignored", state);
        return;
    }
    g_nextState(state);
}
void __cdecl HideObject(int id) {
    if (On() && Players() > 1 && _strnicmp(reinterpret_cast<const char*>(kSceneName), "mission1.", 9) == 0)
        for (int k : kFoleyKit)
            if (k == id) {
                dslog::Write("Co-op scripts: Foley's kit (object %d) stays on the table", id);
                return;
            }
    g_nextHide(id);
}
}  // namespace

void features::ApplyCoopScripts() {
    static bool hooked = false;
    if (hooked) return;
    const bool a = patch::HookSlot(kSlotPlayers, reinterpret_cast<const void*>(&PlayerCount),
                                   reinterpret_cast<void**>(&g_nextPlayers), kPlayersStub);
    const bool b = patch::HookSlot(kSlotFrontEndState, reinterpret_cast<const void*>(&SetFrontEndState),
                                   reinterpret_cast<void**>(&g_nextState), kSetState);
    const bool c = patch::HookSlot(kSlotHide, reinterpret_cast<const void*>(&HideObject),
                                   reinterpret_cast<void**>(&g_nextHide), kHideObject);
    hooked = a && b && c;
    dslog::Write(hooked ? "[ok]   Co-op scripts: the missions' own co-op handling answers to the player count"
                        : "[fail] Co-op scripts: script API slots 0x8A8 / 0x474 / 0x2C4 not as expected - left alone");
}
