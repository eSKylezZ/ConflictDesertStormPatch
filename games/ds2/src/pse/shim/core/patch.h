#pragma once
// ds2fix (PSE host plugin build, src/pse/ds2fix.cpp) only: this repo's core/patch.h is replaced by DesertStormFix's
// broker-backed one (a superset: the same BytePatch / ApplyGroup / HookCall / HookIndirectCall / WriteJump / Write,
// plus patch::OwnerScope, HookSlot, HookFunction ...). Its implementation is src/core/patch.cpp at the repo root.
// CMake puts this folder first on the include path of the ds2fix target and defines DS2_PSE_PATCH_H.
#include DS2_PSE_PATCH_H
