#pragma once
// GlobalStormFix (PSE) features: one Apply per internal plugin "cgsfix.<feature>" (cgsfix.cpp runs each in its owner
// scope at start-up). Separate from games/cgs/src/features/features.h (the standalone build, which is kept working
// unchanged) - this is cgsfix's own set: widescreen + Mods folder (the user's request) plus devbg.cpp/graphics.cpp/
// frame.cpp copied UNCHANGED from the standalone build so this PSE build can be tested the same safe off-screen way
// (games/cgs/CLAUDE.md "Testing") - without them, verifying this port live would mean a visible foreground window.
// hud.cpp (menu/HUD scaling) is NOT copied, so OnFrameHud() below is an inert stub frame.cpp still calls.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace features {
struct Settings {
    bool widescreen = true;   // FeatWidescreen
    bool mods = true;         // FeatMods
    bool modsMenu = false;    // FeatModsMenu (default off until verified in game)
};
const Settings& Get();

void ApplyWidescreen();   // pse/features/widescreen.cpp (copy of the standalone's, same byte patches)
void ApplyMods();         // pse/features/mods.cpp: <game>\Mods\<mod>\... files replace / add whole game files by name
void ApplyModsMenu();     // pse/features/modsmenu.cpp: MODS item + screen in the game's own main menu
void ApplyFrameHook();    // pse/features/frame.cpp (copy): per-frame hook graphics.cpp's D3D9 re-hook needs
void ApplyDevBackground();      // pse/features/devbg.cpp (copy, dev builds only; no-op in DS_DIST)
void OnDirectInputCreated(void* di);   // devbg.cpp: wraps the keyboard device for Dev\KeyInject (dev builds only)
inline void OnFrameHud() {}            // hud.cpp isn't built here - frame.cpp's copy calls this; nothing to do

// mods.cpp
void ModLog(const char* fmt, ...);   // log + Mods\GlobalStormFix-report.txt
int ModCount();
std::vector<std::pair<std::string, bool>> ModList();         // {folder name (as on disk), enabled}
bool SetModEnabled(const std::string& folderName, bool enable);   // renames on disk, re-scans; false = rename failed
}  // namespace features
