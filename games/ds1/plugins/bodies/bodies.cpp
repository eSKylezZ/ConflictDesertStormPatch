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
// Its own plugin since 2026-10-24 (PSE\Plugins\dsfix.bodies.dll, dsfix kit): the .skin data, the mod report and the
// FeatMods / FeatLoadout state come from dsfix.dll's dsfix.services.
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "kit/kit.h"

namespace {
constexpr uint32_t kApplySkin = 0x435BE0;
constexpr uint8_t kApplySkinEntry[] = {0x56, 0x8B, 0xF1, 0x8B, 0x4C, 0x24, 0x08};  // push esi; mov esi,ecx; mov ecx,[esp+8]
constexpr uint32_t kEnvironment = 0x63C948, kLoadModel = 0x4DD5C0, kControlFp = 0x569956, kLoadPrecision = 0x63C8C8;
constexpr uint32_t kTextureManager = 0x63C930, kTextureBatch = 0x54A3E0, kLoadPending = 0x60EC8C;

const DsfixServicesV1* g_services = nullptr;

// A line for the log + the mod report (dsfix.dll's mods.cpp).
void ModLog(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    g_services->ModReport(line);
}

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
    const std::string body = entry ? std::string(g_services->SkinBody(reinterpret_cast<const char*>(entry + 4))) : std::string();
    uint8_t* want = original;
    if (!body.empty()) {
        uint8_t* model = LoadModel(body.c_str(), At<int>(original, 8));
        if (!model) {
            ModLog("[fail] Mods: body %s didn't load as a character - export it from that soldier's own body "
                             "(e.g. HERO01_ARMSTRONG for Bradley)", body.c_str());
        } else if (Bones(model) != Bones(original) || Objects(model) != Objects(original)) {
            ModLog("[fail] Mods: body %s doesn't fit this soldier (%d bones / %d parts, his own body %d / %d) - "
                             "export it based on his body (e.g. HERO01_ARMSTRONG for Bradley)",
                             body.c_str(), Bones(model), Objects(model), Bones(original), Objects(original));
        } else {
            want = model;
            BindNormalMap(model, original, g_services->SkinNormalMap(reinterpret_cast<const char*>(entry + 4)));
        }
    }
    if (At<uint8_t*>(inst, 0xC8) == want) return;
    if (!rec) g_swapped.push_back({inst, original});
    At<uint8_t*>(inst, 0xC8) = want;
    dslog::Write("Bodies: soldier %p -> %s", soldier, want == original ? "his own body" : body.c_str());
}

// A body loaded mid-level only registers its textures; they load in the texture batch FUN_0054a3e0 (a no-op unless
// something is pending). dsfix.dll's skinapply.cpp runs it for the skin itself; the body's textures may come after that
// (the hooks chain in either order), so the batch runs here too. During a level load: after it.
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

// Multi-part skins (ModKit exporter 1.8+): a body exported with several materials / more than 65 535 vertices has one
// skinned (kind 2) object per part; the extra parts ride on spare bone objects (a DS1 model can't gain objects - its
// animations have one track per object slot), the main skin keeps its slot, the last skinned object of the chain (every
// shipped body has exactly one). The instance's skin draw (FUN_005032d0, per kind-2 object: FUN_0052a5a0 thiscall(skin
// data +0xB4, texture override)) passed the uniform (instance +0x2B0, set by the skin apply for the squad / script
// uniforms) to every part, so a squad soldier wore it on all of them. Now only the main skin gets it and the extra parts
// draw their own textures. Replaced: 0x503384..0x503396 = mov ecx,[edi+0x2B0]; push ecx; mov ecx,[esi+0xB4]; call
// 0x52A5A0 (esi = the object, edi = the instance).
constexpr uint32_t kSkinDrawSite = 0x503384;
constexpr uint8_t kSkinDrawBytes[] = {0x8B, 0x8F, 0xB0, 0x02, 0x00, 0x00, 0x51, 0x8B, 0x8E, 0xB4,
                                      0x00, 0x00, 0x00, 0xE8, 0x0A, 0x72, 0x02, 0x00};
__declspec(naked) void SkinDrawStub() {
    __asm {
        mov ecx, [edi + 0x2B0]
        test ecx, ecx
        jz draw
        mov eax, [esi + 0xE8]
    next:
        test eax, eax
        jz draw
        cmp dword ptr [eax + 0xC], 2
        je extra
        mov eax, [eax + 0xE8]
        jmp next
    extra:
        xor ecx, ecx   // a skinned object follows: this one is an extra part - its own texture
    draw:
        push ecx
        mov ecx, [esi + 0xB4]
        mov eax, 0x52A5A0
        call eax
        mov eax, 0x503396
        jmp eax
    }
}
// A level load frees the soldiers and their instances: forget the swaps. After it, load what bodies applied during it
// registered.
void OnFrameBodies() {
    if (*reinterpret_cast<uint8_t*>(kLoadPending) == 1) g_swapped.clear();
    else if (g_texturesPending) LoadPendingTextures();
}

bool Load() {
    g_services = kit::Services();
    if (!g_services) return false;
    // Multi-part skins: always (a mod may also replace a hero body by file name); one skin object = unchanged.
    if (patch::WriteJump(kSkinDrawSite, reinterpret_cast<const void*>(&SkinDrawStub), sizeof kSkinDrawBytes, kSkinDrawBytes))
        dslog::Write("[ok]   Bodies: multi-part skins - the uniform goes on the main skin only");
    else
        dslog::Write("[fail] Bodies: skin draw at 0x%08X not recognised - multi-part bodies wear the uniform everywhere",
                     kSkinDrawSite);
    // Mod bodies come only from .skin files in the Mods folder: without it the skin apply stays as it is.
    if (!g_services->ModsFolderEnabled()) {
        dslog::Write("[off]  Bodies: body swap off (Mods folder off)");
        return true;
    }
    // Entry hook through the broker (chained with dsfix.skins'): g_apply = the next subscriber or the trampoline.
    if (!patch::HookFunction(kApplySkin, reinterpret_cast<const void*>(&ApplySkinHook), reinterpret_cast<void**>(&g_apply),
                             kApplySkinEntry, sizeof kApplySkinEntry)) {
        dslog::Write("[fail] Bodies: skin apply at 0x%08X not recognised - mod bodies off", kApplySkin);
        return true;
    }
    dslog::Write("[ok]   Bodies: skin apply hook (bodies from .skin files)");
    return kit::OnFrame(&OnFrameBodies);
}
}  // namespace

DSFIX_KIT_PLUGIN("dsfix.bodies", "Soldier bodies from mods", PSE_PLUGIN_SIMULATION, Load)
