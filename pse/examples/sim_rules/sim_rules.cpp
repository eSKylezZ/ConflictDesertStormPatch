// example.sim_rules - a plugin flagged PSE_PLUGIN_SIMULATION that changes nothing. It exists for the online plugin-set
// check (docs/sdk/online-compat.md): a PC that has it can only play online with a PC that has the very same file.
// pse/tests/test_compat.cpp and the two-copy online test install it on one side only to see the join refused.
#include <pse/pse_api.h>

namespace {
PSEPluginHandle g_self = 0;
}

extern "C" {

__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info = {
    sizeof(PSEPluginInfo), 1, "example.sim_rules", "Simulation rules (example plugin)", "PSE",
    PSE_MAKE_VERSION(1, 2, 0), 0, nullptr,
    PSE_PLUGIN_SIMULATION,   // "changes gameplay": online peers must have the same name + version + file hash
};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    g_self = pse->plugin;
    auto log = static_cast<const PSELogInterface*>(pse->Query(g_self, PSE_LOG_INTERFACE, 1));
    if (log) log->Write(g_self, PSE_LOG_INFO, "sim_rules: loaded (a simulation plugin - online peers need it too)");
    return 1;
}

}  // extern "C"
