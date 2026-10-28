#pragma once
#include <cstdint>

// One Apply*() per feature, called once from Init() (dllmain.cpp) before the game's own start-up code runs.
// Features come over from DesertStormFix / DesertStorm2Fix once their Global Storm addresses are mapped (PORTING.md).
namespace features {

// Which ConflictGlobal.exe this is. Every feature verifies its patch sites' bytes anyway (patch::ApplyGroup), so an
// unknown build only means those features stay off.
enum class GameExe { Unknown, Retail, NoCd };
GameExe IdentifyExe();
const char* ExeName(GameExe e);

void ApplyFrameHook();   // per game frame: graphics re-hooks (frame.cpp)
void ApplyWidescreen();  // launcher lists every display mode (widescreen, 4K)
void ApplyHud();  // HUD / menu art scaled + aligned for the resolution (hud.cpp)
void OnFrameHud();   // per frame: front-end squeezed to 4:3 on wide screens
void ApplyDevBackground();
void OnDirectInputCreated(void* directInput8A);   // each IDirectInput8A the game creates (devbg.cpp: key injection)  // dev builds: off-screen windowed test runs, back-buffer shots, StartLevel (devbg.cpp)

}  // namespace features
