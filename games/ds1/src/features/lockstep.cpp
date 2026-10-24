// Online co-op by lockstep: every PC runs the whole mission (a normal split-screen co-op session, online.cpp /
// coop.cpp BeginOnlineSession) and only the players' inputs travel. Each frame is one tick; a tick runs only when every
// player's input for it has arrived, so all PCs compute the same world (determinism: determinism.cpp's findings).
//
// From the frame a mission starts loading (dispatch mode [0x60EC8C] = 1 during an online session):
//   - virtual clock: the game timers' clock calls (0x4BA3E4 / 0x4BA5B7 -> FUN_004ba370) and the game thread's
//     timeGetTime / GetTickCount (import slots 0x5D82F4 / 0x5D806C) see 1/60 s per tick, frozen inside the load frame
//   - FUN_004b7d50(seed): srand + the game's two random tables, seed from the host's START (net START command)
//   - the post-load "PLEASE PRESS ANY KEY" wait (FUN_00539a20 at 0x40C055) is skipped: each pass of it updates the game
//     timers, so its length otherwise changes the simulation; the timers' history is then set alike (NormalizeTimers)
//   - the sound library draws from its own random generator (its draws depend on each PC's audio hardware)
//   - every player's input block is set alike each tick and after the player set-up (LockstepFixBlocks)
//   - the game keeps running without focus (WM_ACTIVATE inactive branch 0x411390 jne -> jmp, [0x60EE2C] = 1)
// Every tick (start of the frame, before the update): this PC's player's input is sampled for tick + g_delay - per
// action the button evaluator FUN_00450770(block, action, mask) for masks 1..15, AxisValue's parts (pad axis, key held,
// gyro, splitscreen.cpp AxisParts) and the smoothed mouse movement FUN_00538080 - and sent (unreliable, the last
// inputs from the tick the other PCs still need on, in every packet); then the frame waits for every player's input for this tick and the game reads
// those: FUN_00450770 (entry hook), AxisValue and the mouse-look stub (splitscreen.cpp) answer from them for every
// player's block. PAUSE / OBJECTIVES are never sent (no pause menu online yet). The briefing pop-up is closed like
// customise.cpp does (state 0xE + script message 0x3B), at the same tick on every PC; the loadout screen stays off.
// A world hash (positions + health of every scene object) is compared every kHashEvery ticks; a mismatch is reported.
#include <windows.h>

#include <mmreg.h>
#include <intrin.h>

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/net.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/puppetsync.h"

namespace {
constexpr int kActions = 80, kDefaultDelay = 3, kHashEvery = 60, kMaxPlayers = 4;
constexpr uint32_t kBlocks = 0x60F5B8, kBlockSize = 0x478;
constexpr uint32_t kButtonEval = 0x450770, kMouseSmoothed = 0x538080, kMouseSmoother = 0x754BEC;
// Clock sites shared with the dev determinism test (features.h kClock*, higher priority = determinism wins)
constexpr uint32_t kClock = features::kClockTarget, kClockCallCtor = features::kClockCallCtor,
                   kClockCallTick = features::kClockCallTick;
constexpr uint32_t kTimerA = 0x63C954, kTimerB = 0x63C958, kReseed = 0x4B7D50;
constexpr uint32_t kIatTimeGetTime = features::kIatTimeGetTime, kIatGetTickCount = features::kIatGetTickCount;
constexpr uint32_t kDispatchMode = 0x60EC8C, kNextLevel = 0x5E7028, kGameWindow = 0x606A60, kObjects = 0x63C9A0;
constexpr uint32_t kFrontEndState = 0x617C18, kStateBusy = 0x617C20, kSetState = 0x47EF20, kScripts = 0x60F578;
constexpr uint32_t kActiveFlag = 0x60EE2C, kInactiveBranch = 0x411390, kPopup = 0x4D33A0, kViewController = 0x63C98C;
constexpr uint32_t kVirtualStart = 1000000;
constexpr uint32_t kKeyWait = 0x539A20, kKeyWaitCall = 0x40C055;   // post-load any-key wait
constexpr uint32_t kRandRange = 0x4B7DF0, kCrtRand = 0x567469;   // game random table (min, max) / CRT rand()
// The zoom FUN_0044d3a0 (fastcall block) reads the raw mouse wheel itself: FUN_0044ee60 up / FUN_0044ee90 down (no
// arguments; ecx = the block at both calls 0x44D480 / 0x44D4A8) - answered per player from the sent inputs.
constexpr uint32_t kWheelUp = 0x44EE60, kWheelDown = 0x44EE90, kWheelUpCall = 0x44D480, kWheelDownCall = 0x44D4A8;
constexpr int kActionPause = 6, kActionObjectives = 7, kNoJoystick = 7;

struct Input {
    uint16_t masks[kActions] = {};   // FUN_00450770(block, action, mask) for mask 1..15 -> bit mask-1
    uint8_t parts[kActions] = {};    // bit 0 key held (AxisValue), bit 1 pad axis touched (clears block +0x458)
    float pad[kActions] = {};        // AxisValue's pad axis value
    float gyro[kActions] = {};
    float mouse[2] = {};
    float lookScale = 1.0f;
    // the player's own MOUSE OPTIONS (block +0x43C / +0x440 sensitivity, +0x444 invert, +0x448 acceleration): the game
    // divides the mouse movement by them for every block, and every PC filled all blocks with its own values - player
    // 1 aimed straight on his PC and at the ground on player 2's (2-PC test 2026-10-11). Applied to the block per tick.
    float mouseSens[2] = {1.0f, 1.0f};
    uint8_t mouseFlags = 0;          // bit 0 invert, bit 1 acceleration, bit 7 = sent (a keyboard / mouse player)
    uint8_t wheel = 0;               // mouse wheel this tick: bit 0 up, bit 1 down (zoom, FUN_0044ee60 / FUN_0044ee90)
    std::string events;              // the host's game events (damage, script messages - authority.cpp) for this tick
};

bool g_installed = false, g_armed = false, g_running = false, g_desync = false, g_outOfStep = false;
// Owner mode (2026-10-15, authority.cpp): every PC runs its own player's soldiers at once from its own devices, the host
// runs the AI and the mission, the others follow records - no shared inputs, no waiting, no virtual clock. The input
// lockstep below (dev Dev\LockstepInputs = 1) desynced as soon as a player touched the AI or the world.
bool g_ownerMode = true;
int g_badChecks = 0, g_briefDiffs = 0;   // consecutive differing checks / short differences (authority)
int g_local = -1, g_players = 0;   // g_local: this PC's first player (lobby: a PC may have several)
uint32_t g_localMask = 0, g_senders = 0;   // this PC's players; each PC's first player (the ones that send)
// input delay in ticks: the host's choice from the measured ping (hostsync token), the same on every PC
int g_delay = kDefaultDelay;
uint32_t g_tick = 0, g_virtual = 0, g_seed = 0, g_hostSkipCutscenes = 0;
DWORD g_gameThread = 0;
std::map<uint32_t, Input> g_inputs[kMaxPlayers];
Input g_current[kMaxPlayers];
float g_ownSens[2] = {1.0f, 1.0f};
uint8_t g_ownMouseFlags = 0;   // 0 = not taken yet (nothing applied)
std::map<uint32_t, uint32_t> g_hashes[kMaxPlayers];
bool g_briefingClosed = false, g_briefingMessage = false;
std::string g_status;              // shown in the pop-up bar while waiting

// ---- online log (every build) ----
// DesertStormFix-online.log next to the game: the lockstep's events of online missions (start, stop, desyncs with their
// object dumps, waits, host corrections) and the hang watchdog - also in release builds, which have no other log, so a
// player can send it after a problem between two homes. New file per game start, written only once a session plays.
FILE* g_onlineLog = nullptr;
DWORD g_onlineLogStart = 0;
CRITICAL_SECTION g_onlineLogLock;

void OnlineLog(const char* fmt, ...) {
    char text[600];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    dslog::Write("%s", text);
    static bool init = false;
    if (!init) {
        init = true;
        InitializeCriticalSection(&g_onlineLogLock);
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (char* slash = strrchr(path, '\\')) strcpy_s(slash + 1, path + MAX_PATH - (slash + 1), "DesertStormFix-online.log");
        fopen_s(&g_onlineLog, path, "w");
        g_onlineLogStart = GetTickCount();
        if (g_onlineLog) fprintf(g_onlineLog, "DesertStormFix %s (built %s %s) online log\n", DS_VERSION, __DATE__, __TIME__);
    }
    if (!g_onlineLog) return;
    EnterCriticalSection(&g_onlineLogLock);
    const DWORD t = GetTickCount() - g_onlineLogStart;
    fprintf(g_onlineLog, "[%3lu:%02lu.%03lu] %s\n", t / 60000, t / 1000 % 60, t % 1000, text);
    fflush(g_onlineLog);
    LeaveCriticalSection(&g_onlineLogLock);
}

// Hang watchdog: when a running mission's game thread hasn't finished a tick for 5 s, a helper thread writes where it is
// (instruction address, code addresses on its stack, waiting for the other player's input or not) and this game's
// visible windows (an error box would show here). Again after 30 s if still stuck.
volatile DWORD g_lastTickDone = 0;
volatile bool g_inInputWait = false;
DWORD g_watchdogThread = 0;

BOOL CALLBACK ListWindow(HWND wnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(wnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(wnd)) return TRUE;
    char title[160] = {}, cls[80] = {};
    GetWindowTextA(wnd, title, sizeof title);
    GetClassNameA(wnd, cls, sizeof cls);
    OnlineLog("  window \"%s\" (%s)", title, cls);
    return TRUE;
}

void ReportHang(DWORD stuckMs) {
    OnlineLog("Lockstep: HANG - no tick for %lu s at tick %u (%s)", stuckMs / 1000, g_tick,
              g_inInputWait ? "waiting for the other player's input" : "inside the game");
    if (HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_gameThread)) {
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(t, &c)) {
                char line[400];
                int n = snprintf(line, sizeof line, "  game thread eip %08lX, stack:", c.Eip);
                const uint32_t* sp = reinterpret_cast<const uint32_t*>(c.Esp);
                int found = 0;
                for (int i = 0; i < 2048 && found < 16; ++i) {
                    uint32_t v;
                    __try { v = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
                    if (v > 0x401000 && v < 0x5D7000) n += snprintf(line + n, sizeof line - n, " %08X", v), ++found;
                }
                OnlineLog("%s", line);
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    EnumWindows(ListWindow, 0);
}

DWORD WINAPI Watchdog(void*) {
    DWORD reported = 0;
    for (;;) {
        Sleep(1000);
        if (!g_running || !g_lastTickDone) {
            reported = 0;
            continue;
        }
        const DWORD stuck = GetTickCount() - g_lastTickDone;
        if (stuck < 5000) {
            reported = 0;
            continue;
        }
        if (reported == 0 || (reported == 1 && stuck >= 30000)) {
            ReportHang(stuck);
            ++reported;
        }
    }
}

// ---- hooks ----
using ButtonFn = int(__thiscall*)(void*, int, uint32_t);
ButtonFn g_buttonOriginal = nullptr;
using TimeFn = DWORD(WINAPI*)();
TimeFn g_realTimeGetTime = nullptr, g_realGetTickCount = nullptr;

int PlayerOf(const void* block) {
    const intptr_t d = static_cast<const uint8_t*>(block) - reinterpret_cast<const uint8_t*>(kBlocks);
    return d >= 0 && d % kBlockSize == 0 && d / static_cast<intptr_t>(kBlockSize) < g_players
               ? static_cast<int>(d / kBlockSize) : -1;
}

int __fastcall WheelUp(void* block) {
    if (g_armed && !g_running && PlayerOf(block) >= 0) return 0;
    const int p = g_running ? PlayerOf(block) : -1;
    return p < 0 ? reinterpret_cast<int(__fastcall*)(void*)>(kWheelUp)(block) : g_current[p].wheel & 1;
}
int __fastcall WheelDown(void* block) {
    if (g_armed && !g_running && PlayerOf(block) >= 0) return 0;
    const int p = g_running ? PlayerOf(block) : -1;
    return p < 0 ? reinterpret_cast<int(__fastcall*)(void*)>(kWheelDown)(block) : (g_current[p].wheel >> 1) & 1;
}

// From the frame lockstep arms (the level load) until the first tick, the players' blocks get no input at all (buttons,
// axes, mouse): the game updates during the load frame would otherwise read each PC's own devices.
int __fastcall Button(void* block, void*, int action, uint32_t mask) {
    const int p = g_armed ? PlayerOf(block) : -1;
    if (p < 0) return g_buttonOriginal(block, action, mask);
    if (!g_running) return 0;
    if (action < 0 || action >= kActions || !(mask & 15)) return 0;
    return (g_current[p].masks[action] >> ((mask & 15) - 1)) & 1;
}

// The game timers' clock calls 0x4BA3E4 / 0x4BA5B7 (call FUN_004ba370): broker chains shared with determinism.cpp,
// one subscriber (and next) per site.
using ClockFn = uint32_t(__cdecl*)();
ClockFn g_clockCtorNext = nullptr, g_clockTickNext = nullptr;
uint32_t __cdecl ClockCtor() { return g_armed && !g_ownerMode ? g_virtual : g_clockCtorNext(); }
uint32_t __cdecl ClockTick() { return g_armed && !g_ownerMode ? g_virtual : g_clockTickNext(); }
DWORD WINAPI VirtualTimeGetTime() {
    return g_armed && !g_ownerMode && GetCurrentThreadId() == g_gameThread ? g_virtual : g_realTimeGetTime();
}
DWORD WINAPI VirtualGetTickCount() {
    return g_armed && !g_ownerMode && GetCurrentThreadId() == g_gameThread ? g_virtual : g_realGetTickCount();
}

void HookVirtualSound();
extern bool g_virtualSound;
#ifndef DS_DIST
void InstallProbe();
#endif
int SoundPlayingAnswer(uint8_t* mgr, uint32_t owner);
void __cdecl OpenBriefing(uint32_t state);
int __cdecl KeyWait(int timeout);
void ScanVoices();

// The sound library's random draws come from a private generator. A voice start draws its pitch variation from the
// game's random table (FUN_004b7df0 at 0x55E518), the bank's sample pick (0x5616F7) and the ambient ENVSND interval
// (0x55FE77) call the CRT rand() - all only when the audio side actually plays something, which depends on each PC's
// sound hardware (free voices, buffers). One extra draw shifts every later AI / weapon / effect roll: a 2-PC soak test
// (2026-10-12) split at tick 1901 on exactly one 0x55E51D draw, the world 30 ticks later. Audio only - no game effect.
uint32_t g_soundRng = 0x13579BDF;
int SoundRandInt() {
    g_soundRng = g_soundRng * 214013u + 2531011u;
    return static_cast<int>((g_soundRng >> 16) & 0x7FFF);
}
int __cdecl SoundRand() { return SoundRandInt(); }
// Gameplay asking whether / when a soldier or object was drawn: FUN_004c9f10 (thiscall object: ever drawn, at 0x43F522)
// and FUN_004c9ef0 (game time it was last drawn, at 0x4248F2 / 0x42AF45 / 0x43F53C). What each PC draws depends on its
// screen and DRAW DISTANCE (a 2-PC test with different draw distances split in every run) - online all answer "never".
int __fastcall EverDrawn(void* object) {
    if (g_armed) return 0;
    return reinterpret_cast<int(__fastcall*)(void*)>(0x4C9F10)(object);
}
int __fastcall TimeDrawn(void* object) {
    if (g_armed) return 0;
    return reinterpret_cast<int(__fastcall*)(void*)>(0x4C9EF0)(object);
}
// FUN_004cd980 (thiscall instance) = frames since drawn; online every PC answers "long ago" (what each one drew differs)
int __fastcall FramesSinceDrawn(void* instance) {
    if (g_armed) return 100000;
    return reinterpret_cast<int(__fastcall*)(void*)>(0x4CD980)(instance);
}
float __cdecl SoundRandRange(float lo, float hi) { return lo + (hi - lo) * (SoundRandInt() / 32767.0f); }

// Raw keyboard queries: FUN_00539da0 / 00539dd0 / 00539e10 / 00539820 (key held / pressed / released / any key; all
// start "mov eax, [0x754C6C]" - the input-off flag - and are cdecl) read this PC's own keyboard. The players' controls
// don't use them while online (the action evaluator is answered from the sent inputs), but the front-end screens a
// mission opens (the briefing pop-up, pause menu, objectives) and a few game checks do: a key held on one PC as the
// briefing opened (tick 32) moved view 0's camera on that PC only (3 of 6 soak runs split there). While lockstep runs
// they answer "no key" - except inside Sample(), where the original evaluator reads this PC's keyboard for the player.
// Entry detours (broker function hooks): the stub answers "no key" or goes on through the chain (`next`).
volatile uint32_t g_blockRawKeys = 0;
void* g_rawKeyNext[4] = {};
#define RAW_KEY_STUB(name, i)                            \
    __declspec(naked) void name() {                      \
        __asm cmp g_blockRawKeys, 0                      \
        __asm jne off                                    \
        __asm jmp dword ptr [g_rawKeyNext + i * 4]       \
        __asm off:                                       \
        __asm xor eax, eax                               \
        __asm ret                                        \
    }
RAW_KEY_STUB(RawKeyHeld, 0)
RAW_KEY_STUB(RawKeyPressed, 1)
RAW_KEY_STUB(RawKeyReleased, 2)
RAW_KEY_STUB(RawAnyKey, 3)
#undef RAW_KEY_STUB

bool HookRawKeys() {
    const uint8_t entry[] = {0xA1, 0x6C, 0x4C, 0x75, 0x00};
    const struct { uint32_t at; void (*fn)(); } sites[] = {
        {0x539DA0, &RawKeyHeld}, {0x539DD0, &RawKeyPressed}, {0x539E10, &RawKeyReleased}, {0x539820, &RawAnyKey}};
    for (const auto& h : sites)
        if (!patch::Matches(h.at, entry, sizeof entry) && *reinterpret_cast<const uint8_t*>(h.at) != 0xE9 &&
            !g_rawKeyNext[&h - sites])
            return false;   // (0xE9 = another broker hook already there, e.g. multikbm.cpp)
    bool ok = true;
    for (const auto& h : sites)
        ok = patch::HookFunction(h.at, reinterpret_cast<const void*>(h.fn), &g_rawKeyNext[&h - sites], entry,
                                 sizeof entry) && ok;
    return ok;
}

#ifndef DS_DIST
// dev: CRT rand() calls counted per call site (logged per tick by TickLog, "crt site:n")
constexpr uint32_t kCrtSites[] = {0x41AD70, 0x41B49D, 0x4220B0, 0x42CD7F, 0x42CE7E, 0x49DE42, 0x4B8C2D, 0x4B8C3A,
                                  0x4B8C47, 0x4B8C56, 0x50FE61, 0x50FFE5, 0x510199, 0x51022B, 0x510C4B, 0x510C7F,
                                  0x510CA0, 0x510CC1, 0x510E36, 0x510E4D, 0x510F04, 0x510F28, 0x510F4C, 0x52899E,
                                  0x55A97E, 0x55A9FE, 0x55AAA1, 0x55AB64, 0x55AB7B, 0x55AB92, 0x55AC43, 0x55AC5A,
                                  0x55AC71};
uint32_t g_crtCount[sizeof kCrtSites / sizeof kCrtSites[0]] = {};
int __cdecl CountedCrtRand() {
    const uint32_t site = reinterpret_cast<uint32_t>(_ReturnAddress()) - 5;
    for (size_t i = 0; i < sizeof kCrtSites / sizeof kCrtSites[0]; ++i)
        if (kCrtSites[i] == site) ++g_crtCount[i];
    return reinterpret_cast<int(__cdecl*)()>(kCrtRand)();
}
void HookCrtCount() {
    DWORD v = 0, size = sizeof v;
    RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "LockstepTickLog", RRF_RT_REG_DWORD, nullptr, &v, &size);
    if (!v) return;
    for (uint32_t s : kCrtSites) patch::HookCall(s, reinterpret_cast<const void*>(&CountedCrtRand), kCrtRand);
}
#endif

#ifndef DS_DIST
// dev: every weapon trigger FUN_004a1ad0 (thiscall weapon; shooter, 3 more; ret 0x10) during an online mission -
// "#f tick site shooter-id team" (which soldier fired on one PC only)
using TriggerFn = int(__thiscall*)(void*, void*, uint32_t, uint32_t, uint32_t);
int __fastcall TriggerLog(void* weapon, void*, void* shooter, uint32_t b, uint32_t c, uint32_t d) {
    if (g_running) {
        const auto* s = static_cast<const uint8_t*>(shooter);
        dslog::Write("#f %u %08X %d %d", g_tick, reinterpret_cast<uint32_t>(_ReturnAddress()) - 5,
                     s ? *reinterpret_cast<const int*>(s + 8) : -1, s ? s[0x10] : -1);
    }
    return reinterpret_cast<TriggerFn>(0x4A1AD0)(weapon, shooter, b, c, d);
}
void HookTriggerLog() {
    DWORD v = 0, size = sizeof v;
    RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "LockstepTickLog", RRF_RT_REG_DWORD, nullptr, &v, &size);
    if (!v) return;
    for (uint32_t site : {0x4021B1u, 0x441042u, 0x44108Au, 0x4410ADu, 0x4410C3u, 0x48529Du, 0x4852DCu, 0x4A4DB8u})
        patch::HookCall(site, reinterpret_cast<const void*>(&TriggerLog), 0x4A1AD0);
}
#endif

