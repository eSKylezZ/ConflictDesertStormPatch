// Fixes only the Japanese PS2 release has ("Conflict Delta: Wangan Sensou 1991", SLPM_654.60, Nov 2003 - the EU PS2,
// Xbox, GameCube and PC builds lack them; PLATFORM_DIFFERENCES.md "PS2 Japan"). Optional: settings featJpFixes (live;
// online the host's value, hostsync .J).
//
// Stuck tanks (JP FUN_002204c8 = PC 0x518320, tank controller vtable 0x5DAD98 slot 0, thiscall(dt)): the game nudges a
// tank with a random force (rand & 7: up / down / sideways, mass +0x4C x 0.6) only when its contact counters say so
// (+0xF90 > 0, +0xF94 == 0) and a track is driven. JP adds: tracks driven together (|left + right| > 5, smoothed
// +0xF68 / +0xF6C) but speed below 0.1 (|v|^2 < 0.01, linear velocity at [esp+0x20..0x28] from 0x5184D9) = stuck ->
// the nudge every frame at 1.8 x mass, and the sideways cases lift as well (y 0.6 instead of 0). Patches:
//   0x51850E  cmp [esi+0xF90],edi; jle 0x518667   -> jmp StuckStub (stuck: straight to the nudge 0x518550)
//   0x518599 / 0x518650  fmul [0x5DACA8] (0.6)   -> fmul [g_impulseScale]
//   0x518640  mov [esp+0x18], 0 (sideways y)    -> 0.6 while on (rewritten when the setting changes)
//
// Impossible routes (JP FUN_00143ab8 / FUN_00143fd0 = PC route-to FUN_0045ba60 thiscall(soldier, goal*, notify) and
// its scan state machine FUN_0045bdf0): when the A* FUN_00509d00 only reaches the closest point (result 1 = goal
// unreachable), JP teleports the soldier to the goal cell's marker + 10 cm ("Panic Teleport DUE TO Impossible Route
// Request!") through the game's own teleport FUN_0049ce70(pos, 1, 0, 0, 0) (as at 0x45B06C) - only when neither the
// soldier nor the target is inside any view (FUN_004dbda0, world [0x63C948]). Route-to still fails that time; the
// next request starts at the goal. Here also: not player-driven, alive, not in a vehicle, no cinematic, at most once
// per soldier every 5 s, off while an online mission runs (each PC's views differ).
//   0x45BD4F  jne 0x45BAA9 after the A* (goal marker [esp+0x2C], esi = soldier) -> RouteFailStub
//   0x45BFA8  push 0; mov ecx,esi; call 0x434620 (scan failed; goal marker soldier +0x2300) -> call ScanFailed
// Item drops (JP "GetSafeDropposition failed : using grunts zone and hand position", in the drop / plant code JP
// FUN_0018ff88 = PC FUN_00431c70): the item is placed at a probe point 1 m in front of the soldier; when the scenery
// hit there is 50 cm or more above the probe (the point is inside a wall / under stairs) the game threw the item from
// inside the geometry - lost. JP uses the soldier's zone and position instead, the existing no-zone branch:
//   0x431EBF  jp 0x431F17 (7A 56) -> jp 0x431EDB (7A 1A)   (live with the setting)
// Path builder FUN_0050cba0 (always on - only where the game would corrupt memory or crash; JP has the same guards):
//   a path has 500 slots (ids +0x177C, positions +0xC); the node loop had no cap ("OUT OF ROUTING MEMORY" in JP) ->
//   0x50CD21 stops at 498 nodes; a marker index out of range left a null at [esp+0x44] that 0x50CC8B / 0x50CC94
//   dereference -> 0x50CC7A skips the node's jitter; the quadtree leaf lookup 0x505F80 (call 0x50CCAA) can return null
//   -> a zero leaf (no jitter).
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
constexpr uint32_t kGateSite = 0x51850E, kGateOrigTarget = 0x518667, kGateCont = 0x51851A, kNudge = 0x518550;
constexpr uint32_t kScaleSites[] = {0x518599, 0x518650};
constexpr uint32_t kSideLiftImm = 0x518640 + 4;
constexpr float kNormalScale = 0.6f, kStuckScale = 1.8f;
constexpr uint32_t kLiftOff = 0, kLiftOn = 0x3F19999A;  // 0.6f
constexpr uint32_t kDropJump = 0x431EBF + 1;
constexpr uint8_t kDropOff = 0x56, kDropOn = 0x1A;
bool g_dropOk = false;

