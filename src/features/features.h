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
void ApplyInGameInput(); // in-game menus: hide the cursor + pad prompts while a pad is in use
void ApplySplitScreen(); // local split screen: player set-up for players 2-4 (views are built per frame)
void ApplyDiscord();     // Discord Rich Presence (mission, single player / split screen + players)
void ApplyDevHooks();    // development builds only: test hooks (start level)

void OnFrame();            // once per game frame (from the frame-cap hook), before update + render
void OnFrameInput();       //   in-game input device tracking (ingame_input.cpp)
void OnFrameSplitScreen(); //   split-screen viewports (splitscreen.cpp)
void OnFrameDiscord();     //   Discord status snapshot (discord.cpp)

int SplitScreenPlayers();  // local players in the current level (1 = single player)

void OnSettingsChanged();  // re-applies everything that depends on settings
}  // namespace features
