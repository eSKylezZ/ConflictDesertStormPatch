#pragma once
#include <windows.h>

// Gamepad navigation for the launcher dialogs (they run before the game has any input code). Port of DesertStormFix's
// launcher_pad (games/ds1/src/features/launcher_pad.h); the dialogs' button ids come from Attach.
//   D-pad / stick     move focus to the nearest control in that direction (on-screen layout, not tab order);
//                     Left/Right on a dropdown change its value instead
//   A / Cross         press the focused button / tick the focused checkbox / next dropdown value
//   B / Circle        Cancel (Settings / Advanced)
//   Start / Options   OK (Settings / Advanced) / Play (launcher)
//   Y / Triangle      Settings (launcher) / Advanced Settings (Settings)
// Hints follow the device in use: keyboard & mouse (Enter / Esc keys), PlayStation or Xbox icons; the orange
// focus frame only shows while the pad is driving. A dialog ignores the pad until all buttons are released
// after it becomes active (the press that closed Settings must not also press a launcher button).
namespace launcherpad {
enum class Kind { Launcher, Settings, Advanced };

// Call after the game's WM_INITDIALOG handling. The button hints go at (hintX, hintY, hintW, hintH) in dialog
// units; the dialog grows to fit them the first time a pad is seen.
// Buttons the pad presses: launcher = start (Play) + alt (Settings); settings = start (OK) + back (Cancel).
struct Buttons {
    int start, alt, back;
};
void Attach(HWND dlg, Kind kind, Buttons buttons, int hintX, int hintY, int hintW, int hintH);
// Call first in the dialog proc; returns true when the message was fully handled (result in *result).
bool OnMessage(HWND dlg, UINT msg, WPARAM wp, LPARAM lp, INT_PTR* result);
}  // namespace launcherpad
