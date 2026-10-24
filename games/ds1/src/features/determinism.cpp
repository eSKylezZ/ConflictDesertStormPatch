// Dev: determinism check for online lockstep (Dev\DetTest = 1, dev builds only). Does the game, given the same inputs
// and the same time steps, compute exactly the same world? From the frame a mission starts loading:
//   - virtual clock: the two game timers (FUN_004ba3a0 ctor / FUN_004ba5b0 tick read the clock FUN_004ba370 - its only
//     callers, 0x4BA3E4 / 0x4BA5B7, hooked) see 1/60 s per frame, whatever the real frame rate; the timers' last clock
//     (+0xD4) / total (+0x10) / frame count (+0xC8) restart from fixed values; timing.cpp's exact-frame-time correction
//     steps aside (DetActive)
//   - random numbers: FUN_004b7d50(fixed seed) = srand (rand 0x567469, seed 0x5FE850) + the game's two pre-filled
//     random tables (floats 0x61BFE0 read by FUN_004b7df0 - boids, AI ...; ints 0x619FE0), normally filled once at
//     start-up from a per-process seed
//   - scripted keys (WM_KEYDOWN / WM_KEYUP posted from the game thread at fixed frames: handled before the next frame)
//   - per frame a hash of every scene object's position (object table [[0x63C9A0]+0x30], ids 0..[+0x34]; object +0x30)
//     and health (+0x54), the RNG seed and the gameplay timer's total ms -> DesertStormFix-det-<DetRun>.txt
// Two runs (e.g. at different frame caps) with identical lines = deterministic so far; the first differing frame and
// its object list (logged every 60 frames, and at every frame for Dev\DetDetail) show what diverged.
#include <windows.h>
#include <mmreg.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "features/features.h"

#ifndef DS_DIST
namespace {
// Clock sites shared with lockstep.cpp (features.h kClock*): determinism's subscribers run first (kClockDeterminism)
constexpr uint32_t kClock = features::kClockTarget, kClockCallCtor = features::kClockCallCtor,
                   kClockCallTick = features::kClockCallTick;
constexpr uint32_t kTimerA = 0x63C954, kTimerB = 0x63C958, kRngSeed = 0x5FE850;
constexpr uint32_t kDispatchMode = 0x60EC8C, kNextLevel = 0x5E7028, kObjects = 0x63C9A0, kGameWindow = 0x606A60;
constexpr uint32_t kFrontEndState = 0x617C18, kStateBriefing = 0x14;
constexpr uint32_t kVirtualStart = 1000000;
uint32_t kFrames = 3600;   // Dev\DetFrames (default 3600): frames hashed / keys + briefing close handled
uint32_t g_keyBase = 0;     // Dev\DetKeysAfterBriefing = 1: scripted key frames count from the briefing's close
bool g_keysArmed = true;

bool g_enabled = false, g_started = false, g_done = false;
uint32_t g_frame = 0, g_virtual = 0, g_run = 0, g_detail = 0;
FILE* g_out = nullptr;
FILE* g_dump = nullptr;   // Dev\DetDumpObject: that object's first kDumpSize bytes every frame (binary)
uint32_t g_dumpId = 0;
constexpr uint32_t kDumpSize = 0x2C00;

DWORD DevDword(const char* name, DWORD fallback) {
    DWORD v, size = sizeof v;
    return RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", name, RRF_RT_REG_DWORD, nullptr, &v,
                        &size) == ERROR_SUCCESS ? v : fallback;
}

std::string DevString(const char* name) {
    char v[128];
    DWORD size = sizeof v;
    return RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", name, RRF_RT_REG_SZ, nullptr, v, &size) ==
           ERROR_SUCCESS ? std::string(v) : std::string();
}

using ClockFn = uint32_t(__cdecl*)();
ClockFn g_clockCtorNext = nullptr, g_clockTickNext = nullptr;   // one broker next per call site
uint32_t __cdecl ClockCtor() { return g_started ? g_virtual : g_clockCtorNext(); }
uint32_t __cdecl ClockTick() { return g_started ? g_virtual : g_clockTickNext(); }

