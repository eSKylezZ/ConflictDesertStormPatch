#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// One Apply*() per feature, called once from Init() (dllmain.cpp) before the game's own start-up code runs.
// Features come over from DesertStormFix (reference/ds1-features) once their Vietnam addresses are mapped
// (PORTING.md). widescreen / hud still carry DS2 addresses: their byte checks fail on Vietnam.exe, so they stay off.
namespace features {

// Which Vietnam.exe this is. Every feature verifies its patch sites' bytes anyway (patch::ApplyGroup), so an
// unknown build only means those features stay off.
enum class GameExe { Unknown, Retail100, Retail100Fixed, Patched };
GameExe IdentifyExe();
const char* ExeName(GameExe e);

void ApplyWidescreen();  // launcher lists every display mode (widescreen, 4K)
void ApplyHud();
void OnFrameHud();  // per frame (frame.cpp): HUD layout width  // HUD / menu art scaled to the resolution (hud.cpp)
void ApplyMods();  // Mods\<mod>\** files served ahead of the .dat archives (mods.cpp)
// Backend of the MODS menu (modsmenu.cpp):
void ModsRescan();                         // re-reads Mods\ into the index (hooks stay installed)
std::vector<std::pair<std::string, bool>> ModsList();  // every Mods\ folder, name without "_", on/off
bool ModsToggle(const std::string& name);  // renames the folder ("_" prefix) and rescans; false = not found / IO error
int ModsInstalledCount();                  // folders under Mods\ (on + off)
int ModsActiveCount();                     // folders currently indexed (on)
void ApplyModsMenu();  // MODS item in the main menu + its own front-end screen (modsmenu.cpp)
void ApplyFrontEnd();
void LoadingFrameBegin();  // around the "press any key" frame (frame.cpp)
void LoadingFrameEnd();  // front end laid out at 4:3, centred (frontend.cpp)
extern void (*g_afterFrontEndLoad)();  // after each LoadFrontEnd (dev StartLevel)
void ApplyFrameHook();  // per-frame hook in the message loop (frame.cpp)
void ApplyProbes();  // dev builds: Dev\Probe call logging (probe.cpp)
void ApplyDevBackground();  // dev builds: off-screen windowed test runs, back-buffer shots, StartLevel (devbg.cpp)

}  // namespace features
