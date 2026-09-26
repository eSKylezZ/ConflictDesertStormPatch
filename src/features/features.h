#pragma once
#include <cstdint>

// Each Apply* brings the game in line with the current settings (patch or un-patch). Called once before
// WinMain, and again when the launcher saves new settings (before any game code uses the patched sites).
namespace features {
enum class DisplayMode : uint32_t { Fullscreen, Windowed, Borderless };  // settings::Values::displayMode
enum class PadType { Keyboard, PlayStation, Xbox, Generic };

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
void ApplyGraphics();    // anti-aliasing (MSAA) + anisotropic filtering via the D3D8 device
void ApplyCoop();        // front-end CO-OP entry + join screen (Xbox-style local co-op set-up)
void ApplyFontSharp();   // sharper text: font texture enlarged to the HUD scale
void ApplyControls();    // Options -> CONTROLS: keyboard & mouse / controller (per-player pad layouts)
void ApplyMouse();       // mouse acceleration off by default (game option, value kept in our settings)
void ApplyAudio();       // audio listener stays behind the soldier while aiming (no louder mix when zoomed)
void ApplyTooltips();    // inventory: "use" hints (MediKit on yourself, fire mode)
void ApplyRumble();      // weapon rumble retuned (nothing for throwing / placing); notes what fired per player
void ApplyDevHooks();    // development builds only: test hooks (start level)
void OnFrameDev();       // development builds only: F9 = test pop-up message

void OnFrame();            // once per game frame (from the frame-cap hook), before update + render
void OnFrameInput();       //   in-game input device tracking (ingame_input.cpp)
void OnFrameSplitScreen(); //   split-screen viewports (splitscreen.cpp)
void OnFrameDiscord();     //   Discord status snapshot (discord.cpp)
void OnFrameCoop();        //   co-op join screen (coop.cpp)
void OnFrameFontSharp();   //   drops sharp font copies of unloaded fonts (fontsharp.cpp)
void OnFrameControls();    //   builds the CONTROLS screens (controls.cpp)

PadType JoystickType(int joystick);  // the game's joystick j by vendor (Xbox, else PlayStation layout)
// Reads joystick j now: bits 0-15 = the game's buttons (after the per-pad fix-up), 16-19 left stick up/right/down/left.
uint32_t ReadJoystick(int joystick);
void FollowActivePad();  // once per frame: player 1's joystick = the pad used last (not in co-op missions)
int SplitScreenPlayers();  // local players in the current level (1 = single player)
bool SplitHudOffset(float& x, float& y);  // during a split view's HUD pass: that view's origin
const char* ControlsText(uint32_t hash);  // CONTROLS screens' own texts, nullptr if not ours
void* ControlsScreen(uint32_t state);     // our front-end screens for states 0x3F.., nullptr if none
bool KeyProfileSaved(int profile);        // keyboard & mouse profile 0-3 exists
void ApplyCoopKeyProfiles();              // co-op keyboard players' chosen profiles into their binding sets
int CoopKeyProfile(int player);           // co-op player's keyboard profile (-1 = current keys / not a keyboard)
bool KeyboardInUse();                     // the last menu input came from the keyboard / mouse
bool PadStyleXbox();                      // the pad last used is an Xbox pad
const char* CoopText(uint32_t hash);  // text of the co-op screen's own string hashes, nullptr if not ours
int CoopPlayers();                    // players of the running co-op session (0 = none)
int CoopJoystick(int player);         // that player's game joystick index, -1 = keyboard & mouse
int PlayerOfJoystick(int joystick);   // player (0-3) a game joystick belongs to: co-op session / join screen, else 0
// Trigger feel for what a player fires: held weapon, or the weapon that fired in the last 1.5 s (mounted / vehicle).
enum class TriggerFeel { Default, None, Light, Rifle, Heavy, MachineGun, Launcher };
TriggerFeel PlayerTriggerFeel(int player);
const char* ControlIconText(int player, uint32_t action);  // icon character of a player's button for an action
const char* GameText(uint32_t hash);  // the game's own text for a catalog.dat hash (current language), or nullptr
int CoopPromptStyle(int player);      // co-op player's icon style: 0 keyboard, 1 PlayStation, 2 Xbox; -1 no session

void OnSettingsChanged();  // re-applies everything that depends on settings
}  // namespace features
