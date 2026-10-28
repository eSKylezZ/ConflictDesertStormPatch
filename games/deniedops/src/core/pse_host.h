#pragma once
// deniedopsfix: the host-side interface DesertStormFix's broker-backed patch.cpp expects (pse_host::Core = the owner of
// patches made outside any feature scope), implemented by src/pse/deniedopsfix.cpp.
#include <cstddef>

#include "pse/pse_api.h"

namespace pse_host {
constexpr uint32_t kDeniedOpsFixVersion = PSE_MAKE_VERSION(0, 1, 0);   // = CMake VERSION

PSEPluginHandle Core();
PSEPluginHandle Find(const char* name);
}  // namespace pse_host
