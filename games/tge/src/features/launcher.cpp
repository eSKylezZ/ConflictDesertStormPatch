// Pad navigation in the launcher dialogs (part of tgefix.controller): DesertStormFix's launcher_pad (ported in
// launcher_pad.cpp) on TGE's three nested dialogs (mapped 2026-10-25):
//   106 launcher   procedure 0x5BF9C0, `push` imm32 at 0x5BFA13 (DialogBoxParamA at 0x5BFA23): OK 1 = Play,
//                  0x405 Settings..., 2 Cancel (quits); start = Play, Triangle / Y = Settings.
//   108 Settings   procedure 0x5BF8B0, imm32 0x5BF905 (display.cpp's SettingsProc already hooks it - this chains on the
//                  same slot): combos 0x3FA device, 0x3FC resolution, 0x405 colour depth (CBN_SELCHANGE is matched by
//                  the combo's HWND in lParam - launcher_pad's ChangeCombo sends it), 0x40A Restore Defaults (writes
//                  the registry at once), 0x3FD Advanced Settings...; start = OK, Circle / B = Cancel, Triangle / Y =
//                  Advanced.
//   107 Advanced   procedure 0x5BF5D0, imm32 0x5BF615: seven auto checkboxes (only the first a tab stop), some disabled
//                  by device caps; start = OK, Circle / B = Cancel.
// Every procedure returns 0 from WM_INITDIALOG (no control focused): the first direction press focuses the top-left
// control. The hint line goes below each dialog's buttons; the dialog grows to fit it.
#include <windows.h>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "features/launcher_pad.h"

namespace {
constexpr uint32_t kLauncherSlot = 0x5BFA13, kLauncherProc = 0x5BF9C0;
constexpr uint32_t kSettingsSlot = 0x5BF905, kSettingsProc = 0x5BF8B0;
constexpr uint32_t kAdvancedSlot = 0x5BF615, kAdvancedProc = 0x5BF5D0;

DLGPROC g_launcherNext = reinterpret_cast<DLGPROC>(kLauncherProc);
DLGPROC g_settingsNext = reinterpret_cast<DLGPROC>(kSettingsProc);
DLGPROC g_advancedNext = reinterpret_cast<DLGPROC>(kAdvancedProc);

INT_PTR Wrap(DLGPROC next, HWND dlg, UINT msg, WPARAM wp, LPARAM lp, launcherpad::Kind kind) {
    INT_PTR result = 0;
    if (msg != WM_INITDIALOG && launcherpad::OnMessage(dlg, msg, wp, lp, &result)) return result;
    const INT_PTR r = next(dlg, msg, wp, lp);
    if (msg == WM_INITDIALOG) {
        switch (kind) {
            case launcherpad::Kind::Launcher: launcherpad::Attach(dlg, kind, {1, 0x405, 0}, 6, 220, 205, 13); break;
            case launcherpad::Kind::Settings: launcherpad::Attach(dlg, kind, {1, 0x3FD, 2}, 7, 177, 207, 26); break;
            default: launcherpad::Attach(dlg, kind, {1, 0, 2}, 7, 189, 204, 26); break;
        }
    }
    return r;
}
INT_PTR CALLBACK LauncherProc(HWND d, UINT m, WPARAM w, LPARAM l) { return Wrap(g_launcherNext, d, m, w, l, launcherpad::Kind::Launcher); }
INT_PTR CALLBACK SettingsProc(HWND d, UINT m, WPARAM w, LPARAM l) { return Wrap(g_settingsNext, d, m, w, l, launcherpad::Kind::Settings); }
INT_PTR CALLBACK AdvancedProc(HWND d, UINT m, WPARAM w, LPARAM l) { return Wrap(g_advancedNext, d, m, w, l, launcherpad::Kind::Advanced); }
}  // namespace

void features::ApplyLauncherPad() {
    if (!Get().controller) return;
    const bool ok =
        patch::HookSlot(kLauncherSlot, reinterpret_cast<const void*>(&LauncherProc), reinterpret_cast<void**>(&g_launcherNext), kLauncherProc) &&
        patch::HookSlot(kSettingsSlot, reinterpret_cast<const void*>(&SettingsProc), reinterpret_cast<void**>(&g_settingsNext), kSettingsProc) &&
        patch::HookSlot(kAdvancedSlot, reinterpret_cast<const void*>(&AdvancedProc), reinterpret_cast<void**>(&g_advancedNext), kAdvancedProc);
    dslog::Write(ok ? "[ok]   Launcher: pad navigation" : "[fail] Launcher: dialog procedures not hooked - no pad navigation");
}