// Visual effects - particles / emitters (0x4F0000..0x4F4A00, run by FUN_004f2840) and the surface impact effects
// (0x54C5D6..0x54C7D8) - drew from the game's random tables, and they run for what is near / on each PC's screen: a
// 2-PC test with different resolutions split on 74 extra particle draws. Their draws come from a private generator
// with the same semantics as the game's (FUN_004b7df0 float range, 0x4B7E30 raw, 0x4B7E60 % n, 0x4B7E90 lo + % span).
uint32_t g_fxRng = 0x2468ACE1;
uint32_t FxRaw() {
    g_fxRng = g_fxRng * 1664525u + 1013904223u;
    return g_fxRng >> 1;
}
float __cdecl FxRange(float lo, float hi) { return lo + (hi - lo) * ((FxRaw() & 0xFFFFFF) / 16777215.0f); }
uint32_t __cdecl FxRawCall() { return FxRaw(); }
uint32_t __cdecl FxMod(uint32_t n) { return n ? FxRaw() % n : 0; }
int __cdecl FxSpan(int lo, int hi) { return hi != lo ? lo + static_cast<int>(FxRaw() % static_cast<uint32_t>(hi - lo)) : lo; }
void HookEffectDraws() {
    int failed = 0;
    for (uint32_t site : {0x4F02E1u, 0x4F05E5u, 0x4F05FEu, 0x4F0765u, 0x4F0D8Bu, 0x4F0D9Eu, 0x4F0DB1u, 0x4F0F60u,
                          0x4F19C9u, 0x4F19D5u, 0x4F19E1u, 0x4F1B38u, 0x4F1B48u, 0x4F1B58u, 0x4F1C45u, 0x4F1C6Cu,
                          0x4F1C80u, 0x4F1DB7u, 0x4F1DCEu, 0x4F1DE5u, 0x4F1E43u, 0x4F1E51u, 0x4F1E5Fu, 0x4F2A3Au,
                          0x4F2A55u, 0x4F2A72u, 0x4F2A8Fu, 0x4F2B5Cu, 0x4F2B72u, 0x4F2B88u, 0x4F2C84u, 0x4F2CD4u,
                          0x4F49B2u, 0x4F49C5u, 0x4F49E0u, 0x4F49F3u, 0x4F4A06u, 0x54C64Bu, 0x54C65Eu, 0x54C713u,
                          0x54C7B2u, 0x54C7C5u, 0x54C7D8u})
        failed += !patch::HookCall(site, reinterpret_cast<const void*>(&FxRange), kRandRange);
    for (uint32_t site : {0x4F0643u, 0x4F0788u})
        failed += !patch::HookCall(site, reinterpret_cast<const void*>(&FxRawCall), 0x4B7E30);
    for (uint32_t site : {0x4F1F99u, 0x4F2091u, 0x54C5D6u, 0x54C624u})
        failed += !patch::HookCall(site, reinterpret_cast<const void*>(&FxMod), 0x4B7E60);
    for (uint32_t site : {0x4F06A2u, 0x4F07E6u})
        failed += !patch::HookCall(site, reinterpret_cast<const void*>(&FxSpan), 0x4B7E90);
    if (failed) dslog::Write("[fail] Lockstep: %d effect random draw(s) not separated", failed);
}

#ifndef DS_DIST
// dev: every remaining call of the game's random-table functions counted per call site (scanned at install: E8 rel32
// to 0x4B7DF0 / 0x4B7E30 / 0x4B7E60 / 0x4B7E90), logged per tick by TickLog ("tbl site:n")
std::map<uint32_t, uint32_t> g_tblCount;
float __cdecl TblRange(float lo, float hi) {
    ++g_tblCount[reinterpret_cast<uint32_t>(_ReturnAddress()) - 5];
    return reinterpret_cast<float(__cdecl*)(float, float)>(kRandRange)(lo, hi);
}
uint32_t __cdecl TblRaw() {
    ++g_tblCount[reinterpret_cast<uint32_t>(_ReturnAddress()) - 5];
    return reinterpret_cast<uint32_t(__cdecl*)()>(0x4B7E30)();
}
uint32_t __cdecl TblMod(uint32_t n) {
    ++g_tblCount[reinterpret_cast<uint32_t>(_ReturnAddress()) - 5];
    return reinterpret_cast<uint32_t(__cdecl*)(uint32_t)>(0x4B7E60)(n);
}
int __cdecl TblSpan(int lo, int hi) {
    ++g_tblCount[reinterpret_cast<uint32_t>(_ReturnAddress()) - 5];
    return reinterpret_cast<int(__cdecl*)(int, int)>(0x4B7E90)(lo, hi);
}
void HookTableCount() {
    DWORD v = 0, size = sizeof v;
    RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "LockstepTickLog", RRF_RT_REG_DWORD, nullptr, &v, &size);
    if (!v) return;
    const struct { uint32_t target; const void* fn; } fns[] = {{kRandRange, reinterpret_cast<const void*>(&TblRange)},
        {0x4B7E30, reinterpret_cast<const void*>(&TblRaw)}, {0x4B7E60, reinterpret_cast<const void*>(&TblMod)},
        {0x4B7E90, reinterpret_cast<const void*>(&TblSpan)}};
    int n = 0;
    for (uint32_t a = 0x401000; a < 0x5D7000 - 5; ++a) {
        const auto* b = reinterpret_cast<const uint8_t*>(a);
        if (*b != 0xE8) continue;
        const uint32_t target = a + 5 + *reinterpret_cast<const int32_t*>(b + 1);
        for (const auto& f : fns)
            if (target == f.target && patch::HookCall(a, f.fn, f.target)) ++n;
    }
    dslog::Write("[dev]  Lockstep: %d random-table call sites counted", n);
}
#endif

// Device lost (fullscreen Alt+Tab, another program taking the screen, display off): the message loop's device check
// FUN_00548920 (renderer: TestCooperativeLevel == D3DERR_DEVICELOST -> 0) makes the loop Sleep(100) instead of running
// a frame - an online PC doing that stopped every PC (they wait for its input: the 2-PC test's 5 s "waiting" hangs).
// While an online mission runs the frame runs anyway, without the render loop (framerec.cpp asks LockstepSkipRender).
bool g_skipRender = false, g_catchUp = false;
LONGLONG g_due = 0;   // when the next tick is due (QPC)
int __fastcall DeviceCheck(void* renderer) {
    const int r = reinterpret_cast<int(__thiscall*)(void*)>(0x548920)(renderer);
    const bool lost = r != 1 && g_running;
    if (lost != g_skipRender) OnlineLog(lost ? "Lockstep: display device lost - the game runs on without drawing"
                                             : "Lockstep: display device back");
    g_skipRender = lost;
    return lost ? 1 : r;
}

