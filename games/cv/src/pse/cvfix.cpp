// VietnamFix as the host plugin of the Pivotal Script Extender (PSE).
//
// Loaded by PSE: <game>\dinput8.dll = the PSE shim, PSE\pse_loader.dll + PSE\Games\pse_cv.dll (the CV game layer:
// detection, pse.ids, pse.game.cv) and this DLL as PSE\Plugins\cvfix.dll + cvfix.pse ("host = true"; the CV layer
// allows the name "cvfix"). Docs: docs/sdk/reference/game-cv.md (repo root).
//
// Scope (2026-10-24 port): widescreen display modes and the Mods folder only, per the user's request - the same
// two features the standalone dinput8.dll build (src/dllmain.cpp) already ships, reused here UNCHANGED
// (features/widescreen.cpp, features/mods.cpp) by giving them this folder's core/patch.h (-> the shared broker,
// games/ds1/src/core/patch.cpp) instead of src/core/patch.h's plain version; every byte they patch was re-verified
// against the real exe for this port (pse/ids/cv.ids.txt --verify-exe). features/devbg.cpp + features/graphics.cpp
// are reused too, unchanged, so this build can still be tested off-screen and muted like the standalone
// (games/cv/CLAUDE.md "Testing"); hud.cpp / frontend.cpp / frame.cpp / probe.cpp are NOT built here (out of scope,
// and frame.cpp's ApplyFrameHook / frontend.cpp's virtual-screen centring aren't needed by widescreen or mods) - see
// games/cv/CLAUDE.md for what that leaves out for a future port. g_afterFrontEndLoad (features.h) is a dev hook
// frontend.cpp normally wires into its own LoadFrontEnd detour; without that file this build defines the storage
// below but nothing ever calls through it, so devbg.cpp's "StartLevel" dev value is inert here (no auto-boot into a
// mission - background runs still work: the launcher is OK'd unseen and the front end's own logging and the mods /
// widescreen apply logs happen before any level loads).
//
//   - every pse:: call goes through pse/hostlink (pse.host), linked in from the repo root (pse/);
//   - patching = DesertStormFix's broker-backed games/ds1/src/core/patch.cpp, so every byte group / hook has an
//     owner: the feature's sub-plugin "cvfix.<feature>" (OwnerScope);
//   - settings: none yet (widescreen / mods have no on/off switch here, same as the standalone build - both just
//     apply at load); no in-game menu for them (the PLUGINS menu is DesertStormFix's, DS1-only so far);
//   - log lines (loader + plugins + features) -> VietnamFix.log in dev builds; release builds (no log of their
//     own) leave them to the loader's PSE\Logs\loader.log.
#include <windows.h>

#include <deque>
#include <string>

#include "core/log.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "features/features.h"
#include "features/graphics.h"
#include "hostlink.h"
#include "loader.h"

namespace features {
void (*g_afterFrontEndLoad)() = nullptr;   // frontend.cpp isn't built here; see the file header comment above
}  // namespace features

namespace {
PSEPluginHandle g_core = 0;

#ifndef DS_DIST
void PSE_CALL LogSink(const char* line) { dslog::Write("%s", line); }
void PSE_CALL ReportSink(const char* line, int error) { dslog::Write("%s%s", error ? "[report] [fail] " : "[report] ", line); }
#endif

// One row per feature = one internal plugin; Apply runs inside its owner scope.
const pse_host::Feature kFeatures[] = {
    {"cvfix.widescreen", "Widescreen display modes", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyWidescreen},
    {"cvfix.mods", "Mods folder", PSE_PLUGIN_SIMULATION, &features::ApplyMods},
    {"cvfix.modsmenu", "MODS menu (main menu item + mod list screen)", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyModsMenu},
    {"cvfix.graphics", "Direct3D 9 hooks (background test mode, windowed device)", PSE_PLUGIN_CLIENT_ONLY,
     &graphics::Install},
    {"cvfix.devbg", "Background test mode (dev builds)", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyDevBackground},
};
}  // namespace

bool pse_host::Init(const PSELoadInterface* pse) {
    if (!pse::hostlink::Attach(pse)) return false;
    g_core = pse->plugin;
#ifndef DS_DIST
    const PSEHostInterface* host = pse::hostlink::Host();
    host->SetLogSink(&LogSink);
    host->SetReportSink(&ReportSink);
#endif
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
        infos.push_back({sizeof(PSEPluginInfo), 1, f.name, f.display, "VietnamFix", kCvFixVersion, PSE_API_VERSION,
                         nullptr, f.flags});
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

extern "C" {
__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info;
const char* const kGames[] = {"cv", nullptr};
const PSEPluginInfo PSEPlugin_Info = {sizeof(PSEPluginInfo), 1, "cvfix", "VietnamFix", "VietnamFix",
                                      pse_host::kCvFixVersion, 0, kGames, PSE_PLUGIN_SIMULATION};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    dslog::Open();
    if (!pse_host::Init(pse)) {
        dslog::Write("[fail] cvfix.dll needs PSE's pse.host (manifest host = true, game layer pse_cv.dll)");
        return 0;
    }
    dslog::Write("VietnamFix " DS_VERSION " loaded as a PSE host plugin (game %s)", pse->gameId);
    dslog::Write("Game: Vietnam.exe, %s", features::ExeName(features::IdentifyExe()));
    pse_host::RegisterFeatures(kFeatures, sizeof kFeatures / sizeof kFeatures[0]);
    for (const auto& f : kFeatures) pse_host::RunAs(f.name, f.apply);
    return 1;
}
}
