#pragma once
// GlobalStormFix (PSE) features: one Apply per internal plugin "cgsfix.<feature>" (cgsfix.cpp runs each in its owner
// scope at start-up). Separate from games/cgs/src/features/features.h (the standalone build, which is kept working
// unchanged) - this is cgsfix's own, smaller set: widescreen + mods only (per the user's request). Everything else
// the standalone build has (frame cap/timing, HUD scaling, menu squeeze, dev background mode) stays standalone-only
// for now; port it the same way if/when the PSE build should carry it too.
#include <cstdint>

namespace features {
struct Settings {
    bool widescreen = true;   // FeatWidescreen
    bool mods = true;         // FeatMods
};
const Settings& Get();

void ApplyWidescreen();   // pse/features/widescreen.cpp (copy of the standalone's, same byte patches)
void ApplyMods();         // pse/features/mods.cpp: <game>\Mods\<mod>\... files replace / add whole game files by name

// mods.cpp
void ModLog(const char* fmt, ...);   // log + Mods\GlobalStormFix-report.txt
int ModCount();
}  // namespace features
