#pragma once
// tgefix: the host-side interface DesertStormFix's broker-backed patch.cpp expects (pse_host::Core = the owner of
// patches made outside any feature scope), implemented by src/tgefix.cpp.
#include <cstddef>

#include "pse/pse_api.h"

namespace pse_host {
constexpr uint32_t kTgeFixVersion = PSE_MAKE_VERSION(0, 1, 0);   // "tgefix" and every tgefix.<feature> (= CMake VERSION)

PSEPluginHandle Core();
PSEPluginHandle Find(const char* name);
}  // namespace pse_host
