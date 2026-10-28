#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace features {
struct Settings {
    bool widescreen = true;
    bool mods = true;
    bool modsMenu = true;
};
const Settings& Get();

// SteamStub (unpack.cpp): ConflictDeniedOps.exe's .text is encrypted on disk and decrypted by the stub after PSE has
// loaded the plugins - code patches wait for the game's CRT start-up. `apply` runs once, after the hand-over.
void AfterUnpack(void (*apply)());
bool TextDecrypted();

void ApplyWidescreen();
void ApplyMods();
void ApplyModsMenu();
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
}  // namespace features
