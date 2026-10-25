// dsfix.online: online co-op - the connection (net.cpp), the online menus and lobby (online.cpp, lobby*.cpp,
// onlinepause.cpp), the plugin-set check + mod sync (onlinecompat.cpp, modsync.cpp), lockstep / host authority
// (lockstep.cpp, authority.cpp, puppetsync.cpp, hostsync.cpp), own view + nameplates (viewmode.cpp, nameplates.cpp)
// and the determinism harness (determinism.cpp, dev). One plugin DLL since 2026-10-25, they call each other
// throughout. What they need of dsfix.dll goes through the kit's host client (src/core/dsfix_host.spec); what dsfix.dll
// needs of them is the interface dsfix.online (online.spec; dsfix.dll's wrappers answer defaults without this DLL:
// no online menus, no session, split screen and single player unchanged).
#include <windows.h>

#include <string>
#include <vector>

#include "core/log.h"
#include "core/net.h"
#include "features/features.h"
#include "kit/kit.h"
#include "online/generated/online_iface.h"

#include "online/generated/online_export.inc"

namespace {
bool Load() {
    if (!kit::Services()) return false;
    features::ApplyNameplates();
    features::ApplyViews();
    features::ApplyDeterminism();
    return RegisterOnlineInterface();
}
}  // namespace

DSFIX_KIT_PLUGIN("dsfix.online", "Online co-op", PSE_PLUGIN_SIMULATION, Load)
