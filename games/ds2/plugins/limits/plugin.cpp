// ds2fix.limits - Conflict: Desert Storm II's fixed engine limits raised for big battles (games/ds2/plugins/limits), the
// DS2 port of DesertStormFix's limits.cpp (repo root src/features/limits.cpp, research/large_battles.md). Address table
// and research: games/ds2/PORTING.md "Engine limits". CDS II.exe 1.0 (DS1 addresses in brackets):
//
// Byte patches (one all-or-nothing group, verified original bytes):
//   Karma constraints (joints + contacts)  push 1000 -> 8192   0x4F8663 (MdtWorldCreate in the dynamics ctor) [500]
//   collision pairs / objects               800 / 800 -> 16384 / 4096   0x4F8769 / 0x4F876E (McdAxisSortCreate) [400]
//   particles                               48 000-byte budget = 250 slots of 0xC0 -> 2000 slots   0x413CEC [0x4104E6]
//   event queue                             200 -> 2048   0x4CFEF0 (level unload) / 0x4D13E1 (engine init) [0x410528]
//   render command arena [0x680070]         64 KB -> 1 MB   0x4D0F56 [0x4CB2D6]
//   decal store ring (decal mgr +0x40)      64 KB -> 512 KB   0x550C41 [128 KB, 0x521BDD]
//   A* searches per frame (router +0x30)    3 -> 12   0x4B93B3 [0x509ACB]
//   software sound voices (+0x50)           6 (+10 = 16) -> 32 (42)   0x4B2F58 [0x55F22B]
//   script-id table built up to id          1000 -> 65534   0x4CE755 [0x4CA4FB]
//   save buffer                             0x384AF + extras -> + 4 MB   0x4197FD [0x4154A7]
// Code:
//   physics-object list + Karma bodies + collision models: DS2 sizes them per level from the level's .FAS
//   ("DynamicObjects" / "MeBodies" / "MeModels", world +0x1BC8 / +0x1BCC / +0x1BD0, defaults 100 / 250 / 200 without a
//   .FAS; the shipped levels say 9-20 / 72-97 / 96-164 - measured for exactly their own content) and hand them to the
//   dynamics manager's ctor 0x4F8600 thiscall(objects, bodies, models) at 0x50992C. Wrapped: at least 4096 / 2048 / 4096.
//   The list's add 0x4F6D80 (`arr[count++] = obj`, no bounds check; the remove 0x4F6DA0 only nulls the slot, the count
//   never goes down) is replaced: free slots are reused, a full list refuses (logged). The step loops 0x4F7460 /
//   0x4F74B2 skip null slots.
//   projectiles [0x6800E4]: ctor 0x4F3A80 thiscall(count, gravity) at 0x413C6E - 50 -> 1024 (0xA4 bytes each, Global
//   Heap at level init 0x4F5460). Full = a shell / rocket / grenade not fired.
//   AI think budgets: the scheduler [0x62F424] (ctor 0x4014A0, per frame 0x401DF0 from 0x4107BD) thinks a fixed +0x58 = 1
//   soldier (type-1 object lists, from +0x34) and +0x5C = 1 vehicle (type 0, from +0x38) per frame [DS1: adaptive,
//   clamped to 2 / fixed 2]. Set before each scheduler call: max(1, ceil(n / 40)) soldiers (at most 32), max(1, ceil(n /
//   20)) vehicles (at most 16) - unchanged up to 40 soldiers / 20 vehicles. Depends only on game state.
// Saves (the file must stay the game's - saves made with the plugin load in the unmodified game and the other way round):
//   the particle / projectile writers 0x4F1300 / 0x4F4930 (calls 0x41BF0A / 0x41BF1A) write every slot, the loaders read
//   a fixed 250 / 50 (0x41C439 push 0xFA / 0x41C44E push 0x32) - wrapped to write the vanilla number of slots, live ones
//   first. Events: the header byte +0x20 counts them (0x419D9E from the counter 0x4E1B60 at 0x419D99) while the writer
//   0x4E1F20 (0x41BF2C) writes all - both capped at 255. The file write (0x41C6AC, fwrite of the whole zeroed buffer)
//   writes up to the last non-zero byte, never less than the original buffer size, so the file is the game's byte for
//   byte unless a scene really needs more.
// Heap: the bigger pools come from the Global Heap (1 024 000 bytes). ds2fix.memory's AddHeapReserve (its interface) when
// it is there, else our own subscription to the heap set-up call 0x4136FC in front of ds2fix.mods' (priority 200).
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "ds2fix_memory.h"
#include "pse/pse_api.h"

