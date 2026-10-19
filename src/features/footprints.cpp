// Footprints: how long the game's own footprints stay, who leaves them, and wiping them in wind / rain / snow
// (weather.cpp sets the wipe rate). Research: research/weather.md.
//
// The game already leaves prints: a footstep animation event (soldier vtable slot 6 = 0x425050, event type 1) calls
// FUN_00424cc0 (thiscall soldier, event), which - for the squad's soldiers only (vtable +0x1C), on a surface whose
// Default.spl row has flag 0x10 (Sand, Baked Earth, River Bed) - projects LeftPrintDecal / RightPrintDecal
// ([0x60ED90] / [0x60ED94]) down onto the ground through the decal add FUN_004e3af0 (thiscall world [0x63C948],
// 0x4C-byte parameters: +0x24 size 10, +0x2C colour, +0x38 timed 1, +0x3C life 8 s, +0x40 fade 4 s). Flag 0x40 adds a
// dust puff after it. Decals live in a ring buffer (manager [0x63C96C] +0x40, 128 KB - raised by limits, oldest
// evicted), each with a header: +0 alive, +4 flags (bit 0 timed), +8 birth (s, timer [0x63C954] +0x10 / 1000), +0xC
// life, +0x10 fade; the draw ages them (alpha 1 -> 0 over the fade after the life) and kills them at 0.
//
// Here:
// - the squad-only test (0x424CCC: mov eax,[esi]; call [eax+0x1C]; test; je skip - 13 bytes) -> Gate: the game's
//   answer, or with EVERYONE every soldier within kEveryoneRange of a camera (prints are only seen near one, and each
//   costs a projection onto the ground);
// - the add call 0x424E7C -> AddPrint: life / fade / timed from the FOOTPRINTS setting, OFF skips the print (the dust
//   puff stays);
// - the decal store call 0x4E3B7E (FUN_00521da0 -> new decal) -> Store: remembers which decals are prints;
// - each frame, with a wipe rate from the weather (and PRINTS FADE IN WEATHER on), the ring is walked (FUN_004bfe40
//   thiscall(ring, callback(data, size, ctx), ctx)) and prints age faster: birth moves back by rate x game time; a
//   permanent print is made timed first. The game's own fade then takes them away.
// - puddles (weather.cpp, AddPuddle): decals of their own, tagged like the prints - they grow as they age (the decal's
//   growth fields, as blood pools), are kept young while it rains and dry up (fade) after it stops. Their texture is the
//   game's unused TestDecal.png ([0x60ED8C], registered with the prints): the decal part's bind (call 0x520A81 ->
//   renderer SetTexture 0x544220 thiscall(renderer, stage, texture)) swaps in weather.cpp's puddle picture.
// - marks survive cutscenes: MovieStart / MovieEnd (0x486ECC, 0x486F71, 0x487030) emptied the whole decal store
//   (FUN_00521a60 thiscall(manager)) - footprints, puddles, bullet holes and blood vanished after every film. Those
//   three calls do nothing now; the level unload (0x40D9DB) and level set-up (0x4CBB70) still clear it.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
constexpr uint32_t kGateSite = 0x424CCC, kGateSkip = 0x425039;
constexpr uint32_t kAddCall = 0x424E7C, kAddDecal = 0x4E3AF0;
constexpr uint32_t kStoreCall = 0x4E3B7E, kStore = 0x521DA0;
constexpr uint32_t kDecalManager = 0x63C96C, kRingIterate = 0x4BFE40, kDecalTimer = 0x63C954;
constexpr uint32_t kCameras = 0x63C9D8, kRenderer = 0x63C924, kLevelName = 0x606880;
constexpr float kEveryoneRange = 4000.0f;  // cm from a camera
constexpr uint32_t kClear = 0x521A60, kClearCalls[] = {0x486ECC, 0x486F71, 0x487030};
constexpr uint32_t kBindCall = 0x520A81, kBind = 0x544220, kTestDecal = 0x60ED8C;
constexpr uint32_t kWorld = 0x63C948, kPlayerBlock = 0x60F5B8;
constexpr float kPuddleLife = 60.0f, kPuddleFade = 45.0f;   // after the rain: ~5 s, then drying

// FOOTPRINTS: life / fade in seconds (game: 8 + 4).
struct Life {
    float life, fade;
};
constexpr Life kLives[features::kFootprintModes] = {{0, 0}, {8.0f, 4.0f}, {90.0f, 30.0f}, {0, 0}};
const char* const kLabels[features::kFootprintModes] = {"OFF", "GAME (12 S)", "LONG (2 MIN)", "PERMANENT"};
// A permanent print the weather starts wiping: this long (s) at wipe rate 1 before its fade.
constexpr float kWipeLife = 60.0f, kWipeFade = 10.0f;

