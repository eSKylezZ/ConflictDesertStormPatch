#pragma once
// cgsfix: the host-side interface DesertStormFix's broker-backed patch.cpp expects (pse_host::Core = the owner of
// patches made outside any feature scope), implemented by src/pse/cgsfix.cpp.
#include <cstddef>

#include "pse/pse_api.h"

namespace pse_host {
constexpr uint32_t kCgsFixVersion = PSE_MAKE_VERSION(0, 1, 0);   // "cgsfix" and every cgsfix.<feature> (= CMake VERSION)

PSEPluginHandle Core();
PSEPluginHandle Find(const char* name);
}  // namespace pse_host
