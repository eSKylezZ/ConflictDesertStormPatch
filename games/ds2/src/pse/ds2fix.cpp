// DesertStorm2Fix as a host plugin of the Pivotal Script Extender (PSE) - SKELETON (2026-10-19).
//
// The same features as the dinput8.dll build (src/dllmain.cpp), but loaded by PSE: <game>\dinput8.dll = the PSE shim,
// PSE\pse_loader.dll + PSE\Games\pse_ds2.dll (the DS2 game layer: detection, pse.ids, pse.game.ds2, the core game
// events) and this DLL as PSE\Plugins\ds2fix.dll + ds2fix.pse ("host = true"; the DS2 layer allows the name "ds2fix").
// Docs: docs/sdk/reference/game-ds2.md (repo root) "DesertStorm2Fix as a host plugin".
//
//   - every pse:: call goes through pse/hostlink (pse.host), linked in from the repo root (pse/);
//   - patching = DesertStormFix's broker-backed src/core/patch.cpp at the repo root (a superset of games/ds2/src/core/patch.h),
//     so every byte group / call hook / jump has an owner: the feature's sub-plugin "ds2fix.<feature>" (OwnerScope);
//   - log lines (loader + plugins + features) -> DesertStorm2Fix.log (this DLL's sink replaces the layer's loader.log).
// Not yet: settings through pse.settings (still HKLM\...\CDS II\Enhancements), the D3D9 device hooks through the broker
// (graphics.cpp writes vtable slots itself - chained after the layer's, see game-ds2.md), PSE_MSG_EXIT (the layer sends
// it at WM_DESTROY).
#include <windows.h>

#include <deque>

#include "core/log.h"
#include "core/patch.h"
#include "core/proxy.h"
#include "core/pse_host.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/graphics.h"
#include "hostlink.h"
#include "loader.h"

// DirectInput for the launcher pad code (core/gamepad.cpp): the SYSTEM dinput8.dll (the game folder's is the PSE shim).
FARPROC RealDInput8(const char* name) {
    static HMODULE real = nullptr;
    if (!real) {
        char path[MAX_PATH];
        const UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 16) return nullptr;
        strcat_s(path, "\\dinput8.dll");
        real = LoadLibraryA(path);
        if (!real) return nullptr;
    }
    return GetProcAddress(real, name);
}

// ---- pse_host (the DS1 host's interface, core/pse_host.h, implemented for DS2) ----
namespace {
PSEPluginHandle g_core = 0;
void PSE_CALL LogSink(const char* line) { dslog::Write("%s", line); }
void PSE_CALL ReportSink(const char* line, int error) { dslog::Write("%s%s", error ? "[report] [fail] " : "[report] ", line); }
}  // namespace

bool pse_host::Init(const PSELoadInterface* pse) {
    if (!pse::hostlink::Attach(pse)) return false;
    g_core = pse->plugin;
    const PSEHostInterface* host = pse::hostlink::Host();
    host->SetLogSink(&LogSink);
    host->SetReportSink(&ReportSink);
    return true;
}
PSEPluginHandle pse_host::Core() { return g_core; }
PSEPluginHandle pse_host::Register(const PSEPluginInfo* info) { return g_core ? pse::RegisterInternalPlugin(info) : 0; }
PSEPluginHandle pse_host::Find(const char* name) { return g_core && name ? pse::FindPlugin(name) : 0; }
void pse_host::RegisterFeatures(const Feature* table, size_t count) {
    static std::deque<PSEPluginInfo> infos;   // must outlive the process
    for (size_t i = 0; i < count; ++i) {
        const Feature& f = table[i];
        if (Find(f.name)) continue;
        infos.push_back({sizeof(PSEPluginInfo), 1, f.name, f.display, "DesertStorm2Fix", kDs2FixVersion,
                         PSE_API_VERSION, nullptr, f.flags});
        if (!Register(&infos.back())) dslog::Write("[fail] PSE: internal plugin %s not registered", f.name);
    }
}
void pse_host::RunAs(const char* plugin, void (*fn)()) {
    if (!fn) return;
    patch::OwnerScope scope(Find(plugin));
    fn();
}
void pse_host::Shutdown() {
    if (g_core) pse::hostlink::Shutdown();
}

namespace {
// One row per feature = one internal plugin; Apply runs inside its owner scope (dllmain.cpp's Init order).
const pse_host::Feature kFeatures[] = {
    {"ds2fix.widescreen", "Widescreen launcher modes", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyWidescreen},
    {"ds2fix.hud", "HUD scaling", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyHud},
    {"ds2fix.frontend", "Front end at any resolution", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyFrontEnd},
    {"ds2fix.fontsharp", "Sharper text", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyFontSharp},
    {"ds2fix.cinematics", "Skip intro films", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyCinematics},
    {"ds2fix.menus", "Menus (co-op entry)", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyMenus},
    {"ds2fix.splitscreen", "Split screen", PSE_PLUGIN_SIMULATION, &features::ApplySplitScreen},
    {"ds2fix.coop", "Co-op join screen", PSE_PLUGIN_SIMULATION, &features::ApplyCoop},
    {"ds2fix.graphics", "Direct3D hooks", PSE_PLUGIN_CLIENT_ONLY, &graphics::Install},
    {"ds2fix.devbg", "Dev background test mode", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyDevBackground},
};
}  // namespace

extern "C" {
__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info;
const char* const kGames[] = {"ds2", nullptr};
const PSEPluginInfo PSEPlugin_Info = {sizeof(PSEPluginInfo), 1, "ds2fix", "DesertStorm2Fix", "DesertStorm2Fix",
                                      pse_host::kDs2FixVersion, 0, kGames, PSE_PLUGIN_SIMULATION};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    dslog::Open();
    if (!pse_host::Init(pse)) {
        dslog::Write("[fail] ds2fix.dll needs PSE's pse.host (manifest host = true, game layer pse_ds2.dll)");
        return 0;
    }
    dslog::Write("DesertStorm2Fix " DS_VERSION " loaded as a PSE host plugin (game %s)", pse->gameId);
    dslog::Write("Game: CDS II.exe, %s", features::ExeName(features::IdentifyExe()));
    settings::Load();
    pse_host::RegisterFeatures(kFeatures, sizeof kFeatures / sizeof kFeatures[0]);
    for (const auto& f : kFeatures) pse_host::RunAs(f.name, f.apply);
    return 1;
}
}