enum Tag : int { kNone, kPrint, kPuddle };
struct Info {
    int tag;
    float x, y, z, radius, yaw;   // puddles: where they were put
};
std::unordered_map<const void*, Info> g_tags;   // decal -> what it is
Info g_next{};                                   // the decal being added
void* g_lastAdded = nullptr;                     // ... and what it became
std::vector<features::PuddleInfo> g_puddleList;  // alive puddles, from the last ring walk
int g_adding = kNone;
float g_puddleRain = 0.0f;   // weather.cpp: rain now (0 = drying)
int g_puddles = 0;            // alive, counted by the last ring walk
float g_wipeRate = 0.0f;     // extra seconds of age per second (weather.cpp)
uint32_t g_lastMs = 0;
char g_level[32] = {};

uint32_t Mode() { return std::min<uint32_t>(settings::Get().footprints, features::kFootprintModes - 1); }

float DecalNow() {
    const auto* timer = *reinterpret_cast<const uint8_t* const*>(kDecalTimer);
    return timer ? *reinterpret_cast<const uint32_t*>(timer + 0x10) * 0.001f : 0.0f;
}

bool NearCamera(const float* pos, float range) {
    const auto* cams = *reinterpret_cast<const uint8_t* const*>(kCameras);
    const auto* r = *reinterpret_cast<const uint8_t* const*>(kRenderer);
    if (!cams || !r) return false;
    const int views = std::clamp(*reinterpret_cast<const int*>(r + 0x20), 1, 4);
    for (int v = 0; v < views; ++v) {
        const uint32_t id = *reinterpret_cast<const uint32_t*>(cams + 0xEC0C + v * 4);
        if (id >= 128) continue;
        const float* eye = reinterpret_cast<const float*>(cams + 8 + id * 0x1D8 + 0x68);
        const float dx = pos[0] - eye[0], dy = pos[1] - eye[1], dz = pos[2] - eye[2];
        if (dx * dx + dy * dy + dz * dz < range * range) return true;
    }
    return false;
}

// ecx = soldier (FUN_00424cc0 has already moved it to esi); nonzero = go on with the print / dust.
int __fastcall Gate(uint8_t* soldier) {
    using Fn = int(__thiscall*)(void*);
    if (reinterpret_cast<Fn>((*reinterpret_cast<void***>(soldier))[7])(soldier)) return 1;
    return settings::Get().featFootprintsAll && Mode() != 0 && NearCamera(reinterpret_cast<float*>(soldier + 0x30), kEveryoneRange);
}

int __fastcall AddPrint(void* world, void*, uint8_t* params) {
    const uint32_t mode = Mode();
    if (mode == 0) return 0;
    if (mode == 3) {
        *reinterpret_cast<int*>(params + 0x38) = 0;   // untimed: stays until the ring needs the room
    } else {
        *reinterpret_cast<int*>(params + 0x38) = 1;
        *reinterpret_cast<float*>(params + 0x3C) = kLives[mode].life;
        *reinterpret_cast<float*>(params + 0x40) = kLives[mode].fade;
    }
    g_adding = kPrint;
    const int r = reinterpret_cast<int(__thiscall*)(void*, void*)>(kAddDecal)(world, params);
    g_adding = kNone;
    return r;
}

void* __fastcall Store(void* manager, void*) {
    void* decal = reinterpret_cast<void*(__thiscall*)(void*)>(kStore)(manager);
    if (decal) {
        if (g_adding) g_next.tag = g_adding, g_tags[decal] = g_next, g_lastAdded = decal;
        else g_tags.erase(decal);   // a bullet hole / blood pool where a print or puddle once was
    }
    return decal;
}

