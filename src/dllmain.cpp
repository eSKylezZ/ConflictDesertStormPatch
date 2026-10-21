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
    {"dsfix.videomenu", "Video options / restart", kClient, &features::ApplyRestart},
    {"dsfix.widescreen", "Widescreen resolutions", kClient, &features::ApplyWidescreen},
    {"dsfix.dev", "Developer test hooks", kSim, &features::ApplyDevBackground},
    {"dsfix.hud", "HUD / menu scaling", kClient, &features::ApplyHud},
    {"dsfix.controller", "Controllers in game", kClient, &features::ApplyController},
    {"dsfix.hotplug", "Controller hot-plug", kClient, &features::ApplyPadAccess},
    {"dsfix.timing", "Frame timing + physics", kSim, &features::ApplyTiming},
    {"dsfix.framecap", "Frame cap", kClient, &features::ApplyFrameCap},
    {"dsfix.launcher", "Launcher settings", kClient, &features::ApplyLauncher},
    {"dsfix.ingame_input", "Menus follow the input device", kClient, &features::ApplyInGameInput},
    {"dsfix.splitscreen", "Split screen", kSim, &features::ApplySplitScreen},
    {"dsfix.display", "Display mode", kClient, &features::ApplyDisplay},
    {"dsfix.cinematics", "Intro / cutscene skip", kSim, &features::ApplyCinematics},
    {"dsfix.graphics", "MSAA / anisotropy / V-Sync", kClient, &features::ApplyGraphics},
    {"dsfix.coop", "Co-op set-up + saves", kSim, &features::ApplyCoop},
    {"dsfix.versus", "Versus", kSim, &features::ApplyVersus},
    {"dsfix.cheats", "Cheats menu", kSim, &features::ApplyCheats},
    // before WinMain registers the debug bindings (0x4108F9)
    {"dsfix.developer", "Developer page + debug keys", kSim, &features::ApplyDebugKeys},
    {"dsfix.freecam", "Free camera", kClient, &features::ApplyFreeCam},
    {"dsfix.modsmenu", "MODS menu", kClient, &features::ApplyModsMenu},
    {"dsfix.fixmenu", "DESERTSTORMFIX menu", kClient, &features::ApplyFixMenu},
    {"dsfix.pausemenu", "Pause menu items", kClient, &features::ApplyPauseMenu},
    {"dsfix.fontsharp", "Sharper text", kClient, &features::ApplyFontSharp},
    {"dsfix.textfit", "Menu text fitting", kClient, &features::ApplyTextFit},
    {"dsfix.barcolour", "Menu bar / HUD colour", kClient, &features::ApplyBarColour},
    {"dsfix.controls", "Controls menu", kClient, &features::ApplyControls},
    {"dsfix.mouse", "Mouse acceleration", kClient, &features::ApplyMouse},
    {"dsfix.menus", "Menu fixes", kClient, &features::ApplyMenus},
    {"dsfix.followers", "Followers", kSim, &features::ApplyFollowers},
    {"dsfix.difficulty", "VERY HARD / VERY EASY", kSim, &features::ApplyDifficulty},
    {"dsfix.jpfixes", "Japanese-release fixes", kSim, &features::ApplyJpFixes},
    {"dsfix.units", "Unit / vehicle tables", kSim, &features::ApplyUnits},
    {"dsfix.handling", "Vehicle handling", kSim, &features::ApplyHandling},
    {"dsfix.limits", "Engine limits", kSim, &features::ApplyLimits},
    {"dsfix.tooltips", "Inventory tooltips", kClient, &features::ApplyTooltips},
    {"dsfix.rumble", "Weapon rumble", kClient, &features::ApplyRumble},
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
    // .skin "body": soldier meshes from mods
    {"dsfix.bodies", "Soldier bodies from mods", kSim, &features::ApplyBodies},
    {"dsfix.timeofday", "Time of day", kSim, &features::ApplyTimeOfDay},
    {"dsfix.footprints", "Footprints + puddles", kClient, &features::ApplyFootprints},
    {"dsfix.weather", "Weather", kSim, &features::ApplyWeather},
    {"dsfix.voices", "Voices", kSim, &features::ApplyVoices},
    {"dsfix.overview", "Mission overview", kSim, &features::ApplyOverview},
    {"dsfix.loadout", "Loadout + unlocks", kSim, &features::ApplyLoadout},
    {"dsfix.customise", "Briefing / loadout screen", kSim, &features::ApplyCustomise},
    {"dsfix.gear", "Soldier gear", kSim, &features::ApplyGear},
    {"dsfix.diagnostics", "Diagnostics + performance overlay", kClient, &features::ApplyDiagnostics},
    {"dsfix.audio", "Audio while aiming", kClient, &features::ApplyAudio},
    {"dsfix.shadows", "Shadow fix", kClient, &features::ApplyShadows},
    {"dsfix.scripthost", "Lua mission scripts", kSim, &features::ApplyScriptHost},
    {"dsfix.missionrt", "Translated mission runtime", kSim, &features::ApplyMissionRt},
    // setting MISSIONS FROM LUA (dev: Dev\MissionSource) - Mission*.dll / Training*.dll levels run as their translated Lua
    {"dsfix.missionsource", "Translated missions", kSim, &features::ApplyMissionSource},
    // after the asset store + the mission translations have read them: the originals move to PSE\Originals
    {"dsfix.originals", "Original game files", kSim, &features::ApplyOriginalsMove},
    {"dsfix.campaigns", "Custom campaigns", kSim, &features::ApplyCampaigns},
    {"dsfix.console", "Developer console", kSim, &features::ApplyConsole},
    {"dsfix.determinism", "Determinism tests (dev)", kSim, &features::ApplyDeterminism},
    {"dsfix.apitrace", "Script API trace (dev)", kSim, &features::ApplyApiTrace},
    {"dsfix.dev", "Developer test hooks", kSim, &features::ApplyDevHooks},
    // Plugins whose hooks are made later, at run time (their own OwnerScope):
    {"dsfix.lockstep", "Online lockstep", kSim, nullptr},
    {"dsfix.authority", "Online host authority", kSim, nullptr},
    {"dsfix.vehicles", "Vehicle swaps", kSim, nullptr},
    {"dsfix.overlay", "Overlay drawing (icons, late draws)", kClient, nullptr},
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
    // Other external plugins (PSE\Plugins) are loaded by the loader after this returns.
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
