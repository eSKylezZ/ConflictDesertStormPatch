// dsfix.timing: frame timing (timing.cpp) - the game clock's exact integer math (patch group TIMING), the exact frame
// time (the frame timer wrapped), the high-FPS landing fix (character mover) and the Karma push-apart scaled to the
// physics step. Own DLL since 2026-10-25 (SIMULATION: it changes how far things move per frame, so online peers must
// match). It reads the frame cap's frame start, V-Sync's rate and whether a virtual clock runs from dsfix.dll (host
// client). Without it: the game's own timing (turbo at high frame rates, whole-ms frame times).
#include <windows.h>

#include "features/features.h"
#include "kit/kit.h"

namespace {
bool Load() {
    features::ApplyTiming();
    return true;
}
}  // namespace

DSFIX_KIT_PLUGIN("dsfix.timing", "Frame timing + high-FPS fixes", PSE_PLUGIN_SIMULATION, Load)
