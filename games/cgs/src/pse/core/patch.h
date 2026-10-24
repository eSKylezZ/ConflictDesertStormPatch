#pragma once
// cgsfix: DesertStormFix's broker-backed patch API (games/ds1/src/core/patch.h; implementation games/ds1/src/core/
// patch.cpp, compiled into cgsfix): BytePatch / ApplyGroup / HookSlot ..., every hook owned by the patch::OwnerScope's
// plugin ("cgsfix.<feature>"). CMake defines CGS_PSE_PATCH_H. The standalone GlobalStormFix build keeps its own
// games/cgs/src/core/patch.h (same BytePatch layout, its own ApplyGroup) - this header exists only so
// games/cgs/src/pse/features/*.cpp can `#include "core/patch.h"` unchanged and get the broker instead when built into
// cgsfix (include-path priority: src/pse comes before src for that target).
#include CGS_PSE_PATCH_H
