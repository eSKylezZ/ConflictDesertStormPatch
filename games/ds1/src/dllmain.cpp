// dsfix.dll: DesertStormFix as a PSE host plugin (<game>\PSE\Plugins\dsfix.dll + dsfix.pse, docs/sdk/layout.md).
//
// The game loads the PSE shim dinput8.dll (pse/shim) first; at the exe's entry point - before the CRT / WinMain, outside
// the loader lock - the shim starts PSE\pse_loader.dll, which recognises the exe through PSE\Games\pse_ds1.dll and loads
// this DLL before any other plugin (manifest "host = true", allowed by the DS1 game layer). PSEPlugin_Load runs what
// the old dinput8.dll's Init did: settings migration + load, then every feature's Apply.
#include <windows.h>

#include <cstring>

#include "core/log.h"
#include "core/proxy.h"
#include "core/pse_host.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/overlay.h"

// ---- the real system dinput8.dll (gamepad / hot-plug enumerate with their own DirectInput) ----
namespace {
HMODULE g_real = nullptr;
}

FARPROC RealDInput8(const char* name) {
    if (!g_real) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);  // SysWOW64 for this 32-bit process
        if (n == 0 || n > MAX_PATH - 16) return nullptr;
        strcat_s(path, "\\dinput8.dll");
        g_real = LoadLibraryA(path);
        if (!g_real) return nullptr;
    }
    return GetProcAddress(g_real, name);
}

