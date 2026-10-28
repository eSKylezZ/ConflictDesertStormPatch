// dsfix.barcolour: the menu bar / HUD colour (barcolour.cpp) - recoloured menu art / HUD sheet pages bound in place of
// the game's on the image draws, readable text on them, and the colour wheel (also the PLUGINS menu's picker for
// plugins' colour settings). Own DLL since 2026-10-25. The settings MenuBarCustom / MenuBarColour / HudColourCustom /
// HudColour stay DesertStormFix's (dsfix.dll's DESERTSTORMFIX rows); what dsfix.dll calls here is the interface
// dsfix.barcolour (barcolour.spec). Without it: the game's colours, no colour wheel.
#include <windows.h>

#include "features/features.h"
#include "kit/kit.h"
#include "barcolour/generated/barcolour_iface.h"

#include "barcolour/generated/barcolour_export.inc"

namespace {
bool Load() {
    features::ApplyBarColour();
    return RegisterBarcolourInterface();
}
}  // namespace

DSFIX_KIT_PLUGIN("dsfix.barcolour", "Menu bar / HUD colour", PSE_PLUGIN_CLIENT_ONLY, Load)
