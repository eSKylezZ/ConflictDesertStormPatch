// Engine limits raised for big battles (100 vs 100 soldiers, 50+ tanks). The plain value changes are the LIMITS group
// in scripts/patch_defs.py (Karma bodies / constraints / collision pairs, particles, event queue, render arena,
// script-id table, waypoints, save buffer, AI think budget, A* searches, sound voices); this file has what needs code.
// Research: research/large_battles.md.
//
// Physics-object list [0x63C94C] (thiscall ctor 0x4FDF70(capacity) from 0x4CB4F9 / 0x4CC0B6 - made once, it also creates
// the Karma world): every Karma-simulated object (vehicles 0x438D8B / 0x43933D / 0x439690 / 0x439784, dynamic props
// 0x41BE64) is added by 0x4FE4C0 `arr[count++] = obj` with no bounds check, and removing (0x4FE4E0) only nulls the
// slot - the count never goes down, so 100 additions over all levels played wrote past the 400-byte array. Now 4096
// slots, the add reuses free slots and refuses (logged) when all are taken. The step loop 0x4FE5CB skips null slots.
//
// Projectiles [0x63C9A4] (ctor 0x4F3F50(count, gravity) at 0x4104A8, 0xA0 B each from the Global Heap at level init):
// 50 shells / rockets / grenades in flight - a shot past that was silently not fired. -> 1024.
//
// Event queue saves: the number of saved events is one byte of the save header (0x415B50 `mov [esi+0x1C], al` from the
// counter 0x4DBA80) while the writer 0x4DBAC0 writes all of them - with more than 255 a save loaded misaligned. The
// counter and writer calls are wrapped to save at most 255 (the queue is 2048 now).
//
// Save files: the buffer is 4 MB bigger (patch_defs), the file write (fwrite at 0x4156ED, whole buffer) writes up to the
// last non-zero byte (the buffer is zeroed before the sections are written), never less than the original size.
//
// Vehicle think budget: the scheduler [0x60EDA0] thinks a fixed +0x54 = 2 vehicles per frame (50 tanks = each one every
// 25 frames). Set per frame from the vehicle count: max(2, ceil(n / 15)), at most 16 - unchanged up to 30 vehicles.
// Depends only on game state, so online PCs agree.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "generated/patch_tables.h"

namespace {
constexpr uint32_t kPhysicsCtor = 0x4FDF70, kPhysicsCtorCalls[] = {0x4CB4F9, 0x4CC0B6};
constexpr uint32_t kPhysicsAdd = 0x4FE4C0;
constexpr uint32_t kPhysicsSlots = 4096;
const uint8_t kPhysicsAddEntry[] = {0x8B, 0x41, 0x08, 0x8B, 0x51, 0x0C};   // mov eax,[ecx+8]; mov edx,[ecx+0xC]

constexpr uint32_t kProjectileCtor = 0x4F3F50, kProjectileCtorCall = 0x4104A8;
constexpr uint32_t kProjectiles = 1024;
const uint8_t kProjectilePush[] = {0x6A, 0x32, 0x8B, 0xC8};   // push 50; mov ecx, eax (before the call)

constexpr uint32_t kEventCount = 0x4DBA80, kEventCountCall = 0x415B4B;
constexpr uint32_t kEventSave = 0x4DBAC0, kEventSaveCall = 0x4158D8;
constexpr uint32_t kEventSaveable = 0x4DB8B0, kEventWrite = 0x4DB8E0;
constexpr uint32_t kEventSize = 0x48, kMaxSavedEvents = 255;

constexpr uint32_t kSaveWriteCall = 0x4156ED, kFwrite = 0x567203, kSaveExtraCount = 0x4FD840;
constexpr uint32_t kSaveBase = 0x2AF53;

constexpr uint32_t kScheduler = 0x60EDA0, kCharacters = 0x63C9A0, kTeamList = 0x4C9720;
constexpr uint32_t kWorld = 0x63C948;
constexpr float kMinWorldHeight = 10000.0f;   // cm
constexpr float kFlatWorldHeight = 2000.0f;   // only boxes flatter than this are raised (shipped levels keep theirs)

bool g_on = false;

// ---- physics-object list ----
void* __fastcall PhysicsCtor(void* mgr, void*, uint32_t) {
    return reinterpret_cast<void*(__thiscall*)(void*, uint32_t)>(kPhysicsCtor)(mgr, kPhysicsSlots);
}

void __fastcall PhysicsAdd(uint8_t* mgr, void*, void* object) {
    const uint32_t cap = *reinterpret_cast<uint32_t*>(mgr + 4);
    uint32_t& count = *reinterpret_cast<uint32_t*>(mgr + 8);
    void** slots = *reinterpret_cast<void***>(mgr + 0xC);
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
        dslog::Write("[fail] Limits: all %u physics slots in use - object %p not simulated", cap, object);
    }
}