namespace {

// Every feature is an internal PSE plugin "dsfix.<feature>" (docs/sdk/ds1-plugins.md); Init runs the Apply
// functions in this order, each inside its plugin's patch::OwnerScope. SIMULATION = changes gameplay (online peers
// must match), CLIENT_ONLY = cosmetic / UI / local input.
constexpr uint32_t kSim = PSE_PLUGIN_SIMULATION, kClient = PSE_PLUGIN_CLIENT_ONLY;
const pse_host::Feature kFeatures[] = {
    // first: a restart waits for the old process to quit
    {"dsfix.restart", "Restart from OPTIONS > VIDEO (waits for the old process)", kClient, &features::ApplyRestart},
    {"dsfix.dev", "Developer test hooks", kSim, &features::ApplyDevBackground},
    {"dsfix.timing", "Frame timing + physics", kSim, &features::ApplyTiming},
    {"dsfix.framecap", "Frame cap", kClient, &features::ApplyFrameCap},
    {"dsfix.launcher", "Launcher settings", kClient, &features::ApplyLauncher},
    {"dsfix.ingame_input", "Menus follow the input device", kClient, &features::ApplyInGameInput},
    {"dsfix.frontend", "Front-end menus (main menu items, screen router)", kClient, &features::ApplyFrontEnd},
    // before WinMain registers the debug bindings (0x4108F9)
    {"dsfix.modsmenu", "MODS menu", kClient, &features::ApplyModsMenu},
    {"dsfix.fixmenu", "DESERTSTORMFIX menu", kClient, &features::ApplyFixMenu},
    {"dsfix.barcolour", "Menu bar / HUD colour", kClient, &features::ApplyBarColour},
    // before every reader of the game's archives / sound caches / mission DLLs: they may be in PSE\Originals
    {"dsfix.originals", "Original game files", kSim, &features::ApplyOriginalsRedirect},
    {"dsfix.mods", "Mods folder", kSim, &features::ApplyMods},
    // after ApplyMods (its resolver / opener hooks): pse.vfs + the asset store (setting SHARED ASSET STORE in the
    // PLUGINS menu, default off; dev: Dev\PseStore). ApplyMods' sound work opens it first (features::AssetStoreSound).
    {"dsfix.assetstore", "Shared asset store", kSim, &features::ApplyAssetStore},
    // after ApplyMods: the heap is sized to the largest mod file
    {"dsfix.memory", "Global Heap + big textures", kSim, &features::ApplyMemory},
    // after ApplyMods: the .weapon files name the icons
    {"dsfix.weaponicons", "Weapon pictures from mods", kClient, &features::ApplyWeaponIcons},
    // skins applied mid-level (loadout screen, loaded saves); the mod bodies are plugin dsfix.bodies
    {"dsfix.diagnostics", "Diagnostics + performance overlay", kClient, &features::ApplyDiagnostics},
    // Own DLLs in PSE\Plugins (games/ds1/plugins, on the dsfix kit): dsfix.widescreen, dsfix.textfit, dsfix.mouse,
    // dsfix.menus, dsfix.units, dsfix.limits, dsfix.audio, dsfix.shadows, dsfix.difficulty, dsfix.jpfixes,
    // dsfix.handling, dsfix.tooltips, dsfix.rumble, dsfix.bodies, dsfix.followers, dsfix.hud, dsfix.weather (+ footprints), dsfix.timeofday, dsfix.voices, dsfix.fontsharp, dsfix.developer, dsfix.cinematics, dsfix.display,
    // dsfix.photo, dsfix.online (+ nameplates, own view, lockstep / authority, determinism), dsfix.missions (Lua
    // levels, translated missions, co-op scripts, API trace), dsfix.loadout (briefing / loadout screen, kits, gear,
    // vehicle swaps, overview, mid-level skins), dsfix.coop (split screen, co-op, controls, pads, versus), dsfix.graphics (MSAA / anisotropy /
    // V-Sync / scene fog, the D3D9 renderers, OPTIONS > VIDEO) - loaded after this DLL's
    // features.
    // setting MISSIONS FROM LUA (dev: Dev\MissionSource) - Mission*.dll / Training*.dll levels run as their translated Lua
    // after the asset store + the mission translations have read them: the originals move to PSE\Originals
    {"dsfix.originals", "Original game files", kSim, &features::ApplyOriginalsMove},
    {"dsfix.campaigns", "Custom campaigns", kSim, &features::ApplyCampaigns},
    {"dsfix.console", "Developer console", kSim, &features::ApplyConsole},
    {"dsfix.dev", "Developer test hooks", kSim, &features::ApplyDevHooks},
    // its drawing: on the game layer's PSE_EVENT_END_SCENE; its glyph hooks are made later, at run time (own OwnerScope)
    {"dsfix.overlay", "Overlay drawing (icons, late draws)", kClient, &overlay::ApplyEndScene},
};

bool Init(const PSELoadInterface* pse) {
    dslog::Open();
    dslog::Write("DesertStormFix " DS_VERSION " loaded (PSE host plugin dsfix.dll)");
    {
        // Large Address Aware (set on disk by DesertStormFix-Patcher.exe): 4 GB of address space instead of 2 GB.
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        auto top = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        dslog::Write("Address space: %u MB (%s)", static_cast<unsigned>((top >> 20) + 1),
                     top > 0x80000000u ? "4 GB, Large Address Aware" : "2 GB");
    }
    // pse.host + log / report sinks + game layer providers - before any feature patches.
    if (!pse_host::Init(pse)) {
        dslog::Write("[fail] PSE: dsfix.dll got no pse.host from the loader (not allowed as a host plugin, or an "
                     "older / newer pse_loader.dll) - DesertStormFix is off");
        return false;
    }
    pse_host::RegisterFeatures(kFeatures, sizeof kFeatures / sizeof kFeatures[0]);
    settings::Load();   // migrates the old Enhancements key once, then reads every setting
    for (const pse_host::Feature& f : kFeatures) pse_host::RunAs(f.name, f.apply);
    // Other external plugins (PSE\Plugins) are loaded by the loader after this returns; the split-out features among
    // them query dsfix.services in their Load.
    features::RegisterServices();
    features::RegisterOverlayInterface();
    features::RegisterHostInterface();
    return true;
}

const char* const kGames[] = {"ds1", nullptr};
}  // namespace

extern "C" {
// Read from the file before anything runs (docs/sdk/reference/plugins.md): constant data only.
__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info;
const PSEPluginInfo PSEPlugin_Info = {sizeof(PSEPluginInfo), 1, "dsfix", "DesertStormFix", "DesertStormFix",
                                      pse_host::kVersion, PSE_API_VERSION, kGames, PSE_PLUGIN_SIMULATION};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) { return Init(pse) ? 1 : 0; }
}
