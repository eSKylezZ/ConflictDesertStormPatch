// DeniedOpsFix as the PSE host plugin (games/deniedops/CLAUDE.md, docs/sdk/reference/game-deniedops.md). Step 1
// (2026-10-24): no features yet - this just confirms a host plugin loads on top of the game layer. Mods folder
// comes next (see CLAUDE.md "Next").
#include <windows.h>

#include "hostlink.h"
#include "pse/pse_api.h"

namespace {
const char* const kGames[] = {"deniedops", nullptr};
}

extern "C" {
__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info;
const PSEPluginInfo PSEPlugin_Info = {sizeof(PSEPluginInfo), 1, "deniedopsfix", "DeniedOpsFix", "PSE",
                                       PSE_MAKE_VERSION(0, 1, 0), PSE_API_VERSION, kGames, PSE_PLUGIN_SIMULATION};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    if (!pse::hostlink::Attach(pse)) return 0;
    return 1;
}
}