// ---- projectiles ----
void* __fastcall ProjectileCtor(void* mgr, void*, uint32_t, float gravity) {
    return reinterpret_cast<void*(__thiscall*)(void*, uint32_t, float)>(kProjectileCtor)(mgr, kProjectiles, gravity);
}

// ---- event queue in saves ----
int __fastcall EventCount(void* queue, void*) {
    const int n = reinterpret_cast<int(__thiscall*)(void*)>(kEventCount)(queue);
    return std::min<int>(n, kMaxSavedEvents);
}

uint8_t* __fastcall EventSave(uint8_t* queue, void*, uint8_t* cursor, uint32_t) {
    using Saveable = int(__thiscall*)(void*, void*);
    using Write = uint8_t*(__thiscall*)(void*, uint8_t*, void*);
    void* filter = *reinterpret_cast<void**>(queue);
    uint8_t* slots = *reinterpret_cast<uint8_t**>(queue + 8);
    const uint32_t n = *reinterpret_cast<uint32_t*>(queue + 0x10);
    uint32_t written = 0;
    for (uint32_t i = 0; i < n && slots; ++i) {
        uint8_t* e = slots + i * kEventSize;
        if (!reinterpret_cast<Saveable>(kEventSaveable)(e, filter)) continue;
        if (written++ >= kMaxSavedEvents) continue;   // the header counts them in a byte
        cursor = reinterpret_cast<Write>(kEventWrite)(e, cursor, filter);
    }
    if (written > kMaxSavedEvents)
        dslog::Write("Limits: %u pending events - saved the first %u", written, kMaxSavedEvents);
    return cursor;
}

// ---- particles / projectiles in saves ----
// The save writers 0x4F2430 (particles, 0xC8-byte slots) and 0x4F50D0 (projectiles, 0xA0) write EVERY slot, the loaders
// read a fixed 250 / 50 (push 0xFA / push 0x32 at 0x416388 / 0x416399) - with the bigger pools a save's later sections
// were read from the wrong place. Wrapped: the vanilla number of slots, live ones first (particle +0xB0 = 1 free,
// projectile +4 = 0x64 free), so the file layout stays the game's.
constexpr uint32_t kParticleSaveCall = 0x4158BE, kParticleSave = 0x4F2430, kParticleWrite = 0x4F2140;
constexpr uint32_t kProjectileSaveCall = 0x4158CA, kProjectileSave = 0x4F50D0, kProjectileWrite = 0x4F4C70;
constexpr uint32_t kSavedParticles = 250, kSavedProjectiles = 50;

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

uint8_t* __fastcall ParticleSave(uint8_t* mgr, void*, uint8_t* cursor) {
    using Write = uint8_t*(__thiscall*)(void*, uint8_t*);
    return SaveSlots(*reinterpret_cast<uint8_t**>(mgr + 4), *reinterpret_cast<uint32_t*>(mgr + 0xC), 0xC8, kSavedParticles,
                     [](uint8_t* s) { return *reinterpret_cast<uint32_t*>(s + 0xB0) != 1; },
                     [](uint8_t* s, uint8_t* c) { return reinterpret_cast<Write>(kParticleWrite)(s, c); }, cursor);
}

uint8_t* __fastcall ProjectileSave(uint8_t* mgr, void*, uint8_t* cursor) {
    using Write = uint8_t*(__thiscall*)(void*, uint8_t*, uint32_t);
    const uint32_t extra = *reinterpret_cast<uint32_t*>(mgr + 0x20);
    return SaveSlots(*reinterpret_cast<uint8_t**>(mgr), *reinterpret_cast<uint32_t*>(mgr + 8), 0xA0, kSavedProjectiles,
                     [](uint8_t* s) { return *reinterpret_cast<uint32_t*>(s + 4) != 0x64; },
                     [extra](uint8_t* s, uint8_t* c) { return reinterpret_cast<Write>(kProjectileWrite)(s, c, extra); },
                     cursor);
}

