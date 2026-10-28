#pragma once
// GreatEscapeFix features: one Apply per internal plugin "tgefix.<feature>" (src/tgefix.cpp runs each in its owner
// scope at start-up).
#include <cstdint>
#include <string>
#include <vector>

namespace features {
// What the features read from tgefix's pse.settings (HKCU\Software\PSE\tge\Plugins\tgefix), filled before Apply.
struct Settings {
    bool widescreen = true;   // FeatWidescreen
    bool filmsFill = true;    // FilmsFill: films zoomed to the screen's width (widescreen.cpp)
    bool mods = true;         // FeatMods
    bool windowed = false;    // Windowed (launcher Settings... > Windowed; a mode as big as the monitor = borderless)
    bool hudScaling = true;   // HudScaling: menus + HUD scaled with the resolution (guiscale.cpp)
    uint32_t hudScalePercent = 100;   // HudScalePercent 25..400: on top of the resolution's factor
    bool frameCap = true;     // FrameCap (framecap.cpp)
    uint32_t fpsCap = 0;      // FpsCap: 0 = the monitor's refresh rate (max 240), else 30..240
    bool controller = true;   // Controller: PlayStation / Xbox pads mapped to the game's PS2 layout (controller.cpp)
    uint32_t padDeadzone = 40;   // PadDeadzone 0..90 %: stick deadzone
    bool padVibration = true;    // PadVibration: rumble (+ Xbox trigger motors) from the game's force-feedback table
    bool padTriggers = true;     // PadTriggers: DualSense adaptive trigger on the fire trigger
    bool padModernLayout = true; // PadModernLayout: L1 <-> L2, R1 <-> R2 (fire on R2 / RT, as DS1's Default preset)
    bool actionPrompts = true;   // ActionPrompts: "Press [X] to pick up ..." in missions (contextprompts.cpp)
};
const Settings& Get();

void ApplyWidescreen();   // widescreen.cpp: every display mode in the launcher
void ApplyMods();         // mods.cpp: <game>\Mods\<mod>\... files replace / add game files by name
void ApplyGuiScale();     // guiscale.cpp: the GUI laid out on a virtual screen, drawn k times bigger
// guiscale.cpp: a pointer position (back-buffer pixels) -> the GUI's own coordinates (display.cpp's mouse mapping).
void GuiToVirtual(long& x, long& y);
// guiscale.cpp: how many times larger than its design the GUI draws right now (1 outside GUI drawing).
float GuiDrawScale();
void ApplyFontSharp();    // fontsharp.cpp: font pages enlarged + sharpened while text is scaled up (with HudScaling)
void ApplyDisplay();      // display.cpp: the launcher's Windowed box, windowed / borderless window, mouse mapping
void ApplyFrameCap();     // framecap.cpp: wait on every frame event, monitor refresh / FpsCap, max 240
void ApplyFilms();        // films.cpp: Bink films from the Mods folder, films on request
// films.cpp: plays the Bink film `name` ("FMV02", "x.bik"; Mods folder first, then Data\FMV) on the next frame, the
// game frozen until it ends or Esc / Space / Enter skips it.
void PlayFilm(const char* name);
bool FilmFillsScreen();   // films.cpp: the film the game opened last may be zoomed to the width (FilmsFill)
// scripthost.cpp: ".lua" levels (Lua 5.4), campaigns (campaign.txt), "+level" / "+campaign" on the command line.
void ApplyScriptHost();
std::string ScriptArchiveFor(const std::string& stem);   // a .lua level's data archive ("" = its own name's)
void StartCampaign(int index);
// The front end's CUSTOM MISSIONS list: every campaign (campaign.txt), then every .lua level at the top of a switched-on
// mod that no campaign lists.
struct CustomMission {
    std::string name, detail;   // shown: name / "campaign, 3 missions" or "mission"
    int campaign = -1;          // index for StartCampaign, else -1
    std::string level;          // the .lua level
};
std::vector<CustomMission> CustomMissions();
void StartCustomMission(const CustomMission& m);
void ApplyController();   // controller.cpp: DirectInput pads remapped to the game's PS2 numbering, POV = D-pad
// controllerpic.cpp: the PS5 / Xbox pad picture with what each button does (from the key file, modern swap applied).
void DrawControllerPicture(float x, float y, float w, float h, bool xbox, float alpha = 1.0f);
// controllerpic.cpp: the key file the game uses (Documents\SCi\GreatEscape\current.key, else the game folder's
// current.key, else default.key); empty if none.
std::string KeyFilePath();
// contextprompts.cpp (part of tgefix.ingameinput): the context action prompt drawn while one is available.
void ApplyContextPrompts();
void ForgetJoystick(int joystick);   // controller.cpp: slot j emptied / refilled (hot-plug) - forget its pad type
bool PadInUseIsXbox();
void ApplyPadDeadzone();             // controller.cpp: PadDeadzone on the open pads + those opened later               // controller.cpp: player 1's pad (else the first one seen) is an Xbox pad
void ApplyLauncherPad();   // launcher.cpp: pad navigation + hints in the launcher / Settings / Advanced dialogs
void OnFrameHotplug();
// ingame_input.cpp: menus, prompts, key names and the input mode follow the device in use; player 1's pad pulled out
// in a mission pauses the game (hotplug.cpp reports slots emptied / filled).
void ApplyInGameInput();
void PlayerPadChanged(int joystick, bool connected);    // hotplug.cpp (per frame, from controller.cpp): pads plugged in / pulled out
// devbg.cpp (dev builds): Dev\Background = scripted tests off-screen, never activated (release: no-ops / false)
void ApplyDevBackground();
bool Background();
// display.cpp's Settings... dialog wrapper hands every message on (after the game's): picks Dev\Resolution, OK unseen
void DevBgSettingsDialog(void* dlg, unsigned msg);
// tgefix.cpp: stores `Windowed` (pse.settings) - display.cpp calls it from the Settings dialog's OK.
void SaveWindowed(bool on);
// tgefix.cpp: stores a tgefix setting (pse.settings) and updates Get() at once (the game's menus, fixmenus.cpp).
void SetSetting(const char* key, int32_t value);
// display.cpp: the D3D device was just made (tgefix.cpp's PSE_MSG_DEVICE_CREATED listener) - window to the front.
void DisplayDeviceCreated();

// modsmenu.cpp: main menu MODS (a patched GE_FrontEnd.gui + its screen); nothing without FeatMods
void ApplyModsMenu();
// fixmenus.cpp: OPTIONS > GREATESCAPEFIX, OPTIONS > CONTROLS > CONTROLLER (+ the button picture), main menu CUSTOM
// MISSIONS - screens added to the game's GUI files.
void ApplyFixMenus();

// mods.cpp
void ModLog(const char* fmt, ...);   // log + Mods\GreatEscapeFix-report.txt
int ModCount();
// mods.cpp: the switched-on mods in load order (index = ModFile mod number); a built file served like a mod's own.
const std::vector<std::string>& LoadedMods();
void SetModFile(const std::string& name, const std::string& path, int mod);
// mods_tables.cpp: merged tables, .strings, .weapon / .vehicle / .unit (after every index build); a mod text by hash.
void BuildModTables();
const wchar_t* ModText(uint32_t hash);
void ApplyModTables();
// mods_levels.cpp: .uses / .envpatch / .dllpatch built from the player's install; mission DLLs + .level redirects.
void BuildModLevels();
// mods_sound.cpp: bank packs, level sound replacement, streams; banks for .weapon files (borrowed / made from WAVs).
// weaponicons.cpp: picture files for .weapon "hud icon" (hooks before the tables are built).
void ApplyWeaponIcons();
int AddWeaponIcon(const std::string& path, const std::string& basePicture, const std::string& file);
bool WeaponPictureKnown(const std::string& name);
void ScanModSounds();
void ApplyModSounds();
bool BorrowSoundBank(const std::string& name, const std::string& file);
bool MakeWeaponSoundBank(const std::string& donor, const std::string& newName, const std::string& dir,
                         const std::vector<std::string>& shots, const std::string& tail, const std::string& file);
void ApplyModLevels();
// Full path of the switched-on mod file with this name (folders ignored), or null.
const char* ModFilePath(const char* name);
int ModProblems();
// The Mods folder index built again (folders switched on / off since start-up; the MODS menu's APPLY).
void ReloadModsIndex();
// The level the game loaded last is a mission ("*.dll" level) - tgefix.cpp follows the layer's level events.
void SetInMission(bool inMission);
bool InMission();
}  // namespace features
