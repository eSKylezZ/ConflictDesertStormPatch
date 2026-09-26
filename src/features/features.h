#pragma once
#include <cstdint>

// Each Apply* brings the game in line with the current settings (patch or un-patch). Called once before
// WinMain, and again when the launcher saves new settings (before any game code uses the patched sites).
namespace features {
enum class DisplayMode : uint32_t { Fullscreen, Windowed, Borderless };  // settings::Values::displayMode

void ApplyWidescreen();  // launcher lists non-4:3 modes
void ApplyHud();         // resolution-scaled HUD/menus + layout clamps
void ApplyController();  // DualShock 4 / DualSense remap + D-pad
void ApplyTiming();      // exact game clock (no turbo at high FPS)
void ApplyFrameCap();    // frame limiter: min(cap, monitor refresh)
void ApplyLauncher();    // "Enhancements" settings in the launcher's Detail Settings dialog
void ApplyInGameInput(); // in-game menus: hide the cursor + pad prompts while a pad is in use
void ApplySplitScreen(); // local split screen: player set-up for players 2-4 (views are built per frame)
void ApplyDiscord();     // Discord Rich Presence (mission, single player / split screen + players)
void ApplyDisplay();     // display mode: fullscreen / windowed / borderless window
void ApplyCinematics();  // skip the start-up logos / mission cutscenes
void ApplyGraphics();
void ApplyCoop();        // front-end CO-OP entry + join screen (Xbox-style local co-op set-up)    // anti-aliasing (MSAA) + anisotropic filtering via the D3D8 device
void ApplyDevHooks();    // development builds only: test hooks (start level)

void OnFrame();            // once per game frame (from the frame-cap hook), before update + render
void OnFrameInput();       //   in-game input device tracking (ingame_input.cpp)
void OnFrameSplitScreen(); //   split-screen viewports (splitscreen.cpp)
void OnFrameDiscord();     //   Discord status snapshot (discord.cpp)
void OnFrameCoop();        //   co-op join screen (coop.cpp)

int SplitScreenPlayers();  // local players in the current level (1 = single player)
bool SplitHudOffset(float& x, float& y);  // during a split view's HUD pass: that view's origin
const char* CoopText(uint32_t hash);  // text of the co-op screen's own string hashes, nullptr if not ours
int CoopPlayers();                    // players of the running co-op session (0 = none)
int CoopJoystick(int player);         // that player's game joystick index, -1 = keyboard & mouse
const char* GameText(uint32_t hash);
int CoopPromptStyle(int player);      // co-op player's icon style: 0 keyboard, 1 PlayStation, 2 Xbox; -1 no session  // the game's own text for a catalog.dat hash (current language), or nullptr

void OnSettingsChanged();  // re-applies everything that depends on settings
}  // namespace features
