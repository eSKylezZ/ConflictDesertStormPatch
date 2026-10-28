// GlobalStormFix: Conflict: Global Storm's PSE host plugin - widescreen + Mods folder only (the standalone
// games/cgs dinput8.dll build keeps everything else: frame cap/timing, HUD/menu scaling, dev background mode; port
// those the same way later if the PSE build should carry them too).
//
// Loaded by PSE: <game>\dinput8.dll = the PSE shim, PSE\pse_loader.dll + PSE\Games\pse_cgs.dll (the CGS game layer:
// detection, pse.ids, pse.game.cgs - see pse/games/cgs/cgs_layer.cpp for why it has no core-event hooks yet) and this
// DLL as PSE\Plugins\cgsfix.dll + cgsfix.pse ("host = true"). Docs: docs/sdk/reference/game-cgs.md.
//
//   - every pse:: call goes through pse/hostlink (pse.host), linked in from the repo root (pse/);
//   - patching = DesertStormFix's broker-backed games/ds1/src/core/patch.cpp (src/pse/core/patch.h forwards to it),
//     so every byte group / hook has an owner: the feature's sub-plugin "cgsfix.<feature>" (OwnerScope);
//   - settings: pse.settings of "cgsfix" (HKCU\Software\PSE\cgs\Plugins\cgsfix) - FeatWidescreen, FeatMods, migrated
//     once from the standalone build's `HKLM\SOFTWARE\Pivotal\Conflict Global\Enhancements` key if present;
//   - log lines -> GlobalStormFix.log in dev builds; release builds leave them to PSE\Logs\loader.log.
#include <windows.h>

#include <deque>
#include <string>
#include <vector>

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

// Dev: the first access violations (handled or not) with EIP, address and the code addresses on the stack.
LONG CALLBACK CrashLog(EXCEPTION_POINTERS* e) {
    static LONG count = 0;
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || InterlockedIncrement(&count) > 4)
        return EXCEPTION_CONTINUE_SEARCH;
    const CONTEXT* c = e->ContextRecord;
    char line[512];
    int n = sprintf_s(line, "[dev] access violation at %08lX (%s %08lX) eax %08lX ecx %08lX edx %08lX esi %08lX edi %08lX stack:",
                      c->Eip, e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                      static_cast<unsigned long>(e->ExceptionRecord->ExceptionInformation[1]), c->Eax, c->Ecx, c->Edx,
                      c->Esi, c->Edi);
    const DWORD* sp = reinterpret_cast<const DWORD*>(c->Esp);
    for (int i = 0, found = 0; i < 256 && found < 12 && n < 480; ++i) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || !(mbi.Protect & (PAGE_READWRITE | PAGE_READONLY))) break;
        DWORD v = sp[i];
        if (VirtualQuery(reinterpret_cast<void*>(v), &mbi, sizeof mbi) &&
            (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
            n += sprintf_s(line + n, sizeof line - n, " %08lX", v);
            ++found;
        }
    }
    dslog::Write("%s", line);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

struct Feature {
    const char* name;      // "cgsfix.mods"
    const char* display;
    uint32_t flags;        // PSE_PLUGIN_SIMULATION or PSE_PLUGIN_CLIENT_ONLY
    void (*apply)();
};
const Feature kFeatures[] = {
    {"cgsfix.widescreen", "Widescreen display modes", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyWidescreen},
    {"cgsfix.mods", "Mods folder", PSE_PLUGIN_SIMULATION, &features::ApplyMods},
    {"cgsfix.frame", "Per-frame hook (D3D9 re-hook, dev background mode)", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyFrameHook},
    {"cgsfix.devbg", "Background test mode (dev builds)", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyDevBackground},
    {"cgsfix.modsmenu", "MODS item in the main menu", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyModsMenu},
};

