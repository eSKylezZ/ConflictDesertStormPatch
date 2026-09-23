// Joypad: after GetDeviceState in the joystick poll FUN_00538ed0 (0x538F2A), a cave at 0x5E5CB0 remaps
// rgbButtons through a 16-byte table and turns the POV hat into buttons 12-15. Bytes: scripts/patch_defs.py
// (CONTROLLER).
#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "generated/patch_tables.h"

namespace {
constexpr uint32_t kDeadzoneVa = 0x538997;  // DIPROP_DEADZONE immediate, 0-10000
}

void features::ApplyController() {
    const auto& s = settings::Get();
    if (!s.controller) {
        patch::RevertGroup("Controller", gen::kPad);
        return;
    }
    if (patch::ApplyGroup("Controller", gen::kPad))
        patch::WriteValue(kDeadzoneVa, static_cast<int32_t>(s.padDeadzone * 100));
}
