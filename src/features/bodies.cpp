// Soldier bodies from mods: a .skin file's "body = MYBODY" (MYBODY.evo in a mod folder, exported with
// the ModKit's Blender exporter based on that soldier's own body) gives every soldier who wears that skin the new mesh.
//
// A soldier (scene object) draws through its render instance (soldier +0x2C: 0x2C8 bytes, made by FUN_004dd880 ->
// FUN_00501200): the instance points at the loaded model (+0xC8 = the model's first object; objects linked by +0xE8,
// geometry kind +0xC, 2 = skinned, skin data +0xB4) and owns only per-bone pose arrays sized by the model's bone count
// (model +0xFC -> +4). Models are loaded once and shared: FUN_004dd5c0 (thiscall on the environment [0x63C948]: type,
// name, use cache, 0), under the FPU precision the level loader uses (_controlfp 0x569956 with [0x63C8C8]). So a body
// is swapped by pointing the instance at another model with the same skeleton - which the exporter keeps (the base
// model's objects, bones and keys; only the skin geometry is new). The skin texture override (instance +0x2B0, set by
// the skin apply) stays, so the uniform shows on the new body.
//
// Skins are applied by FUN_00435be0 (thiscall soldier, skin entry; 8 callers: scene set-up, saves, the loadout
// screen ...): its entry is wrapped, and after it the soldier gets his skin's body or his own back.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kApplySkin = 0x435BE0, kApplySkinCont = 0x435BE7;
constexpr uint8_t kApplySkinEntry[] = {0x56, 0x8B, 0xF1, 0x8B, 0x4C, 0x24, 0x08};  // push esi; mov esi,ecx; mov ecx,[esp+8]
constexpr uint32_t kEnvironment = 0x63C948, kLoadModel = 0x4DD5C0, kControlFp = 0x569956, kLoadPrecision = 0x63C8C8;
constexpr uint32_t kTextureManager = 0x63C930, kTextureBatch = 0x54A3E0, kLoadPending = 0x60EC8C;

using ApplyFn = void(__thiscall*)(void*, void*);
ApplyFn g_apply = nullptr;

struct Swapped {
    uint8_t* instance;
    uint8_t* original;  // the instance's own model
};
std::vector<Swapped> g_swapped;  // instances given another body this level

template <class T>
T& At(void* p, uint32_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + off); }

uint8_t* LoadModel(const char* name, int type) {
    void* env = *reinterpret_cast<void**>(kEnvironment);
    if (!env) return nullptr;
    using ControlFp = uint32_t(__cdecl*)(uint32_t, uint32_t);
    const uint32_t old = reinterpret_cast<ControlFp>(kControlFp)(0, 0);
    reinterpret_cast<ControlFp>(kControlFp)(*reinterpret_cast<uint32_t*>(kLoadPrecision), 0xFFFFF);
    using Load = uint8_t*(__thiscall*)(void*, int, const char*, int, int);
    uint8_t* model = reinterpret_cast<Load>(kLoadModel)(env, type, name, 1, 0);
    reinterpret_cast<ControlFp>(kControlFp)(old, 0xFFFFF);
    return model;
}

int Bones(uint8_t* model) {
    uint8_t* skel = At<uint8_t*>(model, 0xFC);
    return skel ? At<int>(skel, 4) : -1;
}
int Objects(uint8_t* model) {
    int n = 0;
    for (uint8_t* o = model; o && n < 1000; o = At<uint8_t*>(o, 0xE8)) ++n;
    return n;
}

void UpdateBody(uint8_t* soldier) {
    uint8_t* inst = At<uint8_t*>(soldier, 0x2C);
    if (!inst || !At<uint8_t*>(inst, 0xC8)) return;
    Swapped* rec = nullptr;
    for (Swapped& s : g_swapped)
        if (s.instance == inst) rec = &s;
    uint8_t* original = rec ? rec->original : At<uint8_t*>(inst, 0xC8);
    uint8_t* entry = At<uint8_t*>(soldier, 0x2A68);
    const std::string body = entry ? features::SkinBody(reinterpret_cast<const char*>(entry + 4)) : std::string();
    uint8_t* want = original;
    if (!body.empty()) {
        uint8_t* model = LoadModel(body.c_str(), At<int>(original, 8));
        if (!model) {
            features::ModLog("[fail] Mods: body %s didn't load as a character - export it from that soldier's own body "
                             "(e.g. HERO01_ARMSTRONG for Bradley)", body.c_str());
        } else if (Bones(model) != Bones(original) || Objects(model) != Objects(original)) {
            features::ModLog("[fail] Mods: body %s doesn't fit this soldier (%d bones / %d parts, his own body %d / %d) - "
                             "export it based on his body (e.g. HERO01_ARMSTRONG for Bradley)",
                             body.c_str(), Bones(model), Objects(model), Bones(original), Objects(original));
        } else {
            want = model;
        }
    }
    if (At<uint8_t*>(inst, 0xC8) == want) return;
    if (!rec) g_swapped.push_back({inst, original});
    At<uint8_t*>(inst, 0xC8) = want;
    // A model loaded mid-level only registers its textures: load the pending batch (else it is drawn white).
    using Batch = void(__thiscall*)(void*, void*, int);
    reinterpret_cast<Batch>(kTextureBatch)(*reinterpret_cast<void**>(kTextureManager), nullptr, 0);
    dslog::Write("Bodies: soldier %p -> %s", soldier, want == original ? "his own body" : body.c_str());
}

void __fastcall ApplySkinHook(void* soldier, void*, void* entry) {
    g_apply(soldier, entry);
    __try {
        UpdateBody(static_cast<uint8_t*>(soldier));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dslog::Write("[fail] Bodies: body swap faulted for soldier %p", soldier);
    }
}
}  // namespace

void features::ApplyBodies() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!patch::Matches(kApplySkin, kApplySkinEntry, sizeof kApplySkinEntry)) {
        dslog::Write("[fail] Bodies: skin apply at 0x%08X not recognised - mod bodies off", kApplySkin);
        return;
    }
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return;
    memcpy(tramp, kApplySkinEntry, sizeof kApplySkinEntry);
    tramp[sizeof kApplySkinEntry] = 0xE9;
    const int32_t rel = static_cast<int32_t>(kApplySkinCont - (reinterpret_cast<uint32_t>(tramp) + sizeof kApplySkinEntry + 5));
    memcpy(tramp + sizeof kApplySkinEntry + 1, &rel, 4);
    g_apply = reinterpret_cast<ApplyFn>(tramp);
    patch::WriteJump(kApplySkin, reinterpret_cast<const void*>(&ApplySkinHook));
}

// A level load frees the soldiers and their instances: forget the swaps.
void features::OnFrameBodies() {
    if (*reinterpret_cast<uint8_t*>(kLoadPending) == 1) g_swapped.clear();
}
