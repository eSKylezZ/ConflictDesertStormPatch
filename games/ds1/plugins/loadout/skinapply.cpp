// Skins applied mid-level (the loadout screen, loaded saves): what the game's skin apply FUN_00435be0 (thiscall soldier,
// skin entry; 8 callers: scene set-up, saves, the loadout screen ...) leaves undone. Was part of bodies.cpp until
// 2026-10-24, when the mod bodies moved to their own plugin (PSE\Plugins\dsfix.bodies.dll, which chains its own body
// swap on the same entry through the broker); these two stay here because the loadout screen needs them.
//
// Textures: the skin apply only registers textures (skin, portrait); the level load loads them in one batch
// FUN_0054a3e0 (a no-op unless something is pending). A skin applied after that - a loaded save (FUN_00417180 re-applies
// each soldier's saved skin), the loadout screen - was drawn black with a white portrait. Run the batch then; during a
// level load wait for its end (an early batch would skip the loading bar).
//
// Save loading (FUN_00417180, call 0x417449) finds the saved skin key FUN_00496500(key, uniform, role) in the table of
// the global uniform [0x60ED1C] only - always SAS since the uniform screen is gone (coop.cpp), and the loadout screen
// mixes sides per soldier. A key missing there (Delta / Russian / Iraqi faces, Delta mod variants) came back null and
// the soldier lost his skin. Not found -> the other sides.
// Only with the Mods folder or BRIEFING & LOADOUT on (without both, skins never change mid-level).
#include <windows.h>

#include <cstdint>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
constexpr uint32_t kApplySkin = 0x435BE0;
constexpr uint8_t kApplySkinEntry[] = {0x56, 0x8B, 0xF1, 0x8B, 0x4C, 0x24, 0x08};  // push esi; mov esi,ecx; mov ecx,[esp+8]
constexpr uint32_t kTextureManager = 0x63C930, kTextureBatch = 0x54A3E0, kLoadPending = 0x60EC8C;
constexpr uint32_t kSaveSkinLookupSite = 0x417449, kSkinLookup = 0x496500;

using ApplyFn = void(__thiscall*)(void*, void*);
ApplyFn g_apply = nullptr;
bool g_texturesPending = false;
bool g_hooked = false;

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
    g_apply(soldier, entry);   // the game's apply (and dsfix.bodies' body swap, when installed)
    __try {
        LoadPendingTextures();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dslog::Write("[fail] Skins: texture batch faulted for soldier %p", soldier);
    }
}

void* __cdecl SaveSkinLookup(int key, int uniform, int role) {
    using Lookup = void*(__cdecl*)(int, int, int);
    auto lookup = reinterpret_cast<Lookup>(kSkinLookup);
    if (void* e = lookup(key, uniform, role)) return e;
    for (int side = 0; side < 4; ++side)
        if (side != uniform)
            if (void* e = lookup(key, side, role)) return e;
    dslog::Write("Skins: saved skin key %08X (role %d) not in any skin table", key, role);
    return nullptr;
}
}  // namespace

void features::ApplySkins() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!features::ModsFolderEnabled() && !settings::Get().featLoadout) {
        dslog::Write("[off]  Skins: mid-level skin hooks off (FeatMods and FeatLoadout off)");
        return;
    }
    // Entry hook through the broker: g_apply = its trampoline (the moved 7-byte prologue) or the next subscriber.
    g_hooked = patch::HookFunction(kApplySkin, reinterpret_cast<const void*>(&ApplySkinHook),
                                   reinterpret_cast<void**>(&g_apply), kApplySkinEntry, sizeof kApplySkinEntry);
    if (!g_hooked) {
        dslog::Write("[fail] Skins: skin apply at 0x%08X not recognised - mid-level skins may draw black", kApplySkin);
        return;
    }
    if (!patch::HookCall(kSaveSkinLookupSite, reinterpret_cast<const void*>(&SaveSkinLookup), kSkinLookup))
        dslog::Write("[fail] Skins: save skin lookup at 0x%08X not recognised", kSaveSkinLookupSite);
    dslog::Write("[ok]   Skins: textures of mid-level skins loaded, saved skins found on every side");
}

// After a level load: load what skins applied during it registered.
void features::OnFrameSkins() {
    if (g_hooked && g_texturesPending && *reinterpret_cast<uint8_t*>(kLoadPending) != 1) LoadPendingTextures();
}