// The 18 direct timeGetTime calls (+ GetTickCount) through the import slots: the game thread gets the virtual clock
// too once the test runs (Dev\DetSysTime, default on); other threads (sound streaming) keep the real one.
constexpr uint32_t kIatTimeGetTime = features::kIatTimeGetTime, kIatGetTickCount = features::kIatGetTickCount;
using TimeFn = DWORD(WINAPI*)();
TimeFn g_realTimeGetTime = nullptr, g_realGetTickCount = nullptr;
DWORD g_gameThread = 0;
// Dev\DetNoSound: DirectSoundCreate8 (import slot 0x5D8030) says "no driver" - the game runs silent (experiment: does
// the sound system's real-time playback state feed back into the simulation?)
void* g_dsCreateNext = nullptr;   // never called (the hook says "no driver")
HRESULT WINAPI NoDirectSound(const GUID*, void** out, void*) {
    if (out) *out = nullptr;
    return static_cast<HRESULT>(0x88780078);   // DSERR_NODRIVER
}
DWORD WINAPI VirtualTimeGetTime() {
    return g_started && GetCurrentThreadId() == g_gameThread ? g_virtual : g_realTimeGetTime();
}
DWORD WINAPI VirtualGetTickCount() {
    return g_started && GetCurrentThreadId() == g_gameThread ? g_virtual : g_realGetTickCount();
}

// Dev\DetRandLog: who draws from the random tables, per frame (return address, helper, thread) -> "#r" lines.
// Helpers: 0x4B7DF0 float in [a, b), 0x4B7E30 int, 0x4B7E60 int mod n, 0x4B7E90 int in [a, b); each entry's first
// instruction (an absolute load, position-independent) is replayed by the stub after the record call.
struct Draw {
    uint32_t ret, kind, count;
    bool other;   // not the game thread
};
Draw g_draws[256];
int g_drawCount = 0;
bool g_drawLogOnline = false;   // Dev\LockstepDrawLog: the same per tick of an online game, into the log
void __stdcall RecordDraw(uint32_t kind, uint32_t ret) {
    if ((!g_started || g_done) && !g_drawLogOnline) return;
    const bool other = GetCurrentThreadId() != g_gameThread;
    for (int i = 0; i < g_drawCount; ++i)
        if (g_draws[i].ret == ret && g_draws[i].kind == kind && g_draws[i].other == other) return void(++g_draws[i].count);
    if (g_drawCount < 256) g_draws[g_drawCount++] = {ret, kind, 1, other};
}
void* g_drawNext[4] = {};   // broker entry detours: the trampolines (or the next subscriber)
__declspec(naked) void DrawFloatStub() {
    __asm {
        pushad
        pushfd
        push dword ptr [esp + 36]
        push 0
        call RecordDraw
        popfd
        popad
        jmp dword ptr [g_drawNext + 0 * 4]
    }
}
__declspec(naked) void DrawIntStub() {
    __asm {
        pushad
        pushfd
        push dword ptr [esp + 36]
        push 1
        call RecordDraw
        popfd
        popad
        jmp dword ptr [g_drawNext + 1 * 4]
    }
}
__declspec(naked) void DrawModStub() {
    __asm {
        pushad
        pushfd
        push dword ptr [esp + 36]
        push 2
        call RecordDraw
        popfd
        popad
        jmp dword ptr [g_drawNext + 2 * 4]
    }
}
__declspec(naked) void DrawRangeStub() {
    __asm {
        pushad
        pushfd
        push dword ptr [esp + 36]
        push 3
        call RecordDraw
        popfd
        popad
        jmp dword ptr [g_drawNext + 3 * 4]
    }
}
void FlushDraws(FILE* out) {
    std::sort(g_draws, g_draws + g_drawCount, [](const Draw& a, const Draw& b) { return a.ret != b.ret ? a.ret < b.ret : a.kind < b.kind; });
    for (int i = 0; i < g_drawCount; ++i)
        fprintf(out, "#r %u %08X %u %u%s\n", g_frame, g_draws[i].ret, g_draws[i].kind, g_draws[i].count,
                g_draws[i].other ? " other-thread" : "");
    g_drawCount = 0;
}
void HookDraws() {
    struct Site {
        uint32_t at;
        const void* stub;
        uint8_t len;
    };
    // entries: mov eax, [0x61DFE4] (5 bytes) / mov ecx|ecx|edx, [0x61DFE8] (6 bytes) - moved into broker trampolines
    const Site sites[] = {Site{0x4B7DF0, &DrawFloatStub, 5}, Site{0x4B7E30, &DrawIntStub, 6},
                          Site{0x4B7E60, &DrawModStub, 6}, Site{0x4B7E90, &DrawRangeStub, 6}};
    for (int i = 0; i < 4; ++i)
        if (!patch::HookFunction(sites[i].at, sites[i].stub, &g_drawNext[i], nullptr, 0, sites[i].len))
            dslog::Write("[fail] Determinism: random-table draw entry 0x%08X not hooked", sites[i].at);
    dslog::Write("[dev]  Determinism: random-table draws logged per caller");
}

