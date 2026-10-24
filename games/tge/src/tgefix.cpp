// GreatEscapeFix: The Great Escape's enhancement patch as the host plugin of the Pivotal Script Extender (PSE).
//
// Loaded by PSE: <game>\dinput8.dll = the PSE shim, PSE\pse_loader.dll + PSE\Games\pse_tge.dll (the TGE game layer:
// detection, pse.ids, pse.game.tge, the core game events incl. PSE_MSG_EXIT at WM_DESTROY) and this DLL as
// PSE\Plugins\tgefix.dll + tgefix.pse ("host = true"; the TGE layer allows the name "tgefix"). Other TGE plugins:
// games/tge/plugins/<name>/. Docs: docs/sdk/reference/game-tge.md (repo root).
//
//   - every pse:: call goes through pse/hostlink (pse.host), linked in from the repo root (pse/);
//   - patching = DesertStormFix's broker-backed games/ds1/src/core/patch.cpp, so every byte group / hook has an owner:
//     the feature's sub-plugin "tgefix.<feature>" (OwnerScope);
//   - settings: pse.settings of "tgefix" (HKCU\Software\PSE\tge\Plugins\tgefix) - FeatWidescreen, FeatMods; no in-game
//     menu for them yet (the PLUGINS menu is DesertStormFix's);
//   - log lines (loader + plugins + features) -> GreatEscapeFix.log in dev builds; release builds (no log of their own)
//     leave them to the loader's PSE\Logs\loader.log.
#include <windows.h>

#include <deque>
#include <string>

#include "core/log.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "features/features.h"
#include "hostlink.h"
#include "loader.h"

namespace {
PSEPluginHandle g_core = 0;
features::Settings g_settings;

#ifndef DS_DIST
void PSE_CALL LogSink(const char* line) { dslog::Write("%s", line); }
void PSE_CALL ReportSink(const char* line, int error) { dslog::Write("%s%s", error ? "[report] [fail] " : "[report] ", line); }
#endif

// The level events (the layer's): mission = a "*.dll" level (Level_Init), everything else = menus / films / logos.
void PSE_CALL OnMessage(const PSEMessage* msg, void*) {
    if (msg->type == PSE_MSG_DEVICE_CREATED) features::DisplayDeviceCreated();
}

int PSE_CALL OnLevelLoadBegin(PSEEventId, void* data, void*) {
    const char* level = static_cast<const char*>(data);
    const size_t n = level ? strlen(level) : 0;
    features::SetInMission(n > 4 && _stricmp(level + n - 4, ".dll") == 0);
    dslog::Write("Level: %s", level && *level ? level : "(none)");
    return 0;
}

struct Feature {
    const char* name;      // "tgefix.mods"
    const char* display;
    uint32_t flags;        // PSE_PLUGIN_SIMULATION or PSE_PLUGIN_CLIENT_ONLY
    void (*apply)();
};
// One row per feature = one internal plugin; Apply runs inside its owner scope.
const Feature kFeatures[] = {
    {"tgefix.devbg", "Background test mode (dev builds)", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyDevBackground},
    {"tgefix.widescreen", "Widescreen display modes", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyWidescreen},
    {"tgefix.mods", "Mods folder", PSE_PLUGIN_SIMULATION, &features::ApplyMods},
    {"tgefix.display", "Windowed / borderless window", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyDisplay},
    {"tgefix.hud", "Menu and HUD scaling", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyGuiScale},
    {"tgefix.framecap", "Frame cap", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyFrameCap},
};

void RegisterFeatures() {
    static std::deque<PSEPluginInfo> infos;   // must outlive the process
    for (const Feature& f : kFeatures) {
        if (pse_host::Find(f.name)) continue;
        infos.push_back({sizeof(PSEPluginInfo), 1, f.name, f.display, "GreatEscapeFix", pse_host::kTgeFixVersion,
                         PSE_API_VERSION, nullptr, f.flags});
        if (!pse::RegisterInternalPlugin(&infos.back())) dslog::Write("[fail] PSE: internal plugin %s not registered", f.name);
    }
}

// pse.settings of "tgefix" (bool, restart; flags e.g. PSE_SETTING_LAUNCHER).
bool DeclareBool(const char* key, bool def, const char* label, const char* help, uint32_t flags = 0) {
    const PSESettingsInterface* s = pse::SettingsInterface();
    if (!s) return def;
    PSESettingDesc d = {};
    d.size = sizeof d;
    d.key = key;
    d.type = PSE_SETTING_BOOL;
    d.flags = flags;   // no PSE_SETTING_LIVE: restart
    d.defaultInt = def ? 1 : 0;
    d.minInt = 0;
    d.maxInt = 1;
    d.label = label;
    d.help = help;
    d.group = "GREATESCAPEFIX";
    if (s->Declare(g_core, &d) != PSE_OK) return def;
    return s->GetInt(g_core, key) != 0;
}
int32_t DeclareInt(const char* key, int32_t def, int32_t lo, int32_t hi, int32_t step, const char* label, const char* help) {
    const PSESettingsInterface* s = pse::SettingsInterface();
    if (!s) return def;
    PSESettingDesc d = {};
    d.size = sizeof d;
    d.key = key;
    d.type = PSE_SETTING_INT;
    d.defaultInt = def;
    d.minInt = lo;
    d.maxInt = hi;
    d.step = step;
    d.label = label;
    d.help = help;
    d.group = "GREATESCAPEFIX";
    if (s->Declare(g_core, &d) != PSE_OK) return def;
    return s->GetInt(g_core, key);
}
}  // namespace

