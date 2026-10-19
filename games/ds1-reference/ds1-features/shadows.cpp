// Shadows through walls and floors (an original engine bug).
//
// Soldiers and objects cast stencil shadow volumes (GAMELIB\RenderHL\Shadows.cpp): each frame the triangles facing
// the sun are found, their outline edges are pulled away from the sun into quads, the quads go into the stencil
// buffer (FUN_00523bf0 / FUN_00523db0: front faces +1, back faces -1) and one full-screen quad darkens every pixel
// with stencil >= 1 (FUN_00524110). Every outline point was pulled a fixed 9000 cm (90 m) along the light direction
// - through any floor, wall or building in the way, and walls / buildings cast no volumes of their own. So a soldier
// upstairs shadowed the room below, and shadows came out on the far side of walls.
//
// Both volume builders are replaced by copies that pull each outline point only down to a plane a little below the
// caster's lowest point (world "down" from the caster's world matrix, so rotated objects work too). The shadow still
// reaches the ground under the caster and nearby walls, but not through floors or far through buildings. With the
// sun near the horizon (no useful downward direction) the old length is kept.
//   Objects   FUN_005230d0 (fastcall, this = shadow mesh: +0x80 faces {packed normal, s16 neighbours +4/+6/+8},
//             +0x84 vertices float[3], +0x88 index count, +0x90 enabled; indices FUN_005256a0 / unlock FUN_005256e0;
//             output pointers +0x70..+0x7c). Called once from FUN_00520260 (esi = render entry, +8 world matrix);
//             light = [0x6471fc] (local space, set by FUN_00523070), length [0x5fc398].
//   Soldiers  FUN_00523440 (thiscall(mesh, light, vertices), ret 8; mesh: [this] +0x18 index count, +0x20 face
//             records 0x20 B {s16 neighbours +6/+8/+0xa, lit flag +0x1c}; vertices 0x24 B; indices FUN_0052a8b0 /
//             unlock FUN_0052a8c0; output pointers this+0x3c..+0x48), reached from FUN_005032d0 (edi = object, world
//             matrix +0x10c) via the call at 0x503333; length [0x5d8d20].
// Output (both): quads {a, b, a + L*s, b + L*s} at 0x6497d8 (12-byte vertices), indices 0,1,2, 2,1,3 at 0x647258,
// count [0x647208]; the buffers hold 800 quads (the originals did not check).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kObjectBuilder = 0x5230D0;
constexpr uint32_t kSoldierBuilder = 0x523440;
constexpr uint32_t kSoldierDrawCall = 0x503333;  // call FUN_00529a30 (skin + shadow volume), edi = object
constexpr uint32_t kSoldierDraw = 0x529A30;
constexpr uint32_t kObjectLight = 0x6471FC;      // float[3]
constexpr uint32_t kObjectLength = 0x5FC398;     // 9000.0
constexpr uint32_t kSoldierLength = 0x5D8D20;    // 9000.0
constexpr uint32_t kNormalScale = 0x5D9A68;      // packed normal byte -> float
constexpr uint32_t kQuadCount = 0x647208;
constexpr uint32_t kVertexOut = 0x6497D8;
constexpr uint32_t kIndexOut = 0x647258;
constexpr int kMaxQuads = 800;
constexpr int kMaxFaces = 4096;
constexpr uint32_t kObjectMatrix = 0x10C;        // soldier object: world matrix

// How far below the caster's lowest point the volume ends (cm). Soldiers stand on the ground; objects (weapons in
// hand, props) can be well above it.
constexpr float kSoldierMargin = 120.0f;
constexpr float kObjectMargin = 200.0f;

bool g_enabled = true;
const float* g_soldierMatrix = nullptr;  // world matrix of the soldier being drawn (set at kSoldierDrawCall)

using LockFn = const uint16_t*(__thiscall*)(void*);
using UnlockFn = void(__thiscall*)(void*);

// Extrusion of one volume: a point v moves by L * Length(v), ending on the plane h = floor, h(v) = dot(v, up) + upT
// (world height of a local point).
struct Extrusion {
    float light[3];
    float length;          // the game's fixed multiplier (9000)
    float up[3], upT;      // world height of a local point
    float drop;            // world height lost per unit of the multiplier (dot(L, up) negated)
    float floor;
    bool clip;