// Ring callback: data = decal header.
float g_ageBy = 0.0f;
void __cdecl Walk(uint8_t* decal, int, void*) {
    if (!*reinterpret_cast<int*>(decal)) return;
    const auto it = g_tags.find(decal);
    if (it == g_tags.end()) return;
    if (it->second.tag == kPuddle) {   // kept young while it rains (growth runs on age: never reset, only held)
        ++g_puddles;
        float& birth = *reinterpret_cast<float*>(decal + 8);
        const float now = DecalNow(), life = *reinterpret_cast<float*>(decal + 0xC), fade = *reinterpret_cast<float*>(decal + 0x10);
        if (g_puddleRain > 0.15f && now - birth > life - 5.0f) birth = now - (life - 5.0f);
        const Info& in = it->second;
        const float age = now - birth;
        const float opacity = age < life ? 1.0f : fade > 0 ? std::max(0.0f, 1.0f - (age - life) / fade) : 0.0f;
        // spread so far: the decal grows by scaling its UVs - header +0x14 full size, +0x18 start, +0x1C rate, +0x20
        // the current UV scale (shrinks towards +0x14): the picture covers +0x14 / +0x20 of the decal
        const float full = *reinterpret_cast<float*>(decal + 0x14), scale = *reinterpret_cast<float*>(decal + 0x20);
        const float radius = in.radius * (scale > 0.0f ? std::clamp(full / scale, 0.0f, 1.0f) : 1.0f);
#ifndef DS_DIST
        static int logged = 0;
        if (logged < 4 && age > 2.0f * (logged + 1))
            ++logged, dslog::Write("[dev] Puddle decal growth: age %.1f size %.0f header +0x14 %.2f +0x18 %.2f +0x1C %.2f +0x20 %.2f",
                                   age, in.radius, *reinterpret_cast<float*>(decal + 0x14),
                                   *reinterpret_cast<float*>(decal + 0x18), *reinterpret_cast<float*>(decal + 0x1C), scale);
#endif
        g_puddleList.push_back({in.x, in.y, in.z, radius, in.yaw, opacity, decal, *reinterpret_cast<const uint8_t* const*>(decal + 0x2C)});
        return;
    }
    if (g_ageBy <= 0.0f) return;
    uint32_t& flags = *reinterpret_cast<uint32_t*>(decal + 4);
    float& birth = *reinterpret_cast<float*>(decal + 8);
    if (!(flags & 1)) {   // permanent: give it a life to wipe away
        flags |= 1;
        birth = DecalNow();
        *reinterpret_cast<float*>(decal + 0xC) = kWipeLife;
        *reinterpret_cast<float*>(decal + 0x10) = kWipeFade;
    }
    birth -= g_ageBy;
}
}  // namespace

namespace {
void __fastcall KeepMarks(void*) {}   // cutscene start / end: the store is left as it is

// The puddle picture in place of TestDecal.png at the decal part's bind.
void __fastcall Bind(void* renderer, void*, int stage, void* texture) {
    if (texture) {
        const auto* record = *reinterpret_cast<const uint8_t* const*>(kTestDecal);
        if (record && texture == *reinterpret_cast<void* const*>(record + 0x10))
            if (void* puddle = features::WeatherPuddleTexture()) texture = puddle;
    }
    reinterpret_cast<void(__thiscall*)(void*, int, void*)>(kBind)(renderer, stage, texture);
}
}  // namespace

void features::SetPuddleRain(float rain) { g_puddleRain = rain; }
int features::PuddleCount() { return g_puddles; }

// The decal is still ours and alive (not evicted / reused / faded out).
bool features::DecalAlive(const void* decal) {
    return decal && g_tags.count(decal) && *static_cast<const int*>(decal) != 0;
}

// Takes one of our decals away at once (FUN_00521e50 thiscall(decal, 0): every part hidden, the decal dead).
void features::KillDecal(void* decal) {
    if (!DecalAlive(decal)) return;
    reinterpret_cast<void(__thiscall*)(void*, int)>(0x521E50)(decal, 0);
    g_tags.erase(decal);
}
const std::vector<features::PuddleInfo>& features::Puddles() { return g_puddleList; }

