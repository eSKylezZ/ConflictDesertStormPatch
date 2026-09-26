#pragma once
#include <cstdint>

// Stored in the registry next to the game's own settings
// (HKLM\SOFTWARE\Pivotal Games\Conflict Desert Storm\Enhancements, virtualized per user like the game's keys).
// Changed from the launcher dialog / in-game Options menu, never from files.
namespace settings {
struct Values {
    bool hudScaling = true;
    uint32_t hudScalePercent = 100;  // on top of the automatic min(W/800, H/600)
    bool controller = true;
    uint32_t padDeadzone = 40;       // percent
    uint32_t fpsCap = 240;           // 0 = no fixed cap
    bool fpsCapToRefresh = true;     // also never exceed the monitor refresh rate
    uint32_t splitScreenLayout = 0;  // splitscreen::Layout: 0 horizontal (top / bottom), 1 vertical (side by side)
    bool discordPresence = true;     // show the mission / mode in the user's Discord status
    uint32_t displayMode = 0;        // display::Mode: 0 fullscreen, 1 windowed, 2 borderless window
    bool skipIntro = false;          // no publisher / developer logo slideshow at start-up
    bool skipCutscenes = false;      // mission cutscenes end as soon as they start (Esc / pad skip them anyway)
    uint32_t antialiasing = 0;       // MSAA samples (0 = off; the highest supported count <= this is used)
    uint32_t anisotropy = 16;        // anisotropic filtering level for linear-filtered textures (0 = off)
    bool mouseAcceleration = false;  // the game's MOUSE OPTIONS -> ACCELERATION (its own default was on)
};

const Values& Get();
void Load();
bool Save(const Values& v);
}  // namespace settings