    void Prepare(const float* L, float fixedLength, const float* world) {
        std::memcpy(light, L, sizeof light);
        length = fixedLength;
        clip = false;
        if (!g_enabled || !world) return;
        // D3D row vectors: world = local * M, so world y = x*M[0][1] + y*M[1][1] + z*M[2][1] + M[3][1].
        up[0] = world[1], up[1] = world[5], up[2] = world[9], upT = world[13];
        drop = -(L[0] * up[0] + L[1] * up[1] + L[2] * up[2]);
        const float len = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]) *
                          std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
        clip = len > 0 && drop > 0.1f * len;  // sun at least ~6 degrees up
        floor = 1e30f;
    }
    float Height(const float* v) const { return v[0] * up[0] + v[1] * up[1] + v[2] * up[2] + upT; }
    void Include(const float* v) {
        if (clip) floor = std::min(floor, Height(v));
    }
    void Finish(float margin) { floor -= margin; }
    float Multiplier(const float* v) const {
        if (!clip) return length;
        return std::clamp((Height(v) - floor) / drop, 0.0f, length);
    }
};

struct Output {
    float* vertex;
    uint16_t* index;
    int& count = *reinterpret_cast<int*>(kQuadCount);

    Output() : vertex(reinterpret_cast<float*>(kVertexOut)), index(reinterpret_cast<uint16_t*>(kIndexOut)) { count = 0; }

    // Edge a -> b of a lit face whose neighbour across it is unlit (same vertex order as the game).
    void Edge(const Extrusion& e, const float* a, const float* b) {
        if (count >= kMaxQuads) return;
        const float sa = e.Multiplier(a), sb = e.Multiplier(b);
        for (int i = 0; i < 3; ++i) {
            vertex[i] = b[i];
            vertex[3 + i] = a[i];
            vertex[6 + i] = b[i] + e.light[i] * sb;
            vertex[9 + i] = a[i] + e.light[i] * sa;
        }
        const auto base = static_cast<uint16_t>(count * 4);
        const uint16_t quad[6] = {base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2),
                                  static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 1),
                                  static_cast<uint16_t>(base + 3)};
        std::memcpy(index, quad, sizeof quad);
        vertex += 12;
        index += 6;
        ++count;
    }
};

uint8_t g_lit[kMaxFaces];

// FUN_005230d0 for an object's shadow mesh; `entry` = the render-list entry (+8 world matrix).
void BuildObjectVolume(uint8_t* mesh, const uint8_t* entry) {
    Output out;
    if (!*reinterpret_cast<int*>(mesh + 0x90)) return;
    const auto indices = reinterpret_cast<LockFn>(0x5256A0)(mesh);
    const auto faces = *reinterpret_cast<const uint8_t* const*>(mesh + 0x80);
    const auto vertices = *reinterpret_cast<const float* const*>(mesh + 0x84);
    const int n = std::min(*reinterpret_cast<const int*>(mesh + 0x88) / 3, kMaxFaces);
    const auto light = reinterpret_cast<const float*>(kObjectLight);
    const float scale = *reinterpret_cast<const float*>(kNormalScale);
    const float* world = entry ? *reinterpret_cast<const float* const*>(entry + 8) : nullptr;

    Extrusion e;
    e.Prepare(light, *reinterpret_cast<const float*>(kObjectLength), world);
    for (int f = 0; f < n; ++f) {
        const auto* packed = reinterpret_cast<const int8_t*>(faces + f * 12);
        const float d = light[2] * (packed[2] * scale) + light[1] * (packed[1] * scale) + light[0] * (packed[0] * scale);
        g_lit[f] = d <= 0.0f;  // facing the sun (L points away from it)
        for (int k = 0; k < 3; ++k) e.Include(vertices + indices[f * 3 + k] * 3);
    }
    e.Finish(kObjectMargin);
    auto* ptrs = reinterpret_cast<void**>(mesh + 0x70);  // +0x70/+0x74 vertices start/end, +0x78/+0x7c indices
    ptrs[0] = ptrs[1] = reinterpret_cast<void*>(kVertexOut);
    ptrs[2] = ptrs[3] = reinterpret_cast<void*>(kIndexOut);
    for (int f = 0; f < n; ++f) {
        if (!g_lit[f]) continue;
        const auto* nb = reinterpret_cast<const int16_t*>(faces + f * 12 + 4);
        const uint16_t* t = indices + f * 3;
        for (int k = 0; k < 3; ++k) {
            const int other = nb[k];
            if (other != -1 && other < n && !g_lit[other])
                out.Edge(e, vertices + t[k] * 3, vertices + t[(k + 1) % 3] * 3);
        }
    }
    ptrs[1] = out.vertex, ptrs[3] = out.index;
    reinterpret_cast<UnlockFn>(0x5256E0)(mesh);
}