bool Install() {
    patch::OwnerScope owner(pse_host::Find("dsfix.lockstep"));  // lazy hooks: owned by this feature's PSE plugin
    if (g_installed) return true;
    // FUN_00450770 entry (push ebx; push ebp; push esi; mov esi, ecx - 5 bytes) -> Button (broker entry detour;
    // g_buttonOriginal = the chain's next / the trampoline)
    const uint8_t entry[] = {0x53, 0x55, 0x56, 0x8B, 0xF1};
    if (!patch::SubscribeCall(kClockCallCtor, reinterpret_cast<const void*>(&ClockCtor), kClock,
                              features::kClockLockstep, reinterpret_cast<void**>(&g_clockCtorNext)) ||
        !patch::SubscribeCall(kClockCallTick, reinterpret_cast<const void*>(&ClockTick), kClock,
                              features::kClockLockstep, reinterpret_cast<void**>(&g_clockTickNext))) {
        dslog::Write("[fail] Lockstep: game clock calls not hooked");
        return false;
    }
    if (!patch::HookFunction(kButtonEval, reinterpret_cast<const void*>(&Button),
                             reinterpret_cast<void**>(&g_buttonOriginal), entry, sizeof entry)) {
        dslog::Write("[fail] Lockstep: unexpected bytes at 0x%08X", kButtonEval);
        return false;
    }
    patch::HookCall(kWheelUpCall, reinterpret_cast<const void*>(&WheelUp), kWheelUp);
    if (!patch::HookCall(kKeyWaitCall, reinterpret_cast<const void*>(&KeyWait), kKeyWait))
        dslog::Write("[fail] Lockstep: post-load key wait not hooked - desyncs likely");
    patch::HookCall(kWheelDownCall, reinterpret_cast<const void*>(&WheelDown), kWheelDown);
    // import slots: broker pointer chains shared with determinism.cpp (next = the real function or its hook)
    if (!patch::HookSlot(kIatTimeGetTime, reinterpret_cast<const void*>(&VirtualTimeGetTime),
                         reinterpret_cast<void**>(&g_realTimeGetTime), 0, features::kClockLockstep) ||
        !patch::HookSlot(kIatGetTickCount, reinterpret_cast<const void*>(&VirtualGetTickCount),
                         reinterpret_cast<void**>(&g_realGetTickCount), 0, features::kClockLockstep))
        dslog::Write("[fail] Lockstep: timeGetTime / GetTickCount imports not hooked - desyncs likely");
    if (!g_virtualSound) HookVirtualSound();
    HookEffectDraws();
    if (!patch::HookCall(0x4116A3, reinterpret_cast<const void*>(&DeviceCheck), 0x548920))
        dslog::Write("[fail] Lockstep: device check not hooked - a lost display stops every PC");
    if (!patch::HookCall(0x489592, reinterpret_cast<const void*>(&OpenBriefing), kSetState))
        dslog::Write("[fail] Lockstep: briefing pop-up call not hooked - desyncs likely");
    if (!HookRawKeys()) dslog::Write("[fail] Lockstep: raw keyboard queries not hooked - desyncs likely");
    // light flicker (FUN_0055a950 from the lights code 0x51EE1E) runs for the lights each PC draws: private draws too
    for (uint32_t site : {0x55A97Eu, 0x55A9FEu, 0x55AAA1u, 0x55AB64u, 0x55AB7Bu, 0x55AB92u, 0x55AC43u, 0x55AC5Au, 0x55AC71u})
        patch::HookCall(site, reinterpret_cast<const void*>(&SoundRand), kCrtRand);
    // game logic asking "frames since this instance was drawn" (FUN_004cd980 at 0x43F138): a fixed answer online
    patch::HookCall(0x43F138, reinterpret_cast<const void*>(&FramesSinceDrawn), 0x4CD980);
    patch::HookCall(0x43F522, reinterpret_cast<const void*>(&EverDrawn), 0x4C9F10);
    for (uint32_t site : {0x4248F2u, 0x42AF45u, 0x43F53Cu})
        patch::HookCall(site, reinterpret_cast<const void*>(&TimeDrawn), 0x4C9EF0);
    features::EnsureDormancyHook();
    if (!patch::HookCall(0x55E518, reinterpret_cast<const void*>(&SoundRandRange), kRandRange) ||
        !patch::HookCall(0x55FE77, reinterpret_cast<const void*>(&SoundRand), kCrtRand) ||
        !patch::HookCall(0x5616F7, reinterpret_cast<const void*>(&SoundRand), kCrtRand))
        dslog::Write("[fail] Lockstep: sound random draws not separated (dev draw log on?) - desyncs likely");
#ifndef DS_DIST
    InstallProbe();
    HookCrtCount();
    HookTriggerLog();
    HookTableCount();
#endif
    g_installed = true;
    dslog::Write("Lockstep: hooks installed");
    return true;
}

void Popup(const char* text, int ms) {
    if (void* c = *reinterpret_cast<void**>(kViewController))
        reinterpret_cast<void(__thiscall*)(void*, const char*, int)>(kPopup)(c, text, ms);
}

void KeepRunning(bool on) {
    const uint8_t jne = 0x75, jmp = 0xEB;
    if (on) {
        if (patch::Matches(kInactiveBranch, &jne, 1)) patch::Write(kInactiveBranch, &jmp, 1);
        *reinterpret_cast<volatile uint32_t*>(kActiveFlag) = 1;
    } else if (!features::DevBackground() && patch::Matches(kInactiveBranch, &jmp, 1)) {
        patch::Write(kInactiveBranch, &jne, 1);
    }
    features::SetPadsBackground(on);   // the pads too
}

// The post-load "PLEASE PRESS ANY KEY" wait FUN_00539a20(timeout) (called at 0x40C055 at the end of the level load)
// drains the key messages, then loops until a key / pad button - and every iteration runs the game timer's update
// FUN_004ba5b0 and Sleep. With the clock frozen the time doesn't move, but each pass still advances the timer's frame
// count (+0xC8), its 30-slot dt ring (+0x2C, index +0xA4) and the running average (+0xAC): the number of passes - how
// long each PC waited - was carried into the mission (camera smoothing of view 0 split at tick 33 in 2 of 5 soak runs;
// the determinism test's "the wait's length changes the simulation"). Online the wait is skipped (no pass at all).
using KeyWaitFn = int(__cdecl*)(int);
int __cdecl KeyWait(int timeout) {
    if (g_armed) return 0;
    return reinterpret_cast<KeyWaitFn>(kKeyWait)(timeout);
}

// Both game timers' history the same on every PC at the first tick: a steady 1/60 s (the load frame's progress
// callbacks also update them, at whatever rate the loading screen got).
void NormalizeTimers() {
    for (uint32_t g : {kTimerA, kTimerB})
        if (uint8_t* t = *reinterpret_cast<uint8_t**>(g)) {
            for (int i = 0; i < 30; ++i) reinterpret_cast<float*>(t + 0x2C)[i] = 1.0f / 60;
            *reinterpret_cast<uint32_t*>(t + 0xA4) = 0;
            *reinterpret_cast<float*>(t + 0xA8) = 1.0f / 60;
            *reinterpret_cast<float*>(t + 0xAC) = 1.0f / 60;
            *reinterpret_cast<uint32_t*>(t + 0xC8) = 0;
            *reinterpret_cast<uint32_t*>(t + 0xD4) = g_virtual;
        }
    // The render frame counter [0x5F9C30] (+1 per render, 0x4E2DA2) and each render instance's "drawn at frame" +0xD0:
    // FUN_004cd980 = frames since an instance was drawn, read by game logic (0x43F138 ...). Each PC rendered a different
    // number of menu / loading-screen frames, so an instance not drawn since the load answered differently per PC.
    // Every PC: the same counter, every scene object's instance "not drawn for a long time".
    *reinterpret_cast<uint32_t*>(0x5F9C30) = 1000000;
    uint32_t maxId = 0;
    const uint8_t* mgr = *reinterpret_cast<uint8_t* const*>(kObjects);
    uint8_t* const* table = mgr ? *reinterpret_cast<uint8_t* const* const*>(mgr + 0x30) : nullptr;
    if (mgr) maxId = *reinterpret_cast<const uint32_t*>(mgr + 0x34);
    for (uint32_t id = 0; table && id <= maxId && id < 4096; ++id)
        if (uint8_t* o = table[id])
            if (uint8_t* inst = *reinterpret_cast<uint8_t**>(o + 0x2C)) *reinterpret_cast<uint32_t*>(inst + 0xD0) = 0;
}

// Online: "is a sound of this owner still playing?" FUN_0055e130 (thiscall sound manager [0x63C9AC],
// owner handle; 11 callers in the soldier code - talking, chatter) answered by game time. The voices' playing flag
// (voices block [mgr+0x34] +0x410, [mgr+0x38] voices of 0x68 B: +0 flags bit 0 playing, +4 IDirectSoundBuffer,
// +0x60 owner) is cleared when the audio hardware reaches the end - real time - so the answer depended on how far the
// sound card got. Records per started voice: owner + game-time end = buffer bytes / (block align x frequency); looping
// voices count while the flag is set (they end only by being stopped). Explicit stop by owner FUN_0055e090 (5 game
// call sites) drops the owner's records. The stream slot (+0x3A8 / owner +0x408) and the speech queue (+0xFFC, 2 x
// 0x158) are checked as the original does.
struct VoiceRec {
    int slot;
    void* buffer;
    uint32_t owner, end;
    bool loop, live;
};
std::vector<VoiceRec> g_voices;
bool g_virtualSound = false;   // (declared above)
// The dev determinism harness (Dev\DetTest, mission DLL -> Lua parity runs) uses the same answer with its own virtual
// clock: real-time sound was the remaining noise between two runs of one mission (11B / 12A / Training 1).
bool g_harnessSound = false;
bool SoundVirtual() { return g_armed || (g_harnessSound && features::DetActive()); }
uint32_t SoundNow() { return g_armed ? g_virtual : features::DetVirtualTime(); }

void ScanVoices() {
    const uint8_t* mgr = *reinterpret_cast<uint8_t* const*>(0x63C9AC);
    if (!mgr) return;
    const uint8_t* base = *reinterpret_cast<uint8_t* const*>(mgr + 0x34);
    const int n = *reinterpret_cast<const int*>(mgr + 0x38);
    for (VoiceRec& r : g_voices) {   // the voice slot was stopped / reused (real time or explicit): no longer live
        const uint8_t* v = base + 0x410 + r.slot * 0x68;
        if (!(*v & 1) || *reinterpret_cast<void* const*>(v + 4) != r.buffer) r.live = false;
    }
    for (int i = 0; base && i < n; ++i) {
        const uint8_t* v = base + 0x410 + i * 0x68;
        if (!(*v & 1)) continue;
        void* buf = *reinterpret_cast<void* const*>(v + 4);
        bool known = false;
        for (const VoiceRec& r : g_voices) known |= r.live && r.slot == i && r.buffer == buf;
        if (known || !buf) continue;
        void** vt = *reinterpret_cast<void***>(buf);
        struct { DWORD size, flags, bytes, rate, cpu; } caps{sizeof caps};
        DWORD freq = 0, status = 0, written = 0;
        WAVEFORMATEX wfx{};
        reinterpret_cast<HRESULT(WINAPI*)(void*, void*)>(vt[3])(buf, &caps);
        reinterpret_cast<HRESULT(WINAPI*)(void*, WAVEFORMATEX*, DWORD, DWORD*)>(vt[5])(buf, &wfx, sizeof wfx, &written);
        reinterpret_cast<HRESULT(WINAPI*)(void*, DWORD*)>(vt[8])(buf, &freq);
        reinterpret_cast<HRESULT(WINAPI*)(void*, DWORD*)>(vt[9])(buf, &status);
        if (!freq) freq = wfx.nSamplesPerSec;
        const uint64_t perSec = uint64_t(wfx.nBlockAlign ? wfx.nBlockAlign : 2) * (freq ? freq : 22050);
        const uint32_t ms = static_cast<uint32_t>(uint64_t(caps.bytes) * 1000 / perSec);
        g_voices.push_back({i, buf, *reinterpret_cast<const uint32_t*>(v + 0x60), SoundNow() + ms, (status & 4) != 0, true});
    }
    g_voices.erase(std::remove_if(g_voices.begin(), g_voices.end(),
                                  [](const VoiceRec& r) { return r.loop ? !r.live : int(r.end - SoundNow()) <= 0; }),
                   g_voices.end());
}

uint32_t g_soundAsked = 0, g_soundYes = 0, g_streamYes = 0;   // dev trace: queries / yes / yes by the stream slot
int __fastcall SoundPlaying(uint8_t* mgr, void*, uint32_t owner) {
    ++g_soundAsked;
    const int r = SoundPlayingAnswer(mgr, owner);
    g_soundYes += r;
    return r;
}
int SoundPlayingAnswer(uint8_t* mgr, uint32_t owner) {
    if (!owner) return 0;
    const uint8_t* base = *reinterpret_cast<uint8_t* const*>(mgr + 0x34);
    const int n = *reinterpret_cast<const int*>(mgr + 0x38);
    if (g_virtualSound && SoundVirtual()) {
        for (const VoiceRec& r : g_voices)
            if (r.owner == owner) return 1;
        for (int i = 0; i < n; ++i) {   // started since the last scan (this frame): playing
            const uint8_t* v = base + 0x410 + i * 0x68;
            if (!(*v & 1) || *reinterpret_cast<const uint32_t*>(v + 0x60) != owner) continue;
            bool known = false;
            for (const VoiceRec& r : g_voices) known |= r.slot == i && r.buffer == *reinterpret_cast<void* const*>(v + 4);
            if (!known) return 1;
        }
    } else
        for (int i = 0; i < n; ++i) {
            const uint8_t* v = base + 0x410 + i * 0x68;
            if ((*v & 1) && *reinterpret_cast<const uint32_t*>(v + 0x60) == owner) return 1;
        }
    if ((base[0x3A8] & 1) && *reinterpret_cast<const uint32_t*>(base + 0x408) == owner) return ++g_streamYes, 1;
    for (int q = 0; q < 2; ++q) {
        const uint32_t* e = reinterpret_cast<const uint32_t*>(mgr + 0xFFC + q * 0x158);
        for (uint32_t k = 0; k < e[0]; ++k)
            if (e[7 + k * 7] == owner) return 1;
    }
    return 0;
}

