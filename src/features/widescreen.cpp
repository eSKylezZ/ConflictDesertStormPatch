// Launcher resolution list FUN_004487f0 only adds modes with |w/h - 4/3| < eps; the filter jump is NOPed.
// The 3D projection (FUN_0054fb80) is already Hor+, so nothing else is needed.
#include "core/patch.h"
#include "features/features.h"
#include "generated/patch_tables.h"

void features::ApplyWidescreen() { patch::ApplyGroup("Widescreen", gen::kWidescreen); }