float g_impulseScale = kNormalScale;
bool g_stuck = false;
bool g_hooked = false;
int g_applied = -1;  // side lift written for this setting

bool On() {
    const features::HostSettings* host = features::HostSync();
    return host && host->jpFixes >= 0 ? host->jpFixes != 0 : settings::Get().featJpFixes;
}

void __cdecl CheckStuck(const uint8_t* tank, const float* v) {
    g_stuck = false;
    g_impulseScale = kNormalScale;
    if (!On()) return;
    const float tracks = *reinterpret_cast<const float*>(tank + 0xF68) + *reinterpret_cast<const float*>(tank + 0xF6C);
    if (std::fabs(tracks) <= 5.0f || v[0] * v[0] + v[1] * v[1] + v[2] * v[2] >= 0.01f) return;
    g_stuck = true;
    g_impulseScale = kStuckScale;
    static DWORD lastLog = 0;
    if (GetTickCount() - lastLog > 2000) {
        lastLog = GetTickCount();
        dslog::Write("JP fixes: tank %p potentially stuck - adding an impulse (tracks %.1f)", tank, tracks);
    }
}

// At 0x51850E: esi = tank, edi = 0, linear velocity at [esp+0x20].
__declspec(naked) void StuckStub() {
    __asm {
        pushad
        lea eax, [esp + 0x40]   // [esp+0x20] before pushad
        push eax
        push esi
        call CheckStuck
        add esp, 8
        popad
        cmp byte ptr g_stuck, 0
        jne stuck
        cmp dword ptr [esi + 0xF90], edi
        jle orig
        push kGateCont
        ret
    orig:
        push kGateOrigTarget
        ret
    stuck:
        push kNudge
        ret
    }
}
// --- impossible routes ---------------------------------------------------------------------------------------------
constexpr uint32_t kRouteFailSite = 0x45BD4F, kRouteContinue = 0x45BD55, kRouteFail = 0x45BAA9;
constexpr uint32_t kScanFailSite = 0x45BFA8, kStopRoute = 0x434620;
constexpr uint32_t kWorld = 0x63C948, kMarkers = 0x63C9C0, kInView = 0x4DBDA0, kTeleport = 0x49CE70;
constexpr uint32_t kInstancePos = 0x4CD890;

std::unordered_map<const uint8_t*, DWORD> g_lastTeleport;

void PositionOf(uint8_t* soldier, float out[3]) {
    uint8_t* inst = *reinterpret_cast<uint8_t**>(soldier + 0x2C);
    if (*reinterpret_cast<int*>(inst + 0x1C8) == 0) {
        memcpy(out, inst + 0xF4, 12);
        return;
    }
    reinterpret_cast<void(__thiscall*)(void*, float*)>(kInstancePos)(inst, out);
}

bool InView(const float p[3]) {
    void* world = *reinterpret_cast<void**>(kWorld);
    return world && reinterpret_cast<int(__thiscall*)(void*, const float*)>(kInView)(world, p) != 0;
}

void __cdecl PanicTeleport(uint8_t* soldier, int marker) {
    if (!On() || features::LockstepActive() || !soldier || marker < 0) return;
    const uint8_t* world = *reinterpret_cast<const uint8_t* const*>(kWorld);
    const uint8_t* set = *reinterpret_cast<const uint8_t* const*>(kMarkers);
    if (!world || (world[0x1B8C] & 1) || !set || static_cast<uint32_t>(marker) >= *reinterpret_cast<const uint32_t*>(set + 4))
        return;
    using Query = int(__thiscall*)(void*, int);
    if ((*reinterpret_cast<Query**>(soldier))[0x4C / 4](soldier, 1)) return;          // a player drives it
    if (*reinterpret_cast<float*>(soldier + 0x54) <= 0.0f || *reinterpret_cast<void**>(soldier + 0x267C)) return;
    const DWORD now = GetTickCount();
    auto it = g_lastTeleport.find(soldier);
    if (it != g_lastTeleport.end() && now - it->second < 5000) return;
    float from[3], to[3];
    PositionOf(soldier, from);
    memcpy(to, *reinterpret_cast<const uint8_t* const*>(set) + marker * 0x20, 12);
    to[1] += 10.0f;
    float fromHead[3] = {from[0], from[1] + 180.0f, from[2]}, toHead[3] = {to[0], to[1] + 180.0f, to[2]};
    if (InView(from) || InView(fromHead) || InView(to) || InView(toHead)) return;
    g_lastTeleport[soldier] = now;
    reinterpret_cast<void(__thiscall*)(void*, float*, int, int, float, int)>(kTeleport)(soldier, to, 1, 0, 0.0f, 0);
    dslog::Write("JP fixes: panic teleport (impossible route) soldier %p -> marker %d (%.0f, %.0f, %.0f)", soldier, marker,
                 to[0], to[1], to[2]);
}