void __fastcall StopOwner(void* mgr, void*, uint32_t owner) {
    g_voices.erase(std::remove_if(g_voices.begin(), g_voices.end(), [&](const VoiceRec& r) { return r.owner == owner; }),
                   g_voices.end());
    reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(0x55E090)(mgr, owner);
}

void HookVirtualSound() {
    // FUN_0055e130 (thiscall(mgr, owner): "owner's sound still playing") replaced (entry detour, no next): push ebx;
    // mov ebx, [esp+8]
    const uint8_t entry[] = {0x53, 0x8B, 0x5C, 0x24, 0x08};
    if (!patch::HookFunction(0x55E130, reinterpret_cast<const void*>(&SoundPlaying), nullptr, entry, sizeof entry))
        dslog::Write("[fail] Lockstep: 'sound still playing' not hooked - desyncs likely");
    for (uint32_t site : {0x41A474u, 0x41C1B3u, 0x423BDAu, 0x49C73Bu, 0x4A1D36u})
        patch::HookCall(site, reinterpret_cast<const void*>(&StopOwner), 0x55E090);
    g_virtualSound = true;
    dslog::Write("Lockstep: 'sound still playing' answered by game time");
}

}  // namespace

// Dev determinism harness: sound random draws from the private generator + "sound still playing" by its virtual clock
// (same hooks as online; installing them again later is a no-op).
void features::HarnessSound() {
    g_harnessSound = true;
    if (!patch::HookCall(0x55E518, reinterpret_cast<const void*>(&SoundRandRange), kRandRange) ||
        !patch::HookCall(0x55FE77, reinterpret_cast<const void*>(&SoundRand), kCrtRand) ||
        !patch::HookCall(0x5616F7, reinterpret_cast<const void*>(&SoundRand), kCrtRand))
        dslog::Write("[fail] Determinism: sound random draws not separated");
    if (!g_virtualSound) HookVirtualSound();
}
void features::HarnessSoundFrame() {
    if (g_harnessSound && features::DetActive()) ScanVoices();
}

