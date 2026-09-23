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
};

const Values& Get();
void Load();
bool Save(const Values& v);
}  // namespace settings
