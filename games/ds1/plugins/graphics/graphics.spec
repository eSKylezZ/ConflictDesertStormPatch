# What dsfix.dll calls in the plugin dsfix.graphics (MSAA / anisotropy / V-Sync / scene fog / photo capture hooks on the
# D3D8 device, the D3D9 / D3D9On12 / own-presenter renderers, OPTIONS > VIDEO); `= value` is dsfix.dll's answer without
# the plugin. scripts/gen_host_calls.py -> plugins/graphics/generated/, src/generated/graphics_wrappers.inc.
interface dsfix.graphics
side plugin graphics
pod render::PuddlePart
pod render::WaterLook

# V-Sync (graphics.cpp): the frame cap (framecap.cpp) and the frame timing (timing.cpp) follow it
uint32_t features::VsyncFps() = 0
uint32_t features::VsyncProbeFps() = 0

# scene fog over the loadout screen (the plugin dsfix.loadout, through dsfix.host)
void features::SetSceneFog(bool on, uint32_t colour, float start, float end)
void features::SceneFogBegin()

# OPTIONS > VIDEO (videomenu.cpp): the screen router (frontend.cpp), its texts (the Lookup hook), its display changes
# between frames (the frame dispatcher)
void* features::VideoMenuScreen() = nullptr
const char* features::VideoMenuText(uint32_t hash) = nullptr
void features::OnFrameVideoMenu()

# the D3D9 renderers (render/): split-screen HUD passes (the plugin dsfix.coop), puddle reflections (services.cpp for the
# plugin dsfix.weather; false = Renderer 0 / no plugin: plain decals)
void render::NoteHudStart()
bool render::DrawPuddleReflections(void* dev8, const render::PuddlePart* parts, int count, const render::WaterLook& look) = false
