// Launcher resolution list FUN_004487f0 only adds modes with |w/h - 4/3| < eps; the filter jump is NOPed.
// The 3D projection (FUN_0054fb80) is already Hor+, so nothing else is needed.
//
// Its own PSE plugin since 2026-10-23 (PSE\Plugins\dsfix.widescreen.dll, on the dsfix kit - games/ds1/plugins/kit).
#include "core/patch.h"
#include "kit/kit.h"
#include "generated/patch_tables.h"

void ApplyWidescreen() { patch::ApplyGroup("Widescreen", gen::kWidescreen); }

DSFIX_KIT_PLUGIN("dsfix.widescreen", "Widescreen resolutions", PSE_PLUGIN_CLIENT_ONLY, [] {
    ApplyWidescreen();
    return true;
})