const features::Settings& features::Get() { return g_settings; }
void features::SaveWindowed(bool on) {
    if (const PSESettingsInterface* s = pse::SettingsInterface()) s->SetInt(g_core, "Windowed", on ? 1 : 0);
}

PSEPluginHandle pse_host::Core() { return g_core; }
PSEPluginHandle pse_host::Find(const char* name) { return g_core && name ? pse::FindPlugin(name) : 0; }

extern "C" {
__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info;
const char* const kGames[] = {"tge", nullptr};
const PSEPluginInfo PSEPlugin_Info = {sizeof(PSEPluginInfo), 1, "tgefix", "GreatEscapeFix", "GreatEscapeFix",
                                      pse_host::kTgeFixVersion, 0, kGames, PSE_PLUGIN_SIMULATION};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    dslog::Open();
    if (!pse::hostlink::Attach(pse)) {
        dslog::Write("[fail] tgefix.dll needs PSE's pse.host (manifest host = true, game layer pse_tge.dll)");
        return 0;
    }
    g_core = pse->plugin;
#ifndef DS_DIST
    const PSEHostInterface* host = pse::hostlink::Host();
    host->SetLogSink(&LogSink);
    host->SetReportSink(&ReportSink);
#endif
    dslog::Write("GreatEscapeFix " DS_VERSION " loaded as a PSE host plugin (game %s)", pse->gameId);
    g_settings.widescreen = DeclareBool("FeatWidescreen", true, "WIDESCREEN MODES",
                                        "List every display mode in the launcher, not only the four 4:3 / 5:4 ones");
    g_settings.filmsFill = DeclareBool("FilmsFill", true, "FILMS FILL THE SCREEN",
                                       "Films and the main menu's background fill a widescreen (top and bottom cropped) "
                                       "instead of black side bars");
    g_settings.mods = DeclareBool("FeatMods", true, "MODS FOLDER",
                                  "Files in <game>\\Mods\\<mod>\\ replace or add game files by name");
    g_settings.windowed = DeclareBool("Windowed", false, "WINDOWED",
                                      "Play in a window (a mode as big as the monitor = a borderless window); "
                                      "launcher Settings... > Windowed", PSE_SETTING_LAUNCHER | PSE_SETTING_LIVE);
    g_settings.hudScaling = DeclareBool("HudScaling", true, "HUD & MENU SCALING",
                                        "Scale the menus and the HUD with the resolution (800x600 looks as before)");
    g_settings.hudScalePercent = static_cast<uint32_t>(DeclareInt(
        "HudScalePercent", 100, 25, 400, 25, "HUD SIZE", "Size of the menus and HUD on top of the resolution scaling, %"));
    g_settings.frameCap = DeclareBool("FrameCap", true, "FRAME CAP",
                                      "Cap the frame rate (the game runs uncapped otherwise - over 1000 fps)");
    g_settings.fpsCap = static_cast<uint32_t>(DeclareInt("FpsCap", 0, 0, 240, 10, "FRAME LIMIT",
                                                         "Frames per second; 0 = the monitor's refresh rate (max 240)"));
    RegisterFeatures();
    for (const Feature& f : kFeatures) {
        patch::OwnerScope scope(pse_host::Find(f.name));
        f.apply();
    }
    if (const PSEMessagingInterface* messaging = pse::MessagingInterface())
    {
        messaging->Subscribe(pse->plugin, PSE_EVENT_LEVEL_LOAD_BEGIN, &OnLevelLoadBegin, nullptr, 0);
        messaging->RegisterListener(pse->plugin, nullptr, &OnMessage, nullptr);
    }
    return 1;
}
}
