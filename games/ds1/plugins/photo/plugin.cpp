// dsfix.photo: photo mode (photomode.cpp), its frames (photoframes.cpp), the free camera (freecam.cpp), the extended
// CHEATS menu (cheats.cpp) and the pause menu's extra items (pausemenu.cpp) - one plugin DLL since 2026-10-24, they
// call each other throughout. What they need of dsfix.dll goes through the kit's host client (src/core/dsfix_host.spec);
// what dsfix.dll needs of them is the interface dsfix.photo (photo.spec; dsfix.dll's wrappers answer defaults
// without this DLL: no photo mode, the game's own CHEATS screen, no free camera, no extra pause items).
#include <windows.h>

#include <string>
#include <vector>

#include "core/log.h"
#include "features/features.h"
#include "kit/kit.h"
#include "photo/generated/photo_iface.h"

#include "photo/generated/photo_export.inc"

namespace {
bool Load() {
    if (!kit::Services()) return false;
    // remembered unlock (was a raw value in DesertStormFix's key); hidden, copied once
    if (!kit::DeclareInt("CheatsUnlocked", 0, 0, 1)) return false;
    features::ApplyCheats();
    features::ApplyFreeCam();
    features::ApplyPauseMenu();
    return RegisterPhotoInterface();
}
}  // namespace

DSFIX_KIT_PLUGIN("dsfix.photo", "Photo mode, free camera, cheats menu", PSE_PLUGIN_SIMULATION, Load)
