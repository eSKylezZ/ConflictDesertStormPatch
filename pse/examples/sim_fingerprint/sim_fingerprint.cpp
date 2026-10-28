// example.sim_fingerprint - a PSE_PLUGIN_SIMULATION plugin that declares a simulation fingerprint
// (PSEPlugin_SimFingerprint, docs/sdk/online-compat.md). The online plugin-set check compares that string's SHA-256
// instead of the DLL's own hash, so a debug build and a release build of this source can play together - as long as
// the author changes the string whenever the plugin's effect on the game changes. Changes nothing in the game.
// pse/tests/test_plugins.cpp loads it next to example.sim_rules (file hash).
#include <pse/pse_api.h>

namespace {
PSEPluginHandle g_self = 0;
}

extern "C" {

__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info = {
    sizeof(PSEPluginInfo), 1, "example.sim_fingerprint", "Simulation fingerprint (example plugin)", "PSE",
    PSE_MAKE_VERSION(1, 0, 0), 0, nullptr, PSE_PLUGIN_SIMULATION,
};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    g_self = pse->plugin;
    return 1;
}

// Bump the revision whenever the plugin changes what happens in a mission.
__declspec(dllexport) const char* PSE_CALL PSEPlugin_SimFingerprint(void) {
    return "example.sim_fingerprint rules revision 1";
}

}  // extern "C"
