#pragma once
// cvfix (PSE host plugin build, src/pse/cvfix.cpp) only: this repo's core/patch.h is replaced by DesertStormFix's
// broker-backed one (a superset: the same BytePatch / ApplyGroup / HookCall / HookIndirectCall / WriteJump / Write,
// plus patch::OwnerScope, HookSlot, HookFunction ...). Its implementation is games/ds1/src/core/patch.cpp.
// CMake puts this folder first on the include path of the cvfix target and defines CV_PSE_PATCH_H.
#include CV_PSE_PATCH_H
