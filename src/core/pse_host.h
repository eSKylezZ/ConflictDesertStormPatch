#pragma once
// DesertStormFix as a host plugin of the Pivotal Script Extender (pse/, docs/sdk/DESIGN.md, docs/sdk/layout.md):
// dsfix.dll in <game>\PSE\Plugins, loaded by PSE\pse_loader.dll with the private pse.host table; the features are its
// internal sub-plugins ("dsfix.<feature>", docs/sdk/ds1-plugins.md).
#include <cstddef>

#include "pse/pse_api.h"

namespace pse_host {
constexpr uint32_t kVersion = PSE_MAKE_VERSION(3, 3, 0);   // "dsfix" and every dsfix.<feature> (= CMake VERSION)

// First thing in PSEPlugin_Load (before any feature patches): pse.host attached, log + mod-report sinks, the game
// layer's providers. false = not loaded as a host plugin (no pse.host) - DesertStormFix can't run.
bool Init(const PSELoadInterface* pse);
// "dsfix": DesertStormFix's own (external, host) plugin. Owner of patches made outside any feature's scope.
PSEPluginHandle Core();
// Registers one internal feature plugin (info must be static). Returns 0 before Init.
PSEPluginHandle Register(const PSEPluginInfo* info);
// A registered plugin by name ("dsfix.lockstep"), or 0. For patch::OwnerScope around a feature's lazy hooks.
PSEPluginHandle Find(const char* name);

// One DesertStormFix feature = one internal plugin; Init / OnSettingsChanged run its Apply function inside its
// patch::OwnerScope, so the broker records show which feature owns which bytes. Several rows may share a plugin.
struct Feature {
    const char* name;      // "dsfix.hud"
    const char* display;   // "HUD scaling"
    uint32_t flags;        // PSE_PLUGIN_SIMULATION or PSE_PLUGIN_CLIENT_ONLY
    void (*apply)();       // may be null: plugin only (its hooks are made later, in its own scope)
};
// Registers every plugin of the table (first row of a name wins), then nothing else.
void RegisterFeatures(const Feature* table, size_t count);
// Window destroyed (never DLL_PROCESS_DETACH - loader lock): the loader sends PSE_MSG_EXIT and unloads the other
// external plugins in reverse order (dsfix.dll itself stays loaded).
void Shutdown();
// Runs fn inside the named plugin's owner scope ("dsfix" if it isn't registered).
void RunAs(const char* plugin, void (*fn)());
}  // namespace pse_host
