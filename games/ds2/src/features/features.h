#pragma once
#include <cstdint>

// One Apply*() per feature, called once from Init() (dllmain.cpp) before the game's own start-up code runs.
// Features come over from DesertStormFix (reference/ds1-features) once their DS2 addresses are mapped (PORTING.md).
namespace features {

// Which CDS II.exe this is. Every feature verifies its patch sites' bytes anyway (patch::ApplyGroup), so an
// unknown build only means those features stay off.
enum class GameExe { Unknown, Retail100, Retail100NoDisc };
GameExe IdentifyExe();
const char* ExeName(GameExe e);

void ApplyWidescreen();  // launcher lists every display mode (widescreen, 4K)
void ApplyHud();  // HUD / menu art scaled to the resolution (hud.cpp)
void ApplyFrontEnd();  // front-end laid out at 1280x1024, scaled + centred by graphics.cpp (frontend.cpp)
void ApplyFontSharp();  // sharper text: font pages swapped for enlarged, sharpened copies (fontsharp.cpp)
void ApplyCinematics();  // skip the start-up logo films (SkipIntro) (cinematics.cpp)
void ApplyMenus();  // main menu SINGLE PLAYER / CO-OP / OPTIONS / QUIT + co-op screen state (menus.cpp, menus.h)
void ApplySplitScreen();  // local split screen 2-4 players (splitscreen.cpp)
int SplitPlayers();       // views built for the running level (1 = no split screen)
// split screen: w / h become the smallest view's size (HUD art scale k per view, hud.cpp); unchanged otherwise
void SplitViewSize(int& w, int& h);
void ApplyCoop();  // co-op join screen + session (coop.cpp; the join screen needs the menu feature's menus.h)
// co-op session from the join screen (coop.cpp): players joined (0 = none), each player's joystick (-1 = keyboard)
int CoopPlayers();
int CoopJoystick(int player);
void ApplyDevBackground();  // dev builds: off-screen windowed test runs, back-buffer shots, StartLevel (devbg.cpp)

}  // namespace features