void RegisterFeatures() {
    static std::deque<PSEPluginInfo> infos;   // must outlive the process
    for (const Feature& f : kFeatures) {
        if (pse_host::Find(f.name)) continue;
        infos.push_back({sizeof(PSEPluginInfo), 1, f.name, f.display, "GlobalStormFix", pse_host::kCgsFixVersion,
                         PSE_API_VERSION, nullptr, f.flags});
        if (!pse::RegisterInternalPlugin(&infos.back())) dslog::Write("[fail] PSE: internal plugin %s not registered", f.name);
    }
}

bool DeclareBool(const char* key, bool def, const char* label, const char* help, uint32_t flags = 0) {
    const PSESettingsInterface* s = pse::SettingsInterface();
    if (!s) return def;
    PSESettingDesc d = {};
    d.size = sizeof d;
    d.key = key;
    d.type = PSE_SETTING_BOOL;
    d.flags = flags;
    d.defaultInt = def ? 1 : 0;
    d.minInt = 0;
    d.maxInt = 1;
    d.label = label;
    d.help = help;
    d.group = "GLOBALSTORMFIX";
    if (s->Declare(g_core, &d) != PSE_OK) return def;
    return s->GetInt(g_core, key) != 0;
}
}  // namespace

const features::Settings& features::Get() { return g_settings; }

PSEPluginHandle pse_host::Core() { return g_core; }
PSEPluginHandle pse_host::Find(const char* name) { return g_core && name ? pse::FindPlugin(name) : 0; }

extern "C" {
__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info;
const char* const kGames[] = {"cgs", nullptr};
const PSEPluginInfo PSEPlugin_Info = {sizeof(PSEPluginInfo), 1, "cgsfix", "GlobalStormFix", "GlobalStormFix",
                                      pse_host::kCgsFixVersion, 0, kGames, PSE_PLUGIN_SIMULATION};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    dslog::Open();
    if (!pse::hostlink::Attach(pse)) {
        dslog::Write("[fail] cgsfix.dll needs PSE's pse.host (manifest host = true, game layer pse_cgs.dll)");
        return 0;
    }
    g_core = pse->plugin;
#ifndef DS_DIST
    const PSEHostInterface* host = pse::hostlink::Host();
    host->SetLogSink(&LogSink);
    host->SetReportSink(&ReportSink);
    AddVectoredExceptionHandler(0, CrashLog);
#endif
    dslog::Write("GlobalStormFix " DS_VERSION " loaded as a PSE host plugin (game %s)", pse->gameId);
    // One-time migration from the standalone build's registry key (games/cgs/CLAUDE.md "Environment": HKLM virtualized
    // to a HKCU VirtualStore copy) - a player who already ran the standalone GlobalStormFix keeps their widescreen
    // choice when they switch to the PSE build. The old key is never changed.
    {
        const std::vector<pse::RegistrySource> sources = {
            {HKEY_LOCAL_MACHINE, "SOFTWARE\\Pivotal\\Conflict Global\\Enhancements", 0},
            {HKEY_CURRENT_USER,
             "Software\\Classes\\VirtualStore\\MACHINE\\SOFTWARE\\WOW6432Node\\Pivotal\\Conflict Global\\Enhancements",
             0},
        };
        pse::MigrateRegistryValues(sources, HKEY_CURRENT_USER, pse::PluginSettingsKey(g_core), "MigratedFromEnhancements");
    }
    g_settings.widescreen = DeclareBool("FeatWidescreen", true, "WIDESCREEN MODES",
                                        "List every display mode in the launcher, not only the four 4:3 / 5:4 ones");
    g_settings.mods = DeclareBool("FeatMods", true, "MODS FOLDER",
                                  "Files in <game>\\Mods\\<mod>\\ replace or add whole game files by name");
    g_settings.modsMenu = DeclareBool("FeatModsMenu", true, "MODS MENU",
                                      "MODS item in the main menu: turn Mods\\ folders on / off");
    RegisterFeatures();
    for (const Feature& f : kFeatures) {
        patch::OwnerScope scope(pse_host::Find(f.name));
        f.apply();
    }
    return 1;
}
}