// Dev\DetGateLog: the soldier chatter gate FUN_004272e0 (fastcall soldier; one caller 0x426503 in FUN_00426500) with
// the fields it tests, per call -> "#g frame id flags+0x284 target+0x255C +0x2D8 +0x238 +0x237C +0x2398 timerA class0
// result" (fields read before the call: it sets +0x2398 when it says yes).
bool Read(const void* p, void* out, size_t n);
int __fastcall ChatterGate(uint8_t* s) {
    uint32_t f[8] = {};
    Read(s + 8, &f[0], 4);
    Read(s + 0x284, &f[1], 4);
    Read(s + 0x255C, &f[2], 4);
    Read(s + 0x2D8, &f[3], 4);
    Read(s + 0x238, &f[4], 4);
    Read(s + 0x237C, &f[5], 4);
    Read(s + 0x2398, &f[6], 4);
    if (const uint8_t* t = *reinterpret_cast<uint8_t* const*>(kTimerA)) Read(t + 0x10, &f[7], 4);
    const int cls = reinterpret_cast<int(__thiscall*)(uint8_t*, int)>((*reinterpret_cast<void***>(s))[0x4C / 4])(s, 0);
    const int r = reinterpret_cast<int(__fastcall*)(uint8_t*)>(0x4272E0)(s);
    if (g_started && !g_done && g_out)
        fprintf(g_out, "#g %u %u %08X %08X %u %u %u %u %u %d %d\n", g_frame, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], cls, r);
    return r;
}

uint32_t Fnv(uint32_t h, const void* p, size_t n) {
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 16777619u;
    return h;
}

