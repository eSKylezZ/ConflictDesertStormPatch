// dsfix.graphics: the D3D8 device hooks (MSAA, anisotropic filtering, V-Sync, scene fog, the photo capture around
// Present; graphics.cpp), the D3D8-on-D3D9 / D3D9On12 / own DXGI presenter renderers with HDR, bloom and puddle
// reflections (d3d8to9.cpp, shader8.cpp, presenter.cpp, water.cpp) and OPTIONS > VIDEO (videomenu.cpp) - one plugin DLL
// since 2026-10-25. What they need of dsfix.dll goes through the kit's host client (src/core/dsfix_host.spec); what
// dsfix.dll needs of them is the interface dsfix.graphics (graphics.spec). The overlay draws on the game layer's
// PSE_EVENT_END_SCENE in dsfix.dll, the frame cap / frame timing / frame recorder and the "+restart" wait stay there
// too. Without this DLL: the system's D3D8 as the game made it (no MSAA / anisotropy / V-Sync), no VIDEO page.
#include <windows.h>

#include "core/dsfix_services.h"
#include "features/features.h"
#include "kit/kit.h"
#include "render/d3d8to9.h"
#include "render/water.h"
#include "graphics/generated/graphics_iface.h"

#include "graphics/generated/graphics_export.inc"

namespace {
void PSE_CALL SettingsChanged() { features::ApplyAnisotropy(); }   // the launcher's OK: TEXTURE FILTERING live

bool Load() {
    const DsfixServicesV1* services = kit::Services();
    if (!services) return false;
    features::ApplyGraphics();   // the Direct3DCreate8 import slot: before WinMain makes the game's Direct3D object
    services->SetSettingsListener(kit::Self(), &SettingsChanged);
    return RegisterGraphicsInterface();
}
}  // namespace

DSFIX_KIT_PLUGIN("dsfix.graphics", "MSAA / anisotropy / V-Sync, D3D9 renderers, video options", PSE_PLUGIN_CLIENT_ONLY,
                 Load)
