// dsfix.loadout: the mission briefing + loadout screen (customise.cpp), kits / unlocks / carried kits (loadout.cpp),
// soldier gear (gear.cpp), vehicle swaps (vehicles.cpp), the mission overview (overview.cpp) and skins applied mid-level
// (skinapply.cpp) - one plugin DLL since 2026-10-25, they call each other throughout. What they need of dsfix.dll goes
// through the kit's host client (src/core/dsfix_host.spec); what dsfix.dll needs of them is the interface
// dsfix.loadout (loadout.spec; without the plugin: no briefing / loadout screen, the game's own kits and vehicles).
// Its co-save holds KITS, GEAR and VEHS; saves from before the merge have GEAR / VEHS under dsfix.gear /
// dsfix.vehicles - the manifest adopts those names.
#include <windows.h>

#include <string>
#include <vector>

#include "core/log.h"
#include "features/features.h"
#include "kit/kit.h"
#include "loadout/generated/loadout_iface.h"

#include "loadout/generated/loadout_export.inc"

namespace {
bool Load() {
    if (!kit::Services()) return false;
    features::ApplySkins();
    features::ApplyOverview();
    features::ApplyLoadout();
    features::ApplyCustomise();
    features::ApplyGear();
    return RegisterLoadoutInterface();
}
}  // namespace

DSFIX_KIT_PLUGIN("dsfix.loadout", "Briefing + loadout screen, kits, gear, vehicle swaps", PSE_PLUGIN_SIMULATION, Load)
