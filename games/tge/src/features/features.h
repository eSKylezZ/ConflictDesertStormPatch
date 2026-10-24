#pragma once
// GreatEscapeFix features: one Apply per internal plugin "tgefix.<feature>" (src/tgefix.cpp runs each in its owner
// scope at start-up).
#include <cstdint>
#include <string>

namespace features {
// What the features read from tgefix's pse.settings (HKCU\Software\PSE\tge\Plugins\tgefix), filled before Apply.
struct Settings {
    bool widescreen = true;   // FeatWidescreen
    bool filmsFill = true;    // FilmsFill: films zoomed to the screen's width (widescreen.cpp)
    bool mods = true;         // FeatMods
    bool windowed = false;    // Windowed (launcher Settings... > Windowed; a mode as big as the monitor = borderless)
    bool hudScaling = true;   // HudScaling: menus + HUD scaled with the resolution (guiscale.cpp)
    uint32_t hudScalePercent = 100;   // HudScalePercent 25..400: on top of the resolution's factor
    bool frameCap = true;     // FrameCap (framecap.cpp)
    uint32_t fpsCap = 0;      // FpsCap: 0 = the monitor's refresh rate (max 240), else 30..240
};
const Settings& Get();

void ApplyWidescreen();   // widescreen.cpp: every display mode in the launcher
void ApplyMods();         // mods.cpp: <game>\Mods\<mod>\... files replace / add game files by name
void ApplyGuiScale();     // guiscale.cpp: the GUI laid out on a virtual screen, drawn k times bigger
// guiscale.cpp: a pointer position (back-buffer pixels) -> the GUI's own coordinates (display.cpp's mouse mapping).
void GuiToVirtual(long& x, long& y);
void ApplyDisplay();      // display.cpp: the launcher's Windowed box, windowed / borderless window, mouse mapping
void ApplyFrameCap();     // framecap.cpp: wait on every frame event, monitor refresh / FpsCap, max 240
// devbg.cpp (dev builds): Dev\Background = scripted tests off-screen, never activated (release: no-ops / false)
void ApplyDevBackground();
bool Background();
// display.cpp's Settings... dialog wrapper hands every message on (after the game's): picks Dev\Resolution, OK unseen
void DevBgSettingsDialog(void* dlg, unsigned msg);
// tgefix.cpp: stores `Windowed` (pse.settings) - display.cpp calls it from the Settings dialog's OK.
void SaveWindowed(bool on);
// display.cpp: the D3D device was just made (tgefix.cpp's PSE_MSG_DEVICE_CREATED listener) - window to the front.
void DisplayDeviceCreated();

// mods.cpp
void ModLog(const char* fmt, ...);   // log + Mods\GreatEscapeFix-report.txt
int ModCount();
int ModProblems();
// The level the game loaded last is a mission ("*.dll" level) - tgefix.cpp follows the layer's level events.
void SetInMission(bool inMission);
}  // namespace features
