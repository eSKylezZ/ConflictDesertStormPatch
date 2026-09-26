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

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/padio.h"
#include "features/launcher_pad.h"
#include "features/overlay.h"
#include "features/splitscreen_layout.h"

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
              kIdHudSize = 1205, kIdPad = 1206, kIdDeadzoneLabel = 1207, kIdDeadzone = 1208, kIdNote = 1209,
              kIdSplitLabel = 1210, kIdSplit = 1211, kIdSplitPreview = 1212, kIdDiscord = 1213,
              kIdDisplayLabel = 1214, kIdDisplay = 1215, kIdSkipIntro = 1216, kIdSkipCutscenes = 1217,
              kIdAaLabel = 1218, kIdAa = 1219, kIdAfLabel = 1220, kIdAf = 1221;

constexpr int kDialogW = 221;                  // original client width, dialog units
constexpr int kColumnX = 228, kColumnW = 207;  // our column; 7 DLU margin on both sides
constexpr int kExtraH = 76;                    // dialog grows by this (DLU) for our rows

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
    {"Unlimited (up to 500 fps)", 0, false},
};
constexpr const char* kDisplayModes[] = {"Fullscreen", "Windowed", "Borderless window"};
constexpr uint32_t kAntialiasing[] = {0, 2, 4, 8};
constexpr uint32_t kAnisotropy[] = {0, 2, 4, 8, 16};
constexpr uint32_t kHudSizes[] = {75, 85, 100, 115, 125, 150};
constexpr uint32_t kDeadzones[] = {10, 15, 20, 25, 30, 40, 50};
constexpr const char* kSplitLayouts[splitscreen::kLayoutCount] = {"Horizontal (top / bottom)",
                                                                  "Vertical (side by side)"};

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

