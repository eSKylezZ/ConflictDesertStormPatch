#pragma once
#include <string>
#include <vector>
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
void ApplyShadows();      // shadow volumes end below the caster (no shadows through floors / walls)
void ApplyTooltips();    // inventory: "use" hints (MediKit on yourself, fire mode)
void ApplyDiagnostics(); // game messages / file opens in the log (dev), performance overlay (F11)
void ApplyLoadout();      // squad kits: scene rewrite (dev rules) + runtime weapon spawn for the customise screen
void ApplyMods();          // loose files from Mods\<mod>\ replace / add game files (mods.cpp)
int ModCount();
bool ReadGameTable(const char* name, std::string& out);  // mods.cpp: game table incl. mod rows
bool ModHasFile(const char* name);  // mods.cpp: a mod (or a file the plugin made) provides this file
const char* ModText(uint32_t hash);  // mods.cpp: texts mods define (weapon display names), by catalog hash
const std::vector<std::string>& ModWeapons();  // mods.cpp: weapon ids made by .weapon files
void ModLog(const char* fmt, ...);  // mods.cpp: a mod message - log + Mods\DesertStormFix-report.txt ("[fail]" = problem)
int ModProblems();                  // mods.cpp: "[fail]" messages since the last build of the mod state
int ModWeaponMagazines(const std::string& id);
std::string SkinBody(const char* texture);  // mods.cpp: body model a .skin gives the soldiers wearing this texture
void ApplyBodies();                          // bodies.cpp: soldier bodies from .skin files
void OnFrameBodies();  // mods.cpp: a .weapon file's "magazines" (0 = not set)
// Factions = the skin tables' sides (uniform index 0 SAS, 1 US Delta, 2 Russian, 3 Iraqi) as bits.
constexpr int kFactionSas = 1, kFactionDelta = 2, kFactionRussian = 4, kFactionIraqi = 8;
int WeaponFactions(const char* id);  // mods.cpp: a weapon's factions (.weapon "faction", else by its id)
void ApplyCustomise();     // squad customise screen: orbit camera hook
void SetSceneFog(bool on, uint32_t colour, float start, float end);  // graphics.cpp
void SceneFogBegin();
bool LoadoutUsesFillers();
bool CustomiseOpen();        // squad customise screen is up (customise.cpp)
void OnFrameCustomise();
void OnFrameLoadoutDev();   // dev: F8 spawns an MP5SD, F7 swaps the uniform (loadout.cpp)
// Kit changes on the customise screen (loadout.cpp): slots 0 main weapon, 1 sidearm, 2 launcher, 3 grenades.
std::vector<std::string> KitWeaponIds();                  // every weapon a kit slot can take (squad list + .weapon)
void KitUnlock(const char* weapon);                       // counts as found in this mode (the mission's kits)
bool KitUnlocked(const char* weapon);                     // found in this mode (single player / co-op), or a mod weapon
int KitSlotOf(const char* weapon);                        // -1 = not a kit weapon
std::vector<std::string> KitChoices(int slot);            // weapons this level can hand out for a slot
// weapon "" = remove only; mags > 0 = magazines to hand out (else the slot's / the .weapon file's count)
bool KitSwap(void* soldier, void* oldItem, const std::string& weapon, int slot, int mags = 0);
int KitRounds(void* item);                                // rounds an item holds (reserve + magazine)
// Uniform variants from .skin files with "uniform name = ..." (mods.cpp): rows of the skin tables (uniform 0 SAS /
// 1 US Delta, role = squad slot) that the customise screen offers on its UNIFORM row, not as other soldiers.
struct SkinVariant {
    int uniform, role;
    std::string texture, name;  // name as shown, upper case
};
const std::vector<SkinVariant>& SkinVariants();
// Mod weapon pictures (weaponicons.cpp): HUD weapon panel, inventory, pick-up icon.
int AddWeaponIcon(const std::string& path, const std::string& baseIcon);  // -> n for Weaps.txt "DSFIX_ICON_<n>"
void* WeaponIconTexture(const void* sheet, int image);  // the texture to bind for that image, or null
void ApplyWeaponIcons();
void OnFrameWeaponIcons();
void ResetWeaponIcons();   // hot reload (mods.cpp)
bool ReloadMods();         // mods.cpp: rebuild from the Mods folders + the game's tables again (front-end only)
int ModsGeneration();      // bumped by every reload
const std::vector<std::string>& LoadedMods();  // folder names of the mods in use
// VERSUS menu (versus.cpp): LEGACY ONLINE (the game's network screens) / SPLIT SCREEN (local versus, to come).
void* VersusScreen();                 // screen for front-end state 0x42
void* VersusSetupScreen();            // MATCH SETUP screen, front-end state 0x44
uint32_t VersusItemText();            // the main menu item's text hash
const char* VersusText(uint32_t hash);
void ApplyVersus();
void OnFrameVersus();
bool VersusSession();                // a local versus match (MP level, local host session) is set up
void VersusAssignPlayers();          // splitscreen.cpp AssignPlayers: each player gets their MP soldier
// MODS menu (modsmenu.cpp): the Mods folders ON / OFF (renames "_Name" <-> "Name"; applies after a restart).
void* ModsMenuScreen();              // screen for front-end state 0x43
uint32_t ModsMenuItemText();         // the main menu item's text hash
const char* ModsMenuText(uint32_t hash);
void OnFrameModsMenu();
void ApplyModsMenu();   // main menu: "DESERTSTORMFIX x INSTALLED" / mod count overlay
void LogLayoutOffsetHits();  // hud.cpp, dev: which screen uses which layout-offset site
void ApplyRumble();      // weapon rumble retuned (nothing for throwing / placing); notes what fired per player
void ApplyDevHooks();    // development builds only: test hooks (start level)
void OnFrameDev();       // development builds only: F9 = test pop-up message