// At 0x45BD4F: eax = A* result, esi = soldier, goal marker at [esp+0x2C].
__declspec(naked) void RouteFailStub() {
    __asm {
        test eax, eax
        jnz failed
        push kRouteContinue
        ret
    failed:
        cmp eax, 1
        jne done
        pushad
        push dword ptr [esp + 0x20 + 0x2C]
        push esi
        call PanicTeleport
        add esp, 8
        popad
    done:
        push kRouteFail
        ret
    }
}

// Replaces `push 0; mov ecx, esi; call 0x434620` in the scan state machine (ecx = soldier): teleport, else stop.
void __fastcall ScanFailed(uint8_t* soldier) {
    const auto before = g_lastTeleport.find(soldier);
    const DWORD stamp = before == g_lastTeleport.end() ? 0 : before->second;
    PanicTeleport(soldier, *reinterpret_cast<int*>(soldier + 0x2300));
    const auto after = g_lastTeleport.find(soldier);
    if (after != g_lastTeleport.end() && after->second != stamp) return;   // teleported (JP skips the stop then)
    reinterpret_cast<void(__thiscall*)(void*, int)>(kStopRoute)(soldier, 0);
}

// --- path builder guards -------------------------------------------------------------------------------------------
constexpr uint32_t kPathNext = 0x50CD21, kPathLoop = 0x50CC36, kPathDone = 0x50CD2C;
constexpr uint32_t kPathNode = 0x50CC7A, kPathNodeFlags = 0x50CC90, kPathNodeScale = 0x50CC82, kPathNodeSkip = 0x50CCF6;
constexpr uint32_t kLeafCall = 0x50CCAA, kLeafLookup = 0x505F80;
uint8_t g_zeroLeaf[0x20] = {};

__declspec(naked) void PathNextStub() {
    __asm {
        mov esi, dword ptr [esi + 0x10]
        cmp edi, 498
        jb ok
        xor esi, esi            // 500 slots: stop here (the game would write past the path)
    ok:
        test esi, esi
        jz end
        push kPathLoop
        ret
    end:
        push kPathDone
        ret
    }
}

__declspec(naked) void PathNodeStub() {
    __asm {
        cmp dword ptr [esp + 0x44], 0
        jne have
        push kPathNodeSkip      // marker index out of range: no node data, keep its position as it is
        ret
    have:
        mov eax, dword ptr [esp + 0x14]
        test eax, eax
        jne flags
        push kPathNodeScale
        ret
    flags:
        push kPathNodeFlags
        ret
    }
}

void* __fastcall LeafOrZero(void* tree, void*, const float* p) {
    void* leaf = reinterpret_cast<void*(__thiscall*)(void*, const float*)>(kLeafLookup)(tree, p);
    return leaf ? leaf : g_zeroLeaf;
}

