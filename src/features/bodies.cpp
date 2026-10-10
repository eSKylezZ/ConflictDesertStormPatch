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

// Normal maps. The level binds them once, when it reads Characters_*.rfx: REFX_DOT3_BUMP "model" NORMAL_MAP "x" ->
// FUN_004e4130 registers x (texture manager FUN_0054a0b0(name, 0x11)) and calls FUN_004ebf00(part, texture, model, 0)
// (thiscall, ret 0xC) on every part; for a skinned part that builds tangents and stores the map in its skin data
// (+0xB4: +0x8 = 1 bound, +0xC texture, +0x24 tangent vertex buffer; already bound parts are skipped). A body loaded
// mid-level misses that pass and was drawn without one, so it is bound here: the skin's own normal map, else the
// soldier's (the one on his own body, from the level's .rfx).
constexpr uint32_t kRegisterTexture = 0x54A0B0, kBindNormalMap = 0x4EBF00;

uint8_t* SkinData(uint8_t* model) {  // the skinned part's skin data
    for (uint8_t* o = model; o; o = At<uint8_t*>(o, 0xE8))
        if (At<int>(o, 0xC) == 2) return At<uint8_t*>(o, 0xB4);
    return nullptr;
}

void BindNormalMap(uint8_t* model, uint8_t* original, const std::string& name) {
    uint8_t* sd = SkinData(model);
    if (!sd || At<int>(sd, 8)) return;  // no skinned part, or bound already (a body is loaded once per level)
    void* texture = nullptr;
    if (!name.empty()) {
        using Register = void*(__thiscall*)(void*, const char*, int);
        texture = reinterpret_cast<Register>(kRegisterTexture)(*reinterpret_cast<void**>(kTextureManager), name.c_str(), 0x11);
    } else if (uint8_t* own = SkinData(original); own && At<int>(own, 8)) {
        texture = At<void*>(own, 0xC);
    }
    if (!texture) return;
    using ControlFp = uint32_t(__cdecl*)(uint32_t, uint32_t);
    const uint32_t old = reinterpret_cast<ControlFp>(kControlFp)(0, 0);
    reinterpret_cast<ControlFp>(kControlFp)(*reinterpret_cast<uint32_t*>(kLoadPrecision), 0xFFFFF);
    using Bind = int(__thiscall*)(void*, void*, void*, void*);
    for (uint8_t* o = model; o; o = At<uint8_t*>(o, 0xE8)) reinterpret_cast<Bind>(kBindNormalMap)(o, texture, model, nullptr);
    reinterpret_cast<ControlFp>(kControlFp)(old, 0xFFFFF);
    dslog::Write("Bodies: normal map %s bound, flag %d", name.empty() ? "(the soldier's own)" : name.c_str(), At<int>(sd, 8));
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
            BindNormalMap(model, original, features::SkinNormalMap(reinterpret_cast<const char*>(entry + 4)));
        }
    }
    if (At<uint8_t*>(inst, 0xC8) == want) return;
    if (!rec) g_swapped.push_back({inst, original});
    At<uint8_t*>(inst, 0xC8) = want;
    dslog::Write("Bodies: soldier %p -> %s", soldier, want == original ? "his own body" : body.c_str());
}

// The skin apply (and a body loaded mid-level) only registers textures (skin, portrait, the body's own); the level
// load loads them in one batch FUN_0054a3e0 (a no-op unless something is pending). A skin applied after that - a
// loaded save (FUN_00417180 re-applies each soldier's saved skin), the loadout screen - was drawn black with a white
// portrait. Run the batch then; during a level load wait for its end (an early batch would skip the loading bar).
bool g_texturesPending = false;

void LoadPendingTextures() {
    if (*reinterpret_cast<uint8_t*>(kLoadPending) == 1) {
        g_texturesPending = true;
        return;
    }
    g_texturesPending = false;
    if (void* mgr = *reinterpret_cast<void**>(kTextureManager)) {
        using Batch = void(__thiscall*)(void*, void*, int);
        reinterpret_cast<Batch>(kTextureBatch)(mgr, nullptr, 0);
    }
}

void __fastcall ApplySkinHook(void* soldier, void*, void* entry) {
    g_apply(soldier, entry);
    __try {
        UpdateBody(static_cast<uint8_t*>(soldier));
        LoadPendingTextures();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dslog::Write("[fail] Bodies: body swap faulted for soldier %p", soldier);
    }
}

// Save loading (FUN_00417180, call 0x417449) finds the saved skin key FUN_00496500(key, uniform, role) in the table of
// the global uniform [0x60ED1C] only - always SAS since the uniform screen is gone (coop.cpp), and the loadout screen
// mixes sides per soldier. A key missing there (Delta / Russian / Iraqi faces, Delta mod variants) came back null and
// the soldier lost his skin. Not found -> the other sides.
constexpr uint32_t kSaveSkinLookupSite = 0x417449, kSkinLookup = 0x496500;

void* __cdecl SaveSkinLookup(int key, int uniform, int role) {
    using Lookup = void*(__cdecl*)(int, int, int);
    auto lookup = reinterpret_cast<Lookup>(kSkinLookup);
    if (void* e = lookup(key, uniform, role)) return e;
    for (int side = 0; side < 4; ++side)
        if (side != uniform)
            if (void* e = lookup(key, side, role)) return e;
    dslog::Write("Bodies: saved skin key %08X (role %d) not in any skin table", key, role);
    return nullptr;
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
    if (!patch::HookCall(kSaveSkinLookupSite, reinterpret_cast<const void*>(&SaveSkinLookup), kSkinLookup))
        dslog::Write("[fail] Bodies: save skin lookup at 0x%08X not recognised", kSaveSkinLookupSite);
}

// A level load frees the soldiers and their instances: forget the swaps. After it, load what skins applied during it
// registered.
void features::OnFrameBodies() {
    if (*reinterpret_cast<uint8_t*>(kLoadPending) == 1) g_swapped.clear();
    else if (g_texturesPending) LoadPendingTextures();
}
