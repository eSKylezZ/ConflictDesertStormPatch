#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace settings { struct Values; }   // core/settings.h

// Each Apply* brings the game in line with the current settings (patch or un-patch). Called once before
// WinMain, and again when the launcher saves new settings (before any game code uses the patched sites).
struct lua_State;  // Lua (third_party/lua), for the script host's registrations
struct DsfixWeatherV1;  // plugins/weather/dsfix_weather.h (interface of the plugin dsfix.weather)

namespace features {
enum class DisplayMode : uint32_t { Fullscreen, Windowed, Borderless };  // settings::Values::displayMode
enum class PadType { Keyboard, PlayStation, Xbox, Generic };
// Menu actions of one device on the join / lobby screens (coop.cpp CoopDeviceActions): Cross/A/Space join, Circle/B/
// Backspace leave, Options/Start/Enter start, left / right, Esc back, R1/RB/Tab cycle
enum CoopAction : uint32_t { kCoopJoin = 1, kCoopLeave = 2, kCoopStart = 4, kCoopLeft = 8, kCoopRight = 16, kCoopBack = 32, kCoopCycle = 64 };

// Own plugin DLLs since 2026-10-23 (games/ds1/plugins, dsfix kit): dsfix.widescreen, dsfix.mouse, dsfix.menus,
// dsfix.units, dsfix.limits (interface dsfix.limits - plugins/limits/dsfix_limits.h), dsfix.textfit, dsfix.shadows,
// dsfix.audio; since 2026-10-24 dsfix.difficulty, dsfix.jpfixes, dsfix.handling, dsfix.tooltips, dsfix.rumble,
// dsfix.bodies, dsfix.followers, dsfix.hud (HUD / menu scaling) on interface dsfix.services (core/dsfix_services.h,
// services.cpp).
void RegisterServices();                 // services.cpp: publishes dsfix.services (before the external plugins load)
void RegisterOverlayInterface();         // overlay_iface.cpp: publishes dsfix.overlay (overlay.h for the plugin DLLs)
void RegisterHostInterface();            // host_iface.cpp: publishes dsfix.host (core/dsfix_host.spec)
bool PluginsLoaded();                    // services.cpp: PSE_MSG_POST_LOAD seen (every plugin is loaded)
const settings::Values* DsfixSettingsValues();   // host_iface.cpp: for the kit's settings::Get() in the plugins
std::string CompatLocalPluginsText();     // host_iface.cpp: pse::compat::LocalPlugins() as text lines (dsfix.online)
const void* LuaHostTable();             // host_iface.cpp: the loader's private pse.lua.host (dsfix.missions)
// dsfix.online (plugins/online/glue.cpp): the lobby name of a player ("" = no such player), the connected PCs (bit =
// PC number) / peers, the console's "net ..." command and the connection's news (lines to print), goodbye at exit
std::string LobbyPlayerName(int player);
uint32_t OnlineConnectedPcMask();
int OnlineConnectedPeers();
std::vector<std::string> OnlineConsoleNet(const std::string& arg);
std::vector<std::string> OnlineConsoleEvents();
void OnlineShutdown();
const char* ServiceText(uint32_t hash);  // the plugins' text providers (the Lookup hook asks them first), or nullptr
void* ServiceScreen(uint32_t state);     // the plugins' front-end screens (coop.cpp's state -> screen hook), or nullptr
void ServiceSettingsChanged();           // the plugins' settings listeners (OnSettingsChanged: the launcher saved)
// A plugin's ON / OFF setting through pse.settings; `fallback` when the plugin isn't loaded.
bool PluginBool(const char* plugin, const char* key, bool fallback);
void ApplyController();  // DualShock 4 / DualSense remap + D-pad
void ApplyTiming();      // exact game clock (no turbo at high FPS)
void ApplyFrameCap();    // frame limiter: min(cap, monitor refresh)
void ApplyLauncher();    // "Enhancements" settings in the launcher's Detail Settings dialog
void ApplyInGameInput(); // in-game menus: hide the cursor + pad prompts while a pad is in use
void ApplySplitScreen(); // local split screen: player set-up for players 2-4 (views are built per frame)
void ApplyGraphics();    // anti-aliasing (MSAA) + anisotropic filtering via the D3D8 device
void ApplyCoop();        // front-end CO-OP entry + join screen (Xbox-style local co-op set-up)
void ApplyControls();    // Options -> CONTROLS: keyboard & mouse / controller (per-player pad layouts)
void ApplyMultiKbm();     // several keyboards + mice in split screen (multikbm.cpp)
// multikbm.cpp: devices are numbered in the order seen (keyboards and mice share the numbering)
bool KbmStart();                         // the co-op join screen opened: Raw Input on (false = off / failed)
void OnFrameMultiKbm();                  // frame start: queued Raw Input -> device states
int KbmDeviceCount(bool mice);           // present keyboards / mice
int KbmKeyboardIndex(int k);             // k-th present keyboard -> device number, -1
bool KbmKeyHeld(int device, uint32_t code);   // a keyboard's key (game code: scan, +0x100 extended)
bool KbmDevicePresent(int device);
int KbmMouseClicked();                   // a mouse whose left button went down this frame -> device number, -1
void KbmSetPlayers(const int keyboard[4], const int mouse[4]);   // session: player -> devices (null = none)
void* KbmSmoother(const void* block);    // that keyboard player's look smoother, null = the game's
int KbmEnter(const void* block);         // the game reads this block's input (returns the previous context)
void KbmLeave(int previous);
#ifndef DS_DIST
void KbmInject(int fake, bool mouse, int code, bool up, int dx, int dy, int buttonFlags, int wheel);   // dev tests
#endif
void ApplyCoopScripts();  // the mission scripts' co-op branches: API slot 0x8A8 = player count (coopscripts.cpp)
void* GiveSpawnedWeapon(void* soldier, const char* weapon, int mags);  // loadout.cpp: weapon in his hands, no unlock
bool GiveSpawnedMedikit(void* soldier);                                // loadout.cpp: a MediKit (level's own record)
void ApplyDiagnostics(); // game messages / file opens in the log (dev), performance overlay (F11)
void ApplyLoadout();      // squad kits: scene rewrite (dev rules) + runtime weapon spawn for the customise screen
void ApplyMods();          // loose files from Mods\<mod>\ replace / add game files (mods.cpp)
// PSE asset store (assetstore.cpp, docs/internal/asset-store.md): dev only (Dev\PseStore), off by default.
void ApplyAssetStore();
bool AssetStoreOn();
bool AssetStoreAllArchives();     // store on with its fallback: any level reads every archive's files (converted campaigns need it)
bool AssetStoreServesBorrows();   // store + global fallback (or Dev\PseNoBorrow): mods.cpp skips its archive copies
// The store's sound part (assetstore.cpp; the store is opened - built behind the progress window if needed - on the
// first call): level sound caches rebuilt from the store, banks of any level as bank packs ("SCH\0" + BANK + samples).
bool AssetStoreHasFile(const char* name);   // store + fallback on and some archive ships `name` (any level gets it)
bool AssetStoreSound();                                                       // store open, with sound caches
bool AssetStoreSoundCache(const char* cache, std::string* bytes);              // "mission1.sch" as shipped
bool AssetStoreHasBank(uint32_t key, std::string* cache);                     // key = ElfHash("<NAME>.sbk")
bool AssetStoreBankPack(uint32_t key, std::string* pack, std::string* cache);
int ModCount();
bool ReadGameTable(const char* name, std::string& out);  // mods.cpp: game table incl. mod rows
bool ModHasFile(const char* name);  // mods.cpp: a mod (or a file the plugin made) provides this file
// originals.cpp: the game's archives / sound caches / mission DLLs, moved to <game>\PSE\Originals once processed
void ApplyOriginalsRedirect();   // early: the exe's file opens find moved files
void ApplyOriginalsMove();       // late: moves them
std::string GameFile(const std::string& name);          // game folder path, else the PSE\Originals one if only there
std::vector<std::string> GameFiles(const char* pattern);  // names in both folders (game folder's first, once each)
std::string OriginalPath(const char* name);             // PSE\Originals path when the file was moved, else ""
std::string ModFilePath(const char* name);  // mods.cpp: full path of that file ("" = no mod has it)
const char* ModText(uint32_t hash);  // mods.cpp: texts mods define (weapon display names), by catalog hash
const std::vector<std::string>& ModWeapons();  // mods.cpp: weapon ids made by .weapon files
void ModLog(const char* fmt, ...);  // mods.cpp: a mod message - log + Mods\DesertStormFix-report.txt ("[fail]" = problem)
int ModProblems();                  // mods.cpp: "[fail]" messages since the last build of the mod state
int ModWeaponMagazines(const std::string& id);   // mods.cpp: a .weapon file's "magazines" (0 = not set)
std::string SkinBody(const char* texture);        // mods.cpp: body model a .skin gives the soldiers wearing this texture
std::string SkinNormalMap(const char* texture);  // mods.cpp: normal map a .skin gives its body ("" = the soldier's own)
void ApplySkins();     // skinapply.cpp: textures of skins applied mid-level, saved skins found on every side
void OnFrameSkins();
// Time of day: plugin dsfix.timeofday since 2026-10-24 (games/ds1/plugins/timeofday; interface dsfix.timeofday,
// plugins/timeofday/dsfix_timeofday.h). These keep their old names for customise.cpp / hostsync.cpp and ask the plugin
// (services.cpp); without it: mission default, no rows. TIME OF DAY 0 mission default, 1 dawn, 2 day, 3 dusk, 4 night;
// TIME FLOW 0 mission default, 1-4 one game hour per 10 / 5 / 2 / 1 real minutes.
constexpr int kTimeOfDayCount = 5, kTimeFlowCount = 5;
bool TimeOfDayInstalled();              // the plugin is there (the rows are offered)
int TimeOfDay();
int TimeFlow();
void SetTimeOfDay(int time, int flow);  // applied at once in a mission, remembered (not on a linked level)
bool TimeContinues();                   // this linked level continues the previous one's hour
void RuleTimeOfDay();                   // apply the setting now (the briefing screen opened)
const char* TimeOfDayLabel(int i);
const char* TimeFlowLabel(int i);

// Weather + footprints: plugin dsfix.weather since 2026-10-24 (games/ds1/plugins/weather; interface dsfix.weather,
// plugins/weather/dsfix_weather.h). What dsfix.dll keeps: the FOOTPRINTS row's labels (fixmenu.cpp) and the choice
// count; the loadout WEATHER row, Lua game.weather and the online token ask the plugin (null = no plugin).
constexpr int kFootprintModes = 4;     // OFF, GAME, LONG, PERMANENT
const char* FootprintLabel(int mode);
constexpr int kWeatherCount = 13;      // = DSFIX_WEATHER_COUNT: MISSION DEFAULT ... FOG, RANDOM
const ::DsfixWeatherV1* WeatherPlugin();   // services.cpp: the plugin's interface, or null
std::string LevelTitleText(const char* level);    // core/pse_host.cpp (pse.game.ds1 code): the level's title text ("RESCUE"), "" if unknown
std::string LevelDisplayName(const char* level);  // core/pse_host.cpp (pse.game.ds1 code): "Mission4.env" -> "Mission 4"
// Squad voices: plugin dsfix.voices since 2026-10-24 (interface dsfix.voices, plugins/voices/dsfix_voices.h); these keep
// their old names for customise.cpp / hostsync.cpp and ask the plugin (services.cpp) - without it AUTO, no row.
// VOICE 0 auto (the uniform's accent, or the skin's own voice), 1 SAS, 2 Delta, 3 the skin's own.
constexpr int kVoiceCount = 4;
bool VoicesInstalled();   // the plugin is there (the VOICE row is offered)
int VoiceChoice(int role);
void SetVoiceChoice(int role, int v);   // remembered per soldier and mode
const char* VoiceLabel(int v);
std::string VoiceText(uint8_t* soldier, int role);   // what the VOICE row shows
bool SoldierHasOwnVoice(uint8_t* soldier);           // his skin brings a voice (.skin "voice line")
int SpeechLanguage();                     // mods.cpp: 0 UK / 6 US English speech; -1 another language (no accents)
std::string SkinVoice(const char* texture);  // mods.cpp: bank name of a .skin's own voice ("" = none)
// Factions = the skin tables' sides (uniform index 0 SAS, 1 US Delta, 2 Russian, 3 Iraqi) as bits.
constexpr int kFactionSas = 1, kFactionDelta = 2, kFactionRussian = 4, kFactionIraqi = 8;
int WeaponFactions(const char* id);  // mods.cpp: a weapon's factions (.weapon "faction", else by its id)
void ApplyCustomise();     // squad customise screen: orbit camera hook
void SetSceneFog(bool on, uint32_t colour, float start, float end);  // graphics.cpp
void SceneFogBegin();
bool LoadoutUsesFillers();
bool CustomiseOpen();        // squad customise screen is up (customise.cpp)
bool BriefingRequested();    // this level's mission script opened its briefing pop-up (API +0x7D0; customise.cpp)
bool BriefingMessageDelivered();   // ... and its script got message 0x3B (the pop-up's close) this level
bool CustomiseClose();       // closes it as its last Accept would (dev server / tests); false = wasn't open
bool OnlineJoinerHoldsLevel();   // online.cpp: a joiner's own level start is held back (the host's START loads)
void LobbyDevPress(int device, uint32_t actions);
bool CoopDevBegin();   // dev: the join screen's Begin (2+ joined), focused or not
void AuthorityDevSkipVote(int player);   // dev: a cutscene skip vote by this PC's player (authority.cpp)   // dev: a press on the online lobby (lobbymenu.cpp)
void CustomiseDevInput(uint32_t bits);   // dev: loadout screen input bits (1 up 2 down 4 left 8 right 16 accept
                                         //   32 prev 64 next soldier 128 / 256 prev / next tab) for its next update
bool CustomiseDevVehicle(int dir);   // dev / tests: VEHICLE tab, choice stepped by dir, shown now; false = no such tab
// Lua missions' loadout rules (customise.cpp, set by scripthost.cpp's game.kit / game.briefing / game.loadout)
void ResetScriptLoadout();
void SetScriptLoadout(bool on);
void SetScriptBriefing(const std::string& text);
void SetScriptKit(int id, const std::string& uniform, const std::string& main, const std::string& sidearm,
                  const std::string& launcher, const std::string& grenades, bool lock);
void ApplyScriptKitNow(int id);
bool ScriptLevelActive();    // scripthost.cpp: a .lua level is running
bool ScriptLoadingSave();    // ... and it was started by LOAD GAME
// movie.cpp: scripted 2D films for Lua missions (game.image / draw / music / film ...)
void RegisterFilmFunctions(::lua_State* L);   // into the `game` table on top of the stack
bool ScriptFilmPlaying();   // world frozen, HUD hidden, the loadout screen waits
void ResetScriptFilm();
bool CustomiseHidesHud();    // ... and hides the HUD (not while its overview plays)
std::string LinkedFrom();    // the level this one continues ("Mission4"), "" if none (customise.cpp)
// Mission overview (overview.cpp): the night-vision fly-over, skipped after the briefing, replayed on demand.
bool OverviewAvailable();    // the loaded mission's script has one
void StartOverview();
bool OverviewPlaying();
bool OverviewSkipPending();  // the automatic one is starting: cinematics.cpp answers its movie-cancel query
void ApplyOverview();
void OnFrameOverview();
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
// Soldier gear on the body's connectors (gear.cpp): hats / helmets, packs, face pieces (beards, glasses, cigars).
// Head, back and face take their own kind of model; the 7 pouch spots (belt back / left / right, hips, chest) all take
// pouch-kind models (pouches, holster, satchel, torch, ...).
enum GearSlot { kGearHead, kGearBack, kGearFace, kGearBelt, kGearBeltL, kGearBeltR, kGearHipL, kGearHipR, kGearChestL,
                kGearChestR, kGearSlotCount, kGearFirstPouch = kGearBelt };
void ApplyGear();                                          // night vision gives the face piece back when taken off
std::vector<std::string> GearChoices(int slot);            // models this level can hang there, "" (none) first
std::string GearOn(void* soldier, int slot);               // model worn there, "" = none, "?" = not a known model
bool SetGear(void* soldier, int slot, const std::string& model);   // "" = take off
void FactionGear(void* soldier, int role, int side);       // the faction's standard gear (CONNOBJS.TXT)
std::string GearLabel(const std::string& model);           // as shown ("NONE", "BUSH HAT (DESERT)", ...)
// Mod gear (mods.cpp: .gear / .pouches files).
struct ModGearPiece {
    std::string model;   // model name (a mod .evo or the plugin's retextured copy)
    int kind;            // 0 head, 1 back, 2 face, 3 pouch
    std::string name;    // shown on the row, upper case
};
struct ModPouchSet {
    std::string name;
    std::string model[7];   // belt, belt left, belt right, hip left, hip right, chest left, chest right ("" = none)
};
const std::vector<ModGearPiece>& ModGear();
const std::vector<ModPouchSet>& ModPouchSets();
// Vehicles (vehicles.cpp: the loadout screen's VEHICLE rows; mods.cpp: .vehicle files, files from other levels).
struct ModVehicle {
    std::string id;        // unit name (VEHICLE.TXT row)
    std::string name;      // shown on the row, upper case
    std::string base;      // the game vehicle it is based on
    std::string faction;   // "" = the base's
};
const std::vector<ModVehicle>& ModVehicles();
struct VehicleHandling {
    std::string model;   // upper-case model name
    float grip = -1.0f;  // tyre friction, < 0 = the game's (unlimited)
    float spin = -1.0f;  // chassis spin damping, < 0 = the game's (0.2)
};
const std::vector<VehicleHandling>& ModVehicleHandling();   // .vehicle grip / spin damping (handling.cpp)
void BorrowUnitData(const char* level);   // .unit soldier types: model + kit files and banks the level lacks
void BorrowVehicleData(const char* level, const std::vector<std::string>& units);   // files the level lacks
bool VehicleDataComplete(const std::string& unit);   // its model and its guns' models exist (game data or a mod)
std::string VehicleGuns(const std::string& unit);   // its VEHWPNS weapons, sorted, joined by '+' ("" = none)
void PadLevelPools(const char* level, int extra);     // the level's render pools (<level>.FAS) + extra each
void VehiclesLevelStart(const char* level);   // level init: this load's swaps (remembered choice or the save's)
bool VehicleSwapsThisLoad();                  // this level load swaps a vehicle (scripthost: rebuild the octree)
void VehicleRecord(uint8_t* record);          // NewHLO, before the object is made: a swap rewrites the record
void VehicleCreated(uint8_t* record, void* obj);   // ... and after (dev trace of swapped vehicles)
void OnFrameVehicles();                       // gunners' own weapons hidden; dev: swapped vehicles' height
int VehicleSlots();                           // the squad's vehicle kinds in the loaded mission (0 = no rows)
std::string VehicleSlotLabel(int slot);
std::string VehicleChoiceLabel(int slot);
bool VehicleHasChoices(int slot);
std::string VehicleSlotName(int slot);        // the mission's own vehicle ("M2A1 BRADLEY")
std::string VehicleSlotNote(int slot);        // "4 CREW", "2 VEHICLES, 1 CREW - CHANGED"
std::vector<std::string> VehicleChoiceInfo(int slot);   // side, seats, mod / mission's own
void StepVehicle(int slot, int dir);
bool VehiclesChanged();                       // the screen's choice differs from what this load built
bool VehiclesCommit();                        // screen closed: remembered; true = the mission restarts with them
bool VehicleReloadSkip();                     // the restart after a change is loading / playing its intro
bool VehicleReloadOpened();                   // loadout screen opened: true once after such a restart
void VehicleCosaveInit();                     // co-save chunk callbacks of dsfix.vehicles (pse.serialization)
void VehicleShow(int slot);                   // loadout screen, per frame: slot's vehicle shown (-1 = all hidden)
uint8_t* VehicleObject(int slot);             // what the VEHICLE tab's camera looks at (preview or the level's own)
void VehicleShowEnd();                        // screen closed: the level's vehicles back, previews out of play
void StageSquadOutside(uint8_t* const* soldiers, int count);   // screen open: seated soldiers stand outside
void RetryStageSquad();                       // each screen frame: those who found no free exit spot, again
void ReturnSquadInside();                     // screen closed: back in the same seats
// Pouch sets: the game's per-faction / role pouches (gear table), "NONE" first - one choice for all 7 pouch spots.
int PouchSetCount();
std::string PouchSetLabel(int set);          // "SAS RIFLEMAN", ...
int PouchSetOn(void* soldier);               // the set the soldier wears, -1 = a mix of his own
bool SetPouchSet(void* soldier, int set);
struct GearPiece {
    std::string connector, model;   // model "" = nothing on it
};
std::vector<GearPiece> GearSnapshot(void* soldier);        // every gear connector (pouches too) - kept with saves
void ApplyGearSnapshot(void* soldier, const std::vector<GearPiece>& pieces);
void SetSavedGear(int role, std::vector<GearPiece> pieces);   // a loaded save's gear, put on when the level plays
void GearCaptureForSave();   // the squad's gear for the save being written (co-save chunk of dsfix.gear)
void GearRestoreSaved();     // the saved / loaded gear back on after a save restore
void GearCosaveInit();       // co-save chunk callbacks of dsfix.gear (pse.serialization)
void OnFrameGear();
// Uniform variants from .skin files with "uniform name = ..." (mods.cpp): rows of the skin tables (uniform 0 SAS /
// 1 US Delta, role = squad slot) that the customise screen offers on its UNIFORM row, not as other soldiers.
struct SkinVariant {
    int uniform, role;
    std::string texture, name;  // name as shown, upper case
};
const std::vector<SkinVariant>& SkinVariants();
// Mod weapon pictures (weaponicons.cpp): HUD weapon panel, inventory, pick-up icon.
int AddWeaponIcon(const std::string& path, const std::string& baseIcon);  // -> n for Weaps.txt "DSFIX_ICON_<n>"
void ApplyWeaponIcons();
void OnFrameWeaponIcons();
void ResetWeaponIcons();   // hot reload (mods.cpp)
bool ReloadMods();         // mods.cpp: rebuild from the Mods folders + the game's tables again (front-end only)
int ModsGeneration();      // bumped by every reload
const std::vector<std::string>& LoadedMods();  // folder names of the mods in use (not the converted campaigns)
std::vector<std::string> LoadedPorts();        // converted campaigns in use: <game>\PSE\Ports\<id> folder names
// VERSUS menu (versus.cpp): LEGACY ONLINE (the game's network screens) / SPLIT SCREEN (local versus, to come).
void* VersusScreen();                 // screen for front-end state 0x42
void* VersusSetupScreen();            // MATCH SETUP screen, front-end state 0x44
uint32_t VersusItemText();            // the main menu item's text hash
const char* VersusText(uint32_t hash);
// campaigns.cpp: custom campaigns (Mods\<mod>\campaign.txt -> SINGLE PLAYER -> CUSTOM CAMPAIGNS, state 0x45)
void ApplyCampaigns();
void OnFrameCampaigns();
void CampaignsMenuItem(void* singlePlayerScreen, bool coop);  // from coop.cpp's SINGLE PLAYER set-up wrapper
bool CampaignLevelPicked();   // the next level is a custom / converted campaign's (co-op: start it, no mission list)
void ReloadCampaigns();  // campaigns.cpp: the campaign list again (mods switched in game)
void ReloadPhotoFrames();  // photoframes.cpp: the mods' frames again
void* CampaignsScreen();
const char* CampaignsText(uint32_t hash);
const char* ScriptText(uint32_t hash);   // scripthost.cpp: texts a Lua mission made with game.text
bool CampaignPortLevel(const char* level);     // a mission of a shown converted campaign (PSE\Ports)
const char* CampaignNext(const char* level);         // next mission of the level's campaign, "FrontEnd.dll" after the last
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
const char* QuickStartLevel();  // scripthost.cpp: "+level <name>" on the command line (editor Play), else nullptr
void ApplyScriptHost();  // scripthost.cpp: ".lua" levels run by an embedded Lua instead of a MissionN.dll
// missionrt.cpp: x86rt natives for translated mission DLLs (mission2lua); the image / emulated stack ranges and the
// target of the native call in progress (apitrace.cpp tells the translated mission's API calls from the host's)
void ApplyMissionRt();
bool MissionRtRanges(uint32_t* imageLo, uint32_t* imageHi, uint32_t* origBase, uint32_t* stackLo, uint32_t* stackHi);
uint32_t MissionRtCallTarget();
void ApplyMissionSource();
std::string MissionReplacement(const char* dllName);   // missionsource.cpp: a mod's "--@replaces <dllName>" Lua ("" = none)
const char* MissionScriptText();   // "DLL" / "Lua (translated[, no DLL])" / "Lua (mod mission)" for this level, or null   // missionsource.cpp, dev: Dev\MissionSource = lua runs mission DLLs as translated Lua
void ApplyApiTrace();    // apitrace.cpp, dev: Dev\ApiTrace = n logs every script API call (DLL vs Lua parity)
void ApplyDevHooks();    // development builds only: test hooks (start level)
void OnFrameDev();       // development builds only: F9 = test pop-up message

uint32_t VsyncFps();        // frame rate V-Sync holds the game at, 0 = not synced (graphics.cpp)
uint32_t VsyncProbeFps();   // frame cap while V-Sync is being checked, 0 = not checking
long long FrameStartQpc();  // QPC at the start of this frame's update + render (after the frame-cap wait)
void OnFrameDevServer();  // devserver.cpp: dev builds - live test pipe (docs/internal/devserver.md, tests/live)
// Once per game frame, before update + render. Since 2026-10-19 the game layer (pse/games/ds1/ds1_events.cpp) drives
// frames: the frame-cap hook (framecap.cpp, priority 0 on the frame dispatch call 0x4116EC) waits, runs OnFrameEarly,
// then calls the layer's hook (INNER): pse.tasks, PSE_EVENT_FRAME - other plugins' subscribers first, OnFrame (the
// rest, a subscriber at kFramePriority) last - then the game's frame. Same order as when framecap called pse::Frame
// itself inside OnFrame.
void OnFrameEarly();       //   online lockstep ticks + the determinism test's virtual time (before PSE's frame)
void OnFrame();            //   every other feature's per-frame work (PSE_EVENT_FRAME subscriber)
constexpr int32_t kFramePriority = -1000000;   // below any plugin's PSE_EVENT_FRAME subscriber (default 0)
void OnFrameInput();       //   in-game input device tracking (ingame_input.cpp)
void OnFrameSplitScreen(); //   split-screen viewports (splitscreen.cpp)
void OnFrameCoop();        //   co-op join screen (coop.cpp)
// Menu bar / HUD colour (barcolour.cpp): the texture to bind instead of a menu art / HUD sheet page, or null.
// The plain image draw's texture bind (FUN_0053c9e0: call 0x544220 SetTexture wrapper, thiscall(stage, texture) at
// 0x53CC9B) is one pse.hooks chain with three subscribers, higher priority first: weaponicons.cpp (a mod weapon's
// picture), barcolour.cpp (recoloured menu / HUD page), the plugin dsfix.fontsharp (enlarged font page, its own DLL). Each passes the texture
// it chose on to `next`; later ones only replace their own pages, so the earlier replacement wins.
constexpr uint32_t kGlyphBindSite = 0x53CC9B, kGlyphBindTarget = 0x544220;
constexpr int32_t kGlyphBindWeaponIcons = 200, kGlyphBindBarColour = 100, kGlyphBindFontSharp = 0;
// D3D8 COM vtable slots (graphics.cpp device / IDirect3D8, memory.cpp texture Release; core/comhooks.h): DSFix hooks
// them at this priority - another plugin chains in front of it (sees the game's call first) above it, behind it
// (sees what DSFix passes on: MSAA present parameters, anisotropic filter states, the overlay drawn) below it.
constexpr int32_t kD3dHookPriority = 0;
// Shared clock sites (docs/sdk/reference/hooks.md "DS1 host" > "Clocks"): the game timers' clock calls 0x4BA3E4 /
// 0x4BA5B7 (call 0x4BA370) and the timeGetTime / GetTickCount import slots 0x5D82F4 / 0x5D806C. Higher runs first and
// answers when it is active: the dev determinism test's virtual clock (Dev\DetTest) wins over online lockstep's.
constexpr uint32_t kClockCallCtor = 0x4BA3E4, kClockCallTick = 0x4BA5B7, kClockTarget = 0x4BA370;
constexpr uint32_t kIatTimeGetTime = 0x5D82F4, kIatGetTickCount = 0x5D806C;
constexpr int32_t kClockDeterminism = 200, kClockLockstep = 100;
void ApplyBarColour();        //   hooks the other image draws' texture binds (the plain one: the shared chain above)
void OnFrameBarColour();      //   drops recoloured pages the sheets no longer use
void OpenBarColourPicker(int target);  // DESERTSTORMFIX > MENU BAR COLOUR (0) / HUD COLOUR (1): the colour wheel
bool BarColourPickerOpen();
int BarColourPickerTarget();  //   the target being picked, -1 when not open
void UpdateBarColourPicker(); //   input while open (closes itself on OK / Cancel)
bool DrawBarColourPicker(void* screen);  //   wheel, brightness, preview in the panel; false = draw the list
using ColourDone = void (*)(uint32_t colour, void* ctx);
// The same wheel for any colour (a plugin's pse.settings colour): done(0xRRGGBB) on OK, nothing on Cancel; Delete =
// defaultColour. While open: BarColourPickerOpen / Update / Draw as above (target 2).
void OpenColourPicker(uint32_t colour, uint32_t defaultColour, ColourDone done, void* ctx);
bool ColourPickerDev(const char* op, uint32_t colour);   // dev server: "set" / "ok" / "cancel"
uint32_t ColourPickerColour();                            //   the colour picked right now
// pseui.cpp: pse.ui v1 (pse/include/pse/pse_ui.h) - drawing, texts, pop-ups and keyboard focus for plugins
void RegisterPseUi();                   //   registers the interface (from ApplyFixMenu: before external plugins load)
const char* PseUiText(uint32_t hash);   //   a plugin's registered text (0xC4xxxxxx) in the fonts' bytes, or null
bool FontHasGlyph(unsigned char c, void* font);   // the game font (0x60EDB4 / 0x60EDB8) draws byte c
// UTF-8 -> the fonts' 8-bit text, characters the font lacks replaced (core/pse_ui_model.h); font null = small font.
std::string GameText(const char* utf8, bool upper = false, void* font = nullptr, size_t maxBytes = 0);
struct UiDebug {   // dev server (dev.ui)
    int items = 0, texts = 0, keysQueued = 0, textCount = 0, popupsPending = 0;
    std::string popup, capture;
};
UiDebug PseUiDebug();
// pluginsmenu.cpp: OPTIONS > DESERTSTORMFIX > PLUGINS (state 0x4D: the plugins + their load state) and a plugin's
// settings page built from pse.settings (state 0x4E)
void* PluginsMenuScreen(uint32_t state);
const char* PluginsMenuText(uint32_t hash);
void NotePluginSettingsAtStart();       //   PSE_MSG_POST_LOAD: the values the game started with (restart markers)
// A plugin setting changed since that note and it isn't live (it needs a restart) - fixmenu.cpp's plugin rows.
bool PluginSettingNeedsRestart(const char* plugin, const char* key);   // declared without PSE_SETTING_LIVE
bool PluginSettingRestartPending(const char* plugin, const char* key);
std::string PluginsMenuSummary();       //   "3 LOADED" / "2 LOADED, 1 REFUSED" (the PLUGINS row's value)
struct PluginsMenuDebug {   // dev server (dev.plugins_menu)
    int screen = 0;          // 0 none, 1 list, 2 page
    int row = -1, offset = 0;
    std::string plugin;      // page: the plugin's name
    std::vector<std::string> labels, values, footer;   // every row (not only the visible ones), footer lines
};
PluginsMenuDebug PluginsMenuState();
bool PluginsMenuOpen(const char* plugin);   // dev: opens the list (null) or a plugin's page
// dev: drive the open list / page as its own update would: cursor onto absolute row r (scrolling the window), then
// activate it (dir 0 = Enter, -1 / +1 = Left / Right on a spinner), or Back. False = no PLUGINS screen up / bad row.
bool PluginsMenuSetRow(int row);
bool PluginsMenuActivate(int dir);
bool PluginsMenuBack();
void OnFrameDiagnostics(); //  frame times + performance overlay (diagnostics.cpp)
void OnFrameControls();    //   builds the CONTROLS screens (controls.cpp)
void ApplyConsole();       // console.cpp: developer console (` key) - force-start levels
void OnFrameConsole();
bool ConsoleOpen();        //   the console has the keyboard (KeyHeld answers no)
bool UiKeyboardCaptured(); //   pse.ui: a plugin has the keyboard (KeyHeld answers no) - pseui.cpp
bool UiCaptureKeys(bool on);   // pseui.cpp: DesertStormFix itself takes the keyboard (typing a name), false = refused
bool UiNextKey(bool& isChar, uint32_t& code);   //   one queued key while captured: WM_CHAR (isChar) or WM_KEYDOWN vk
uint32_t CoopDeviceActions(int device, PadType& type, std::string& name);   // coop.cpp: device 0..7 pad, 8 keyboard
void* LobbyScreen();          // lobbymenu.cpp: state 0x4F ONLINE LOBBY
void ApplyNameplates();       // nameplates.cpp: players' names / colours in missions
void ApplyViews();            // viewmode.cpp: online own view + peek window
void OnFrameViews();
bool ViewHidden(int player);  //   this PC doesn't show that player's view now (own view / being peeked)
bool ViewPeekPass();          //   the peek window is being drawn (no HUD)
void SplitHudView(int view, float x, float y, float w, float h);   // splitscreen.cpp: a view's HUD layout for another
void SplitHudReset();                                               //   rectangle / the game's again
bool LoadoutCommand(int from, const std::string& text);   // customise.cpp: "LO ..." loadout screen online
// modsync.cpp: a joiner gets the host's mods (download / switch on / off, then a reload - no restart)
void OnFrameModSync();
void OnFrameOnlinePause();   // onlinepause.cpp: the online mission menu (doesn't pause)
bool OnlinePauseOpen();
bool ModSyncCommand(int from, const std::string& text);   // "MS ..." game commands
bool ModSyncActive();         // the joiner's mod sync screen is up
bool ModSyncWaitingForYes();  //   ... and waits for the player's yes
void ModSyncAccept();
void ModSyncCancel();
struct ModSyncRow {
    std::string text;
    float progress;           // 0..1, -1 = none
};
std::vector<ModSyncRow> ModSyncRows(std::string& title, std::string& footer);
std::vector<std::string> ModSyncHostLines();   // host: each PC's transfer
void OnlineStartSession();    // online.cpp: the host's START (co-op session from the lobby table -> campaign menu)
bool OnlineStartBlocked(std::string* why);   //   why START can't go yet (no one connected, plugin / mod check)
std::vector<std::string> OnlineStatusLines(bool host);   // online.cpp: join code / connection / plugin check lines
void OnlineNotice(const char* text);
bool OnlineCopyJoinCode();    // the host's join code onto the clipboard
bool OnlinePasteReplyCode();  // a joiner's reply code from the clipboard
// mods.cpp: level files the mods provide (.dll / .lua; built = a mission DLL made from a .dllpatch)
struct ModLevel {
    std::string name, mod;
    bool built;
};
std::vector<ModLevel> ModLevels();

PadType JoystickType(int joystick);  // the game's joystick j by vendor (Xbox, else PlayStation layout)
// Reads joystick j now: bits 0-15 = the game's buttons (after the per-pad fix-up), 16-19 left stick up/right/down/left.
uint32_t ReadJoystick(int joystick);
void FollowActivePad();  // once per frame: player 1's joystick = the pad used last (not in co-op missions)
void SetPadDeadzone(uint32_t percent);  // CONTROLLER screen: saved + applied to the open pads at once
void ForgetJoystick(int joystick);  // slot j emptied / re-filled (hot-plug): forget its pad type and direct I/O
void ApplyCheats();                     // cheats.cpp: OPTIONS > CHEATS (unlock code from any pad, extra rows, cheat saves)
bool CheatsActive();                    // any cheat on (save names get "Cheats ")
bool CheatsUnlocked();
void* FixMenuScreen();                  // fixmenu.cpp: OPTIONS > DESERTSTORMFIX (state 0x47), optional features ON / OFF
const char* FixMenuText(uint32_t hash);
void* VideoMenuScreen();                // videomenu.cpp: OPTIONS > VIDEO (state 0x4C), the launcher's settings
const char* VideoMenuText(uint32_t hash);
void OnFrameVideoMenu();
void ApplyRestart();                    // videomenu.cpp: "+restart <pid>" (APPLY & RESTART) waits for the old process
bool Restarting();                      //   this start came from APPLY & RESTART (no launcher)
uint32_t LauncherCaps(uint32_t& stencil);  // launcher.cpp: the device's caps / mode stencil bits before OK masked them
void OnFrameStartCheck();               // launcher.cpp: clears StartPending once the game runs
void ApplyAnisotropy();                 // graphics.cpp: TEXTURE FILTERING changed (live)
bool ChangeDisplayMode(uint32_t modeIndex, uint32_t displayMode);  // graphics.cpp: new mode now (device reset)
void PlaceGameWindow(uint32_t w, uint32_t h, uint32_t mode);       // display.cpp: the game window for a mode
uint32_t EffectiveDisplayMode(uint32_t mode);  // services.cpp (the plugin dsfix.display has the same rule): Renderer 3 (own DXGI presenter) shows fullscreen as borderless
void* OnlineScreen(uint32_t state);     // online.cpp: CO-OP (0x48) and the online screens (0x49..0x4B), nullptr if not ours
const char* OnlineText(uint32_t hash);
uint32_t CoopMenuState();               // the CO-OP screen (SPLIT SCREEN / ONLINE): where the join screen's Back goes
void OnFrameOnline();                   // online.cpp: host's level loads mirrored to the players
void BeginOnlineSession(int players, bool host);   // coop.cpp: the co-op session of an online game (host: + menus)
void EndOnlineSession();
// lockstep.cpp: online co-op - the players' inputs synced tick by tick (see the file)
void OnFrameLockstep();
bool LockstepActive();                  // an online mission is being kept in step
// hostsync.cpp: the host's simulation settings for every PC of an online mission (sent with START, memory only)
struct HostSettings {
    int timeOfDay = 0, timeFlow = 0;
    bool timeOfDayOn = false, voices = false, followers = false;
    int coopScripts = -1;               // the missions' co-op branches (coopscripts.cpp), -1 = not sent
    int voice[4] = {};
    uint32_t cheats = 0, modsHash = 0;
    int drawFar = -1;
    int jpFixes = -1;                   // Japanese-release fixes (jpfixes.cpp), -1 = not sent
    int loadout = -1;                   // briefing + loadout screen on (customise.cpp), -1 = not sent
    int weather = -1;                   // weather.cpp kind (RANDOM resolved by the host), -1 = not sent
    int inputDelay = -1, pingMs = -1;   // lockstep input delay (ticks) the host chose from its worst ping   // renderer +0x40A9C (launcher DRAW DISTANCE: fog / view range - the AI's too); -1 = not sent
};
std::string HostSyncToken();
uint32_t OwnModsHash();                                      // hostsync.cpp: this PC's enabled mods                                  // the host's settings as START's last field
bool ApplyHostSyncToken(const char* token, bool own);         // own = the host's own token
bool LoadoutForMission();                                     // the host's loadout-screen choice online, else own
const HostSettings* HostSync();                               // non-null while an online mission runs on the host's
int CoopVoiceChoice(int role);                                // dsfix.voices: this PC's co-op VOICE choice
uint32_t CheatFlags();                                        // cheats.cpp: the game's cheat values as flags
void SetCheatFlags(uint32_t flags);
void LockstepSetSeed(uint32_t seed, uint32_t skipCutscenes);   // joiner: from the host's START
uint32_t LockstepSeed();
void ScriptReseed(uint32_t seed);   // scripthost.cpp: math.randomseed of the running Lua level
bool LockstepSkipCutscenes(bool own);   // the host's setting while online
bool LockstepAxis(uint8_t* block, int action, int opposite, float& value);   // AxisValue: synced value
bool LockstepMouse(uint8_t* block, float* dx, float* dy);                    // mouse look: synced value
void LockstepFixBlocks();
void HarnessSound();        // dev determinism harness: private sound draws + sound timing by its virtual clock
void HarnessSoundFrame();   //   per harness frame (records the voices started this frame)
bool LockstepSkipRender();
bool LockstepBehind();          // online: the next tick is already due (this PC draws slower than 60 frames a second)
void LockstepCatchUp(bool on);   // a frame that only updates (no drawing / present)   // online, display device lost: frames run without the render loop
void OnlineLogLine(const char* text);   // lockstep.cpp: a line in DesertStormFix-online.log
// lobbynet.cpp: the co-op lobby's players across PCs (lobby.h) - who plays on this PC and with which device
void OnFrameLobby();
bool LobbyCommand(int from, const std::string& text);   // "LB ..." game commands (online.cpp routes them here)
void LobbySendBeforeStart();                            // host: the table, just before START
bool LobbyOnline();                                     // a table is in use (hosting, or the host's arrived)
int LobbyPlayers();                                     // players in the table (0 = none)
int LobbyPcs();
uint32_t LobbyLocalMask();                              // bit n = player n plays on this PC
bool LobbyLocal(int player);
int LobbyFirstLocal();
uint32_t LobbySenderMask(int players);                  // each PC's first player
int LobbyDevice(int player);                            // lobby::kDeviceAny / a joystick 0..7 / kDeviceKeyboard
std::vector<std::string> LobbyConsole(const std::string& arg);
// authority.cpp: online co-op - the host runs every soldier no player drives, joiners show its records
void AuthorityStart(bool host, int players);
void AuthorityStop();
void AuthorityTick(uint32_t tick);
std::string AuthorityTakeEvents();
void AuthorityDeliver(const std::string& events);
void AuthorityCommand(const std::string& text);
void AuthorityReceive(const std::vector<uint8_t>& packet);
void AuthorityReceiveVehicles(const std::vector<uint8_t>& packet);
bool AuthorityOn();
bool AuthorityPuppet(const uint8_t* soldier);
void EnsureDormancyHook();   // scripthost.cpp: the AI dormancy call hooked (online: nobody sleeps)
void LockstepAxisTrace(const uint8_t* block, int action, float value, bool synced);   // dev
uint32_t RenderCount();
void RenderDiffBefore();   // renderdiff.cpp (dev): memory the render loop writes
void RenderDiffAfter();       // framerec.cpp: render loop runs so far (dev traces)
int LastFrontEndResult();      // console.cpp: the front-end update's last result (1 = no render this frame)   // every player's input block the same on every PC (after the player set-up re-ran)
void AxisParts(uint8_t* block, int action, float& pad, bool& touched, bool& key, float& gyro);   // splitscreen.cpp
float LookScale(int player);
void ConsoleStartLevel(const std::string& level);   // console.cpp: start a level now, as 'level <name>'
bool RestartLevel();                    // console.cpp: the level being played again from its start (pause menu)
void ApplyPauseMenu();                  // pausemenu.cpp: RESTART LEVEL on the pause menu
void ApplyFixMenu();                    // fixmenu.cpp: the DESERTSTORMFIX item on the OPTIONS screen
void ApplyFreeCam();
void OnFrameFreeCam();
bool FreeCamActive();                   // freecam.cpp: player 1 flies the free camera
bool FreeCamPhoto(bool on);             // freecam.cpp: photo mode takes / gives back the free camera (no cheats needed)
// photomode.cpp: F5 / pause menu PHOTO MODE - frozen world, free camera, PNG to the Screenshots folder
constexpr uint32_t kPhotoModeText = 0xC0140001;   // pause menu item text (Lookup hook)
const char* PhotoText(uint32_t hash);
void OnFramePhoto();                    // per game frame, after the input poll (cheats.cpp's frame hook)
bool PhotoModeOpen();                   // open or closing (world frozen)
bool PhotoModeAvailable();              // could open now (pause menu item enabled)
void RequestPhotoMode();                // open once play resumes (pause menu)
float PhotoFov();                       // photo zoom (vertical degrees), 0 = not in photo mode
bool PhotoModeShowing();                // open (not closing): frames drawn
int PhotoFrameCount();                  // photoframes.cpp: 0 none, built-in frames, Mods\<mod>\frames\*.png
std::string PhotoFrameName(int index);
void SetPhotoFrame(int index);
void PhotoFrameArt();                   // late draw: the frame's art
void PhotoFrameText();                  // the frame's words (photomode.cpp's late draw, over the art)
bool PhotoAnimate(const void* instance, float& dt);   // a posed soldier's animation time step (customise.cpp's hook)
std::vector<std::string> PhotoSpawnUnits();   // loadout.cpp: unit names photo mode can spawn in this level
void* PhotoSpawnSoldier(int index, const float pos[3], float yaw);
std::vector<std::string> PhotoWeaponChoices();   // loadout.cpp: guns / pistols / launchers a spawned soldier can hold here
void PhotoBeforePresent(void* device);  // graphics.cpp Present hook: read the frame for a photo
void PhotoAfterPresent(void* device);
bool CustomisePending();                // customise.cpp: the loadout screen is still to open in this level
bool ScriptHidesHud();                  // scripthost.cpp: Lua game.hud(false)
void SetScriptHud(bool on);             // the same switch from C++ (dev.hud); reset at every level start
bool FreeCamFlying();                   // dsfix.photo (freecam.cpp): player 1 is flying our free camera
// services.cpp: the loadout screen, the free camera (freecam.cpp: F6 / L3 + R3 once cheats are unlocked), Lua
// hud(false) or the peek window is up - the HUD wrappers draw nothing
bool HudHidden();
const char* CheatsText(uint32_t hash);  // the cheat rows' own texts (Lookup hook)
void OnFrameHotplug();
void SetPadsBackground(bool on);   // hotplug.cpp: joysticks keep working without focus (online sessions)
bool DevNoPads();                  // dev: this copy opens no joysticks
void ApplyPadAccess();   // hotplug.cpp: pads plugged in / out while the game runs, "controller disconnected" pause
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
// Trigger feel for what a player fires (held weapon, or the weapon that fired in the last 1.5 s): plugin dsfix.rumble
// through dsfix.services' provider (services.cpp); Default without it. Same numbering as DsfixTriggerFeel.
enum class TriggerFeel { Default, None, Light, Rifle, Heavy, MachineGun, Launcher };
TriggerFeel PlayerTriggerFeel(int player);
const char* ControlIconText(int player, uint32_t action);  // icon character of a player's button for an action
const char* GameText(uint32_t hash);  // the game's own text for a catalog.dat hash (current language), or nullptr
int CoopPromptStyle(int player);      // co-op player's icon style: 0 keyboard, 1 PlayStation, 2 Xbox; -1 no session

void OnSettingsChanged();  // re-applies everything that depends on settings

// determinism.cpp (dev): Dev\DetTest - virtual clock + fixed seed + scripted keys + per-frame world hash
void ApplyDeterminism();
void OnFrameDeterminism();
bool DetActive();          // the virtual clock runs (timing.cpp leaves the frame time alone)
uint32_t DetVirtualTime();   // its virtual game-thread clock (ms)
void DrawLogTick(uint32_t tick);   // dev Dev\LockstepDrawLog: random draws of the last tick -> log

// Optional front-end features (OPTIONS > DESERTSTORMFIX), as decided at start-up (restart toggles).
bool VersusEnabled();      // versus.cpp: VERSUS menu (off = the game's MULTIPLAYER entry)
bool ModsFolderEnabled();  // mods.cpp: the Mods folder is read (FeatMods, or a "+level" start from the editor)
bool CheatsEnabled();      // cheats.cpp: extended CHEATS menu, free camera, DEVELOPER page

// memory.cpp: Global Heap sized to the mods, texture / address-space figures, 4 GB state
void ApplyMemory();
bool LargeAddressAware();  // the exe has the 4 GB flag (DesertStormFix-Patcher.exe)
void NoteTexture(void* tex, uint32_t w, uint32_t h, uint32_t levels, uint32_t format, uint32_t pool);  // graphics.cpp
struct MemoryStats {
    uint64_t addressTotal = 0, addressUsed = 0, largestFree = 0, privateBytes = 0;
    uint32_t textures = 0;
    uint64_t textureBytesDefault = 0, textureBytesManaged = 0, textureBytesSystem = 0;
    uint32_t d3dFree = 0;  // IDirect3DDevice8::GetAvailableTextureMem
    uint32_t heapSize = 0, heapUsed = 0, heapPeak = 0, heapFails = 0;
    bool heapOurs = false, laa = false;
};
MemoryStats GetMemoryStats();  // refreshed at most once a second
uint32_t LargestModFile(std::string* name);  // mods.cpp: size of the largest file the enabled mods add (0 = none)
}  // namespace features
