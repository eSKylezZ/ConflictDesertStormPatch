// HUD scaling: the scale setters FUN_0053c890/FUN_0053c850 jump into a cave at 0x5D7B00 that multiplies by
// k = max(1, min(W/800, H/600)) * HudScale, plus 93 layout clamps. Bytes: scripts/patch_defs.py (HUD,
// LAYOUT_CLAMPS).
#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "generated/patch_tables.h"

namespace {
constexpr uint32_t kHudScaleVa = 0x5D7B00;  // float read by the cave's compute_k
}

void features::ApplyHud() {
    const auto& s = settings::Get();
    if (!s.hudScaling) {
        patch::RevertGroup("HUD scaling", gen::kHud);
        return;
    }
    if (patch::ApplyGroup("HUD scaling", gen::kHud))
        patch::WriteValue(kHudScaleVa, static_cast<float>(s.hudScalePercent) / 100.0f);
}
