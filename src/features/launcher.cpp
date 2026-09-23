// "Enhancements" settings in the launcher. The launcher's Settings button opens the Detail Settings dialog
// (resource 108) via FUN_00447980: DialogBoxParamA(..., 108, ..., 0x447900). The dialog proc pushed at 0x44798E
// is replaced with DetailProc, which forwards everything to the game's proc and adds a column of controls:
//   WM_INITDIALOG  -> widen the dialog, create the controls, fill them from settings
//   OK (1040)      -> save to the registry and re-apply the features (the game has not started yet)
//   Defaults(1034) -> our controls back to their defaults too
// The game's command handler FUN_004479d0 ignores unknown control ids, so ours (1200+) never reach its logic.
// The launcher itself (dialog 106, proc pushed at 0x44793C) gets MainProc; both dialogs get gamepad navigation
// (launcher_pad.cpp).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/launcher_pad.h"

namespace {
constexpr uint32_t kMainProcPush = 0x44793C;  // push 0x4478c0 (launcher dialog 106 proc) in FUN_00447930
constexpr uint32_t kMainGameProc = 0x4478C0;
constexpr uint32_t kProcPush = 0x44798E;      // push 0x447900 (Detail Settings dialog proc) in FUN_00447980
constexpr uint32_t kGameProc = 0x447900;
const DLGPROC g_mainGameProc = reinterpret_cast<DLGPROC>(kMainGameProc);
const DLGPROC g_gameProc = reinterpret_cast<DLGPROC>(kGameProc);

// Game control ids in dialog 108
constexpr int kIdOk = 1040, kIdCancel = 1041, kIdDefaults = 1034;
// Launcher (dialog 106): OK = IDOK, Cancel = IDCANCEL
constexpr int kIdSettingsButton = 1029;
// Ours
constexpr int kIdGroup = 1200, kIdFpsLabel = 1201, kIdFps = 1202, kIdHud = 1203, kIdHudSizeLabel = 1204,
              kIdHudSize = 1205, kIdPad = 1206, kIdDeadzoneLabel = 1207, kIdDeadzone = 1208, kIdNote = 1209;

constexpr int kDialogW = 221;                  // original client width, dialog units
constexpr int kColumnX = 228, kColumnW = 207;  // our column; 7 DLU margin on both sides

struct FpsChoice {
    const char* label;
    uint32_t cap;
    bool toRefresh;
};
constexpr FpsChoice kFps[] = {
    {"Monitor refresh rate (max 240)", 240, true},
    {"Monitor refresh rate", 0, true},
    {"30 fps", 30, false},
    {"60 fps", 60, false},
    {"120 fps", 120, false},
    {"144 fps", 144, false},
    {"165 fps", 165, false},
    {"240 fps", 240, false},
    {"Unlimited (not recommended)", 0, false},
};
constexpr uint32_t kHudSizes[] = {75, 85, 100, 115, 125, 150};
constexpr uint32_t kDeadzones[] = {10, 15, 20, 25, 30, 40, 50};

HFONT g_font = nullptr;

RECT Dlu(HWND dlg, int x, int y, int w, int h) {
    RECT r{x, y, x + w, y + h};
    MapDialogRect(dlg, &r);
    return r;
}

HWND Add(HWND dlg, const char* cls, const char* text, DWORD style, int id, int x, int y, int w, int h) {
    RECT r = Dlu(dlg, x, y, w, h);
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, r.left, r.top, r.right - r.left,
                             r.bottom - r.top, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             GetModuleHandleA(nullptr), nullptr);
    SendMessageA(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
    return c;
}

void AddItem(HWND combo, const char* text, LPARAM data) {
    auto i = SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    SendMessageA(combo, CB_SETITEMDATA, i, data);
}

// Selects the item whose data == value, adding "<value><suffix>" if the registry holds an unlisted value.
void SelectData(HWND combo, LPARAM value, const char* suffix) {
    auto n = SendMessageA(combo, CB_GETCOUNT, 0, 0);
    for (LRESULT i = 0; i < n; ++i) {
        if (SendMessageA(combo, CB_GETITEMDATA, i, 0) == value) {
            SendMessageA(combo, CB_SETCURSEL, i, 0);
            return;
        }
    }
    char text[32];
    snprintf(text, sizeof text, "%d%s", static_cast<int>(value), suffix);
    AddItem(combo, text, value);
    SelectData(combo, value, suffix);
}

LPARAM SelectedData(HWND dlg, int id) {
    HWND c = GetDlgItem(dlg, id);
    return SendMessageA(c, CB_GETITEMDATA, SendMessageA(c, CB_GETCURSEL, 0, 0), 0);
}

// Frame choice data = cap | toRefresh << 16
LPARAM FpsData(uint32_t cap, bool toRefresh) { return static_cast<LPARAM>(cap | (toRefresh ? 0x10000u : 0u)); }

void UpdateEnabled(HWND dlg) {
    bool hud = IsDlgButtonChecked(dlg, kIdHud) == BST_CHECKED;
    bool pad = IsDlgButtonChecked(dlg, kIdPad) == BST_CHECKED;
    EnableWindow(GetDlgItem(dlg, kIdHudSizeLabel), hud);
    EnableWindow(GetDlgItem(dlg, kIdHudSize), hud);
    EnableWindow(GetDlgItem(dlg, kIdDeadzoneLabel), pad);
    EnableWindow(GetDlgItem(dlg, kIdDeadzone), pad);
}

void Fill(HWND dlg, const settings::Values& v) {
    SelectData(GetDlgItem(dlg, kIdFps), FpsData(v.fpsCap, v.fpsCapToRefresh), " fps");
    CheckDlgButton(dlg, kIdHud, v.hudScaling ? BST_CHECKED : BST_UNCHECKED);
    SelectData(GetDlgItem(dlg, kIdHudSize), v.hudScalePercent, "%");
    CheckDlgButton(dlg, kIdPad, v.controller ? BST_CHECKED : BST_UNCHECKED);
    SelectData(GetDlgItem(dlg, kIdDeadzone), v.padDeadzone, "%");
    UpdateEnabled(dlg);
}

void Save(HWND dlg) {
    settings::Values v = settings::Get();
    auto fps = static_cast<uint32_t>(SelectedData(dlg, kIdFps));
    v.fpsCap = fps & 0xFFFF;
    v.fpsCapToRefresh = (fps >> 16) != 0;
    v.hudScaling = IsDlgButtonChecked(dlg, kIdHud) == BST_CHECKED;
    v.hudScalePercent = static_cast<uint32_t>(SelectedData(dlg, kIdHudSize));
    v.controller = IsDlgButtonChecked(dlg, kIdPad) == BST_CHECKED;
    v.padDeadzone = static_cast<uint32_t>(SelectedData(dlg, kIdDeadzone));
    if (settings::Save(v)) features::OnSettingsChanged();
}

void MoveBy(HWND dlg, int id, int dx) {
    HWND c = GetDlgItem(dlg, id);
    RECT r;
    GetWindowRect(c, &r);
    MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT*>(&r), 2);
    SetWindowPos(c, nullptr, r.left + dx, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Tab / arrow-key order is the controls' z-order, which in the game's resources does not follow the layout
// (launcher: OK, Cancel, Settings - so Right from OK jumped to Cancel; an original bug). Moves each listed
// control to the end of the order, in sequence.
void MoveToEndOfTabOrder(HWND dlg, std::initializer_list<int> ids) {
    for (int id : ids)
        if (HWND c = GetDlgItem(dlg, id))
            SetWindowPos(c, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Build(HWND dlg) {
    g_font = reinterpret_cast<HFONT>(SendMessageA(dlg, WM_GETFONT, 0, 0));

    // Widen the dialog by one column, keep it centred, and move OK/Cancel to the new bottom-right corner.
    RECT extra = Dlu(dlg, 0, 0, kColumnX + kColumnW + 7 - kDialogW, 0);
    int dx = extra.right;
    RECT wr;
    GetWindowRect(dlg, &wr);
    SetWindowPos(dlg, nullptr, wr.left - dx / 2, wr.top, wr.right - wr.left + dx, wr.bottom - wr.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    MoveBy(dlg, kIdOk, dx);
    MoveBy(dlg, kIdCancel, dx);

    const int x = kColumnX + 6, w = kColumnW - 12, lx = x + 12, cx = x + 72, cw = w - 72;
    Add(dlg, "BUTTON", "Enhancements", BS_GROUPBOX, kIdGroup, kColumnX, 7, kColumnW, 215);

    Add(dlg, "STATIC", "&Frame rate limit:", SS_LEFT, kIdFpsLabel, x, 22, 70, 8);
    HWND fps = Add(dlg, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kIdFps, cx, 20, cw, 120);
    for (const auto& c : kFps) AddItem(fps, c.label, FpsData(c.cap, c.toRefresh));

    Add(dlg, "BUTTON", "Scale &HUD and menus to the resolution", BS_AUTOCHECKBOX | WS_TABSTOP, kIdHud, x, 44, w, 10);
    Add(dlg, "STATIC", "HUD s&ize:", SS_LEFT, kIdHudSizeLabel, lx, 61, 58, 8);
    HWND hud = Add(dlg, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kIdHudSize, cx, 59, cw, 120);
    for (uint32_t s : kHudSizes) {
        char t[16];
        snprintf(t, sizeof t, "%u%%%s", s, s == 100 ? " (default)" : "");
        AddItem(hud, t, s);
    }

    Add(dlg, "BUTTON", "&Controller support (DualShock 4 / DualSense)", BS_AUTOCHECKBOX | WS_TABSTOP, kIdPad, x, 84,
        w, 10);
    Add(dlg, "STATIC", "Stick dead&zone:", SS_LEFT, kIdDeadzoneLabel, lx, 101, 58, 8);
    HWND dz = Add(dlg, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kIdDeadzone, cx, 99, cw, 120);
    for (uint32_t d : kDeadzones) {
        char t[16];
        snprintf(t, sizeof t, "%u%%%s", d, d == 40 ? " (default)" : "");
        AddItem(dz, t, d);
    }

    Add(dlg, "STATIC", "DesertStormFix " DS_VERSION, SS_LEFT, kIdNote, x, 207, w, 8);
    Fill(dlg, settings::Get());
    // Left column, our column, then the bottom row left to right.
    MoveToEndOfTabOrder(dlg, {kIdDefaults, kIdOk, kIdCancel});
}

INT_PTR CALLBACK DetailProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    INT_PTR handled;
    if (launcherpad::OnMessage(dlg, msg, wp, lp, &handled)) return handled;
    if (msg == WM_COMMAND) {
        int id = LOWORD(wp);
        if (id == kIdOk) Save(dlg);  // before the game's handler ends the dialog
        if (id == kIdHud || id == kIdPad) UpdateEnabled(dlg);
    }
    INT_PTR result = g_gameProc(dlg, msg, wp, lp);
    if (msg == WM_INITDIALOG) {
        Build(dlg);
        launcherpad::Attach(dlg, launcherpad::Kind::DetailSettings, kColumnX + 6, 174, kColumnW - 12, 29);
    }
    if (msg == WM_COMMAND && LOWORD(wp) == kIdDefaults) Fill(dlg, settings::Values{});
    return result;
}

// Launcher (dialog 106): only adds gamepad navigation. Hint line goes under the OK/Settings/Cancel row.
INT_PTR CALLBACK MainProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    INT_PTR handled;
    if (launcherpad::OnMessage(dlg, msg, wp, lp, &handled)) return handled;
    INT_PTR result = g_mainGameProc(dlg, msg, wp, lp);
    if (msg == WM_INITDIALOG) MoveToEndOfTabOrder(dlg, {IDOK, kIdSettingsButton, IDCANCEL});
    if (msg == WM_INITDIALOG) launcherpad::Attach(dlg, launcherpad::Kind::Launcher, 6, 226, 205, 14);
    return result;
}

bool SwapPushedProc(uint32_t site, uint32_t expected, DLGPROC ours) {
    uint8_t original[5] = {0x68};
    memcpy(original + 1, &expected, 4);
    if (!patch::Matches(site, original, 5)) {
        dslog::Write("[fail] Launcher: unexpected bytes at 0x%08X", site);
        return false;
    }
    uint32_t proc = reinterpret_cast<uint32_t>(ours);
    return patch::Write(site + 1, &proc, 4);
}
}  // namespace

void features::ApplyLauncher() {
    if (SwapPushedProc(kProcPush, kGameProc, &DetailProc))
        dslog::Write("[ok]   Launcher settings: Detail Settings dialog extended");
    if (SwapPushedProc(kMainProcPush, kMainGameProc, &MainProc))
        dslog::Write("[ok]   Launcher: gamepad navigation");
}

void features::OnSettingsChanged() {
    ApplyHud();
    ApplyController();
    ApplyFrameCap();
}