bool Read(const void* p, void* out, size_t n) {
    __try {
        memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Scripted keys: {frame, virtual key, down}. Walk forward, strafe left, walk again, crouch on and off.
struct Key {
    uint32_t frame;
    UINT vk;
    bool down;
};
constexpr Key kKeys[] = {{600, 'W', true},  {1500, 'W', false}, {900, 'A', true},  {1200, 'A', false},
                         {1800, 'D', true}, {2100, 'D', false}, {2200, 'W', true}, {3000, 'W', false},
                         {2400, 'C', true}, {2410, 'C', false}, {2700, 'C', true}, {2710, 'C', false}};

void PostKeys() {
    const HWND wnd = *reinterpret_cast<HWND*>(kGameWindow);
    for (const Key& k : kKeys)
        if (g_keysArmed && k.frame + g_keyBase == g_frame && wnd) {
            const UINT sc = MapVirtualKeyA(k.vk, MAPVK_VK_TO_VSC);
            PostMessageA(wnd, k.down ? WM_KEYDOWN : WM_KEYUP, k.vk, 1 | sc << 16 | (k.down ? 0 : 0xC0000000));
        }
}

void DumpObject() {
    static uint8_t buf[kDumpSize];
    memset(buf, 0, sizeof buf);
    const uint8_t* mgr = *reinterpret_cast<uint8_t* const*>(kObjects);
    const uint8_t* const* table = nullptr;
    const uint8_t* obj = nullptr;
    if (mgr && Read(mgr + 0x30, &table, 4) && table) Read(table + g_dumpId, &obj, 4);
    if (obj) Read(obj, buf, sizeof buf);
    fwrite(buf, 1, sizeof buf, g_dump);
}

void Hash() {
    const uint8_t* mgr = *reinterpret_cast<uint8_t* const*>(kObjects);
    uint32_t h = 2166136261u, maxId = 0, live = 0;
    const uint8_t* const* table = nullptr;
    if (mgr) {
        Read(mgr + 0x30, &table, 4);
        Read(mgr + 0x34, &maxId, 4);
    }
    const bool detail = g_detail || g_frame % 60 == 0;
    for (uint32_t id = 0; table && id <= maxId && id < 4096; ++id) {
        const uint8_t* obj = nullptr;
        if (!Read(table + id, &obj, 4) || !obj) continue;
        float s[4] = {};
        if (!Read(obj + 0x30, s, 12) || !Read(obj + 0x54, s + 3, 4)) continue;
        h = Fnv(h, &id, 4);
        h = Fnv(h, s, sizeof s);
        ++live;
        uint32_t vt = 0;
        Read(obj, &vt, 4);
        if (detail && g_out) fprintf(g_out, "  %u %.4f %.4f %.4f %.3f %08X\n", id, s[0], s[1], s[2], s[3], vt);
    }
    const uint32_t rng = *reinterpret_cast<uint32_t*>(kRngSeed);
    uint32_t total = 0;
    if (const uint8_t* t = *reinterpret_cast<uint8_t* const*>(kTimerB)) Read(t + 0x10, &total, 4);
    const uint32_t floatIdx = *reinterpret_cast<uint32_t*>(0x61DFE4), intIdx = *reinterpret_cast<uint32_t*>(0x61DFE8);
    h = Fnv(h, &rng, 4);
    if (g_out)
        fprintf(g_out, "%u %08X rng %08X tables %u %u objects %u total %u\n", g_frame, h, rng, floatIdx, intIdx, live, total);
    // x87 control word at the frame's start (precision / rounding: D3D8 leaves single precision) - logged on change
    static uint16_t lastCw = 0xFFFF;
    uint16_t cw;
    __asm fnstcw cw
    if (cw != lastCw && g_out) fprintf(g_out, "#c %u %04X\n", g_frame, cw);
    lastCw = cw;
}

// The mouse poll FUN_005386d0 / joystick loop FUN_00539340 take no stack arguments: replaced by "nothing read".
int NoMousePoll() { return 0; }
void NoPadPoll() {}

void Start() {
    g_gameThread = GetCurrentThreadId();
    g_started = true;
    // The real mouse and pads of whoever uses the PC must not reach the test (DirectInput reads them even in the
    // background): mouse poll FUN_005386d0 -> xor eax, eax; ret (no mouse look), joystick loop FUN_00539340 -> ret.
    // The scripted keys come through window messages, which only this test sends to the unfocused window.
    if (DevDword("DetBlockInput", 1)) {   // entry detours without next (replacements), owner dsfix.determinism
        patch::OwnerScope owner(pse_host::Find("dsfix.determinism"));
        if (!patch::HookFunction(0x5386D0, reinterpret_cast<const void*>(&NoMousePoll), nullptr) ||
            !patch::HookFunction(0x539340, reinterpret_cast<const void*>(&NoPadPoll), nullptr))
            dslog::Write("[fail] Determinism: mouse / pad polls not blocked");
    }
    g_frame = 0;
    g_virtual = kVirtualStart;
    // FUN_004b7d50(seed): srand + the game's two random tables (2048 floats 0x61BFE0 for FUN_004b7df0(min, max),
    // index 0x61DFE4; 2048 ints 0x619FE0, index 0x61DFE8) - filled once at start-up from a per-process seed
    reinterpret_cast<void(__cdecl*)(uint32_t)>(0x4B7D50)(12345);
    for (uint32_t g : {kTimerA, kTimerB})
        if (uint8_t* t = *reinterpret_cast<uint8_t**>(g)) {
            *reinterpret_cast<uint32_t*>(t + 0xD4) = g_virtual;
            *reinterpret_cast<uint32_t*>(t + 0x10) = 0;
            *reinterpret_cast<uint32_t*>(t + 0xC8) = 0;
        }
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    snprintf(slash ? slash + 1 : path, MAX_PATH - (slash ? slash + 1 - path : 0), "DesertStormFix-det-%u.txt", g_run);
    g_out = fopen(path, "w");
    g_dumpId = DevDword("DetDumpObject", 0);
    if (g_dumpId) {
        char dump[MAX_PATH];
        snprintf(dump, sizeof dump, "%.*sobj-%u.bin", int(strlen(path) - 4), path, g_dumpId);
        g_dump = fopen(dump, "wb");
    }
    dslog::Write("[dev]  Determinism: started (level %s, run %u) -> %s", *reinterpret_cast<char**>(kNextLevel), g_run, path);
}
}  // namespace

void features::DrawLogTick(uint32_t tick) {   // lockstep.cpp: after each tick's wait (the draws of the previous update)
    if (!g_drawLogOnline) return;
    g_gameThread = GetCurrentThreadId();
    std::sort(g_draws, g_draws + g_drawCount, [](const Draw& a, const Draw& b) { return a.ret != b.ret ? a.ret < b.ret : a.kind < b.kind; });
    for (int i = 0; i < g_drawCount; ++i)
        dslog::Write("#r %u %08X %u %u%s", tick, g_draws[i].ret, g_draws[i].kind, g_draws[i].count, g_draws[i].other ? " other-thread" : "");
    g_drawCount = 0;
}

bool features::DetActive() { return g_started; }   // (the clock stays virtual after the logged frames)
uint32_t features::DetVirtualTime() { return g_virtual; }

void features::ApplyDeterminism() {
    if (DevDword("LockstepDrawLog", 0)) {
        HookDraws();
        g_drawLogOnline = true;
    }
    g_enabled = DevDword("DetTest", 0) != 0;
    if (!g_enabled) return;
    g_run = DevDword("DetRun", 1);
    g_detail = DevDword("DetDetail", 0);
    kFrames = DevDword("DetFrames", 3600);
    g_keysArmed = DevDword("DetKeysAfterBriefing", 0) == 0;
    const bool ok = patch::SubscribeCall(kClockCallCtor, reinterpret_cast<const void*>(&ClockCtor), kClock,
                                         features::kClockDeterminism, reinterpret_cast<void**>(&g_clockCtorNext)) &&
                    patch::SubscribeCall(kClockCallTick, reinterpret_cast<const void*>(&ClockTick), kClock,
                                         features::kClockDeterminism, reinterpret_cast<void**>(&g_clockTickNext));
    dslog::Write(ok ? "[dev]  Determinism test armed (run %u)" : "[fail] Determinism: clock hooks not applied", g_run);
    // Real-time sound was the remaining run-to-run noise (11B / 12A / Training 1 parity, 2026-10-19): sound random draws
    // from a private generator, "sound still playing" by the virtual clock (lockstep.cpp's online hooks).
    if (DevDword("DetPrivateSound", 1)) {
        features::HarnessSound();
        dslog::Write("[dev]  Determinism: sound draws private, sound timing virtual");
    }
    if (DevDword("DetRandLog", 1)) HookDraws();
    if (DevDword("DetNoSound", 0)) {
        // import slot pointer hook ahead of dev.cpp's GlobalFocusAudio (priority 0): never calls on
        if (!patch::HookSlot(0x5D8030, reinterpret_cast<const void*>(&NoDirectSound), &g_dsCreateNext, 0,
                             features::kClockDeterminism))
            dslog::Write("[fail] Determinism: DirectSoundCreate8 import not hooked");
        dslog::Write("[dev]  Determinism: no sound device (DirectSoundCreate8 -> DSERR_NODRIVER)");
    }
    if (DevDword("DetGateLog", 1)) patch::HookCall(0x426503, reinterpret_cast<const void*>(&ChatterGate), 0x4272E0);
    if (DevDword("DetSysTime", 1)) {
        // import slots: broker pointer chains shared with lockstep.cpp - determinism's run first (virtual once started,
        // else on to lockstep's / the real function)
        if (!patch::HookSlot(kIatTimeGetTime, reinterpret_cast<const void*>(&VirtualTimeGetTime),
                             reinterpret_cast<void**>(&g_realTimeGetTime), 0, features::kClockDeterminism) ||
            !patch::HookSlot(kIatGetTickCount, reinterpret_cast<const void*>(&VirtualGetTickCount),
                             reinterpret_cast<void**>(&g_realGetTickCount), 0, features::kClockDeterminism))
            dslog::Write("[fail] Determinism: timeGetTime / GetTickCount imports not hooked");
        dslog::Write("[dev]  Determinism: timeGetTime / GetTickCount virtual on the game thread too");
    }
}

void features::OnFrameDeterminism() {
    if (!g_enabled) return;
    if (!g_started) {
        char* next = *reinterpret_cast<char**>(kNextLevel);
        // Dev\MpForceLevel = "MP_MISSION3.dll": a hosted legacy-MP round (alone, LAN) loads that map whatever the lobby
        // chose - its map row ignores posted keys (MP parity runs)
        if (*reinterpret_cast<int8_t*>(kDispatchMode) == 1 && next && _strnicmp(next, "MP_Mission", 10) == 0) {
            const std::string force = DevString("MpForceLevel");
            if (!force.empty() && _stricmp(next, force.c_str()) != 0 && force.size() < 64) {
                dslog::Write("[dev]  Determinism: level %s -> %s (Dev\\MpForceLevel)", next, force.c_str());
                strcpy_s(next, 64, force.c_str());
            }
        }
        if (*reinterpret_cast<int8_t*>(kDispatchMode) == 1 && next &&
            (_strnicmp(next, "Mission", 7) == 0 || _strnicmp(next, "Training", 8) == 0 ||
             _strnicmp(next, "MP_Mission", 10) == 0)) Start();   // MP levels: booted alone, no network session
        return;
    }
    ++g_frame;
    g_virtual = kVirtualStart + static_cast<uint32_t>(uint64_t(g_frame) * 1000 / 60);
    features::HarnessSoundFrame();
    if (g_done) return;
    Hash();
    if (g_dump) DumpObject();
    if (g_out) FlushDraws(g_out);   // the draws since the previous frame
    PostKeys();
    // Scripted mission events at fixed frames (counted like the keys): Dev\DetMsgFrame + DetMsgId / DetMsgA / DetMsgB =
    // a script message to the level's script object (e.g. 0x15 door 37 open = Mission 1's cell door: Foley rescued);
    // Dev\DetCompleteFrame = the debug "complete mission" (0x444DC0: script message 0x11).
    static const uint32_t msgFrame = DevDword("DetMsgFrame", 0), completeFrame = DevDword("DetCompleteFrame", 0);
    if (g_keysArmed && msgFrame && msgFrame + g_keyBase == g_frame)
        if (auto* scripts = *reinterpret_cast<uint8_t**>(0x60F578)) {
            static uint32_t param[8] = {};
            param[0] = DevDword("DetMsgA", 0);
            param[1] = DevDword("DetMsgB", 0);
            const uint32_t id = DevDword("DetMsgId", 0);
            reinterpret_cast<void(__cdecl*)(int, void*)>(*reinterpret_cast<uint32_t*>(scripts + 8))(id, param);
            if (g_out) fprintf(g_out, "# script message %X (%u, %u) at frame %u\n", id, param[0], param[1], g_frame);
        }
    if (g_keysArmed && completeFrame && completeFrame + g_keyBase == g_frame) {
        reinterpret_cast<void(__cdecl*)()>(0x444DC0)();
        if (g_out) fprintf(g_out, "# complete mission at frame %u\n", g_frame);
    }
    // The mission briefing pop-up pauses the mission (keys during its opening transition are ignored): closed like
    // customise.cpp does - state 0xE once the state setter takes calls ([0x617C20] = 0), then the mission script's
    // message 0x3B (the briefing's own update would send it; without it no mission logic runs).
    static bool closed = false, messaged = false;
    const uint32_t state = *reinterpret_cast<uint32_t*>(kFrontEndState);
    if (state == kStateBriefing && !*reinterpret_cast<uint32_t*>(0x617C20)) {
        reinterpret_cast<void(__cdecl*)(uint32_t)>(0x47EF20)(0xE);
        if (!closed && g_out) fprintf(g_out, "# briefing closed at frame %u\n", g_frame);
        if (!closed && !g_keysArmed) g_keyBase = g_frame, g_keysArmed = true;
        closed = true;
    }
    if (closed && !messaged && state == 0xE) {
        messaged = true;
        if (auto* scripts = *reinterpret_cast<uint8_t**>(0x60F578)) {
            reinterpret_cast<void(__cdecl*)(int, int)>(*reinterpret_cast<uint32_t*>(scripts + 8))(0x3B, 0);
            if (g_out) fprintf(g_out, "# script message 0x3B at frame %u\n", g_frame);
        }
    }
    if (g_frame >= kFrames) {
        g_done = true;   // the clock keeps running virtually (switching back would jump it)
        if (g_out) fclose(g_out), g_out = nullptr;
        if (g_dump) fclose(g_dump), g_dump = nullptr;
        dslog::Write("[dev]  Determinism: %u frames logged", kFrames);
    }
}
#else
bool features::DetActive() { return false; }
uint32_t features::DetVirtualTime() { return 0; }
void features::DrawLogTick(uint32_t) {}
void features::ApplyDeterminism() {}
void features::OnFrameDeterminism() {}
#endif