// ---- object counts in saves ----
// The save header's object counts per type (saver [0x60EDC4] +8..+0x18) are taken ONCE at level init (FUN_00415530 from
// 0x40CB1C), but the object walk (FUN_004c8e90 from 0x41589E) writes a record for every object there is at save time;
// the loader 0x416120 reads exactly the header's count per type (records looked up by id, unknown ids skipped by a fixed
// size). An object made after level init (photo mode's spawned soldiers' weapons, a loadout swap's new gun) made the
// type's section longer than its count -> every later record read from the wrong place, the load jumped into heap
// data. Recounted right before the sections are written (same lists, same mask 0x20000 as the walk), so the header
// matches the file; the extra objects' records (id -1 or ids a freshly loaded level doesn't have) are skipped on load -
// which needed the weapon skip size fixed too (below).
constexpr uint32_t kSaveSectionsCall = 0x4156AB, kSaveSections = 0x415850, kCountObjects = 0x4C9100;
constexpr uint32_t kSaveMask = 0x20000;
struct CountSlot { uint32_t offset; int type; };
constexpr CountSlot kSaveCounts[] = {{8, 1}, {0xA, 0}, {0xC, 2}, {0xE, 3}, {0x10, 5}, {0x12, 6}, {0x14, 7}, {0x18, 9}};

uint8_t* __fastcall SaveSections(uint8_t* saver, void*, uint8_t* buf) {
    if (void* mgr = *reinterpret_cast<void**>(kCharacters)) {
        using Count = int(__thiscall*)(void*, int, int, int, uint32_t);
        for (const CountSlot& c : kSaveCounts) {
            const int n = reinterpret_cast<Count>(kCountObjects)(mgr, c.type, 3, -1, kSaveMask);
            uint16_t& saved = *reinterpret_cast<uint16_t*>(saver + c.offset);
            if (n == saved || n < 0 || n > 0xFFFF) continue;
            dslog::Write("Limits: save - %d object(s) of type %d (level start: %u)", n, c.type, saved);
            saved = static_cast<uint16_t>(n);
        }
    }
    return reinterpret_cast<uint8_t*(__thiscall*)(void*, uint8_t*)>(kSaveSections)(saver, buf);
}

// The loader skips the record of an object it can't find by a fixed size per type - right for every type but weapons
// (type 6): 0x2C where the record is 0x8C (base 0x60 FUN_00416db0 + 0x14 FUN_004179f0 + 0x18 FUN_00417a40), so one
// unknown weapon id shifted everything after it. Never hit by the game (all its saved objects exist after a reload).
// The je around the restore call is NOPed and the call wrapped: no object -> skip 0x8C.
constexpr uint32_t kWeaponSkipJe = 0x4162A9, kWeaponRestoreCall = 0x4162AE, kWeaponRestore = 0x417A40;
constexpr uint32_t kWeaponRecord = 0x8C;
const uint8_t kWeaponSkipJeBytes[] = {0x74, 0x0C};   // je 0x4162B7 (add esi, 0x2C)

uint8_t* __fastcall WeaponRestore(void* weapon, void*, uint8_t* record) {
    if (!weapon) return record + kWeaponRecord;
    return reinterpret_cast<uint8_t*(__thiscall*)(void*, uint8_t*)>(kWeaponRestore)(weapon, record);
}

// ---- save file size ----
size_t __cdecl SaveWrite(const void* buf, size_t size, size_t count, FILE* f) {
    const uint32_t original = kSaveBase + 0x280 + 15 * reinterpret_cast<uint32_t(__cdecl*)()>(kSaveExtraCount)();
    const uint8_t* b = static_cast<const uint8_t*>(buf);
    size_t used = size;
    while (used > original && !b[used - 1]) --used;
    if (used > original)
        dslog::Write("Limits: save game %u bytes (original buffer %u)", static_cast<unsigned>(used), original);
    return reinterpret_cast<size_t(__cdecl*)(const void*, size_t, size_t, FILE*)>(kFwrite)(buf, used, count, f);
}

// ---- vehicle think budget ----
uint32_t CountVehicles() {
    void* mgr = *reinterpret_cast<void**>(kCharacters);
    if (!mgr) return 0;
    uint32_t n = 0;
    for (int team = 0; team < 4; ++team)
        for (auto* o = reinterpret_cast<uint8_t*(__thiscall*)(void*, int, int)>(kTeamList)(mgr, team, 0); o && n < 100000;
             o = *reinterpret_cast<uint8_t**>(o + 0x14))
            ++n;
    return n;
}
}  // namespace

uint32_t features::LimitsHeapExtra() {
    // projectiles 1024 x 0xA0, particles +350 000, render arena +960 KB, events +1848 x 0x48 = 1.6 MB, decal store
    // +384 KB, plus slack
    return g_on ? 2560u * 1024 : 0;
}

