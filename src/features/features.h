#pragma once

// Each Apply* brings the game in line with the current settings (patch or un-patch). Called once before
// WinMain, and again when the launcher saves new settings (before any game code uses the patched sites).
namespace features {
void ApplyWidescreen();  // launcher lists non-4:3 modes
void ApplyHud();         // resolution-scaled HUD/menus + layout clamps
void ApplyController();  // DualShock 4 / DualSense remap + D-pad
void ApplyTiming();      // exact game clock (no turbo at high FPS)
void ApplyFrameCap();    // frame limiter: min(cap, monitor refresh)
void ApplyLauncher();    // "Enhancements" settings in the launcher's Detail Settings dialog

void OnSettingsChanged();  // re-applies everything that depends on settings
}  // namespace features
