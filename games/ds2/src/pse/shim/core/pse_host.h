#pragma once
// ds2fix (PSE host plugin build): the host-side interface DesertStormFix's broker-backed patch.cpp expects
// (pse_host::Core = the owner of patches made outside any feature scope), implemented by src/pse/ds2fix.cpp.
#include <cstddef>

#include "pse/pse_api.h"

namespace pse_host {
constexpr uint32_t kDs2FixVersion = PSE_MAKE_VERSION(0, 1, 0);   // "ds2fix" and every ds2fix.<feature> (= CMake VERSION)

bool Init(const PSELoadInterface* pse);
PSEPluginHandle Core();
PSEPluginHandle Register(const PSEPluginInfo* info);
PSEPluginHandle Find(const char* name);
struct Feature {
    const char* name;      // "ds2fix.hud"
    const char* display;
    uint32_t flags;        // PSE_PLUGIN_SIMULATION or PSE_PLUGIN_CLIENT_ONLY
    void (*apply)();
};
void RegisterFeatures(const Feature* table, size_t count);
void Shutdown();
void RunAs(const char* plugin, void (*fn)());
}  // namespace pse_host
