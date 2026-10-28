#pragma once
// cvfix (PSE host plugin build): the host-side interface VietnamFix's reused feature files expect via
// patch::OwnerScope / RunAs (pse_host::Core = the owner of patches made outside any feature scope), implemented by
// src/pse/cvfix.cpp. Modelled on games/ds2/src/pse/shim/core/pse_host.h.
#include <cstddef>

#include "pse/pse_api.h"

namespace pse_host {
constexpr uint32_t kCvFixVersion = PSE_MAKE_VERSION(0, 1, 0);   // "cvfix" and every cvfix.<feature> (= CMake VERSION)

bool Init(const PSELoadInterface* pse);
PSEPluginHandle Core();
PSEPluginHandle Register(const PSEPluginInfo* info);
PSEPluginHandle Find(const char* name);
struct Feature {
    const char* name;      // "cvfix.mods"
    const char* display;
    uint32_t flags;        // PSE_PLUGIN_SIMULATION or PSE_PLUGIN_CLIENT_ONLY
    void (*apply)();
};
void RegisterFeatures(const Feature* table, size_t count);
void Shutdown();
void RunAs(const char* plugin, void (*fn)());
}  // namespace pse_host
