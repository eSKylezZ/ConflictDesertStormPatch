#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace features {
struct Settings {
    bool widescreen = true;
    bool mods = true;
    bool modsMenu = true;
    bool campaigns = true;
    bool thirdPerson = true;
};
const Settings& Get();

// SteamStub (unpack.cpp): ConflictDeniedOps.exe's .text is encrypted on disk and decrypted by the stub after PSE has
// loaded the plugins - code patches wait for the game's CRT start-up. `apply` runs once, after the hand-over.
void AfterUnpack(void (*apply)());
bool TextDecrypted();

void ApplyWidescreen();
void ApplyMods();
void ApplyModsMenu();
void ApplyLoadingScreen();   // loadingscreen.cpp: <operation>.loading texts from mods on the loading screen
void ApplyDevInput();   // dev builds only (devinput.cpp): Dev\KeyScript drives the menus in background tests
void ModLog(const char* fmt, ...);   // log + Mods\DeniedOpsFix-report.txt

// Mods folder (mods.cpp) for the MODS menu: every folder under Mods\ (switched-off ones start with '_' or '.'),
// sorted by the bare name; SetModEnabled renames a folder ('_' prefix) and rebuilds the index at once.
struct ModFolder {
    std::string folder;   // as on disk
    std::string name;     // without the '_' / '.' prefix
    bool enabled = false;
};
std::vector<ModFolder> ModList();
bool SetModEnabled(const std::string& folder, bool on);
std::string ModFilePath(const char* name);   // full path of a switched-on mod's file by bare name, "" = none
// The current level's scene came from a mod with a <scene stem>.level sidecar: its "--@<key> <value>" ("" = none).
// Keys: script (none = drop the slot's mission script), camera (third = start in third person).
std::string SceneLevelOption(const char* key);
uint32_t SceneGeneration();   // changes with every scene index opened (a level load)

// Third-person camera (thirdperson.cpp): F7 toggles; a mod level's "--@camera third" starts in it.
void ApplyThirdPerson();

// Lua missions on ported levels (luamission.cpp): "--@lua <bundle>.lua" in a mod level's .level sidecar runs that
// bundle (ModKit ds1todolua) with the natives table `dox` (script API calls, guarded memory, clock, log).
void ApplyLuaMission();

// On-screen prompts (prompt.cpp): one line of text drawn over the game (lower middle) while it keeps being set - the
// Lua missions' tutorial texts. UTF-8.
void ApplyPrompt();
void SetPrompt(const char* utf8);

// Custom campaigns (campaigns.cpp): Mods\<mod>\campaign.txt, the CUSTOM CAMPAIGNS menu (modsmenu.cpp builds the
// screens), mission start by mission DLL name and the chain after a won mission.
void ApplyCampaigns();
bool StartMission(const char* dll);   // an Operations.xml mission (dllname) or a mod mission DLL (+ .level sidecar)
bool StartCampaignMission(const char* mod, int index);   // mission `index` of Mods\<mod>\campaign.txt, chained
struct CampaignRow {
    std::wstring text;
    bool locked = false;
};
std::vector<CampaignRow> CampaignRows(int* start);   // the current list's rows, *start = the row to focus
std::wstring CampaignTitle();
void CampaignPick(int row);   // Enter on a row
bool CampaignBack();          // Back: true = handled (stepped up a list), false = leave to the main menu
bool CampaignsAvailable();
void CampaignsOpened();       // the screen was opened from the main menu: back to the campaigns list
}  // namespace features