namespace {
PSEPluginHandle g_self = 0;
const PSELogInterface* g_log = nullptr;
const PSEHooksInterface* g_hooks = nullptr;
const PSEMessagingInterface* g_msg = nullptr;
bool g_on = false;

void Log(PSELogLevel level, const char* fmt, ...) {
    if (!g_log) return;
    char line[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    g_log->Write(g_self, level, "%s", line);
}

bool Matches(uint32_t va, const void* bytes, size_t len) {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(va), bytes, len) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
T& At(void* base, uint32_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off); }

// ---- byte patches ------------------------------------------------------------------------------------------------
struct Site { const char* name; uint32_t va; uint8_t original[4]; uint8_t patched[4]; uint32_t len; };
const Site kSites[] = {
    {"Karma constraints 1000 -> 8192", 0x4F8663, {0xE8, 0x03, 0x00, 0x00}, {0x00, 0x20, 0x00, 0x00}, 4},
    {"collision pairs 800 -> 16384", 0x4F8769, {0x20, 0x03, 0x00, 0x00}, {0x00, 0x40, 0x00, 0x00}, 4},
    {"collision space objects 800 -> 4096", 0x4F876E, {0x20, 0x03, 0x00, 0x00}, {0x00, 0x10, 0x00, 0x00}, 4},
    {"particles 250 -> 2000 (48 000 -> 384 000 bytes)", 0x413CEC, {0x80, 0xBB, 0x00, 0x00}, {0x00, 0xDC, 0x05, 0x00}, 4},
    {"event queue 200 -> 2048 (level unload)", 0x4CFEF0, {0xC8, 0x00, 0x00, 0x00}, {0x00, 0x08, 0x00, 0x00}, 4},
    {"event queue 200 -> 2048 (engine init)", 0x4D13E1, {0xC8, 0x00, 0x00, 0x00}, {0x00, 0x08, 0x00, 0x00}, 4},
    {"render command arena 64 KB -> 1 MB", 0x4D0F56, {0x00, 0x00, 0x01, 0x00}, {0x00, 0x00, 0x10, 0x00}, 4},
    {"decal store 64 KB -> 512 KB", 0x550C41, {0x00, 0x00, 0x01, 0x00}, {0x00, 0x00, 0x08, 0x00}, 4},
    {"A* searches per frame 3 -> 12", 0x4B93B3, {0x03, 0x00, 0x00, 0x00}, {0x0C, 0x00, 0x00, 0x00}, 4},
    {"software sound voices 6 -> 32", 0x4B2F58, {0x06, 0x00, 0x00, 0x00}, {0x20, 0x00, 0x00, 0x00}, 4},
    {"script-id table up to id 65534", 0x4CE755, {0xE8, 0x03, 0x00, 0x00}, {0xFE, 0xFF, 0x00, 0x00}, 4},
    {"save buffer + 4 MB", 0x4197FD, {0xAF, 0x84, 0x03, 0x00}, {0xAF, 0x84, 0x43, 0x00}, 4},
};
constexpr uint32_t kSaveExtra = 0x400000;   // the save buffer patch above

// ---- physics-object list + Karma bodies + collision models -----------------------------------------------------------
constexpr uint32_t kDynCtor = 0x4F8600, kDynCtorCall = 0x50992C;
constexpr uint32_t kPhysicsAdd = 0x4F6D80;
const uint8_t kPhysicsAddEntry[] = {0x8B, 0x41, 0x08, 0x8B, 0x51, 0x0C};   // mov eax,[ecx+8]; mov edx,[ecx+0xC]
constexpr uint32_t kMinObjects = 4096, kMinBodies = 2048, kMinModels = 4096;
void* g_physicsAddNext = nullptr;   // never called: the add is replaced
void* g_dynCtorNext = nullptr;
uint32_t g_fasObjects = 0, g_fasBodies = 0, g_fasModels = 0;

void* __fastcall DynCtor(void* mgr, void*, uint32_t objects, uint32_t bodies, uint32_t models) {
    g_fasObjects = objects, g_fasBodies = bodies, g_fasModels = models;
    Log(PSE_LOG_INFO, "Limits: level physics - objects %u -> %u, Karma bodies %u -> %u, collision models %u -> %u", objects,
        std::max(objects, kMinObjects), bodies, std::max(bodies, kMinBodies), models, std::max(models, kMinModels));
    return reinterpret_cast<void*(__thiscall*)(void*, uint32_t, uint32_t, uint32_t)>(g_dynCtorNext)(
        mgr, std::max(objects, kMinObjects), std::max(bodies, kMinBodies), std::max(models, kMinModels));
}

void __fastcall PhysicsAdd(uint8_t* mgr, void*, void* object) {
    const uint32_t cap = At<uint32_t>(mgr, 4);
    uint32_t& count = At<uint32_t>(mgr, 8);
    void** slots = At<void**>(mgr, 0xC);
    for (uint32_t i = 0; i < count; ++i)
        if (!slots[i]) {
            slots[i] = object;
            return;
        }
    if (count < cap) {
        slots[count++] = object;
        return;
    }
    static DWORD last = 0;
    if (GetTickCount() - last > 5000) {
        last = GetTickCount();
        Log(PSE_LOG_ERROR, "[fail] Limits: all %u physics slots in use - object %p not simulated", cap, object);
    }
}

// ---- projectiles ----
constexpr uint32_t kProjectileCtor = 0x4F3A80, kProjectileCtorCall = 0x413C6E, kProjectiles = 1024;
const uint8_t kProjectilePush[] = {0x6A, 0x32, 0x8B, 0xC8};   // push 50; mov ecx, eax (before the call)
void* g_projCtorNext = nullptr;
void* __fastcall ProjectileCtor(void* mgr, void*, uint32_t, float gravity) {
    return reinterpret_cast<void*(__thiscall*)(void*, uint32_t, float)>(g_projCtorNext)(mgr, kProjectiles, gravity);
}

// ---- AI think budgets ----
constexpr uint32_t kSchedulerCall = 0x4107BD, kScheduler = 0x401DF0, kSchedulerObj = 0x62F424, kObjects = 0x6800E0;
void* g_schedNext = nullptr;
uint32_t g_lastSoldierBudget = 1, g_lastVehicleBudget = 1;

// Objects of one type over every team: lists [mgr+4][team * types + type] (types = byte +0x11, teams = byte +0x10, as
// 0x4CD340 reads them), next +0x18.
uint32_t CountObjects(uint32_t type) {
    uint8_t* mgr = *reinterpret_cast<uint8_t**>(kObjects);
    if (!mgr) return 0;
    const uint32_t teams = mgr[0x10], types = mgr[0x11];
    uint8_t** lists = At<uint8_t**>(mgr, 4);
    if (!lists || type >= types) return 0;
    uint32_t n = 0;
    for (uint32_t team = 0; team < teams; ++team)
        for (uint8_t* o = lists[team * types + type]; o && n < 100000; o = At<uint8_t*>(o, 0x18)) ++n;
    return n;
}

void __fastcall Scheduler(uint8_t* sched, void*) {
    __try {
        const uint32_t soldiers = std::clamp<uint32_t>((CountObjects(1) + 39) / 40, 1, 32);
        const uint32_t vehicles = std::clamp<uint32_t>((CountObjects(0) + 19) / 20, 1, 16);
        At<uint32_t>(sched, 0x58) = soldiers;
        At<uint32_t>(sched, 0x5C) = vehicles;
        if (soldiers != g_lastSoldierBudget || vehicles != g_lastVehicleBudget) {
            g_lastSoldierBudget = soldiers, g_lastVehicleBudget = vehicles;
            Log(PSE_LOG_INFO, "Limits: AI thinks %u soldier(s) and %u vehicle(s) per frame", soldiers, vehicles);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    reinterpret_cast<void(__thiscall*)(void*)>(g_schedNext)(sched);
}

// ---- saves: particles / projectiles / events, file size ----
constexpr uint32_t kParticleSaveCall = 0x41BF0A, kParticleSave = 0x4F1300, kParticleWrite = 0x4F0F50;
constexpr uint32_t kProjectileSaveCall = 0x41BF1A, kProjectileSave = 0x4F4930, kProjectileWrite = 0x4F44E0;
constexpr uint32_t kSavedParticles = 250, kSavedProjectiles = 50;   // the loaders' fixed counts
constexpr uint32_t kEventCountCall = 0x419D99, kEventCount = 0x4E1B60;
constexpr uint32_t kEventSaveCall = 0x41BF2C, kEventSave = 0x4E1F20, kEventWrite = 0x4E1E40;
constexpr uint32_t kEventSize = 0x4C, kMaxSavedEvents = 255;
constexpr uint32_t kSaveWriteCall = 0x41C6AC, kSaveWrite = 0x4C2800;   // fwrite(buf, 1, size, f) wrapper
void* g_particleSaveNext = nullptr;
void* g_projectileSaveNext = nullptr;
void* g_eventCountNext = nullptr;
void* g_eventSaveNext = nullptr;
void* g_saveWriteNext = nullptr;

template <class Live, class Write>
uint8_t* SaveSlots(uint8_t* slots, uint32_t count, uint32_t stride, uint32_t keep, Live live, Write write, uint8_t* cursor) {
    uint32_t written = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (uint32_t i = 0; i < count && written < keep && slots; ++i) {
            uint8_t* s = slots + i * stride;
            if (live(s) != (pass == 0)) continue;
            cursor = write(s, cursor);
            ++written;
        }
    return cursor;
}

// Particle manager: +4 slots (0xC0 each), +0xC count; slot +0xB0 top bits 01 = free (0x4F0AB0, the loader's count).
uint8_t* __fastcall ParticleSave(uint8_t* mgr, void*, uint8_t* cursor) {
    if (At<uint32_t>(mgr, 0xC) <= kSavedParticles)
        return reinterpret_cast<uint8_t*(__thiscall*)(void*, uint8_t*)>(g_particleSaveNext)(mgr, cursor);
    using Write = uint8_t*(__thiscall*)(void*, uint8_t*);
    return SaveSlots(At<uint8_t*>(mgr, 4), At<uint32_t>(mgr, 0xC), 0xC0, kSavedParticles,
                     [](uint8_t* s) { return (At<uint32_t>(s, 0xB0) & 0xC0000000u) != 0x40000000u; },
                     [](uint8_t* s, uint8_t* c) { return reinterpret_cast<Write>(kParticleWrite)(s, c); }, cursor);
}

// Projectile manager: +0 slots (0xA4 each), +8 count, +0x20 passed to the writer; slot +0x9C = 0x64 free.
uint8_t* __fastcall ProjectileSave(uint8_t* mgr, void*, uint8_t* cursor) {
    if (At<uint32_t>(mgr, 8) <= kSavedProjectiles)
        return reinterpret_cast<uint8_t*(__thiscall*)(void*, uint8_t*)>(g_projectileSaveNext)(mgr, cursor);
    using Write = uint8_t*(__thiscall*)(void*, uint8_t*, uint32_t);
    const uint32_t extra = At<uint32_t>(mgr, 0x20);
    return SaveSlots(At<uint8_t*>(mgr, 0), At<uint32_t>(mgr, 8), 0xA4, kSavedProjectiles,
                     [](uint8_t* s) { return At<uint32_t>(s, 0x9C) != 0x64; },
                     [extra](uint8_t* s, uint8_t* c) { return reinterpret_cast<Write>(kProjectileWrite)(s, c, extra); },
                     cursor);
}

int __fastcall EventCount(void* queue, void*) {
    const int n = reinterpret_cast<int(__thiscall*)(void*)>(g_eventCountNext)(queue);
    return std::min<int>(n, kMaxSavedEvents);
}

// Event queue: +0 cdecl filter(event +0x44) -> char (< 0 = not saved), +8 slots (0x4C each), +0x10 count. Saved = not
// flag 0x20 (+4), type 0x63 (+0), filter >= 0 - what the counter 0x4E1B60 and the writer 0x4E1E40 test.
uint8_t* __fastcall EventSave(uint8_t* queue, void*, uint8_t* cursor, uint32_t arg) {
    if (!arg) return cursor;   // the game's loop writes nothing then
    using Filter = char(__cdecl*)(uint32_t);
    using Write = uint8_t*(__thiscall*)(void*, uint8_t*, void*);
    void* filter = At<void*>(queue, 0);
    uint8_t* slots = At<uint8_t*>(queue, 8);
    const uint32_t n = At<uint32_t>(queue, 0x10);
    uint32_t saveable = 0;
    for (uint32_t i = 0; i < n && slots; ++i) {
        uint8_t* e = slots + i * kEventSize;
        if ((e[4] & 0x20) || At<uint32_t>(e, 0) != 0x63 || reinterpret_cast<Filter>(filter)(At<uint32_t>(e, 0x44)) < 0)
            continue;
        if (saveable++ >= kMaxSavedEvents) continue;   // the header counts them in a byte
        cursor = reinterpret_cast<Write>(kEventWrite)(e, cursor, filter);
    }
    if (saveable > kMaxSavedEvents) Log(PSE_LOG_INFO, "Limits: %u pending events - saved the first %u", saveable, kMaxSavedEvents);
    return cursor;
}

// The saver [0x62F458] zeroes its buffer (+0x84, size +0x88) before writing the sections; the game wrote it whole.
size_t __cdecl SaveWrite(const void* buf, size_t size, size_t count, void* file) {
    const size_t original = count > kSaveExtra ? count - kSaveExtra : count;
    const uint8_t* b = static_cast<const uint8_t*>(buf);
    size_t used = count;
    if (size == 1)
        while (used > original && !b[used - 1]) --used;
    if (used > original)
        Log(PSE_LOG_INFO, "Limits: save game %u bytes (original buffer %u)", static_cast<unsigned>(used),
            static_cast<unsigned>(original));
    return reinterpret_cast<size_t(__cdecl*)(const void*, size_t, size_t, void*)>(g_saveWriteNext)(buf, size, used, file);
}

// ---- Global Heap room ----
// particles +336 000, projectiles +159 736, events +140 448, render arena +983 040, decal ring +458 752 = 2.0 MB + slack.
constexpr uint32_t kHeapExtra = 2560 * 1024;
constexpr uint32_t kHeapSetupCall = 0x4136FC, kHeapCtor = 0x4C0650;
void* g_heapNext = nullptr;
bool g_heapAsked = false;

void* __fastcall HeapSetup(void* heap, void*, uint32_t size, const char* name) {
    Log(PSE_LOG_INFO, "Limits: global heap %u -> %u bytes for the bigger pools", size, size + kHeapExtra);
    return reinterpret_cast<void*(__thiscall*)(void*, uint32_t, const char*)>(g_heapNext)(heap, size + kHeapExtra, name);
}

void AskForHeap() {
    if (g_heapAsked || !g_on) return;
    g_heapAsked = true;
    const auto* mem = static_cast<const Ds2FixMemoryV1*>(g_msg->QueryInterface(g_self, DS2FIX_MEMORY_INTERFACE, 1));
    if (mem && mem->size >= sizeof(Ds2FixMemoryV1) && mem->AddHeapReserve(g_self, kHeapExtra)) {
        Log(PSE_LOG_INFO, "Limits: %u KB of global heap reserved through ds2fix.memory", kHeapExtra / 1024);
        return;
    }
    // In front of ds2fix.mods' hook (priority 0) - it adds the largest mod file on top.
    if (g_hooks->HookCallSite(g_self, kHeapSetupCall, kHeapCtor, reinterpret_cast<void*>(&HeapSetup), 200, &g_heapNext,
                              nullptr) != PSE_OK)
        Log(PSE_LOG_ERROR, "[fail] Limits: global heap set-up at 0x%08X not hooked - the bigger pools may not fit (levels "
                           "can lose textures)", kHeapSetupCall);
}

void PSE_CALL OnMessage(const PSEMessage* msg, void*) {
    if (msg->type == PSE_MSG_POST_LOAD) AskForHeap();
}

bool Hook(uint32_t site, uint32_t target, void* fn, void** next) {
    const PSEResult r = g_hooks->HookCallSite(g_self, site, target, fn, 0, next, nullptr);
    if (r != PSE_OK) Log(PSE_LOG_ERROR, "[fail] Limits: call at 0x%08X not hooked (pse.hooks %d)", site, r);
    return r == PSE_OK;
}

#ifndef DS_DIST
// ---- dev: values in memory + a scripted save / load (HKCU\Software\DesertStorm2Fix\Dev) ----------------------------
//   LimitsTestSave = save path without ".sav" (REG_SZ): Save_Game after LimitsTestAfterMs (default 15000) of mission play
//   LimitsTestLoad = save path without ".sav": Save_Load right after (or alone) - the round trips the test runs use.
// Both run with Dev\NoLimits = 1 too (= the unmodified game's saves / loads).
DWORD DevDword(const char* name, DWORD def) {
    DWORD v = 0, size = sizeof v;
    return RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStorm2Fix\\Dev", name, RRF_RT_REG_DWORD, nullptr, &v, &size) ==
                   ERROR_SUCCESS ? v : def;
}
bool DevString(const char* name, wchar_t* out, DWORD chars) {
    DWORD size = chars * sizeof(wchar_t);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\DesertStorm2Fix\\Dev",
                        std::wstring(name, name + std::strlen(name)).c_str(), RRF_RT_REG_SZ, nullptr, out, &size) ==
               ERROR_SUCCESS && out[0];
}

template <class T>
T Read(uint32_t va, uint32_t off = 0) {
    __try {
        const uint8_t* p = *reinterpret_cast<const uint8_t* const*>(va);
        return p ? *reinterpret_cast<const T*>(p + off) : T{};
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return T{};
    }
}

uint32_t LiveParticles() {
    __try {
        uint8_t* m = *reinterpret_cast<uint8_t**>(0x6800B4);
        if (!m) return 0;
        uint32_t n = 0;
        for (uint32_t i = 0; i < At<uint32_t>(m, 0xC); ++i)
            n += (At<uint32_t>(At<uint8_t*>(m, 4) + i * 0xC0, 0xB0) & 0xC0000000u) != 0x40000000u;
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void LogValues(const char* when) {
    Log(PSE_LOG_INFO,
        "[dev] Limits (%s): physics list %u / %u, Karma world %p, projectiles %u, particles %u (live %u), events %u, "
        "render arena %u, A* per frame %u, voices %u + 10 = %u, save buffer %u, global heap %u (used %u), AI %u / %u, "
        "objects soldiers %u vehicles %u",
        when, Read<uint32_t>(0x680090, 8), Read<uint32_t>(0x680090, 4), Read<void*>(0x6897AC), Read<uint32_t>(0x6800E4, 8),
        Read<uint32_t>(0x6800B4, 0xC), LiveParticles(), Read<uint32_t>(0x6800B8, 0x10), Read<uint32_t>(0x680070, 4),
        Read<uint32_t>(0x6800E8, 0x30), Read<uint32_t>(0x6800EC, 0x50), Read<uint32_t>(0x6800EC, 0x54),
        Read<uint32_t>(0x62F458, 0x88), Read<uint32_t>(0x664380, 0x48), Read<uint32_t>(0x664380, 0x50),
        Read<uint32_t>(kSchedulerObj, 0x58), Read<uint32_t>(kSchedulerObj, 0x5C), CountObjects(1), CountObjects(0));
}

wchar_t g_testSave[MAX_PATH], g_testLoad[MAX_PATH];
DWORD g_missionAt = 0, g_lastLog = 0, g_frames = 0;
int g_testStep = 0;   // 0 waiting, 1 done

int PSE_CALL DevLevelEnd(PSEEventId, void* data, void*) {
    const char* name = data ? static_cast<const char*>(data) : "";
    LogValues(name);
    g_missionAt = _stricmp(name, "FrontEnd") ? GetTickCount() : 0;
    return 0;
}

int PSE_CALL DevFrame(PSEEventId, void*, void*) {
    if (!g_missionAt) return 0;
    const DWORD now = GetTickCount();
    ++g_frames;
    if (now - g_lastLog > 10000) {
        if (g_lastLog && g_frames)
            Log(PSE_LOG_INFO, "[dev] Limits: %u frames in %u ms = %.2f ms / frame", g_frames, now - g_lastLog,
                static_cast<double>(now - g_lastLog) / g_frames);
        g_lastLog = now;
        g_frames = 0;
        LogValues("playing");
    }
    if (g_testStep || now - g_missionAt < DevDword("LimitsTestAfterMs", 15000)) return 0;
    if (*reinterpret_cast<volatile uint8_t*>(0x627158) != 1) return 0;   // dispatch mode: mission running
    g_testStep = 1;
    void* saver = *reinterpret_cast<void**>(0x62F458);
    if (!saver) return 0;
    if (g_testSave[0]) {
        LogValues("before save");
        const int r = reinterpret_cast<int(__thiscall*)(void*, const wchar_t*, int)>(0x41C600)(saver, g_testSave, 0);
        Log(PSE_LOG_INFO, "[dev] Limits: save %ls.sav returned %d", g_testSave, r);
    }
    if (g_testLoad[0]) {
        const int r = reinterpret_cast<int(__thiscall*)(void*, const wchar_t*)>(0x41C7C0)(saver, g_testLoad);
        Log(PSE_LOG_INFO, "[dev] Limits: load %ls.sav returned %d", g_testLoad, r);
        LogValues("after load");
    }
    return 0;
}

void DevInstall() {
    DevString("LimitsTestSave", g_testSave, MAX_PATH);
    DevString("LimitsTestLoad", g_testLoad, MAX_PATH);
    g_msg->Subscribe(g_self, PSE_EVENT_LEVEL_LOAD_END, &DevLevelEnd, nullptr, 0);
    g_msg->Subscribe(g_self, PSE_EVENT_FRAME, &DevFrame, nullptr, 0);
}
#endif

#ifndef DS_DIST
bool DevOff() {   // dev A/B: HKCU\Software\DesertStorm2Fix\Dev\NoLimits = 1 leaves every engine limit as shipped
    DWORD v = 0, size = sizeof v;
    return RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStorm2Fix\\Dev", "NoLimits", RRF_RT_REG_DWORD, nullptr, &v,
                        &size) == ERROR_SUCCESS && v;
}
#endif
}  // namespace

// ---- plugin entry points ---------------------------------------------------------------------------------------------
extern "C" {
__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info;
const char* const kGames[] = {"ds2", nullptr};
const PSEPluginInfo PSEPlugin_Info = {sizeof(PSEPluginInfo), 1, "ds2fix.limits", "DesertStorm2Fix - engine limits",
                                      "DesertStorm2Fix", PSE_MAKE_VERSION(0, 1, 0), 0, kGames, PSE_PLUGIN_SIMULATION};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    g_self = pse->plugin;
    g_log = static_cast<const PSELogInterface*>(pse->Query(g_self, PSE_LOG_INTERFACE, 1));
    g_hooks = static_cast<const PSEHooksInterface*>(pse->Query(g_self, PSE_HOOKS_INTERFACE, 1));
    g_msg = static_cast<const PSEMessagingInterface*>(pse->Query(g_self, PSE_MESSAGING_INTERFACE, 1));
    if (!g_log) return 0;
    if (!g_hooks || !g_msg) {
        Log(PSE_LOG_ERROR, "ds2fix.limits: the loader lacks pse.hooks / pse.messaging v1");
        return 0;
    }
#ifndef DS_DIST
    DevInstall();
    if (DevOff()) {
        Log(PSE_LOG_INFO, "[dev] Limits: off (Dev\\NoLimits)");
        return 1;
    }
#endif
    if (!Matches(kPhysicsAdd, kPhysicsAddEntry, sizeof kPhysicsAddEntry) ||
        !Matches(kProjectileCtorCall - 4, kProjectilePush, sizeof kProjectilePush)) {
        Log(PSE_LOG_ERROR, "[fail] Limits: unexpected game code - engine limits left as they were");
        return 1;
    }
    // Save-side wrappers first: they keep the file layout with any pool size (and change nothing with the game's own),
    // so the pools are only raised once all of them are in.
    bool saves = Hook(kEventCountCall, kEventCount, reinterpret_cast<void*>(&EventCount), &g_eventCountNext);
    saves = Hook(kEventSaveCall, kEventSave, reinterpret_cast<void*>(&EventSave), &g_eventSaveNext) && saves;
    saves = Hook(kParticleSaveCall, kParticleSave, reinterpret_cast<void*>(&ParticleSave), &g_particleSaveNext) && saves;
    saves = Hook(kProjectileSaveCall, kProjectileSave, reinterpret_cast<void*>(&ProjectileSave), &g_projectileSaveNext) && saves;
    saves = Hook(kSaveWriteCall, kSaveWrite, reinterpret_cast<void*>(&SaveWrite), &g_saveWriteNext) && saves;
    if (!saves) {   // the loader drops this plugin's hooks with it; nothing raised
        Log(PSE_LOG_ERROR, "[fail] Limits: save hooks not installed - engine limits left as they were");
        return 0;
    }
    PSEBytePatch patches[sizeof kSites / sizeof kSites[0]];
    for (size_t i = 0; i < sizeof kSites / sizeof kSites[0]; ++i)
        patches[i] = {kSites[i].va, kSites[i].original, kSites[i].patched, kSites[i].len, 0,
#ifdef DS_DIST
                      nullptr};
#else
                      kSites[i].name};
#endif
    const PSEResult r = g_hooks->PatchGroup(g_self, "Limits: engine pools", patches, sizeof kSites / sizeof kSites[0]);
    if (r != PSE_OK) {
        Log(PSE_LOG_ERROR, "[fail] Limits: engine pools not patched (pse.hooks %d)", r);
        return 0;
    }
    g_on = true;
    bool ok = g_hooks->HookFunction(g_self, kPhysicsAdd, sizeof kPhysicsAddEntry, kPhysicsAddEntry,
                                    reinterpret_cast<void*>(&PhysicsAdd), 0, &g_physicsAddNext, nullptr) == PSE_OK;
    ok = Hook(kDynCtorCall, kDynCtor, reinterpret_cast<void*>(&DynCtor), &g_dynCtorNext) && ok;
    ok = Hook(kProjectileCtorCall, kProjectileCtor, reinterpret_cast<void*>(&ProjectileCtor), &g_projCtorNext) && ok;
    ok = Hook(kSchedulerCall, kScheduler, reinterpret_cast<void*>(&Scheduler), &g_schedNext) && ok;
    g_msg->RegisterListener(g_self, nullptr, &OnMessage, nullptr);
    Log(ok ? PSE_LOG_INFO : PSE_LOG_ERROR,
        ok ? "[ok]   Limits: raised (constraints 8192, physics objects >= %u, bodies >= %u, collision models >= %u / pairs "
             "16384, projectiles %u, particles 2000, events 2048, render arena 1 MB, decals 512 KB, A* 12, voices 32)"
           : "[fail] Limits: some hooks not installed (physics objects %u, bodies %u, models %u, projectiles %u)",
        kMinObjects, kMinBodies, kMinModels, kProjectiles);
    return 1;
}

__declspec(dllexport) void PSE_CALL PSEPlugin_Unload(void) {}
}
