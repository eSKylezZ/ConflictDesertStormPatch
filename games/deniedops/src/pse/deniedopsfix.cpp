// DeniedOpsFix: Conflict: Denied Ops' enhancement patch as the host plugin of the Pivotal Script Extender (PSE)
// (games/deniedops/CLAUDE.md, docs/sdk/reference/game-deniedops.md).
//
// Loaded by PSE: <game>\dinput8.dll = the PSE shim, PSE\pse_loader.dll + PSE\Games\pse_deniedops.dll (the layer:
// detection, pse.game.deniedops) and this DLL as PSE\Plugins\deniedopsfix.dll + .pse ("host = true").
//   - every pse:: call goes through pse/hostlink (pse.host);
//   - patching = DesertStormFix's broker-backed games/ds1/src/core/patch.cpp: every hook is owned by the feature's
//     internal plugin "deniedopsfix.<feature>";
//   - the exe is SteamStub-wrapped: code patches wait until the stub has decrypted .text (features/unpack.cpp);
//   - settings: pse.settings of "deniedopsfix" (HKCU\Software\PSE\deniedops\Plugins\deniedopsfix) - FeatWidescreen,
//     FeatMods, FeatModsMenu, FeatCampaigns, FeatThirdPerson; no in-game menu for them yet.
#include <windows.h>

#include <deque>

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

struct Feature {
    const char* name;
    const char* display;
    uint32_t flags;
    void (*apply)();
};
const Feature kFeatures[] = {
    {"deniedopsfix.widescreen", "Widescreen display modes", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyWidescreen},
    {"deniedopsfix.mods", "Mods folder", PSE_PLUGIN_SIMULATION, &features::ApplyMods},
    {"deniedopsfix.campaigns", "Custom campaigns", PSE_PLUGIN_SIMULATION, &features::ApplyCampaigns},
    {"deniedopsfix.modsmenu", "MODS menu", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyModsMenu},
    {"deniedopsfix.loadingscreen", "Loading-screen texts from mods", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyLoadingScreen},
    {"deniedopsfix.thirdperson", "Third-person camera", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyThirdPerson},
    {"deniedopsfix.luamission", "Lua missions on ported levels", PSE_PLUGIN_SIMULATION, &features::ApplyLuaMission},
    {"deniedopsfix.prompt", "On-screen prompts for Lua missions", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyPrompt},
#ifndef DS_DIST
    {"deniedopsfix.devinput", "Dev: scripted menu input", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyDevInput},
#endif
};

void RegisterFeatures() {
    static std::deque<PSEPluginInfo> infos;   // must outlive the process
    for (const Feature& f : kFeatures) {
        if (pse_host::Find(f.name)) continue;
        infos.push_back({sizeof(PSEPluginInfo), 1, f.name, f.display, "DeniedOpsFix", pse_host::kDeniedOpsFixVersion,
                         PSE_API_VERSION, nullptr, f.flags});
        if (!pse::RegisterInternalPlugin(&infos.back())) dslog::Write("[fail] PSE: internal plugin %s not registered", f.name);
    }
}

bool DeclareBool(const char* key, bool def, const char* label, const char* help) {
    const PSESettingsInterface* s = pse::SettingsInterface();
    if (!s) return def;
    PSESettingDesc d = {};
    d.size = sizeof d;
    d.key = key;
    d.type = PSE_SETTING_BOOL;
    d.defaultInt = def ? 1 : 0;
    d.minInt = 0;
    d.maxInt = 1;
    d.label = label;
    d.help = help;
    d.group = "DENIEDOPSFIX";
    if (s->Declare(g_core, &d) != PSE_OK) return def;
    return s->GetInt(g_core, key) != 0;
}
}  // namespace

const features::Settings& features::Get() { return g_settings; }
PSEPluginHandle pse_host::Core() { return g_core; }
PSEPluginHandle pse_host::Find(const char* name) { return g_core && name ? pse::FindPlugin(name) : 0; }

extern "C" {
__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info;
const char* const kGames[] = {"deniedops", nullptr};
const PSEPluginInfo PSEPlugin_Info = {sizeof(PSEPluginInfo), 1, "deniedopsfix", "DeniedOpsFix", "DeniedOpsFix",
                                      pse_host::kDeniedOpsFixVersion, 0, kGames, PSE_PLUGIN_SIMULATION};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    dslog::Open();
    if (!pse::hostlink::Attach(pse)) {
        dslog::Write("[fail] deniedopsfix.dll needs PSE's pse.host (manifest host = true, game layer pse_deniedops.dll)");
        return 0;
    }
    g_core = pse->plugin;
#ifndef DS_DIST
    const PSEHostInterface* host = pse::hostlink::Host();
    host->SetLogSink(&LogSink);
    host->SetReportSink(&ReportSink);
#endif
    dslog::Write("DeniedOpsFix " DS_VERSION " loaded as a PSE host plugin (game %s)", pse->gameId);
    g_settings.widescreen = DeclareBool("FeatWidescreen", true, "WIDESCREEN MODES",
                                        "List every display mode the graphics card offers, not only the game's 15");
    g_settings.mods = DeclareBool("FeatMods", true, "MODS FOLDER",
                                  "Files in <game>\\Mods\\<mod>\\ replace or add game files by name");
    g_settings.modsMenu = DeclareBool("FeatModsMenu", true, "MODS MENU",
                                      "A MODS item in the main menu switches each Mods folder on or off");
    g_settings.campaigns = DeclareBool("FeatCampaigns", true, "CUSTOM CAMPAIGNS",
                                       "A CUSTOM CAMPAIGNS item in the main menu plays the campaign.txt campaigns of mods");
    g_settings.thirdPerson = DeclareBool("FeatThirdPerson", true, "THIRD-PERSON CAMERA",
                                         "F7 switches between first and third person (DS1 levels start in third person)");
    RegisterFeatures();
    for (const Feature& f : kFeatures) {
        patch::OwnerScope scope(pse_host::Find(f.name));
        f.apply();
    }
    return 1;
}
}
