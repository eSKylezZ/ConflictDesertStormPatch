#pragma once
// ds2fix (PSE host plugin build) only: DesertStormFix's COM vtable hooks through the broker (src/core/comhooks.h at the
// repo root: one pointer hook per class and slot, chained with the DS2 game layer's own D3D9 device hooks).
// CMake defines DS2_PSE_COMHOOKS_H for the ds2fix target.
#include DS2_PSE_COMHOOKS_H