// FUN_00523440 for a soldier's skinned mesh (vertices 0x24 bytes, position first).
void BuildSoldierVolume(int* self, const float* light, const uint8_t* verts) {
    Output out;
    const auto* data = reinterpret_cast<const uint8_t*>(*self);
    const int n = std::min(*reinterpret_cast<const int*>(data + 0x18) / 3, kMaxFaces);
    auto* records = *reinterpret_cast<uint8_t* const*>(data + 0x20);
    const auto indices = reinterpret_cast<LockFn>(0x52A8B0)(self);
    auto V = [verts](uint16_t i) { return reinterpret_cast<const float*>(verts + i * 0x24); };

    Extrusion e;
    e.Prepare(light, *reinterpret_cast<const float*>(kSoldierLength), g_soldierMatrix);
    for (int f = 0; f < n; ++f) {
        const float* p0 = V(indices[f * 3]);
        const float* p1 = V(indices[f * 3 + 1]);
        const float* p2 = V(indices[f * 3 + 2]);
        const float ax = p0[0] - p1[0], ay = p0[1] - p1[1], az = p0[2] - p1[2];
        const float bx = p0[0] - p2[0], by = p0[1] - p2[1], bz = p0[2] - p2[2];
        const float d = (az * bx - bz * ax) * light[1] + (by * ax - ay * bx) * light[2] + (bz * ay - by * az) * light[0];
        records[f * 0x20 + 0x1c] = d < 0.0f;
        e.Include(p0), e.Include(p1), e.Include(p2);
    }
    e.Finish(kSoldierMargin);
    self[0xf] = self[0x10] = static_cast<int>(kVertexOut);
    self[0x11] = self[0x12] = static_cast<int>(kIndexOut);
    for (int f = 0; f < n; ++f) {
        if (!records[f * 0x20 + 0x1c]) continue;
        const auto* nb = reinterpret_cast<const int16_t*>(records + f * 0x20 + 6);
        const uint16_t* t = indices + f * 3;
        for (int k = 0; k < 3; ++k) {
            const int other = nb[k];
            if (other != -1 && other < n && !records[other * 0x20 + 0x1c])
                out.Edge(e, V(t[k]), V(t[(k + 1) % 3]));
        }
    }
    self[0x10] = reinterpret_cast<int>(out.vertex);
    self[0x12] = reinterpret_cast<int>(out.index);
    reinterpret_cast<UnlockFn>(0x52A8C0)(self);
}