// A puddle centred at (x, y, z) on the ground (y = the surface), radius cm. False = no decal (decals off, nothing
// under it ...).
void* features::AddPuddle(float x, float y, float z, float radius, float yaw, float startSize) {
    const auto* record = *reinterpret_cast<void* const*>(kTestDecal);
    auto* world = *reinterpret_cast<void**>(kWorld);
    const auto* soldier = *reinterpret_cast<const uint8_t* const*>(kPlayerBlock + 0x310);
    auto* manager = *reinterpret_cast<uint8_t**>(kDecalManager);
    if (!record || !world || !soldier || !manager || !*reinterpret_cast<int*>(manager + 0x44)) return nullptr;
    void* node = *reinterpret_cast<void* const*>(soldier + 0x1C);   // any scene node: the add climbs to the root
    if (!node) return nullptr;
    alignas(4) uint8_t p[0x4C] = {};
    auto F = [&](uint32_t off) -> float& { return *reinterpret_cast<float*>(p + off); };
    auto I = [&](uint32_t off) -> int& { return *reinterpret_cast<int*>(p + off); };
    *reinterpret_cast<const void**>(p + 0) = record;
    *reinterpret_cast<void**>(p + 4) = node;
    F(8) = x, F(0xC) = y + 40.0f, F(0x10) = z;
    F(0x14) = yaw;
    F(0x18) = 0, F(0x1C) = -1.0f, F(0x20) = 0;
    F(0x24) = radius;
    F(0x28) = 100.0f;
    I(0x2C) = -1;
    F(0x30) = radius * std::clamp(startSize, 0.05f, 1.0f), F(0x34) = 0.9f;   // grows like a blood pool
    I(0x38) = 1;
    F(0x3C) = kPuddleLife, F(0x40) = kPuddleFade;
    I(0x44) = 1;
    g_next = {kPuddle, x, y, z, radius, yaw};
    g_lastAdded = nullptr;
    g_adding = kPuddle;
    reinterpret_cast<int(__thiscall*)(void*, void*)>(kAddDecal)(world, p);
    g_adding = kNone;
    return g_lastAdded;
}

const char* features::FootprintLabel(int mode) { return kLabels[std::clamp(mode, 0, kFootprintModes - 1)]; }

void features::SetFootprintWipe(float rate) { g_wipeRate = std::max(0.0f, rate); }

void features::OnFrameFootprints() {
    const char* level = reinterpret_cast<const char*>(kLevelName);
    if (strncmp(level, g_level, sizeof g_level - 1) != 0) {   // new level: a new ring
        strncpy_s(g_level, level, sizeof g_level - 1);
        g_tags.clear();
        g_puddleList.clear();
    }
    const auto* timer = *reinterpret_cast<const uint8_t* const*>(kDecalTimer);
    if (!timer) return;
    const uint32_t ms = *reinterpret_cast<const uint32_t*>(timer + 0x10);
    const uint32_t dms = ms - g_lastMs;
    g_lastMs = ms;
    if (dms == 0 || dms > 500 || g_tags.empty()) return;
    auto* manager = *reinterpret_cast<uint8_t**>(kDecalManager);
    if (!manager || !*reinterpret_cast<int*>(manager + 0x44)) return;
    void* ring = *reinterpret_cast<void**>(manager + 0x40);
    if (!ring) return;
    g_ageBy = settings::Get().featFootprintsWeather ? g_wipeRate * dms * 0.001f : 0.0f;
    g_puddles = 0;
    g_puddleList.clear();
    reinterpret_cast<void(__thiscall*)(void*, void(__cdecl*)(uint8_t*, int, void*), void*)>(kRingIterate)(ring, &Walk, nullptr);
    if (g_tags.size() > 20000) g_tags.clear();
}

void features::ApplyFootprints() {
    // mov eax,[esi]; call [eax+0x1C]; test eax,eax; je 0x425039
    const uint8_t original[13] = {0x8B, 0x06, 0xFF, 0x50, 0x1C, 0x85, 0xC0, 0x0F, 0x84, 0x60, 0x03, 0x00, 0x00};
    uint8_t gate[13] = {0xE8, 0, 0, 0, 0, 0x85, 0xC0, 0x0F, 0x84, 0, 0, 0, 0};
    const int32_t toGate = static_cast<int32_t>(reinterpret_cast<uint32_t>(&Gate) - (kGateSite + 5));
    const int32_t toSkip = static_cast<int32_t>(kGateSkip - (kGateSite + 13));
    std::memcpy(gate + 1, &toGate, 4);
    std::memcpy(gate + 9, &toSkip, 4);
    // call Gate; test eax,eax; je skip: one byte group (the broker checks the original bytes).
    const BytePatch gatePatch[] = {{DS_PATCH_NAME("footprint gate"), kGateSite, original, gate, sizeof gate, false}};
    bool ok = patch::ApplyGroup("Footprints: gate", gatePatch) &&
                    patch::HookCall(kAddCall, reinterpret_cast<const void*>(&AddPrint), kAddDecal) &&
                    patch::HookCall(kStoreCall, reinterpret_cast<const void*>(&Store), kStore) &&
                    patch::HookCall(kBindCall, reinterpret_cast<const void*>(&Bind), kBind);
    for (uint32_t site : kClearCalls) ok = patch::HookCall(site, reinterpret_cast<const void*>(&KeepMarks), kClear) && ok;
    dslog::Write(ok ? "[ok]   Footprints: life / everyone / weather wipe" : "[fail] Footprints: unexpected code - not changed");
}
