// dsfix.dll's side of the features that live in DesertStormFix's group plugins: each features::X they implement is a
// generated wrapper here that calls the plugin's interface, or answers the spec's default when the plugin is missing
// (scripts/gen_host_calls.py; the specs are plugins/<group>/<group>.spec).
#include <windows.h>

#include <string>
#include <vector>

#include "core/log.h"
#include "core/pse_host.h"
#include "features/features.h"
#include "loader.h"

#include "photo/generated/photo_iface.h"
#include "generated/photo_wrappers.inc"

#include "core/net.h"
#include "core/settings.h"
#include "online/generated/online_iface.h"
#include "generated/online_wrappers.inc"
#include "missions/generated/missions_iface.h"
#include "generated/missions_wrappers.inc"
#include "loadout/generated/loadout_iface.h"
#include "generated/loadout_wrappers.inc"
#include "features/padio.h"
#include "coop/generated/coop_iface.h"
#include "generated/coop_wrappers.inc"
#include "render/d3d8to9.h"
#include "render/water.h"
#include "graphics/generated/graphics_iface.h"
#include "generated/graphics_wrappers.inc"
#include "barcolour/generated/barcolour_iface.h"
#include "generated/barcolour_wrappers.inc"
