// dsfix.coop: split screen (splitscreen.cpp), the co-op join screen + session, co-op saves and the co-op mission flow
// (coop.cpp), the CONTROLS menu + pad layouts (controls.cpp, padlayout.cpp), controllers in game + hot-plug + direct pad
// I/O (controller.cpp, hotplug.cpp, padio.cpp), several keyboards / mice (multikbm.cpp) and VERSUS (versus.cpp) - one
// plugin DLL since 2026-10-25, they call each other throughout. What they need of dsfix.dll goes through the kit's host
// client (src/core/dsfix_host.spec); what dsfix.dll needs of them is the interface dsfix.coop (coop.spec). The main
// menu's CO-OP / VERSUS items and the front-end screen router stay in dsfix.dll (frontend.cpp). Without this DLL: one
// player, the game's own pad handling, no CONTROLS / co-op / versus screens.
#include <windows.h>

#include <string>
#include <vector>

#include "core/dsfix_services.h"
#include "core/log.h"
#include "features/features.h"
#include "features/padio.h"
#include "kit/kit.h"
#include "coop/generated/coop_iface.h"

#include "coop/generated/coop_export.inc"

namespace {
void PSE_CALL SettingsChanged() { features::ApplyController(); }   // the launcher's OK / Set to Defaults

bool Load() {
    const DsfixServicesV1* services = kit::Services();
    if (!services) return false;
    // (dsfix.dll's kFeatures order before the move: controller, hot-plug, ..., split screen, ..., co-op, versus, ...,
    // controls, several keyboards)
    features::ApplyController();
    features::ApplyPadAccess();
    features::ApplySplitScreen();
    features::ApplyCoop();
    features::ApplyVersus();
    features::ApplyControls();
    features::ApplyMultiKbm();
    services->SetSettingsListener(kit::Self(), &SettingsChanged);
    return RegisterCoopInterface();
}
}  // namespace

DSFIX_KIT_PLUGIN("dsfix.coop", "Split screen, co-op, controls, controllers, versus", PSE_PLUGIN_SIMULATION, Load)
