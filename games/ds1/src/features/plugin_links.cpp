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
