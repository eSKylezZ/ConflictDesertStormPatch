// Game clock FUN_004ba370 computed ms as QPC*1000/QPF on an x87 FPU that D3D8 leaves at 24-bit precision,
// so the clock moved in 8-64 ms steps on long uptimes and the frame timer reused the previous dt when it had
// not moved (turbo at high FPS). Replaced with exact integer math; see tools/build_timer.py.
#include "core/patch.h"
#include "features/features.h"
#include "generated/patch_tables.h"

void features::ApplyTiming() { patch::ApplyGroup("Frame timing", gen::kTiming); }
