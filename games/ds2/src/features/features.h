#pragma once
#include <cstdint>
#include <string>
#include <vector>

// One Apply*() per feature, called once from Init() (dllmain.cpp) before the game's own start-up code runs.
// Features come over from DesertStormFix (reference/ds1-features) once their DS2 addresses are mapped (PORTING.md).
namespace features {

// Which CDS II.exe this is. Every feature verifies its patch sites' bytes anyway (patch::ApplyGroup), so an
// unknown build only means those features stay off.
enum class GameExe { Unknown, Retail100, Retail100NoDisc };
GameExe IdentifyExe();
const char* ExeName(GameExe e);

void ApplyWidescreen();  // launcher lists every display mode (widescreen, 4K)
void ApplyHud();  // HUD / menu art scaled to the resolution (hud.cpp)
void ApplyHudFixes();  // DS1's HUD fixes: raw vs scaled sizes, bars, text centring (hudfix.cpp; run by ApplyHud)
// Sheets the HUD setters scaled (hud.cpp): put at the current k again (HUD size changed, virtual screen on / off),
// saved / restored around the in-mission menus, forgotten at each level load.
void RescaleHudSheets();
void SaveHudSheets();
void RestoreHudSheets();
void ForgetHudSheets();
// HUD scaling on / off + size in percent (HudScaling / HudScalePercent), switched live and saved (in-game option)
bool HudScaling();
uint32_t HudScalePercent();
void SetHudScale(bool on, uint32_t percent);
void OnHudScale(float k);  // hud.cpp -> hudfix.cpp: the HUD art scale k changed (paddings in pixels follow)
void ApplyFrontEnd();  // front-end laid out at 1280x1024, scaled + centred by graphics.cpp (frontend.cpp)
void ApplyFontSharp();  // sharper text: font pages swapped for enlarged, sharpened copies (fontsharp.cpp)
void ApplyCinematics();  // skip the start-up logo films (SkipIntro) (cinematics.cpp)
void ApplyMenus();  // main menu SINGLE PLAYER / CO-OP / OPTIONS / QUIT + co-op screen state (menus.cpp, menus.h)
void ApplySplitScreen();  // local split screen 2-4 players (splitscreen.cpp)
int SplitPlayers();       // views built for the running level (1 = no split screen)
// split screen: w / h become the smallest view's size (HUD art scale k per view, hud.cpp); unchanged otherwise
void SplitViewSize(int& w, int& h);
void ApplyCoop();  // co-op join screen + session (coop.cpp; the join screen needs the menu feature's menus.h)
// co-op session from the join screen (coop.cpp): players joined (0 = none), each player's joystick (-1 = keyboard)
int CoopPlayers();
int CoopJoystick(int player);
// Mods folder (mods.cpp): <game>\Mods\<mod>\** files replace / add files the game opens by name; report in
// Mods\DesertStorm2Fix-report.txt (ModLog = log line + report line; "[fail]" lines count as problems)
void ApplyMods();
int ModCount();
int ModProblems();
void ModLog(const char* fmt, ...);
bool ModsFolderEnabled();                         // FeatMods
const std::vector<std::string>& LoadedMods();     // folders in the index now (name order, without '_')
// The index rebuilt from the folders as they are now (the MODS menu's Apply): changed = file names whose source changed;
// restart = those the game had already read outside a mission (start-up / front-end) + a file too big for the heap made
// at start-up; nextLevel = changed names read by missions only (read again at the next level load).
struct ModReload {
    bool ok = false;
    int changed = 0, restart = 0, nextLevel = 0;
    std::string firstRestart;
};
ModReload ReloadMods();
// Files in the index now: every file whose name ends in ext (".SKIN"), as {path, mod folder}; and one file's path
// ("" = no mod has it). ModHasTexture: X.dds / .tga / .png / .bmp / .ddl (or the name itself) in a mod.
struct ModFileRef {
    std::string path, mod;
};
std::vector<ModFileRef> ModFilesWithExtension(const char* ext);
std::string ModFilePath(const char* name);
bool ModHasTexture(const std::string& name);
// Levels from mods (mods_levels.cpp, sub-plugin ds2fix.modlevels): mission DLLs loaded from mods, <stem>.level
// ("--@archive mission01"), game data by reference (.uses / .envpatch / .rmdpatch / .dllpatch built from the player's
// install at start-up and by the MODS menu's Apply - BuildModReferences, called by mods.cpp after the index), octree +
// AI navigation rebuilt for a scene a mod changed. AddBuiltModFile = mods.cpp's index entry for such a built file.
void ApplyModLevels();
void BuildModReferences();
bool AddBuiltModFile(const std::string& name, const std::string& path, const std::string& mod);
// Tables, texts and readable weapon / vehicle / unit files (mods_tables.cpp): BuildModTables (called by mods.cpp after
// every index build) merges <TABLE>.txt / .replace.txt parts with the game's rows, checks the unit / vehicle tables,
// turns .weapon / .vehicle / .unit files into rows (+ retextured model copies) and reads .strings; ModText = a mod's
// text for a catalog hash (menus.cpp's text lookup hook asks it), null = none. SetModFile / AddModRoot (mods.cpp) =
// a built file into the index, replacing a mod's own file of that name / a folder the opener reads from disk.
void BuildModTables();
// true while a mod gives a unit more UnitWpns.txt rows than the game (more objects at level start): mods_levels.cpp
// rebuilds the level octree then (the shipped .OCT no longer fits)
bool ModKitsGrown();
const wchar_t* ModText(uint32_t hash);
void SetModFile(const std::string& name, const std::string& path, int mod);
void AddModRoot(const std::string& folder);
// .skin files (skins.cpp, sub-plugin ds2fix.skins): textures / bodies / names per soldier; read with the index
// (ApplySkins at start-up, ReloadSkins after the MODS menu's Apply - applied to soldiers made from then on)
void ApplySkins();
void ReloadSkins();
// Sounds from the Mods folder (mods_sound.cpp, sub-plugin ds2fix.modsound): .sch bank packs added to every level's
// sound cache, a .sch named like a level cache = that level's sounds replaced, .psf music, .bik films; ReloadModSounds
// after the MODS menu's Apply. For .weapon files (mods_weapons): MakeWeaponSoundBank = new bank <newName> (key ELF hash
// of "<newName>.sbk") made from game bank <donor> (WEAPS.TXT col 68) with its shot samples (and / or the distant tail)
// re-encoded from WAV files in `dir`, added to every level; BorrowSoundBank = a game bank of another level added to
// every level (sound = <weapon>). Both report into the mod report (`file` names the .weapon) and return false on a
// problem. Call them before a level loads (at start-up / ReloadMods); ClearWeaponSoundBanks drops what they made.
void ApplyModSounds();
void ReloadModSounds();
bool MakeWeaponSoundBank(const std::string& donor, const std::string& newName, const std::string& dir,
                         const std::vector<std::string>& shots, const std::string& tail, const std::string& file);
bool BorrowSoundBank(const std::string& name);
void ClearWeaponSoundBanks();
// .uses "bank X" (mods_levels): game bank X (with its samples) added to every level for an editor mod that doesn't ship
// it; `file` names the .uses for the report. ClearGameBankReferences drops those (not the .weapon ones) before a rebuild.
bool BorrowGameBank(const std::string& name, const std::string& file);
void ClearGameBankReferences();
// MODS menu (modsmenu.cpp): main menu item MODS -> every folder in <game>\Mods ON / OFF (renames '_'), Apply, and the
// main menu's "N MODS INSTALLED (M ACTIVE)" line; needs the menu feature (menus.cpp) and FeatMods
void ApplyModsMenu();
// Weapon pictures (weaponicons.cpp, sub-plugin ds2fix.weaponicons, PSE build): a .weapon's "hud icon = <picture file>"
// -> AddWeaponIcon = n for the Weaps.txt picture name "DS2FIX_ICON_<n>" (-1 = not available: standalone build / hook
// failed - keep the base picture); basePicture = the row's own picture name (draw offset, fallback), file = the
// .weapon (report). WeaponPictureKnown = a game picture name ("IMAGE_WEAPON_SAW"). Apply before ds2fix.mods.
void ApplyWeaponIcons();
int AddWeaponIcon(const std::string& path, const std::string& basePicture, const std::string& file);
bool WeaponPictureKnown(const std::string& name);
// Lua mission scripts (scripthost.cpp, sub-plugin ds2fix.scripthost; needs FeatMods): ".lua" levels run by Lua 5.4
// (header lines --@archive / --@base / --@scene), "+level <name>" on the command line, custom campaigns
// (campaigns.cpp: Mods\<mod>\campaign.txt, SINGLE PLAYER > CUSTOM CAMPAIGNS). ScriptArchiveFor (mods_levels.cpp, at a
// level's first archive open): isLua = the level is a .lua one, result = its "--@archive" value ("" = none given).
// StartCustomLevel: the campaign flow loads `name` next (a .lua / mod level, or a campaign entry by name).
void ApplyScriptHost();
void ApplyCampaigns();
std::string ScriptArchiveFor(const char* level, bool& isLua);
bool ScriptLevelActive();
bool ScriptHasBase();
const char* QuickStartLevel();
void StartCustomLevel(const char* name);
const char* CampaignNext(const char* level);
void ApplyDevBackground();  // dev builds: off-screen windowed test runs, back-buffer shots, StartLevel (devbg.cpp)

}  // namespace features