bool InstallRoutes() {
    static const uint8_t routeFail[] = {0x0F, 0x85, 0x54, 0xFD, 0xFF, 0xFF};
    static const uint8_t scanFail[] = {0x6A, 0x00, 0x8B, 0xCE, 0xE8, 0x6F, 0x86, 0xFD, 0xFF};
    static const uint8_t pathNext[] = {0x8B, 0x76, 0x10, 0x85, 0xF6, 0x0F, 0x85, 0x0A, 0xFF, 0xFF, 0xFF};
    static const uint8_t pathNode[] = {0x8B, 0x44, 0x24, 0x14, 0x85, 0xC0, 0x75, 0x0E};
    if (!patch::Matches(kRouteFailSite, routeFail, sizeof routeFail) || !patch::Matches(kScanFailSite, scanFail, sizeof scanFail) ||
        !patch::Matches(kPathNext, pathNext, sizeof pathNext) || !patch::Matches(kPathNode, pathNode, sizeof pathNode)) {
        dslog::Write("[fail] JP fixes: routing code not recognised - route fixes not applied");
        return false;
    }
    uint8_t scan[9] = {0x8B, 0xCE, 0xE8, 0, 0, 0, 0, 0x90, 0x90};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&ScanFailed) - (kScanFailSite + 7));
    memcpy(scan + 3, &rel, 4);
    const BytePatch scanPatch[] = {{DS_PATCH_NAME("mov ecx, esi; call ScanFailed"), kScanFailSite, scanFail, scan,
                                    sizeof scan, false}};
    // Jumps over whole instructions (the rest nop): jmp@ code groups, checked against the original bytes.
    return patch::WriteJump(kRouteFailSite, reinterpret_cast<const void*>(&RouteFailStub), sizeof routeFail, routeFail) &&
           patch::ApplyGroup("JP fixes: route scan failure", scanPatch) &&
           patch::WriteJump(kPathNext, reinterpret_cast<const void*>(&PathNextStub), sizeof pathNext, pathNext) &&
           patch::WriteJump(kPathNode, reinterpret_cast<const void*>(&PathNodeStub), sizeof pathNode, pathNode) &&
           patch::HookCall(kLeafCall, reinterpret_cast<const void*>(&LeafOrZero), kLeafLookup);
}
}  // namespace

void features::ApplyJpFixes() {
    static bool done = false;
    if (done) return;
    done = true;
    if (InstallRoutes()) dslog::Write("JP fixes: route hooks installed (panic teleport, path cap, null guards)");
    static const uint8_t drop[] = {0x7A, 0x56};
    g_dropOk = patch::Matches(kDropJump - 1, drop, sizeof drop);
    if (!g_dropOk) dslog::Write("[fail] JP fixes: item drop code not recognised - drop fix not applied");
    static const uint8_t gate[] = {0x39, 0xBE, 0x90, 0x0F, 0x00, 0x00, 0x0F, 0x8E, 0x4D, 0x01, 0x00, 0x00};
    static const uint8_t scale[] = {0xD8, 0x0D, 0xA8, 0xAC, 0x5D, 0x00};
    static const uint8_t lift[] = {0xC7, 0x44, 0x24, 0x18, 0x00, 0x00, 0x00, 0x00};
    if (!patch::Matches(kGateSite, gate, sizeof gate) || !patch::Matches(kScaleSites[0], scale, sizeof scale) ||
        !patch::Matches(kScaleSites[1], scale, sizeof scale) || !patch::Matches(kSideLiftImm - 4, lift, sizeof lift)) {
        dslog::Write("[fail] JP fixes: tank controller not recognised - stuck-tank fix not applied");
        return;
    }
    const uint32_t scaleAddr = reinterpret_cast<uint32_t>(&g_impulseScale);
    g_hooked = patch::WriteJump(kGateSite, reinterpret_cast<const void*>(&StuckStub), sizeof gate, gate) &&
               patch::WriteValue(kScaleSites[0] + 2, scaleAddr) &&
               patch::WriteValue(kScaleSites[1] + 2, scaleAddr);
    dslog::Write(g_hooked ? "JP fixes: stuck-tank hook installed" : "[fail] JP fixes: stuck-tank hook not written");
    OnFrameJpFixes();
}

void features::OnFrameJpFixes() {
    const int on = On() ? 1 : 0;
    if (on == g_applied) return;
    g_applied = on;
    if (g_hooked) patch::WriteValue(kSideLiftImm, on ? kLiftOn : kLiftOff);
    if (g_dropOk) patch::WriteValue(kDropJump, on ? kDropOn : kDropOff);
    dslog::Write(on ? "[ok]   JP fixes: stuck tanks pushed free, panic teleport, safe item drops (Japanese release)" : "[off]  JP fixes: the game's own rules");
}