uint32_t VsyncFps();        // frame rate V-Sync holds the game at, 0 = not synced (graphics.cpp)
uint32_t VsyncProbeFps();   // frame cap while V-Sync is being checked, 0 = not checking
long long FrameStartQpc();  // QPC at the start of this frame's update + render (after the frame-cap wait)
void OnFrame();            // once per game frame (from the frame-cap hook), before update + render
void OnFrameInput();       //   in-game input device tracking (ingame_input.cpp)
void OnFrameSplitScreen(); //   split-screen viewports (splitscreen.cpp)
void OnFrameDiscord();     //   Discord status snapshot (discord.cpp)
void OnFrameCoop();        //   co-op join screen (coop.cpp)
void OnFrameFontSharp();   //   drops sharp font copies of unloaded fonts (fontsharp.cpp)
void OnFrameDiagnostics(); //  frame times + performance overlay (diagnostics.cpp)
void OnFrameControls();    //   builds the CONTROLS screens (controls.cpp)

PadType JoystickType(int joystick);  // the game's joystick j by vendor (Xbox, else PlayStation layout)
// Reads joystick j now: bits 0-15 = the game's buttons (after the per-pad fix-up), 16-19 left stick up/right/down/left.
uint32_t ReadJoystick(int joystick);
void FollowActivePad();  // once per frame: player 1's joystick = the pad used last (not in co-op missions)
void SetPadDeadzone(uint32_t percent);  // CONTROLLER screen: saved + applied to the open pads at once
void ForgetJoystick(int joystick);  // slot j emptied / re-filled (hot-plug): forget its pad type and direct I/O
void OnFrameHotplug();   // hotplug.cpp: pads plugged in / out while the game runs, "controller disconnected" pause
int SplitScreenPlayers();
bool DevBackground();    // dev background test mode is on (no cursor clipping / capture)
bool GameFocused();      // the game window is in front (dev background mode: always) - dev.cpp
bool KeyHeld(int vk);    // a key is down: GetAsyncKeyState in front, or the game's own key table (posted keys)
void ApplyDevBackground();  // dev: Dev\RunInBackground  // local players in the current level (1 = single player)
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
int CoopClass(int player);             // versus: the class (character skill 0 rifleman, 1 heavy, 2 sniper, 3 engineer)
int CoopTeam(int player);              // versus: the team the player picked on the join screen (-1 = none)
void OpenVersusJoin(bool keepPlayers);  // VERSUS > SPLIT SCREEN: the join screen in versus mode (coop.cpp)
void VersusPlayersJoined();             // join screen Begin in versus mode -> the match level (versus.cpp)
bool VersusTakeStart();                 // the join screen's update should load the level now (once)
void VersusJoinBack();                  // join screen Back in versus mode -> MATCH SETUP, players kept
bool VersusTeams();                     // the chosen versus mode has teams (team deathmatch; not deathmatch)
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