void features::OnFrameLimits() {
    if (!g_on) return;
    // World box (world +0x1A7C min x/y/z, +0x1A88 max, from the level geometry): a ground vehicle's think FUN_0043f750
    // takes it out of play - and its render instance hidden - once its position leaves the box shrunk by 1 m
    // (FUN_004c9d30; aircraft types 5/6/9/10 exempt). On a flat level the box is ~2 m tall, so tanks standing on the
    // ground counted as outside (FlatBattle: 59 of 60 vehicles parked and invisible). A box under 20 m tall is raised to
    // 100 m; taller ones (every shipped level, e.g. Mission 9 = 50 m) are left alone - the box also feeds ground rays.
    if (uint8_t* world = *reinterpret_cast<uint8_t**>(kWorld)) {
        const float minY = *reinterpret_cast<float*>(world + 0x1A80);
        float& maxY = *reinterpret_cast<float*>(world + 0x1A8C);
        if (maxY > minY && maxY - minY < kFlatWorldHeight) maxY = minY + kMinWorldHeight;
    }
    uint8_t* sched = *reinterpret_cast<uint8_t**>(kScheduler);
    if (!sched) return;
    const uint32_t vehicles = CountVehicles();
    *reinterpret_cast<uint32_t*>(sched + 0x54) = std::clamp<uint32_t>((vehicles + 14) / 15, 2, 16);
}

void features::ApplyLimits() {
    static bool done = false;
    if (done) return;
    done = true;
#ifndef DS_DIST
    {   // dev A/B: Dev\NoLimits = 1 leaves every engine limit as shipped
        DWORD v = 0, size = sizeof v;
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "NoLimits", RRF_RT_REG_DWORD, nullptr, &v,
                         &size) == ERROR_SUCCESS && v) {
            dslog::Write("[dev] Limits: off (Dev\\NoLimits)");
            return;
        }
    }
#endif
    if (!patch::Matches(kPhysicsAdd, kPhysicsAddEntry, sizeof kPhysicsAddEntry) ||
        !patch::Matches(kProjectileCtorCall - 4, kProjectilePush, sizeof kProjectilePush)) {
        dslog::Write("[fail] Limits: unexpected game code - engine limits left as they were");
        return;
    }
    // Save-side wrappers first: they keep the file layout with any pool size (and change nothing with the game's own),
    // so the pools are only raised once all of them are in.
    bool saves = patch::HookCall(kEventCountCall, reinterpret_cast<const void*>(&EventCount), kEventCount);
    saves = patch::HookCall(kEventSaveCall, reinterpret_cast<const void*>(&EventSave), kEventSave) && saves;
    saves = patch::HookCall(kParticleSaveCall, reinterpret_cast<const void*>(&ParticleSave), kParticleSave) && saves;
    saves = patch::HookCall(kProjectileSaveCall, reinterpret_cast<const void*>(&ProjectileSave), kProjectileSave) && saves;
    saves = patch::HookCall(kSaveWriteCall, reinterpret_cast<const void*>(&SaveWrite), kFwrite) && saves;
    saves = patch::HookCall(kSaveSectionsCall, reinterpret_cast<const void*>(&SaveSections), kSaveSections) && saves;
    if (patch::Matches(kWeaponSkipJe, kWeaponSkipJeBytes, sizeof kWeaponSkipJeBytes) &&
        patch::HookCall(kWeaponRestoreCall, reinterpret_cast<const void*>(&WeaponRestore), kWeaponRestore)) {
        static const uint8_t nops[] = {0x90, 0x90};
        const BytePatch skip[] = {{DS_PATCH_NAME("je over the weapon restore"), kWeaponSkipJe, kWeaponSkipJeBytes, nops,
                                   sizeof nops, false}};
        saves = patch::ApplyGroup("Limits: weapon restore", skip) && saves;
    } else {
        saves = false;
    }
    if (!saves) {
        dslog::Write("[fail] Limits: save hooks not installed - engine limits left as they were");
        return;
    }
    if (!patch::ApplyGroup("Limits: engine pools", gen::kLimits)) return;
    g_on = true;   // the Global Heap gets room for the bigger pools (memory.cpp)
    // FUN_004fe4c0 replaced (an entry hook that never runs the original).
    bool ok = patch::HookFunction(kPhysicsAdd, reinterpret_cast<const void*>(&PhysicsAdd), nullptr, kPhysicsAddEntry,
                                  sizeof kPhysicsAddEntry);
    for (uint32_t site : kPhysicsCtorCalls) ok = patch::HookCall(site, reinterpret_cast<const void*>(&PhysicsCtor), kPhysicsCtor) && ok;
    ok = patch::HookCall(kProjectileCtorCall, reinterpret_cast<const void*>(&ProjectileCtor), kProjectileCtor) && ok;
    dslog::Write(ok ? "Limits: raised (bodies 2048, constraints 8192, physics objects %u, projectiles %u, events 2048, "
                      "waypoints 8192, script ids 65534)"
                    : "[fail] Limits: some hooks not installed (physics objects %u, projectiles %u)",
                 kPhysicsSlots, kProjectiles);
}