#ifndef DS_DIST
DWORD DevDword(const char* name) {
    DWORD v = 0, size = sizeof v;
    RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", name, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}

// Dev self-check (Dev\ShadowCompare = 1): for the first calls the game's own builder runs first (trampoline), then
// ours without the plane; both outputs must match. Logs the result per builder.
void* g_origObject = nullptr;
void* g_origSoldier = nullptr;
int g_compareLeft[2] = {0, 0};
float g_refVertex[kMaxQuads * 12];
uint16_t g_refIndex[kMaxQuads * 6];

void* Trampoline(uint32_t va, size_t len) {
    auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return nullptr;
    std::memcpy(t, reinterpret_cast<const void*>(va), len);
    t[len] = 0xE9;
    const int32_t rel = static_cast<int32_t>(va + len) - static_cast<int32_t>(reinterpret_cast<uint32_t>(t + len + 5));
    std::memcpy(t + len + 1, &rel, 4);
    return t;
}

void Compare(int which, const char* name, auto runOriginal, auto runOurs) {
    runOriginal();
    const int refCount = *reinterpret_cast<int*>(kQuadCount);
    std::memcpy(g_refVertex, reinterpret_cast<void*>(kVertexOut), sizeof g_refVertex);
    std::memcpy(g_refIndex, reinterpret_cast<void*>(kIndexOut), sizeof g_refIndex);
    const bool was = g_enabled;
    g_enabled = false;
    runOurs();
    g_enabled = was;
    const int count = *reinterpret_cast<int*>(kQuadCount);
    float worst = 0;
    bool indicesSame = std::memcmp(g_refIndex, reinterpret_cast<void*>(kIndexOut), count * 12) == 0;
    const auto* v = reinterpret_cast<const float*>(kVertexOut);
    for (int i = 0; i < std::min(count, refCount) * 12; ++i) worst = std::max(worst, std::fabs(v[i] - g_refVertex[i]));
    dslog::Write("[dev] shadow compare %s: game %d quads, ours %d, indices %s, max vertex difference %.3f", name,
                 refCount, count, indicesSame ? "same" : "DIFFERENT", worst);
    --g_compareLeft[which];
}
#endif

void ObjectVolume(uint8_t* mesh, const uint8_t* entry) {
#ifndef DS_DIST
    if (g_compareLeft[0] > 0 && g_origObject) {
        Compare(0, "object", [&] { reinterpret_cast<void(__fastcall*)(void*)>(g_origObject)(mesh); },
                [&] { BuildObjectVolume(mesh, entry); });
    }
#endif
    BuildObjectVolume(mesh, entry);
}

void __fastcall SoldierVolume(int* self, int, const float* light, const uint8_t* verts) {
#ifndef DS_DIST
    if (g_compareLeft[1] > 0 && g_origSoldier) {
        Compare(1, "soldier",
                [&] { reinterpret_cast<void(__thiscall*)(void*, const float*, const uint8_t*)>(g_origSoldier)(self, light, verts); },
                [&] { BuildSoldierVolume(self, light, verts); });
    }
#endif
    BuildSoldierVolume(self, light, verts);
}

// Entry of FUN_005230d0: ecx = mesh; esi = the caller's render entry (FUN_00520260).
__declspec(naked) void ObjectVolumeStub() {
    __asm {
        push esi
        push ecx
        call ObjectVolume
        add esp, 8
        ret
    }
}

// Call at 0x503333 (FUN_005032d0 -> FUN_00529a30): remembers the soldier object for its world matrix.
// (Literal numbers: inline asm reads a C++ constant as a memory operand.)
static_assert(kObjectMatrix == 0x10C && kSoldierDraw == 0x529A30);
__declspec(naked) void SoldierDrawStub() {
    __asm {
        lea eax, [edi + 0x10C]
        mov g_soldierMatrix, eax
        mov eax, 0x529A30
        jmp eax
    }
}
}  // namespace

void features::ApplyShadows() {
    static bool applied = false;
    if (applied) return;
    static const uint8_t kObjectEntry[] = {0x81, 0xEC, 0x40, 0x03, 0x00, 0x00};  // sub esp, 0x340
    static const uint8_t kSoldierEntry[] = {0x83, 0xEC, 0x54, 0x53, 0x55};      // sub esp, 0x54; push ebx; push ebp
    if (!patch::Matches(kObjectBuilder, kObjectEntry, sizeof kObjectEntry) ||
        !patch::Matches(kSoldierBuilder, kSoldierEntry, sizeof kSoldierEntry)) {
        dslog::Write("[fail] Shadows: unexpected bytes at the volume builders - not applied");
        return;
    }
#ifndef DS_DIST
    g_enabled = DevDword("ShadowFixOff") == 0;
    if (DevDword("ShadowCompare")) {
        g_origObject = Trampoline(kObjectBuilder, sizeof kObjectEntry);
        g_origSoldier = Trampoline(kSoldierBuilder, sizeof kSoldierEntry);
        g_compareLeft[0] = g_compareLeft[1] = 20;
    }
#endif
    if (!patch::HookCall(kSoldierDrawCall, reinterpret_cast<const void*>(&SoldierDrawStub), kSoldierDraw) ||
        !patch::WriteJump(kObjectBuilder, reinterpret_cast<const void*>(&ObjectVolumeStub)) ||
        !patch::WriteJump(kSoldierBuilder, reinterpret_cast<const void*>(&SoldierVolume))) {
        dslog::Write("[fail] Shadows: hooks");
        return;
    }
    applied = true;
    dslog::Write("[ok]   Shadows: volumes end below the caster (no shadows through floors / walls)%s",
                 g_enabled ? "" : " - dev: switched off");
}