// Preview under the split-screen choice: the 2, 3 and 4 player layouts as small 16:9 screens, the views
// numbered and coloured per player, split by the game's grey divider lines.
void DrawSplitPreview(HWND dlg, const DRAWITEMSTRUCT& di) {
    const auto layout = static_cast<splitscreen::Layout>(SelectedData(dlg, kIdSplit));
    HDC dc = di.hDC;
    const RECT& rc = di.rcItem;
    FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, g_font);
    TEXTMETRICA tm;
    GetTextMetricsA(dc, &tm);

    constexpr COLORREF kPlayer[] = {RGB(58, 96, 150), RGB(150, 62, 58), RGB(62, 124, 70), RGB(150, 128, 48)};
    constexpr int kGap = 6;
    const int boxW = (rc.right - rc.left - 2 * kGap) / 3;
    const int boxH = std::min(boxW * 9 / 16, static_cast<int>(rc.bottom - rc.top - tm.tmHeight - 1));
    for (int players = 2; players <= 4; ++players) {
        const int bx = rc.left + (players - 2) * (boxW + kGap), by = rc.top;
        RECT screen{bx, by, bx + boxW, by + boxH};
        FillRect(dc, &screen, GetSysColorBrush(COLOR_BTNSHADOW));  // shows through as the divider lines
        for (int i = 0; i < players; ++i) {
            splitscreen::View v = splitscreen::ViewRect(layout, players, i);
            RECT r{bx + static_cast<int>(v.x * boxW), by + static_cast<int>(v.y * boxH),
                   bx + static_cast<int>((v.x + v.w) * boxW), by + static_cast<int>((v.y + v.h) * boxH)};
            InflateRect(&r, -1, -1);
            HBRUSH b = CreateSolidBrush(kPlayer[i]);
            FillRect(dc, &r, b);
            DeleteObject(b);
            char n[2] = {static_cast<char>('1' + i), 0};
            SetTextColor(dc, RGB(255, 255, 255));
            DrawTextA(dc, n, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        FrameRect(dc, &screen, GetSysColorBrush(COLOR_3DDKSHADOW));
        char caption[16];
        snprintf(caption, sizeof caption, "%d players", players);
        RECT cr{bx, by + boxH + 1, bx + boxW, rc.bottom};
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        DrawTextA(dc, caption, -1, &cr, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }
    SelectObject(dc, oldFont);
}

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
    SelectData(GetDlgItem(dlg, kIdSplit), v.splitScreenLayout, "");
    CheckDlgButton(dlg, kIdDiscord, v.discordPresence ? BST_CHECKED : BST_UNCHECKED);
    SelectData(GetDlgItem(dlg, kIdDisplay), v.displayMode, "");
    CheckDlgButton(dlg, kIdSkipIntro, v.skipIntro ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, kIdSkipCutscenes, v.skipCutscenes ? BST_CHECKED : BST_UNCHECKED);
    SelectData(GetDlgItem(dlg, kIdAa), v.antialiasing, "x");
    SelectData(GetDlgItem(dlg, kIdAf), v.anisotropy, "x");
    InvalidateRect(GetDlgItem(dlg, kIdSplitPreview), nullptr, FALSE);
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
    v.splitScreenLayout = static_cast<uint32_t>(SelectedData(dlg, kIdSplit));
    v.discordPresence = IsDlgButtonChecked(dlg, kIdDiscord) == BST_CHECKED;
    v.displayMode = static_cast<uint32_t>(SelectedData(dlg, kIdDisplay));
    v.skipIntro = IsDlgButtonChecked(dlg, kIdSkipIntro) == BST_CHECKED;
    v.skipCutscenes = IsDlgButtonChecked(dlg, kIdSkipCutscenes) == BST_CHECKED;
    v.antialiasing = static_cast<uint32_t>(SelectedData(dlg, kIdAa));
    v.anisotropy = static_cast<uint32_t>(SelectedData(dlg, kIdAf));
    if (settings::Save(v)) features::OnSettingsChanged();
}

void MoveBy(HWND dlg, int id, int dx, int dy) {
    HWND c = GetDlgItem(dlg, id);
    RECT r;
    GetWindowRect(c, &r);
    MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT*>(&r), 2);
    SetWindowPos(c, nullptr, r.left + dx, r.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
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

    // Widen the dialog by one column and make it taller, keep it centred, and move the bottom row down / OK and
    // Cancel to the new bottom-right corner.
    RECT extra = Dlu(dlg, 0, 0, kColumnX + kColumnW + 7 - kDialogW, kExtraH);
    const int dx = extra.right, dy = extra.bottom;
    RECT wr;
    GetWindowRect(dlg, &wr);
    SetWindowPos(dlg, nullptr, wr.left - dx / 2, wr.top - dy / 2, wr.right - wr.left + dx, wr.bottom - wr.top + dy,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    MoveBy(dlg, kIdOk, dx, dy);
    MoveBy(dlg, kIdCancel, dx, dy);
    MoveBy(dlg, kIdDefaults, 0, dy);

    const int x = kColumnX + 6, w = kColumnW - 12, lx = x + 12, cx = x + 72, cw = w - 72;
    Add(dlg, "BUTTON", "Enhancements", BS_GROUPBOX, kIdGroup, kColumnX, 7, kColumnW, 215 + kExtraH);

    Add(dlg, "STATIC", "&Frame rate limit:", SS_LEFT, kIdFpsLabel, x, 22, 70, 8);
    HWND fps = Add(dlg, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kIdFps, cx, 20, cw, 120);
    for (const auto& c : kFps) AddItem(fps, c.label, FpsData(c.cap, c.toRefresh));

    Add(dlg, "STATIC", "Displa&y mode:", SS_LEFT, kIdDisplayLabel, x, 38, 70, 8);
    HWND display = Add(dlg, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kIdDisplay, cx, 36, cw, 60);
    for (uint32_t i = 0; i < std::size(kDisplayModes); ++i) AddItem(display, kDisplayModes[i], i);
    Add(dlg, "BUTTON", "S&kip the intro (logos and opening film)", BS_AUTOCHECKBOX | WS_TABSTOP, kIdSkipIntro, x, 55, w, 10);
    Add(dlg, "BUTTON", "Skip mission c&utscenes automatically", BS_AUTOCHECKBOX | WS_TABSTOP, kIdSkipCutscenes, x,
        69, w, 10);

    Add(dlg, "STATIC", "Anti-a&liasing:", SS_LEFT, kIdAaLabel, x, 88, 70, 8);
    HWND aa = Add(dlg, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kIdAa, cx, 86, cw, 80);
    for (uint32_t a : kAntialiasing) {
        char t[16];
        snprintf(t, sizeof t, a ? "%ux MSAA" : "Off", a);
        AddItem(aa, t, a);
    }
    Add(dlg, "STATIC", "Te&xture filtering:", SS_LEFT, kIdAfLabel, x, 104, 70, 8);
    HWND af = Add(dlg, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kIdAf, cx, 102, cw, 80);
    for (uint32_t a : kAnisotropy) {
        char t[32];
        snprintf(t, sizeof t, a ? "%ux anisotropic%s" : "Game default", a, a == 16 ? " (default)" : "");
        AddItem(af, t, a);
    }

    Add(dlg, "BUTTON", "Scale &HUD and menus to the resolution", BS_AUTOCHECKBOX | WS_TABSTOP, kIdHud, x, 120, w, 10);
    Add(dlg, "STATIC", "HUD s&ize:", SS_LEFT, kIdHudSizeLabel, lx, 137, 58, 8);
    HWND hud = Add(dlg, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kIdHudSize, cx, 135, cw, 120);
    for (uint32_t s : kHudSizes) {
        char t[16];
        snprintf(t, sizeof t, "%u%%%s", s, s == 100 ? " (default)" : "");
        AddItem(hud, t, s);
    }

    Add(dlg, "BUTTON", "&Controller support (DualShock 4 / DualSense)", BS_AUTOCHECKBOX | WS_TABSTOP, kIdPad, x, 160,
        w, 10);
    Add(dlg, "STATIC", "Stick dead&zone:", SS_LEFT, kIdDeadzoneLabel, lx, 177, 58, 8);
    HWND dz = Add(dlg, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kIdDeadzone, cx, 175, cw, 120);
    for (uint32_t d : kDeadzones) {
        char t[16];
        snprintf(t, sizeof t, "%u%%%s", d, d == 40 ? " (default)" : "");
        AddItem(dz, t, d);
    }

    Add(dlg, "BUTTON", "Show the game in my Discord st&atus", BS_AUTOCHECKBOX | WS_TABSTOP, kIdDiscord, x, 192, w,
        10);

    Add(dlg, "STATIC", "Spli&t screen:", SS_LEFT, kIdSplitLabel, x, 209, 70, 8);
    HWND split = Add(dlg, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kIdSplit, cx, 207, cw, 60);
    for (uint32_t i = 0; i < splitscreen::kLayoutCount; ++i) AddItem(split, kSplitLayouts[i], i);
    Add(dlg, "STATIC", "", SS_OWNERDRAW, kIdSplitPreview, cx, 223, cw, 30);

    Add(dlg, "STATIC", "DesertStormFix " DS_VERSION, SS_LEFT, kIdNote, x, 288, w, 8);
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
        if (id == kIdSplit && HIWORD(wp) == CBN_SELCHANGE)
            InvalidateRect(GetDlgItem(dlg, kIdSplitPreview), nullptr, FALSE);
    }
    if (msg == WM_DRAWITEM && wp == kIdSplitPreview) {
        DrawSplitPreview(dlg, *reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
        return TRUE;
    }
    INT_PTR result = g_gameProc(dlg, msg, wp, lp);
    if (msg == WM_INITDIALOG) {
        Build(dlg);
        launcherpad::Attach(dlg, launcherpad::Kind::DetailSettings, kColumnX + 6, 179 + kExtraH, kColumnW - 12, 29);
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

void features::OnFrame() {
    overlay::OnFrame();
    OnFrameInput();
    OnFrameSplitScreen();
    OnFrameDiscord();
    OnFrameCoop();
    OnFrameFontSharp();
    OnFrameControls();
    padio::Update();
    FollowActivePad();
    OnFrameDev();
}

void features::OnSettingsChanged() {
    ApplyHud();
    ApplyController();
    ApplyInGameInput();
    ApplyFrameCap();
    ApplyDiscord();
    ApplyDisplay();
    ApplyCinematics();
}