namespace {
// ---- inputs ----
// device (lobby.h): kDeviceAny = keyboard & mouse + this PC's first pad, kDeviceKeyboard = keyboard & mouse only, 0..7 =
// that game joystick only (block +0x38: 1 keyboard / mouse, 2 pad; +0x3C8 the joystick - never -1, which turns the
// block off)
Input Sample(int p, int device = -1) {
    Input in;
    auto* block = reinterpret_cast<uint8_t*>(kBlocks + p * kBlockSize);
    int32_t& joystick = *reinterpret_cast<int32_t*>(block + 0x3C8);
    uint32_t& devices = *reinterpret_cast<uint32_t*>(block + 0x38);
    const int32_t shared = joystick;
    const uint32_t sharedDevices = devices;
    joystick = device >= 0 && device < 8 ? device : 0;
    devices = device < 0 ? 3u : device >= 8 ? 1u : 2u;
    for (int a = 0; a < kActions; ++a) {
        if (a == kActionPause || a == kActionObjectives) continue;
        for (uint32_t m = 1; m < 16; ++m)
            if (g_buttonOriginal(block, a, m)) in.masks[a] |= static_cast<uint16_t>(1u << (m - 1));
        bool touched, key;
        features::AxisParts(block, a, in.pad[a], touched, key, in.gyro[a]);
        in.parts[a] = static_cast<uint8_t>((key ? 1 : 0) | (touched ? 2 : 0));
    }
    if (*reinterpret_cast<uint32_t*>(block + 0x38) & 1)
        reinterpret_cast<void(__thiscall*)(void*, float*, float*)>(kMouseSmoothed)(reinterpret_cast<void*>(kMouseSmoother),
                                                                             &in.mouse[0], &in.mouse[1]);
    in.lookScale = features::LookScale(p);
    in.mouseSens[0] = g_ownSens[0];   // this PC's own mouse options (taken when the mission started)
    in.mouseSens[1] = g_ownSens[1];
    in.mouseFlags = g_ownMouseFlags;
    // both read the block's pad state through ecx when the wheel didn't move ([ecx+0xE0] / [ecx+0xE4]) - a call without
    // ecx = the block crashed at 0x44EE7B on the first mission frame
    in.wheel = static_cast<uint8_t>((reinterpret_cast<int(__fastcall*)(void*)>(kWheelUp)(block) ? 1 : 0) |
                                    (reinterpret_cast<int(__fastcall*)(void*)>(kWheelDown)(block) ? 2 : 0));
    joystick = shared;
    devices = sharedDevices;
    return in;
}

// One Input packed, only what is set: u8 n + n x {u8 action, u16 masks, u8 parts} for the actions with a button mask or
// key / pad flags, then u8 n + n x {u8 action, f32} for the pad values and again for gyro, f32 mouse x, y, look scale,
// u8 wheel, f32 mouse sensitivity x 2, u8 mouse flags - ~30 bytes when idle (was 250: a long resend window must fit).
void Pack(std::vector<uint8_t>& d, const Input& in) {
    auto put = [&d](const void* p, size_t n) { d.insert(d.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
    size_t at = d.size();
    d.push_back(0);
    for (uint8_t a = 0; a < kActions; ++a)
        if (in.masks[a] || in.parts[a]) d.push_back(a), put(&in.masks[a], 2), d.push_back(in.parts[a]), ++d[at];
    for (const float* v : {in.pad, in.gyro}) {
        at = d.size();
        d.push_back(0);
        for (uint8_t a = 0; a < kActions; ++a)
            if (v[a] != 0.0f) d.push_back(a), put(&v[a], 4), ++d[at];
    }
    put(in.mouse, sizeof in.mouse);
    put(&in.lookScale, 4);
    d.push_back(in.wheel);
    put(in.mouseSens, sizeof in.mouseSens);
    d.push_back(in.mouseFlags);
    const uint16_t n = static_cast<uint16_t>(std::min<size_t>(in.events.size(), 0xFFFF));
    put(&n, 2);
    put(in.events.data(), n);
}

bool Unpack(const std::vector<uint8_t>& d, size_t& at, Input& in) {
    auto get = [&](void* p, size_t n) {
        if (at + n > d.size()) return false;
        memcpy(p, &d[at], n);
        at += n;
        return true;
    };
    uint8_t n;
    if (!get(&n, 1)) return false;
    for (int k = 0; k < n; ++k) {
        uint8_t a;
        if (!get(&a, 1) || a >= kActions || !get(&in.masks[a], 2) || !get(&in.parts[a], 1)) return false;
    }
    for (float* v : {in.pad, in.gyro}) {
        if (!get(&n, 1)) return false;
        for (int k = 0; k < n; ++k) {
            uint8_t a;
            float f;
            if (!get(&a, 1) || !get(&f, 4) || a >= kActions) return false;
            v[a] = f;
        }
    }
    uint16_t len;
    if (!get(in.mouse, sizeof in.mouse) || !get(&in.lookScale, 4) || !get(&in.wheel, 1) ||
        !get(in.mouseSens, sizeof in.mouseSens) || !get(&in.mouseFlags, 1) || !get(&len, 2) || at + len > d.size())
        return false;
    in.events.assign(reinterpret_cast<const char*>(d.data() + at), len);
    at += len;
    return true;
}

// Each PC tells the others which tick it needs next (its g_tick); this PC's inputs are kept from the lowest such tick on
// and every packet carries them from there (as many as fit). A 2-PC test over the internet ended in both PCs waiting:
// the joiner's packets with tick 21953 were lost, and as every packet only repeated the last 4 ticks it never came again.
uint32_t g_peerNeed[kMaxPlayers] = {};
uint32_t LowestNeed() {
    uint32_t need = g_tick;
    for (int p = 0; p < g_players; ++p)
        if (p != g_local && (g_senders >> p) & 1) need = std::min(need, g_peerNeed[p]);
    return need;
}

// packet: 'L' 11 (7 before the mission start was added), u8 player, u8 count, u32 hash tick (+1, 0 = none), u32 hash,
// u32 the tick this PC needs next, u32 mission start (seed), count x {u32 tick, packed Input}. Packets of another mission
// start are dropped: a joiner's tick 0 from a level it had loaded on its own let the host start while the joiner then
// reloaded the host's level and waited for good (2-home session 2026-10-22).
constexpr size_t kInputHeader = 20;
void Send() {
    std::vector<uint8_t> d(kInputHeader, 0);
    d[0] = 'L', d[1] = 11, d[2] = static_cast<uint8_t>(g_local);
    memcpy(&d[16], &g_seed, 4);
    uint32_t ht = 0, hv = 0;
    if (!g_hashes[g_local].empty()) ht = g_hashes[g_local].rbegin()->first + 1, hv = g_hashes[g_local].rbegin()->second;
    memcpy(&d[4], &ht, 4);
    memcpy(&d[8], &hv, 4);
    memcpy(&d[12], &g_tick, 4);
    const uint32_t last = g_tick + g_delay;
    for (uint32_t t = LowestNeed(); t <= last && d[3] < 250; ++t) {
        auto it = g_inputs[g_local].find(t);
        if (it == g_inputs[g_local].end()) continue;
        std::vector<uint8_t> one(4);
        memcpy(one.data(), &t, 4);
        Pack(one, it->second);
        if (d[3] && d.size() + one.size() > 1400) break;   // the receiver takes 1500 bytes; the next packets carry the rest
        d.insert(d.end(), one.begin(), one.end());
        ++d[3];
    }
    net::SendRaw(d.data(), static_cast<int>(d.size()));
}

void ReceiveCache(const std::vector<uint8_t>& d);

void Receive() {
    for (const net::Raw& r : net::TakeRaw()) {
        const auto& d = r.data;
        if (d.size() >= 8 && d[0] == 'L' && d[1] == puppetsync::kPacketType) {   // another PC's soldier records (authority.cpp)
            // 3-4 players: the joiners only talk to the host - it passes each joiner's records on to the others
            if (g_local == 0 && features::LobbyPcs() > 2 && r.from > 1) net::SendRaw(d.data(), static_cast<int>(d.size()));
            features::AuthorityReceive(d);
            continue;
        }
        if (d.size() >= 8 && d[0] == 'L' && d[1] == 8) {   // another PC's vehicle records (authority.cpp)
            if (g_local == 0 && features::LobbyPcs() > 2 && r.from > 1) net::SendRaw(d.data(), static_cast<int>(d.size()));
            features::AuthorityReceiveVehicles(d);
            continue;
        }
        if (d.size() >= 8 && d[0] == 'L' && d[1] == 4 && g_local != 0) {   // the host's state cache (joiners)
            ReceiveCache(d);
            continue;
        }
        if (d.size() < kInputHeader || d[0] != 'L' || d[1] != 11 || d[2] >= g_players || d[2] == g_local) continue;
        const int p = d[2];
        uint32_t ht, hv, need, seed;
        memcpy(&seed, &d[16], 4);
        if (seed != g_seed) {   // another mission start's
            static DWORD told = 0;
            if (GetTickCount() - told > 2000)
                told = GetTickCount(), OnlineLog("Online: inputs of another mission start (%08X, this one %08X) dropped", seed, g_seed);
            continue;
        }
        memcpy(&ht, &d[4], 4);
        memcpy(&hv, &d[8], 4);
        memcpy(&need, &d[12], 4);
        if (ht) g_hashes[p][ht - 1] = hv;
        g_peerNeed[p] = std::max(g_peerNeed[p], need);
        size_t at = kInputHeader;
        for (int k = 0; k < d[3] && at + 4 <= d.size(); ++k) {
            uint32_t t;
            memcpy(&t, &d[at], 4);
            at += 4;
            Input in;
            if (!Unpack(d, at, in)) break;
            if (t >= g_tick) g_inputs[p][t] = in;
        }
    }
}

// The objects hashed at a hash tick (id, position, value +0x54, vtable), kept for the last few - written to the log on a
// desync, so the PCs' logs show which objects differ.
struct ObjState {
    uint32_t id;
    float v[4];
    uint32_t vt;
};
std::map<uint32_t, std::vector<ObjState>> g_snapshots;

bool IsPlayerSoldier(const uint8_t* o) {
    for (int p = 0; p < g_players; ++p)
        if (*reinterpret_cast<const uint8_t* const*>(kBlocks + p * kBlockSize + 0x310) == o) return true;
    return false;
}

uint32_t WorldHash() {
    std::vector<ObjState>& snap = g_snapshots[g_tick];
    snap.clear();
    while (g_snapshots.size() > 4) g_snapshots.erase(g_snapshots.begin());
    uint32_t h = 2166136261u;
    auto fnv = [&h](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 16777619u;
    };
    const uint8_t* mgr = *reinterpret_cast<uint8_t* const*>(kObjects);
    if (!mgr) return h;
    const auto* table = *reinterpret_cast<const uint8_t* const* const*>(mgr + 0x30);
    const uint32_t maxId = *reinterpret_cast<const uint32_t*>(mgr + 0x34);
    for (uint32_t id = 0; table && id <= maxId && id < 4096; ++id)
        if (const uint8_t* o = table[id]) {
            // with the host running the other soldiers the joiners follow its records a little later: only the
            // players' own soldiers must be equal
            if (features::AuthorityOn() && (*reinterpret_cast<const uint32_t*>(o) != 0x5D88E0 ||
                                            features::AuthorityPuppet(o) || !IsPlayerSoldier(o)))
                continue;
            fnv(&id, 4);
            fnv(o + 0x30, 12);
            fnv(o + 0x54, 4);
            ObjState st{id, {}, *reinterpret_cast<const uint32_t*>(o)};
            memcpy(st.v, o + 0x30, 12);
            memcpy(&st.v[3], o + 0x54, 4);
            snap.push_back(st);
        }
    // each player's camera too: the aim follows it, and a player aiming elsewhere on another PC (2-PC test 2026-10-11:
    // Bradley aimed down on one screen) changed no position or health until someone was shot. Dump ids 10000 + view
    // (angles), 20000 + view (eye).
    // (cameras are each PC's own view when the host runs the other soldiers - camera collision with the puppets differs
    // by thousandths of a degree)
    if (const uint8_t* cams = *reinterpret_cast<const uint8_t* const*>(0x63C9D8); cams && !features::AuthorityOn())
        for (int v = 0; v < g_players; ++v) {
            const uint32_t id = *reinterpret_cast<const uint32_t*>(cams + 0xEC0C + v * 4);
            if (id >= 128) continue;
            const uint8_t* cam = cams + 8 + id * 0x1D8;
            fnv(cam + 0x50, 12);
            fnv(cam + 0x68, 12);
            ObjState a{10000u + v, {}, id}, e{20000u + v, {}, id};
            memcpy(a.v, cam + 0x50, 12);
            memcpy(e.v, cam + 0x68, 12);
            snap.push_back(a);
            snap.push_back(e);
        }
    const uint32_t rng = *reinterpret_cast<uint32_t*>(0x5FE850);
    if (!features::AuthorityOn()) fnv(&rng, 4);   // (the joiners don't run the AI: other draws)
    return h;
}

void CheckHashes() {
    for (auto it = g_hashes[g_local].begin(); it != g_hashes[g_local].end();) {
        bool all = true, same = true;
        for (int p = 0; p < g_players; ++p) {
            if (p == g_local) continue;
            auto o = g_hashes[p].find(it->first);
            if (o == g_hashes[p].end()) all = false;
            else if (o->second != it->second) same = false;
        }
        if (!all) {
            ++it;
            continue;
        }
        static int matched = 0;
        if (same && ++matched % 10 == 0) dslog::Write("Lockstep: tick %u in step with every player (%08X)", it->first, it->second);
        // With the host in charge of the other soldiers (authority.cpp) a player's soldier can differ for a moment -
        // a hit lands here a few ticks after the host - and is equal again by the next check. Only a difference that
        // lasts kPersistChecks checks is a desync; the brief ones are counted.
        const int persist = features::AuthorityOn() ? 3 : 1;
        if (same) {
            if (g_badChecks > 0 && g_badChecks < persist) ++g_briefDiffs;
            g_badChecks = 0;
        } else ++g_badChecks;
        const bool real = !same && g_badChecks >= persist;
        if (same && g_outOfStep) {
            g_outOfStep = false;
    g_badChecks = g_briefDiffs = 0;
            OnlineLog("Lockstep: back in step at tick %u (%08X)", it->first, it->second);
        }
        if (real && !g_outOfStep) {
            g_outOfStep = true;
            if (g_desync) OnlineLog("Lockstep: out of step again at tick %u", it->first);
        }
        if (real && !g_desync) {
            g_desync = true;
            OnlineLog("Lockstep: DESYNC at tick %u (world hash %08X differs)", it->first, it->second);
            Popup("Online: the games got out of step - the host's state is applied", 5000);
            const uint32_t rng = *reinterpret_cast<uint32_t*>(0x5FE850);
            uint16_t cw;
            __asm fnstcw cw
            OnlineLog("Lockstep: tick %u objects (rng now %08X, tables %u %u, x87 control word %04X):", it->first, rng,
                         *reinterpret_cast<uint32_t*>(0x61DFE4), *reinterpret_cast<uint32_t*>(0x61DFE8), cw);
            for (const ObjState& o : g_snapshots[it->first])
                OnlineLog("  obj %u %.4f %.4f %.4f %.4f %08X", o.id, o.v[0], o.v[1], o.v[2], o.v[3], o.vt);
        }
        for (int p = 0; p < g_players; ++p) g_hashes[p].erase(it->first);
        it = g_hashes[g_local].begin();
    }
}

// ---- host state cache (Pivotal's ParamCache) ----
// Conflict: Denied Ops, Global Storm and this game's own DirectPlay versus all keep the host authoritative: it sends a
// cache of the soldiers' state keyed by an id every machine shares (DO / GS: the scene script id; versus message 0x11:
// position, heading, stance, health, full refresh every 500 ms) and the other machines take it. Here every PC runs the
// same ticks, so the cache is compared exactly instead of interpolated: every kCacheEvery ticks each PC records its
// soldiers (vtable 0x5D88E0: position +0x30, health +0x54) for that tick; the host sends its record and each joiner
// compares it with its own record of the same tick - a difference is added to the soldier's current state (all of its
// position copies - the ones the ladder probe found - and the render instance's). In step nothing changes; out of
// step (a desync source not found yet) the soldiers follow the host instead of drifting apart.
// Packet: 'L' 4, u8 part, u8 parts, u32 tick, n x {u16 id, f32 x, y, z, health}.
constexpr int kCacheEvery = 30, kCacheKeep = 16, kCacheResends = 3;
constexpr uint32_t kSoldierVtable = 0x5D88E0;
constexpr uint32_t kPosCopies[] = {0x30, 0xA0, 0x1FC, 0x224, 0x240, 0x368, 0x384, 0x390};
constexpr uint32_t kInstancePosCopies[] = {0xF4, 0x13C};

#pragma pack(push, 1)
struct SoldierRec {
    uint16_t id;
    float pos[3];
    float health;
};
#pragma pack(pop)
std::map<uint32_t, std::vector<SoldierRec>> g_ownCache;    // tick -> this PC's record
std::map<uint32_t, std::vector<SoldierRec>> g_hostCache;   // tick -> the host's (joiners), parts merged
std::map<uint32_t, std::pair<int, int>> g_hostParts;       // tick -> (parts received bit mask, parts)
std::vector<std::vector<uint8_t>> g_cacheOut;              // host: the last record's packets, resent
int g_cacheResend = 0;
uint32_t g_corrections = 0, g_lastApplied = 0;
bool g_cacheApplied = false;

uint8_t** ObjectTable(uint32_t& maxId) {
    const uint8_t* mgr = *reinterpret_cast<uint8_t* const*>(kObjects);
    if (!mgr) return nullptr;
    maxId = *reinterpret_cast<const uint32_t*>(mgr + 0x34);
    return *reinterpret_cast<uint8_t** const*>(mgr + 0x30);
}

std::vector<SoldierRec> RecordSoldiers() {
    std::vector<SoldierRec> out;
    uint32_t maxId = 0;
    uint8_t** table = ObjectTable(maxId);
    for (uint32_t id = 0; table && id <= maxId && id < 0xFFFF; ++id)
        if (const uint8_t* o = table[id]; o && *reinterpret_cast<const uint32_t*>(o) == kSoldierVtable &&
                                         (!features::AuthorityOn() || IsPlayerSoldier(o))) {
            SoldierRec r{static_cast<uint16_t>(id)};
            memcpy(r.pos, o + 0x30, 12);
            memcpy(&r.health, o + 0x54, 4);
            out.push_back(r);
        }
    return out;
}

void SendCache(uint32_t tick, const std::vector<SoldierRec>& recs) {
    constexpr size_t kPerPacket = 50;   // 50 x 18 + 8 = 908 bytes
    const size_t parts = recs.empty() ? 1 : (recs.size() + kPerPacket - 1) / kPerPacket;
    g_cacheOut.clear();
    for (size_t k = 0; k < parts; ++k) {
        std::vector<uint8_t> d = {'L', 4, static_cast<uint8_t>(k), static_cast<uint8_t>(parts), 0, 0, 0, 0};
        memcpy(&d[4], &tick, 4);
        const size_t from = k * kPerPacket, to = std::min(recs.size(), from + kPerPacket);
        const auto* b = reinterpret_cast<const uint8_t*>(recs.data() + from);
        d.insert(d.end(), b, b + (to - from) * sizeof(SoldierRec));
        g_cacheOut.push_back(d);
    }
    for (const auto& d : g_cacheOut) net::SendRaw(d.data(), static_cast<int>(d.size()));
    g_cacheResend = kCacheResends;   // unreliable: sent again on later ticks (the next record replaces it anyway)
}

void ReceiveCache(const std::vector<uint8_t>& d) {
    const int part = d[2], parts = d[3];
    uint32_t tick;
    memcpy(&tick, &d[4], 4);
    if (parts < 1 || parts > 16 || part >= parts || (d.size() - 8) % sizeof(SoldierRec)) return;
    if (tick + kCacheEvery * kCacheKeep < g_tick) return;   // too old to compare
    if (g_cacheApplied && tick <= g_lastApplied) return;    // a resend of a record already compared
    auto& got = g_hostParts[tick];
    got.second = parts;
    if (got.first & (1 << part)) return;   // a resend
    got.first |= 1 << part;
    const auto* r = reinterpret_cast<const SoldierRec*>(&d[8]);
    auto& recs = g_hostCache[tick];
    recs.insert(recs.end(), r, r + (d.size() - 8) / sizeof(SoldierRec));
}

// Joiners: every host record that is complete and has this PC's record of the same tick is compared and applied.
void ApplyHostCache() {
    for (auto it = g_hostCache.begin(); it != g_hostCache.end();) {
        const uint32_t tick = it->first;
        auto own = g_ownCache.find(tick);
        const auto parts = g_hostParts[tick];
        const bool complete = parts.second && parts.first == (1 << parts.second) - 1;
        if (own == g_ownCache.end() || !complete) {
            if (tick + kCacheEvery * kCacheKeep < g_tick) {   // never completed / this PC's record is gone
                g_hostParts.erase(tick);
                it = g_hostCache.erase(it);
            } else ++it;
            continue;
        }
        std::map<uint16_t, const SoldierRec*> mine;
        for (const SoldierRec& r : own->second) mine[r.id] = &r;
        uint32_t maxId = 0;
        uint8_t** table = ObjectTable(maxId);
        int fixed = 0, worstId = -1;
        float worst = 0.0f, worstHealth = 0.0f;
        bool worstPlayer = false;
        for (const SoldierRec& h : it->second) {
            auto m = mine.find(h.id);
            if (m == mine.end() || !table || h.id > maxId) continue;
            uint8_t* o = table[h.id];
            if (!o || *reinterpret_cast<uint32_t*>(o) != kSoldierVtable) continue;
            const float dp[3] = {h.pos[0] - m->second->pos[0], h.pos[1] - m->second->pos[1], h.pos[2] - m->second->pos[2]};
            const float dh = h.health - m->second->health;
            if (!dp[0] && !dp[1] && !dp[2] && !dh) continue;
            for (uint32_t off : kPosCopies)
                for (int k = 0; k < 3; ++k) reinterpret_cast<float*>(o + off)[k] += dp[k];
            if (uint8_t* inst = *reinterpret_cast<uint8_t**>(o + 0x2C))
                for (uint32_t off : kInstancePosCopies)
                    for (int k = 0; k < 3; ++k) reinterpret_cast<float*>(inst + off)[k] += dp[k];
            *reinterpret_cast<float*>(o + 0x54) += dh;
            const float d = sqrtf(dp[0] * dp[0] + dp[1] * dp[1] + dp[2] * dp[2]);
            if (worstId < 0 || d > worst || (d == worst && dh != 0.0f))
                worstId = h.id, worstHealth = dh, worstPlayer = IsPlayerSoldier(o);
            worst = std::max(worst, d);
            ++fixed;
        }
        if (fixed) {
            g_corrections += fixed;
            OnlineLog("Lockstep: host state of tick %u applied at tick %u - %d soldier(s) corrected (up to %.1f cm; "
                      "soldier %d%s, health %+.1f)", tick, g_tick, fixed, worst, worstId, worstPlayer ? " = a player's" : "",
                      worstHealth);
        }
        g_cacheApplied = true;
        g_lastApplied = std::max(g_lastApplied, tick);
        g_hostParts.erase(tick);
        it = g_hostCache.erase(it);
    }
    while (g_ownCache.size() > static_cast<size_t>(kCacheKeep)) g_ownCache.erase(g_ownCache.begin());
}

// Every tick, after the inputs (the same point of the frame on every PC).
void StateCache() {
    if (g_tick % kCacheEvery == 0) {
        g_ownCache[g_tick] = RecordSoldiers();
        if (g_local == 0) SendCache(g_tick, g_ownCache[g_tick]);
    } else if (g_local == 0 && g_cacheResend > 0 && g_tick % 4 == 0) {
        --g_cacheResend;
        for (const auto& d : g_cacheOut) net::SendRaw(d.data(), static_cast<int>(d.size()));
    }
    if (g_local != 0) ApplyHostCache();
#ifndef DS_DIST
    // Dev\LockstepNudge = tick: a joiner moves player 1's soldier 1 m at that tick (a forced desync) - the host's
    // cache should pull it back.
    static DWORD nudge = 0xFFFFFFFF;
    if (nudge == 0xFFFFFFFF) {
        DWORD v = 0, size = sizeof v;
        RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "LockstepNudge", RRF_RT_REG_DWORD, nullptr, &v,
                     &size);
        nudge = v;
    }
    if (g_local != 0 && nudge && g_tick == nudge)
        if (uint8_t* o = *reinterpret_cast<uint8_t**>(kBlocks + 0x310)) {
            for (uint32_t off : kPosCopies) reinterpret_cast<float*>(o + off)[0] += 100.0f;
            if (uint8_t* inst = *reinterpret_cast<uint8_t**>(o + 0x2C))
                for (uint32_t off : kInstancePosCopies) reinterpret_cast<float*>(inst + off)[0] += 100.0f;
            dslog::Write("[dev]  Lockstep: player 1's soldier nudged 1 m at tick %u (forced desync)", g_tick);
        }
#endif
}

#ifndef DS_DIST
// Dev\LockstepWatch = first tick: a hardware write watchpoint (DR0, 4 bytes) on view 0's camera eye x (+0x68) from that
// tick for 3 ticks; each write logs the writing instruction and the code addresses found on the stack ("#w"), so the
// PCs' logs show which code moved the camera on one of them. Set from a helper thread (a thread can't change its own
// debug registers through SetThreadContext reliably).
DWORD g_watchFrom = 0xFFFFFFFF;
volatile uintptr_t g_watchAddr = 0;
volatile LONG g_watchHits = 0;
HANDLE g_gameThreadHandle = nullptr;

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    e->ContextRecord->Dr6 = 0;
    if (InterlockedIncrement(&g_watchHits) <= 40) {
        char line[400];
        int n = snprintf(line, sizeof line, "#w %u eip %08X val %.2f stack", g_tick, e->ContextRecord->Eip,
                         *reinterpret_cast<const float*>(g_watchAddr));
        const uint32_t* sp = reinterpret_cast<const uint32_t*>(e->ContextRecord->Esp);
        int found = 0;
        for (int i = 0; i < 400 && found < 10; ++i) {
            uint32_t v;
            __try { v = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            if (v > 0x401000 && v < 0x5D7000) {
                n += snprintf(line + n, sizeof line - n, " %08X", v);
                ++found;
            }
        }
        dslog::Write("%s", line);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct WatchJob { uintptr_t addr; bool on; };
DWORD WINAPI WatchThread(void* p) {
    const WatchJob job = *static_cast<WatchJob*>(p);
    delete static_cast<WatchJob*>(p);
    SuspendThread(g_gameThreadHandle);
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(g_gameThreadHandle, &c);
    if (job.on) {
        c.Dr0 = job.addr;
        c.Dr7 = (c.Dr7 & ~0x000F0003u) | 1 | (1u << 16) | (3u << 18);   // L0, write, 4 bytes
    } else c.Dr7 &= ~0x000F0003u;
    SetThreadContext(g_gameThreadHandle, &c);
    ResumeThread(g_gameThreadHandle);
    return 0;
}

void SetWatch(uintptr_t addr, bool on) {
    if (!g_gameThreadHandle)
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_gameThreadHandle,
                        THREAD_ALL_ACCESS, FALSE, 0);
    static bool veh = false;
    if (!veh) veh = AddVectoredExceptionHandler(1, WatchHandler) != nullptr;
    g_watchAddr = addr;
    HANDLE t = CreateThread(nullptr, 0, WatchThread, new WatchJob{addr, on}, 0, nullptr);
    WaitForSingleObject(t, 2000);   // the game thread is suspended meanwhile - returns once it is resumed
    CloseHandle(t);
}

void WatchTick() {
    if (g_watchFrom == 0xFFFFFFFF) {
        DWORD v = 0, size = sizeof v;
        RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "LockstepWatch", RRF_RT_REG_DWORD, nullptr, &v, &size);
        g_watchFrom = v ? v : 0xFFFFFFFE;
    }
    if (g_watchFrom >= 0xFFFFFFFE) return;
    const uint8_t* cams = *reinterpret_cast<const uint8_t* const*>(0x63C9D8);
    if (!cams) return;
    if (g_tick == g_watchFrom) {
        DWORD addr = 0, size = sizeof addr;   // Dev\LockstepWatchAddr: watch this address instead of the camera
        RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "LockstepWatchAddr", RRF_RT_REG_DWORD, nullptr, &addr, &size);
        const uint32_t id = *reinterpret_cast<const uint32_t*>(cams + 0xEC0C);
        if (addr) {
            SetWatch(addr, true);
            dslog::Write("#w watching %08X from tick %u", addr, g_tick);
        } else if (id < 128) {
            SetWatch(reinterpret_cast<uintptr_t>(cams + 8 + id * 0x1D8 + 0x68), true);
            dslog::Write("#w watching view 0 camera %u eye from tick %u", id, g_tick);
        }
    } else if (g_tick == g_watchFrom + 5)
        SetWatch(0, false);
}
#endif

#ifndef DS_DIST
// Dev\LockstepProbe = first tick: for 40 ticks the camera placement FUN_00473150's world queries are logged ("#q"): ray
// FUN_004de610 (thiscall, 6 args), sweep FUN_004e08e0 (4), zone lookup FUN_004e2080 (3) - the arguments (a pointer as
// the 3 floats it points to, before and after the call) and the result, to find which one answers differently.
DWORD g_probeFrom = 0xFFFFFFFF;
uint32_t g_probeSite = 0;
bool ProbeOn() {
    if (g_probeFrom == 0xFFFFFFFF) {
        DWORD v = 0, size = sizeof v;
        RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "LockstepProbe", RRF_RT_REG_DWORD, nullptr, &v, &size);
        g_probeFrom = v ? v : 0xFFFFFFFE;
    }
    return g_running && g_probeFrom < 0xFFFFFFFE && g_tick >= g_probeFrom && g_tick < g_probeFrom + 40;
}
int ArgText(char* out, size_t size, uint32_t v) {
    float f[3];
    bool ok = v >= 0x10000 && v < 0x80000000;
    if (ok) {
        __try { memcpy(f, reinterpret_cast<const void*>(v), 12); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    }
    if (ok && (f[0] == f[0]) && fabsf(f[0]) < 1e7f && fabsf(f[1]) < 1e7f && fabsf(f[2]) < 1e7f)
        return snprintf(out, size, " [%.3f %.3f %.3f]", f[0], f[1], f[2]);
    if (v >= 0x10000 && v < 0x80000000) return snprintf(out, size, " p");   // a pointer to something else
    return snprintf(out, size, " %X", v);
}
void ProbeLog(const char* what, uint32_t site, const uint32_t* args, int n, int result, bool after) {
    char line[700];
    int m = snprintf(line, sizeof line, "#q %u %08X %s %s", g_tick, site, what, after ? "out" : "in");
    for (int i = 0; i < n; ++i) m += ArgText(line + m, sizeof line - m, args[i]);
    if (after) m += snprintf(line + m, sizeof line - m, " = %X", result);
    dslog::Write("%s", line);
}
using Ray6 = int(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
using Sweep4 = int(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
using Zone3 = int(__thiscall*)(void*, uint32_t, uint32_t, uint32_t);
int __fastcall ProbeRay(void* self, void*, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
    const uint32_t args[] = {a, b, c, d, e, f};
    const bool on = ProbeOn();
    const uint32_t site = reinterpret_cast<uint32_t>(_ReturnAddress()) - 5;
    if (on) ProbeLog("ray", site, args, 6, 0, false);
    const int r = reinterpret_cast<Ray6>(0x4DE610)(self, a, b, c, d, e, f);
    if (on) ProbeLog("ray", site, args, 6, r, true);
    return r;
}
int __fastcall ProbeSweep(void* self, void*, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    const uint32_t args[] = {a, b, c, d};
    const bool on = ProbeOn();
    const uint32_t site = reinterpret_cast<uint32_t>(_ReturnAddress()) - 5;
    if (on) ProbeLog("sweep", site, args, 4, 0, false);
    const int r = reinterpret_cast<Sweep4>(0x4E08E0)(self, a, b, c, d);
    if (on) ProbeLog("sweep", site, args, 4, r, true);
    return r;
}
int __fastcall ProbeZone(void* self, void*, uint32_t a, uint32_t b, uint32_t c) {
    const uint32_t args[] = {a, b, c};
    const bool on = ProbeOn();
    const uint32_t site = reinterpret_cast<uint32_t>(_ReturnAddress()) - 5;
    if (on) ProbeLog("zone", site, args, 3, 0, false);
    const int r = reinterpret_cast<Zone3>(0x4E2080)(self, a, b, c);
    if (on) ProbeLog("zone", site, args, 3, r ? 1 : 0, true);
    return r;
}
void InstallProbe() {
    ProbeOn();
    if (g_probeFrom >= 0xFFFFFFFE) return;
    for (uint32_t s : {0x473319u, 0x473488u, 0x473650u}) patch::HookCall(s, reinterpret_cast<const void*>(&ProbeRay), 0x4DE610);
    patch::HookCall(0x473762, reinterpret_cast<const void*>(&ProbeSweep), 0x4E08E0);
    for (uint32_t s : {0x4733A5u, 0x473592u, 0x4736F0u, 0x4737C5u})
        patch::HookCall(s, reinterpret_cast<const void*>(&ProbeZone), 0x4E2080);
    dslog::Write("[dev]  Lockstep: camera placement queries probed from tick %u", g_probeFrom);
}
#endif

// DRAW DISTANCE (launcher, renderer +0x40A9C) sets the fog / view range the AI uses too: a 2-PC test whose PCs had
// different values split in every run. During an online mission every PC uses the host's (START), its own afterwards.
int g_ownDrawFar = -1;
void ApplyHostDrawDistance(bool on) {
    uint8_t* renderer = *reinterpret_cast<uint8_t**>(0x63C924);
    if (!renderer) return;
    if (on) {
        const features::HostSettings* h = features::HostSync();
        if (!h || h->drawFar < 0) return;
        if (g_ownDrawFar < 0) g_ownDrawFar = renderer[0x40A9C];
        renderer[0x40A9C] = static_cast<uint8_t>(h->drawFar != 0);
        OnlineLog("Lockstep: draw distance %s (host's), own %s", h->drawFar ? "far" : "near", g_ownDrawFar ? "far" : "near");
    } else if (g_ownDrawFar >= 0) {
        renderer[0x40A9C] = static_cast<uint8_t>(g_ownDrawFar);
        g_ownDrawFar = -1;
    }
}

// Skeleton poses: FUN_00502670 (thiscall render instance) computes an instance's bone transforms from its animation and
// sets +0x220 = 1 ("pose valid"; the animation / transform code clears it). The renderer computes it for the soldiers
// it draws (FUN_004d0090 -> 0x5024B0), so with another screen size / aspect a soldier's bones - gun muzzle, head, which
// the AI aims and fires from - could be fresh on one PC and stale on the other (2-PC test with different resolutions:
// an enemy fired one tick earlier on one PC). Every tick, before the update, every soldier's pose is computed anew on
// every PC - for soldiers and weapons (the renderer also picks a level of detail by camera distance, instance +0x1C4,
// for the ones it draws; calling it for every scene object hung the first tick).
void RefreshPoses() {
    uint32_t maxId = 0;
    uint8_t** table = ObjectTable(maxId);
    for (uint32_t id = 0; table && id <= maxId && id < 4096; ++id)
        if (uint8_t* o = table[id]; o && *reinterpret_cast<uint32_t*>(o) == kSoldierVtable)   // (weapons too hung the game)
            if (uint8_t* inst = *reinterpret_cast<uint8_t**>(o + 0x2C); inst && *reinterpret_cast<void**>(inst + 0xC8)) {
                *reinterpret_cast<uint32_t*>(inst + 0x220) = 0;
                reinterpret_cast<void(__thiscall*)(void*)>(0x502670)(inst);
            }
}

int OwnWidth() {
    const uint8_t* r = *reinterpret_cast<uint8_t* const*>(0x63C924);
    return r ? *reinterpret_cast<const int*>(r + 0x40688) : 0;
}
int OwnHeight() {
    const uint8_t* r = *reinterpret_cast<uint8_t* const*>(0x63C924);
    return r ? *reinterpret_cast<const int*>(r + 0x4068C) : 0;
}

void Arm(const char* level) {
    if (!Install()) return;
    g_gameThread = GetCurrentThreadId();
    g_armed = true;
    g_running = false;
    g_desync = false;
    g_tick = 0;
    g_virtual = kVirtualStart;
    g_players = features::CoopPlayers();
    g_localMask = features::LobbyLocalMask();
    g_local = features::LobbyFirstLocal();
    g_senders = features::LobbySenderMask(g_players);
    for (auto& m : g_inputs) m.clear();
    g_due = 0;
    for (auto& m : g_hashes) m.clear();
    for (uint32_t& n : g_peerNeed) n = 0;
    g_ownCache.clear();
    g_hostCache.clear();
    g_hostParts.clear();
    g_cacheOut.clear();
    g_cacheResend = 0;
    g_corrections = 0;
    g_lastApplied = 0;
    g_cacheApplied = false;
    g_outOfStep = false;
    g_briefingClosed = g_briefingMessage = false;
    if (net::LocalPlayer() == 1) {   // the host picks (and its own settings count); joiners got them with START
        g_seed = GetTickCount() ^ 0x5EED5EED;
        g_hostSkipCutscenes = settings::Get().skipCutscenes ? 1 : 0;
    }
    reinterpret_cast<void(__cdecl*)(uint32_t)>(kReseed)(g_seed);
    features::ScriptReseed(g_seed);   // a Lua mission's math.random too
    for (uint32_t g : {kTimerA, kTimerB})
        if (uint8_t* t = *reinterpret_cast<uint8_t**>(g); t && !g_ownerMode) {
            *reinterpret_cast<uint32_t*>(t + 0xD4) = g_virtual;
            *reinterpret_cast<uint32_t*>(t + 0x10) = 0;
            *reinterpret_cast<uint32_t*>(t + 0xC8) = 0;
        }
    KeepRunning(true);
    // A level loaded from a save (RESTART LEVEL after MISSION FAILED restores the mission's start state the same way;
    // LOAD GAME): level init 0x40CB93 / 0x40CBA6 restore it when the saver [0x60EDC4] has +0x94 / +0xA4 set. Only this
    // PC has that save - the host's restart skipped the intro while the joiner started the level fresh and played it
    // (2-PC test 2026-10-15; the joiner's audio was gone after it). Online every PC starts the mission fresh.
    if (g_ownerMode)
        if (auto* saver = *reinterpret_cast<uint8_t**>(0x60EDC4);
            saver && (*reinterpret_cast<uint32_t*>(saver + 0x94) || *reinterpret_cast<uint32_t*>(saver + 0xA4))) {
            *reinterpret_cast<uint32_t*>(saver + 0x94) = 0;
            *reinterpret_cast<uint32_t*>(saver + 0xA4) = 0;
            OnlineLog("Online: the level was to be restored from a save - started fresh (the other PCs have no copy)");
        }
    // ... and the restart flag that goes with it: MISSION FAILED > RESTART sets campaign +0x13CC (0x414E7E), which API
    // +0x3F4 reports as "loading a save" - the host's mission then skipped its intro and start-up while the joiners, who
    // load the level fresh from the host's START, played the intro (user, 2026-10-16). Online every PC starts fresh.
    if (g_ownerMode)
        if (auto* campaign = *reinterpret_cast<uint8_t**>(0x60EDC0); campaign && *reinterpret_cast<uint32_t*>(campaign + 0x13CC)) {
            *reinterpret_cast<uint32_t*>(campaign + 0x13CC) = 0;
            OnlineLog("Online: restart - the mission starts fresh on every PC (intro included)");
        }
    if (net::LocalPlayer() == 1) features::ApplyHostSyncToken(features::HostSyncToken().c_str(), true);   // (see HostSyncToken)
    ApplyHostDrawDistance(true);
    {
        const features::HostSettings* h = features::HostSync();
        g_delay = h && h->inputDelay > 0 ? h->inputDelay : kDefaultDelay;
        if (h) OnlineLog("Lockstep: host's settings: draw distance %s, input delay %d ticks (ping %d ms), mods %08X, "
                         "this PC: %dx%d, mods %08X", h->drawFar < 0 ? "?" : h->drawFar ? "far" : "near", g_delay,
                         h->pingMs, h->modsHash, OwnWidth(), OwnHeight(), features::OwnModsHash());
    }
    if (!g_watchdogThread) CloseHandle(CreateThread(nullptr, 0, Watchdog, nullptr, 0, &g_watchdogThread));
    features::AuthorityStart(net::LocalPlayer() == 1, g_players);
    OnlineLog("Lockstep: %s - player %d of %d, seed %08X", level, g_local + 1, g_players, g_seed);
}

void Stop(const char* why) {
    if (!g_armed) return;
    OnlineLog("Lockstep: stopped (%s) at tick %u, %d brief difference(s)", why, g_tick, g_briefDiffs);
    g_armed = g_running = false;
    ApplyHostDrawDistance(false);
    features::AuthorityStop();
    g_blockRawKeys = 0;
    g_lastTickDone = 0;
    g_inInputWait = false;
    KeepRunning(net::LocalPlayer() > 0 || settings::Get().featRunInBackground);
}

// A player left during the mission: it can't go on - his soldier would be driven by nobody, or by this PC's player
// (2-PC test 2026-10-11: the host quit and player 2 controlled both). Every remaining PC goes back to the main menu.
void EndMission(const char* why) {
    if (!g_armed) return;
    const bool inMission = g_running;
    Popup(why, 8000);
    Stop(why);
    if (inMission) features::ConsoleStartLevel("FrontEnd.dll");
}

bool PlayerGone() {   // the host (joiner's view) or a joiner (host's view) is no longer connected
    const net::Snapshot s = net::Snap();
    if (s.role == net::Snapshot::Role::None) return true;
    for (const auto& p : s.peers)
        if (!p.connected) return true;
    return false;
}

#ifndef DS_DIST
// Dev\LockstepTickLog = n: for the first n ticks, one line per tick - FNV of each player's applied input, each view's
// camera (whole 0x1D8-byte struct + id, angles, eye) and each player object's mode (+8). Diff the PCs' logs to find the
// first tick where they differ and whether the inputs did.
uint32_t Fnv(const void* p, size_t n, uint32_t h = 2166136261u) {
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 16777619u;
    return h;
}
void TickLog() {
    static DWORD limit = 0xFFFFFFFF;
    if (limit == 0xFFFFFFFF) {
        DWORD v = 0, size = sizeof v;
        RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "LockstepTickLog", RRF_RT_REG_DWORD, nullptr, &v, &size);
        limit = v;
    }
    if (g_tick >= limit) return;
    char line[3000];
    int n = snprintf(line, sizeof line, "#t %u", g_tick);
    for (int p = 0; p < g_players; ++p) {
        std::vector<uint8_t> d;
        Pack(d, g_current[p]);
        n += snprintf(line + n, sizeof line - n, " in%d %08X", p, Fnv(d.data(), d.size()));
    }
    if (const uint8_t* cams = *reinterpret_cast<const uint8_t* const*>(0x63C9D8))
        for (int v = 0; v < g_players; ++v) {
            const uint32_t id = *reinterpret_cast<const uint32_t*>(cams + 0xEC0C + v * 4);
            if (id >= 128) continue;
            const uint8_t* cam = cams + 8 + id * 0x1D8;
            const float* a = reinterpret_cast<const float*>(cam + 0x50);
            const float* e = reinterpret_cast<const float*>(cam + 0x68);
            n += snprintf(line + n, sizeof line - n, " cam%d id%u %08X %.3f %.3f %.1f %.1f %.1f", v, id, Fnv(cam, 0x1D8),
                          a[0], a[1], e[0], e[1], e[2]);
        }
    for (int p = 0; p < g_players; ++p)
        if (const uint8_t* pl = reinterpret_cast<const uint8_t* const*>(0x606A68)[p])
            n += snprintf(line + n, sizeof line - n, " pl%d %u", p, *reinterpret_cast<const uint32_t*>(pl + 8));
    // every scene object's position + value +0x54 (as the world hash), and the boids (vtable 0x5D872C) alone
    uint32_t world = 2166136261u, boids = 2166136261u;
    if (const uint8_t* mgr = *reinterpret_cast<uint8_t* const*>(kObjects)) {
        const auto* table = *reinterpret_cast<const uint8_t* const* const*>(mgr + 0x30);
        const uint32_t maxId = *reinterpret_cast<const uint32_t*>(mgr + 0x34);
        for (uint32_t id = 0; table && id <= maxId && id < 4096; ++id)
            if (const uint8_t* o = table[id]) {
                world = Fnv(o + 0x30, 12, Fnv(o + 0x54, 4, world));
                if (*reinterpret_cast<const uint32_t*>(o) == 0x5D872C) boids = Fnv(o + 0x30, 12, Fnv(o + 0x54, 4, boids));
            }
    }
    const uint8_t* atmo = *reinterpret_cast<uint8_t* const*>(0x63C948);
    n += snprintf(line + n, sizeof line - n, " fe %X/%u/%u/%u cine %X msg %d", *reinterpret_cast<uint32_t*>(kFrontEndState),
                  *reinterpret_cast<uint32_t*>(kStateBusy), *reinterpret_cast<uint8_t*>(0x617C32),
                  *reinterpret_cast<uint8_t*>(0x617C33), atmo ? *reinterpret_cast<const uint32_t*>(atmo + 0x1B8C) : 0,
                  g_briefingMessage ? 1 : 0);
    if (const uint8_t* smgr = *reinterpret_cast<uint8_t* const*>(0x63C9AC))
        if (const uint8_t* sb = *reinterpret_cast<const uint8_t* const*>(smgr + 0x34))
            n += snprintf(line + n, sizeof line - n, " snd %u/%u/%u stream %u", g_soundAsked, g_soundYes, g_streamYes,
                          sb[0x3A8] & 1);
    g_soundAsked = g_soundYes = g_streamYes = 0;
    n += snprintf(line + n, sizeof line - n, " render %u fe %d", features::RenderCount(), features::LastFrontEndResult());
    n += snprintf(line + n, sizeof line - n, " tbl");
    for (auto& [site, count] : g_tblCount)
        if (count && n < static_cast<int>(sizeof line) - 40) n += snprintf(line + n, sizeof line - n, " %X:%u", site, count);
    g_tblCount.clear();
    n += snprintf(line + n, sizeof line - n, " crt");
    for (size_t i = 0; i < sizeof kCrtSites / sizeof kCrtSites[0]; ++i)
        if (g_crtCount[i]) n += snprintf(line + n, sizeof line - n, " %X:%u", kCrtSites[i], g_crtCount[i]), g_crtCount[i] = 0;
    n += snprintf(line + n, sizeof line - n, " world %08X boids %08X rng %08X %u %u", world, boids,
                  *reinterpret_cast<uint32_t*>(0x5FE850), *reinterpret_cast<uint32_t*>(0x61DFE4),
                  *reinterpret_cast<uint32_t*>(0x61DFE8));
    dslog::Write("%s", line);
    if (g_tick >= 1 && g_tick <= 45) {   // dev: raw state of players 1 and 2 for an offline diff (lsdump-<player>.bin)
        static FILE* dump = nullptr;
        if (g_tick == 1) {
            if (dump) fclose(dump);
            char path[MAX_PATH];
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            *strrchr(path, '\\') = 0;
            char name[MAX_PATH + 40];
            snprintf(name, sizeof name, "%s\\DesertStormFix-lsdump.bin", path);
            fopen_s(&dump, name, "wb");
        }
        if (dump) {
            for (int p = 0; p < 2; ++p) {
                auto* block = reinterpret_cast<const uint8_t*>(kBlocks + p * kBlockSize);
                const uint8_t* sol = *reinterpret_cast<const uint8_t* const*>(block + 0x310);
                const uint8_t* inst = sol ? *reinterpret_cast<const uint8_t* const*>(sol + 0x2C) : nullptr;
                static uint8_t zero[0x2B10] = {};
                fwrite(&g_tick, 4, 1, dump);
                fwrite(block, kBlockSize, 1, dump);
                fwrite(sol ? sol : zero, 0x2B10, 1, dump);
                fwrite(inst ? inst : zero, 0x2C8, 1, dump);
            }
            fflush(dump);
        }
    }
    if (g_tick >= 28 && g_tick <= 45)   // around the briefing's end: view 0's whole camera + the skip flags
        if (const uint8_t* cams = *reinterpret_cast<const uint8_t* const*>(0x63C9D8)) {
            const uint32_t id = *reinterpret_cast<const uint32_t*>(cams + 0xEC0C);
            dslog::Write("#c %u skip %d %d done %u mgr %08X %08X", g_tick, features::OverviewSkipPending() ? 1 : 0,
                         features::LockstepSkipCutscenes(false) ? 1 : 0, *reinterpret_cast<uint32_t*>(0x618348),
                         *reinterpret_cast<const uint32_t*>(cams + 0xEC34), *reinterpret_cast<const uint32_t*>(cams + 0xEC2C));
            if (id < 128)
                for (int k = 0; k < 0x1D8; k += 64) {
                    char hex[160];
                    int m = 0;
                    for (int j = k; j < k + 64 && j < 0x1D8; j += 4)
                        m += snprintf(hex + m, sizeof hex - m, "%08X ", *reinterpret_cast<const uint32_t*>(cams + 8 + id * 0x1D8 + j));
                    dslog::Write("#c %u +%03X %s", g_tick, k, hex);
                }
        }
}
#endif

// Owner mode: every PC waits at the mission's first frame until all have loaded. Without it each PC began the mission
// (its intro cutscene, the AI) the moment its own load ended - the host's cutscene ran while the joiner was still
// loading (user, 2026-10-16). The tick-0 input packets are the signal: joiners send theirs and wait for the host's; the
// host waits for every joiner's, then sends its own (joiners talk only to the host). Afterwards a PC keeps resending
// while another hasn't reported passing it (LowestNeed() == 0, ~10 s at most).
bool StartBarrier() {
    const bool host = g_local == 0;
    if (!host) g_inputs[g_local][0] = Input{};
    const DWORD start = GetTickCount();
    DWORD lastSend = 0, lastGoneCheck = start;
    bool told = false;
    g_inInputWait = true;
    for (;;) {
        Receive();
        bool all = true;
        if (host) {
            for (int p = 1; p < g_players; ++p)
                if ((g_senders >> p) & 1) all &= g_inputs[p].count(0) != 0;
        } else {
            all = g_inputs[0].count(0) != 0;
        }
        if (all) break;
        const DWORD waited = GetTickCount() - start;
        if (waited > 180000) {
            g_inInputWait = false;
            EndMission("Online: the other player didn't finish loading - mission ended");
            return false;
        }
        if (waited > 500 && GetTickCount() - lastGoneCheck > 250) {
            lastGoneCheck = GetTickCount();
            if (PlayerGone()) {
                g_inInputWait = false;
                EndMission(net::LocalPlayer() == 1 ? "Online: a player left - mission ended"
                                                   : "Online: the host left - mission ended");
                return false;
            }
        }
        if (waited > 1500 && !told) {
            told = true;
            OnlineLog("Online: waiting for the other player%s to finish loading", g_players > 2 ? "s" : "");
        }
        if (!host && GetTickCount() - lastSend >= 100) {
            Send();
            lastSend = GetTickCount();
        }
        g_lastTickDone = GetTickCount();   // (a slow load on the other PC is no hang - this wait has its own limit)
        Sleep(1);
    }
    g_inInputWait = false;
    if (host) g_inputs[0][0] = Input{};   // the go signal
    Send();
    OnlineLog("Online: all players loaded - mission starts (waited %u ms)", GetTickCount() - start);
    return true;
}

// The mission script opens the briefing pop-up through API +0x7D0 (0x489590: push 0x14; call the state setter at
// 0x489592). Online it isn't opened at all: on the one frame the pop-up existed before Briefing() closed it, its code
// placed view 0's camera from a computation that sometimes differed between the PCs (watchpoint, 2026-10-12: the same
// writer 0x4D4B98 from 0x4737E0 on both, different values; 4 of 14 soak runs split there with the world still equal).
// The script message 0x3B the pop-up's close would cause is sent by Briefing() on the next tick.
void __cdecl OpenBriefing(uint32_t state) {
    if (g_armed && state == 0x14) {
        if (!g_briefingClosed) dslog::Write("Lockstep: briefing pop-up skipped at tick %u", g_tick);
        g_briefingClosed = true;
        return;
    }
    reinterpret_cast<void(__cdecl*)(uint32_t)>(kSetState)(state);
}

// the briefing pop-up (state 0x14) is closed as soon as the state setter takes calls - on every PC at the same tick
void Briefing() {
    const uint32_t state = *reinterpret_cast<uint32_t*>(kFrontEndState);
    if (state == 0x14 && !*reinterpret_cast<uint32_t*>(kStateBusy)) {
        reinterpret_cast<void(__cdecl*)(uint32_t)>(kSetState)(0xE);
        if (!g_briefingClosed) dslog::Write("Lockstep: briefing closed at tick %u", g_tick);
        g_briefingClosed = true;
    }
    if (g_briefingClosed && !g_briefingMessage && state == 0xE) {
        g_briefingMessage = true;
        if (auto* scripts = *reinterpret_cast<uint8_t**>(kScripts))
            reinterpret_cast<void(__cdecl*)(int, int)>(*reinterpret_cast<uint32_t*>(scripts + 8))(0x3B, 0);
        dslog::Write("Lockstep: briefing message 0x3B sent at tick %u", g_tick);
    }
}
}  // namespace

bool features::LockstepActive() { return g_armed; }

// Every player's block the same on every PC: keyboard & mouse + pad (+0x38 = 3), joystick slot 7 (always empty); Sample()
// points this PC's player at its real first pad meanwhile. The game reads the device bits outside the input calls too -
// the movement code at 0x44EDEF / 0x450DB4 branches on the keyboard bit - and the level's player set-up (splitscreen.cpp
// AssignPlayers) re-runs after every cutscene with each PC's own view of the devices: a 2-PC test desynced in the
// second after a cutscene ended (tick 6720 equal, 6780 not). Applied at the start of the mission, after every
// player set-up and at every tick.
void features::LockstepFixBlocks() {
    if (!g_running) return;
    for (int p = 0; p < g_players; ++p) {
        auto* block = reinterpret_cast<uint8_t*>(kBlocks + p * kBlockSize);
        *reinterpret_cast<uint32_t*>(block + 0x38) = 3;
        *reinterpret_cast<int32_t*>(block + 0x3C8) = kNoJoystick;
    }
}
void features::LockstepSetSeed(uint32_t seed, uint32_t skipCutscenes) {
    g_seed = seed;
    g_hostSkipCutscenes = skipCutscenes;
}
uint32_t features::LockstepSeed() { return g_seed; }
bool features::LockstepSkipCutscenes(bool own) { return g_armed ? g_hostSkipCutscenes != 0 : own; }

bool features::LockstepAxis(uint8_t* block, int action, int opposite, float& value) {
    const int p = g_armed ? PlayerOf(block) : -1;
    if (p < 0) return false;
    if (!g_running) {   // loading: no input
        value = 0.0f;
        return true;
    }
    const Input& in = g_current[p];
    auto pad = [&](int a) {
        if (a < 0 || a >= kActions) return 0.0f;
        if (in.parts[a] & 2) *reinterpret_cast<uint32_t*>(block + 0x458) = 0;
        return in.pad[a];
    };
    auto key = [&](int a) {
        if (a < 0 || a >= kActions || !(in.parts[a] & 1)) return false;
        *reinterpret_cast<uint32_t*>(0x60F5B0) = 1;
        return true;
    };
    value = pad(action);
    if (opposite != 0x50) {
        if (value == 0.0f) value = pad(opposite);
        else value = -value;
    }
    const bool look = action == 0xA || action == 0xB || action == 0xE || action == 0xF || action == 0x15 || action == 0x16;
    if (look && action < kActions) value += in.gyro[action];
    if (value != 0.0f && look) value *= in.lookScale;
    if (value == 0.0f) {
        if (key(action)) value = 1.0f;
        if (opposite != 0x50) {
            if (value != 0.0f) {
                value = -value;
                return true;
            }
            if (key(opposite)) value = 1.0f;
        }
    }
    return true;
}

bool features::LockstepMouse(uint8_t* block, float* dx, float* dy) {
    const int p = g_armed ? PlayerOf(block) : -1;
    if (p < 0) return false;
    if (!g_running) {   // loading: no movement
        *dx = *dy = 0.0f;
        return true;
    }
    *dx = g_current[p].mouse[0];
    *dy = g_current[p].mouse[1];
    return true;
}

// Called first thing every frame (features::OnFrame).
void features::OnFrameLockstep() {
    // an online session keeps the game running without focus from hosting / joining on (the other PC's START and the
    // ticks must be handled while this window is in the background)
    // OPTIONS > DESERTSTORMFIX > RUN IN BACKGROUND does the same at all times (two copies side by side on one PC: the
    // one without focus had stopped in the menus)
    static bool session = false;
    const bool nowSession = net::LocalPlayer() > 0 || settings::Get().featRunInBackground;
    if (nowSession != session) {
        session = nowSession;
        if (!g_armed) KeepRunning(nowSession);
    }
    const bool loading = *reinterpret_cast<int8_t*>(kDispatchMode) == 1;
    const char* next = *reinterpret_cast<char**>(kNextLevel);
    const bool online = net::LocalPlayer() > 0 && features::CoopPlayers() >= 2;
    if (loading) {
        const bool mission = next && (_strnicmp(next, "Mission", 7) == 0 || _strnicmp(next, "Training", 8) == 0);
        if (online && mission) Arm(next);
        else Stop("level without lockstep");
        return;
    }
    if (!g_armed) return;
    if (!online) return EndMission(net::LocalPlayer() == 1 ? "Online: a player left - mission ended"
                                                            : "Online: the host left - mission ended");
    if (!g_running) {    // first frame of the mission: this PC's devices drive its player's block
        g_running = true;
        g_lastTickDone = GetTickCount();   // (the watchdog also covers a first tick that never ends)
        {   // this PC's own MOUSE OPTIONS live in block 0 (the menu edits player 1); taken before the players' sent
            // values overwrite the blocks, and sent with this PC's player's input
            const auto* own = reinterpret_cast<const uint8_t*>(kBlocks);
            g_ownSens[0] = *reinterpret_cast<const float*>(own + 0x43C);
            g_ownSens[1] = *reinterpret_cast<const float*>(own + 0x440);
            g_ownMouseFlags = static_cast<uint8_t>(0x80 | (*reinterpret_cast<const uint32_t*>(own + 0x444) ? 1 : 0) |
                                                   (*reinterpret_cast<const uint32_t*>(own + 0x448) ? 2 : 0));
            dslog::Write("Lockstep: own mouse options sent (sensitivity %.2f / %.2f, invert %d, acceleration %d)",
                         g_ownSens[0], g_ownSens[1], g_ownMouseFlags & 1, (g_ownMouseFlags >> 1) & 1);
        }
        features::LockstepFixBlocks();
        if (!g_ownerMode) NormalizeTimers();
        // both random sources again: the load drew from them by the PC's own settings (shrub level, detail ...)
        reinterpret_cast<void(__cdecl*)(uint32_t)>(kReseed)(g_seed ^ 0xA5A5A5A5);
        // Mouse look keeps state per block: +0x44C = the pitch accumulator (+= dy / sensitivity, clamped to +-1; block
        // +0x5C AIM_PITCH takes it while the pad's pitch is 0), +0x450 last dy, +0x454 / +0x458 "mouse moved" flags.
        // Mouse movement on a PC before lockstep took over (menus, loading) was left in it - a 1-in-10 split at tick 41
        // (watchpoint 2026-10-12: 0x44B466 wrote -1.0 on one PC, 0.0 on the other, the sent inputs equal).
        for (int p = 0; p < 4; ++p) {
            auto* block = reinterpret_cast<uint8_t*>(kBlocks + p * kBlockSize);
            for (uint32_t off = 0x44C; off <= 0x458; off += 4) *reinterpret_cast<uint32_t*>(block + off) = 0;
        }
        if (!g_ownerMode)
            for (uint32_t t = 0; t < static_cast<uint32_t>(g_delay); ++t)
                for (int p = 0; p < g_players; ++p) g_inputs[p][t] = Input{};
        if (g_ownerMode) OnlineLog("Online: running (player %d of %d, each PC runs its own soldiers)", g_local + 1, g_players);
        else OnlineLog("Lockstep: running (player %d of %d, input delay %d ticks)", g_local + 1, g_players, g_delay);
    }
    features::LockstepFixBlocks();
    if (g_ownerMode) {   // this PC's player from its own devices now, the other players' blocks get nothing
        if (g_tick == 0 && !StartBarrier()) return;
        if (g_tick < 600 && LowestNeed() == 0) {   // a PC that may not have seen this PC's start yet: resent
            static DWORD lastSend = 0;
            if (GetTickCount() - lastSend >= 100) {
                lastSend = GetTickCount();
                Send();
            }
        }
        g_blockRawKeys = 0;
        for (int p = 0; p < g_players; ++p) {   // this PC's players, each from its own device (lobby)
            if (!((g_localMask >> p) & 1)) {
                g_current[p] = Input{};
                continue;
            }
            // the loadout screen (customise.cpp) has the devices meanwhile: the soldiers stand still
            g_current[p] = features::CustomiseOpen() || features::OnlinePauseOpen() ? Input{} : Sample(p, features::LobbyDevice(p));
            const Input& local = g_current[p];
            if (local.mouseFlags & 0x80) {
                auto* block = reinterpret_cast<uint8_t*>(kBlocks + p * kBlockSize);
                *reinterpret_cast<float*>(block + 0x43C) = local.mouseSens[0];
                *reinterpret_cast<float*>(block + 0x440) = local.mouseSens[1];
                *reinterpret_cast<uint32_t*>(block + 0x444) = local.mouseFlags & 1;
                *reinterpret_cast<uint32_t*>(block + 0x448) = (local.mouseFlags >> 1) & 1;
            }
        }
        g_blockRawKeys = 1;
        Receive();
        features::AuthorityTick(g_tick);
        Briefing();
        ++g_tick;
        g_lastTickDone = GetTickCount();
        return;
    }
    // this PC's input for tick + g_delay, then every player's input for this tick
    g_blockRawKeys = 0;   // this PC's keyboard for its own player only
    g_inputs[g_local][g_tick + g_delay] = Sample(g_local);
    g_inputs[g_local][g_tick + g_delay].events = features::AuthorityTakeEvents();   // (only the host has any)
    g_blockRawKeys = 1;
    if (g_tick % kHashEvery == 0) {
        g_hashes[g_local][g_tick] = WorldHash();
#ifndef DS_DIST
        if (g_tick == 0)   // dev: the starting state, to compare the PCs' logs
            for (const ObjState& o : g_snapshots[0])
                dslog::Write("  start obj %u %.4f %.4f %.4f %.4f %08X", o.id, o.v[0], o.v[1], o.v[2], o.v[3], o.vt);
#endif
    }
    Send();
    const DWORD start = GetTickCount();
    DWORD lastSend = start;
    DWORD lastGoneCheck = start;
    bool told = false;
    g_inInputWait = true;
    for (;;) {
        Receive();
        bool all = true;
        for (int p = 0; p < g_players; ++p) all &= g_inputs[p].count(g_tick) != 0;
        if (all) break;
        const DWORD waited = GetTickCount() - start;
        if (waited > 60000) return EndMission("Online: the other player stopped answering - mission ended");
        if (waited > 500 && GetTickCount() - lastGoneCheck > 250) {   // a player who left won't send: end at once
            lastGoneCheck = GetTickCount();
            if (PlayerGone())
                return EndMission(net::LocalPlayer() == 1 ? "Online: a player left - mission ended"
                                                          : "Online: the host left - mission ended");
        }
        if (waited > 1500 && !told) {
            told = true;
            Popup("Online: waiting for the other player ...", 3000);
            OnlineLog("Lockstep: waiting for input of tick %u", g_tick);
        }
        if (GetTickCount() - lastSend > 50) {   // resend (lost packets; the other PC may still be loading)
            Send();
            lastSend = GetTickCount();
        }
        Sleep(1);
    }
    g_inInputWait = false;
    for (int p = 0; p < g_players; ++p) {
        g_current[p] = g_inputs[p][g_tick];
        if (g_current[p].mouseFlags & 0x80) {   // that player's own mouse options on every PC
            auto* block = reinterpret_cast<uint8_t*>(kBlocks + p * kBlockSize);
            *reinterpret_cast<float*>(block + 0x43C) = g_current[p].mouseSens[0];
            *reinterpret_cast<float*>(block + 0x440) = g_current[p].mouseSens[1];
            *reinterpret_cast<uint32_t*>(block + 0x444) = g_current[p].mouseFlags & 1;
            *reinterpret_cast<uint32_t*>(block + 0x448) = (g_current[p].mouseFlags >> 1) & 1;
        }
        // (this PC's own inputs stay until every other PC has passed them - they may have to be sent again)
        const uint32_t keep = p == g_local ? LowestNeed() : g_tick + 1;
        g_inputs[p].erase(g_inputs[p].begin(), g_inputs[p].lower_bound(keep));
    }
    features::DrawLogTick(g_tick);
#ifndef DS_DIST
    TickLog();
    WatchTick();
#endif
    ScanVoices();
    RefreshPoses();
    features::AuthorityDeliver(g_current[0].events);
    features::AuthorityTick(g_tick);
    StateCache();
    CheckHashes();
    Briefing();
    // 60 ticks a second in real time (the frame cap may allow more; the game's time is 1/60 s per tick)
    // Ticks keep real time: each has a due time 1/60 s after the previous one. A PC that draws slower than 60 frames a
    // second falls behind and catches up with frames that update without drawing (framecap.cpp, LockstepBehind) - one
    // tick per drawn frame made the game run in slow motion for everyone when one PC drew 30 (user, 2026-10-15).
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONGLONG period = freq.QuadPart / 60;
    if (!g_tick || !g_due || now.QuadPart - g_due > freq.QuadPart / 4) {
        g_due = now.QuadPart;   // first tick, or a long stall (input wait, loading): no burst of catch-up ticks
    } else {
        while (now.QuadPart < g_due) {
            if (g_due - now.QuadPart > freq.QuadPart / 500) Sleep(1);
            QueryPerformanceCounter(&now);
        }
    }
    g_due += period;
    ++g_tick;
    g_lastTickDone = GetTickCount();
    g_virtual = kVirtualStart + static_cast<uint32_t>(uint64_t(g_tick) * 1000 / 60);
}

#ifndef DS_DIST
// dev: the axis evaluator's answers for player 1's aim / look actions around the start (ticks 36..46), synced or local
void features::LockstepAxisTrace(const uint8_t* block, int action, float value, bool synced) {
    if (!g_armed || g_tick < 36 || g_tick > 46) return;
    if (block != reinterpret_cast<const uint8_t*>(kBlocks)) return;
    if (action != 0xA && action != 0xB && action != 0xE && action != 0xF && action != 0x15 && action != 0x16) return;
    dslog::Write("#a %u action %X %s %.4f ret %08X", g_tick, action, synced ? "synced" : "LOCAL", value,
                 reinterpret_cast<uint32_t>(_ReturnAddress()));
}
#endif

void features::OnlineLogLine(const char* text) { OnlineLog("%s", text); }

bool features::LockstepSkipRender() { return (g_skipRender || g_catchUp) && g_running; }

bool features::LockstepBehind() {
    if (!g_running || !g_due) return false;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart >= g_due;
}

void features::LockstepCatchUp(bool on) { g_catchUp = on; }
