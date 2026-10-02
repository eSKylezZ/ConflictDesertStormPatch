#include "features/launcher_pad.h"

#include <climits>
#include <cstring>

#include "core/gamepad.h"
#include "core/log.h"
#include "generated/prompt_icons.h"

namespace launcherpad {
namespace {
constexpr UINT_PTR kTimerId = 0x0D57;
constexpr UINT kPollMs = 16;
constexpr DWORD kRepeatDelay = 400, kRepeatRate = 110;
constexpr COLORREF kHighlight = RGB(255, 140, 0);
constexpr uint32_t kDirections = gamepad::kUp | gamepad::kDown | gamepad::kLeft | gamepad::kRight;

// Game control ids
constexpr int kLauncherOk = 1, kLauncherSettings = 1029;
constexpr int kDetailOk = 1040, kDetailCancel = 1041;

// What the user is controlling the dialog with; decides the hint icons and the focus highlight.
enum class Input { Keyboard, PlayStation, Xbox };

struct Nav {
    HWND dlg = nullptr;
    Kind kind = Kind::Launcher;
    uint32_t prev = 0;
    DWORD repeatAt = 0;
    bool active = false;       // pad used in this dialog: show the focus highlight
    bool current = false;      // the dialog the user is looking at
    bool waitRelease = true;   // ignore the pad until all buttons are up (a press that closed another dialog)
    HWND focused = nullptr;
    HWND hint = nullptr;
    Input input = Input::Keyboard;
    DWORD lastInputTime = 0;  // GetLastInputInfo: keyboard/mouse activity
    DWORD lastPadTime = 0;    // last pad state change
};
Nav g_navs[2];
int g_openCount = 0;

Nav* Find(HWND dlg) {
    for (Nav& n : g_navs)
        if (n.dlg == dlg) return &n;
    return nullptr;
}

RECT ChildRect(HWND dlg, HWND child) {
    RECT r;
    GetWindowRect(child, &r);
    MapWindowPoints(nullptr, dlg, reinterpret_cast<POINT*>(&r), 2);
    return r;
}

void RedrawAround(HWND dlg, HWND child) {
    if (!child || !IsWindow(child)) return;
    RECT r = ChildRect(dlg, child);
    InflateRect(&r, 5, 5);
    RedrawWindow(dlg, &r, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

bool IsClass(HWND w, const char* cls) {
    char name[32];
    return GetClassNameA(w, name, sizeof name) && lstrcmpiA(name, cls) == 0;
}

// Presses a button the way a real click ends up: auto checkboxes toggle, then the dialog gets BN_CLICKED.
// (BM_CLICK fakes mouse down/up and silently does nothing when triggered from the pad timer.) Posted, so a
// button that opens a modal dialog (Settings) does not run it inside our timer callback.
void Press(HWND dlg, HWND button) {
    if (!button || !IsWindowEnabled(button)) return;
    switch (GetWindowLongA(button, GWL_STYLE) & BS_TYPEMASK) {
        case BS_AUTOCHECKBOX:
            SendMessageA(button, BM_SETCHECK, SendMessageA(button, BM_GETCHECK, 0, 0) == BST_CHECKED ? BST_UNCHECKED
                                                                                                    : BST_CHECKED, 0);
            break;
        case BS_AUTORADIOBUTTON: SendMessageA(button, BM_SETCHECK, BST_CHECKED, 0); break;
        default: break;
    }
    PostMessageA(dlg, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(button), BN_CLICKED), reinterpret_cast<LPARAM>(button));
}

void Click(HWND dlg, int id) { Press(dlg, GetDlgItem(dlg, id)); }

// ---- spatial focus movement ----
// The dialogs' tab order does not follow their layout (most detail checkboxes are not even tab stops), so the
// D-pad moves to the nearest focusable control on screen in the pressed direction instead.
enum class Dir { Up, Down, Left, Right };

bool Focusable(HWND w) {
    if (!IsWindowVisible(w) || !IsWindowEnabled(w)) return false;
    if (IsClass(w, "ComboBox") || IsClass(w, "Edit")) return true;
    if (!IsClass(w, "Button")) return false;
    return (GetWindowLongA(w, GWL_STYLE) & BS_TYPEMASK) != BS_GROUPBOX;
}

void Focus(HWND dlg, HWND w) { SendMessageA(dlg, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(w), TRUE); }

// Gap between [a0,a1] and [b0,b1] (0 when they overlap).
LONG Gap(LONG a0, LONG a1, LONG b0, LONG b1) { return b0 > a1 ? b0 - a1 : a0 > b1 ? a0 - b1 : 0; }

void Move(HWND dlg, HWND from, Dir dir) {
    struct Best {
        HWND w = nullptr;
        LONG score = LONG_MAX;
    } best;
    RECT a = from ? ChildRect(dlg, from) : RECT{0, 0, 0, 0};
    const LONG acx = (a.left + a.right) / 2, acy = (a.top + a.bottom) / 2;
    for (HWND w = GetWindow(dlg, GW_CHILD); w; w = GetWindow(w, GW_HWNDNEXT)) {
        if (w == from || !Focusable(w)) continue;
        RECT b = ChildRect(dlg, w);
        const LONG bcx = (b.left + b.right) / 2, bcy = (b.top + b.bottom) / 2;
        constexpr LONG kSlack = 4;
        LONG along, across;  // distance in the pressed direction, and sideways misalignment
        bool inDirection;    // the whole control lies beyond the current one's edge
        switch (dir) {
            case Dir::Down:
                along = bcy - acy, across = Gap(a.left, a.right, b.left, b.right), inDirection = b.top >= a.bottom - kSlack;
                break;
            case Dir::Up:
                along = acy - bcy, across = Gap(a.left, a.right, b.left, b.right), inDirection = b.bottom <= a.top + kSlack;
                break;
            case Dir::Right:
                along = bcx - acx, across = Gap(a.top, a.bottom, b.top, b.bottom), inDirection = b.left >= a.right - kSlack;
                break;
            default:
                along = acx - bcx, across = Gap(a.top, a.bottom, b.top, b.bottom), inDirection = b.right <= a.left + kSlack;
                break;
        }
        if (from && !inDirection) continue;
        // Same row/column (overlapping) always beats a diagonal neighbour; no focus yet: take the top-left control.
        LONG score = !from ? b.top * 4 + b.left : across ? 100000 + along + across * 3 : along;
        if (score < best.score) best = {w, score};
    }
    if (best.w) Focus(dlg, best.w);
}

// Moves a dropdown list by delta; notifies the dialog exactly like a user selection (CBN_SELCHANGE).
void ChangeCombo(HWND dlg, HWND combo, int delta, bool wrap) {
    int count = static_cast<int>(SendMessageA(combo, CB_GETCOUNT, 0, 0));
    if (count <= 0) return;
    int cur = static_cast<int>(SendMessageA(combo, CB_GETCURSEL, 0, 0));
    int next = cur + delta;
    if (wrap) next = (next + count) % count;
    if (next < 0 || next >= count || next == cur) return;
    SendMessageA(combo, CB_SETCURSEL, next, 0);
    SendMessageA(dlg, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(combo), CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
}

// ---- button hints: rows of (icon, label); Kenney input prompts baked into generated/prompt_icons.h ----
using gen::PromptIcon;
struct HintItem {
    PromptIcon icon;
    const char* label;
};
struct HintRow {
    HintItem items[4];
    int count;
};
struct HintSet {
    HintRow rows[2];
    int count;
};

const HintSet& Hints(Kind kind, Input input) {
    static const HintSet launcherPs{{{{{PromptIcon::PsDpadVertical, "Move"},
                                       {PromptIcon::PsCross, "Select"},
                                       {PromptIcon::PsTriangle, "Settings"},
                                       {PromptIcon::PsOptions, "Play"}},
                                      4}},
                                    1};
    static const HintSet launcherXb{{{{{PromptIcon::XbDpadVertical, "Move"},
                                       {PromptIcon::XbA, "Select"},
                                       {PromptIcon::XbY, "Settings"},
                                       {PromptIcon::XbMenu, "Play"}},
                                      4}},
                                    1};
    static const HintSet detailPs{
        {{{{PromptIcon::PsDpadVertical, "Move"}, {PromptIcon::PsDpadHorizontal, "Change setting"}}, 2},
         {{{PromptIcon::PsCross, "Select"}, {PromptIcon::PsCircle, "Cancel"}, {PromptIcon::PsOptions, "OK"}}, 3}},
        2};
    static const HintSet detailXb{
        {{{{PromptIcon::XbDpadVertical, "Move"}, {PromptIcon::XbDpadHorizontal, "Change setting"}}, 2},
         {{{PromptIcon::XbA, "Select"}, {PromptIcon::XbB, "Cancel"}, {PromptIcon::XbMenu, "OK"}}, 3}},
        2};
    static const HintSet launcherKb{{{{{PromptIcon::KbEnter, "Play"}, {PromptIcon::KbEscape, "Quit"}}, 2}}, 1};
    static const HintSet detailKb{{{{{PromptIcon::KbEnter, "OK"}, {PromptIcon::KbEscape, "Cancel"}}, 2}}, 1};
    bool launcher = kind == Kind::Launcher;
    switch (input) {
        case Input::PlayStation: return launcher ? launcherPs : detailPs;
        case Input::Xbox: return launcher ? launcherXb : detailXb;
        default: return launcher ? launcherKb : detailKb;
    }
}

HBITMAP g_icons[static_cast<int>(PromptIcon::Count)] = {};

HBITMAP IconBitmap(PromptIcon icon) {
    HBITMAP& bmp = g_icons[static_cast<int>(icon)];
    if (!bmp) {
        const int size = gen::kPromptIconSize;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = size;
        bi.bmiHeader.biHeight = -size;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp) memcpy(bits, gen::kPromptIcons[static_cast<int>(icon)], size * size * 4);
    }
    return bmp;
}

void DrawHint(const Nav& n, const DRAWITEMSTRUCT& di) {
    HDC dc = di.hDC;
    RECT rc = di.rcItem;
    FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
    const HintSet& set = Hints(n.kind, n.input);
    auto font = reinterpret_cast<HFONT>(SendMessageA(n.dlg, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    const int icon = gen::kPromptIconSize, gap = 4, spacing = 14, rowGap = 3;
    int y = rc.top + (rc.bottom - rc.top - (set.count * icon + (set.count - 1) * rowGap)) / 2;
    HDC mem = CreateCompatibleDC(dc);
    const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    for (int r = 0; r < set.count; ++r, y += icon + rowGap) {
        const HintRow& row = set.rows[r];
        SIZE text[4]{};
        int width = 0;
        for (int i = 0; i < row.count; ++i) {
            GetTextExtentPoint32A(dc, row.items[i].label, lstrlenA(row.items[i].label), &text[i]);
            width += icon + gap + text[i].cx + (i ? spacing : 0);
        }
        int x = n.kind == Kind::Launcher ? rc.left + (rc.right - rc.left - width) / 2 : rc.left;
        for (int i = 0; i < row.count; ++i) {
            HGDIOBJ prev = SelectObject(mem, IconBitmap(row.items[i].icon));
            AlphaBlend(dc, x, y, icon, icon, mem, 0, 0, icon, icon, blend);
            SelectObject(mem, prev);
            x += icon + gap;
            TextOutA(dc, x, y + (icon - text[i].cy) / 2, row.items[i].label, lstrlenA(row.items[i].label));
            x += text[i].cx + spacing;
        }
    }
    DeleteDC(mem);
    SelectObject(dc, oldFont);
}

void SetInput(Nav& n, Input input) {
    if (input == n.input) return;
    n.input = input;
    dslog::Write("Launcher: input -> %s", input == Input::Keyboard ? "keyboard/mouse" : input == Input::Xbox ? "Xbox" : "PlayStation");
    InvalidateRect(n.hint, nullptr, FALSE);
    if (input == Input::Keyboard && n.active) {  // mouse/keyboard took over: drop the pad highlight
        n.active = false;
        RedrawAround(n.dlg, n.focused);
        n.focused = nullptr;
    }
}

Input PadInput(gamepad::Type type) { return type == gamepad::Type::Xbox ? Input::Xbox : Input::PlayStation; }

DWORD LastKeyboardMouseInput() {
    LASTINPUTINFO li{sizeof li, 0};
    GetLastInputInfo(&li);
    return li.dwTime;
}

void Act(Nav& n, uint32_t pressed) {
    HWND dlg = n.dlg;
    HWND focus = GetFocus();
    if (focus && !IsChild(dlg, focus)) focus = nullptr;
    bool combo = focus && IsClass(focus, "ComboBox");

    if (!n.active) {
        n.active = true;
        SendMessageA(dlg, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);
        n.focused = nullptr;  // force a highlight redraw
    }
    if (pressed & gamepad::kUp) Move(dlg, focus, Dir::Up);
    if (pressed & gamepad::kDown) Move(dlg, focus, Dir::Down);
    if (pressed & gamepad::kLeft) combo ? ChangeCombo(dlg, focus, -1, false) : Move(dlg, focus, Dir::Left);
    if (pressed & gamepad::kRight) combo ? ChangeCombo(dlg, focus, +1, false) : Move(dlg, focus, Dir::Right);
    if ((pressed & gamepad::kAccept) && focus) {
        if (combo)
            ChangeCombo(dlg, focus, +1, true);
        else if (IsClass(focus, "Button"))
            Press(dlg, focus);
    }
    if (n.kind == Kind::Launcher) {
        if (pressed & gamepad::kStart) Click(dlg, kLauncherOk);
        if (pressed & gamepad::kAlt) Click(dlg, kLauncherSettings);
    } else {
        if (pressed & gamepad::kStart) Click(dlg, kDetailOk);
        if (pressed & gamepad::kBack) Click(dlg, kDetailCancel);
    }
}

void Tick(Nav& n) {
    gamepad::State s = gamepad::Poll();
    uint32_t pressed = s.buttons & ~n.prev;
    DWORD now = GetTickCount();
    uint32_t held = s.buttons & kDirections;
    if (pressed & kDirections) {
        n.repeatAt = now + kRepeatDelay;
    } else if (held && held == (n.prev & kDirections) && static_cast<LONG>(now - n.repeatAt) >= 0) {
        pressed |= held;  // auto-repeat while a direction stays held
        n.repeatAt = now + kRepeatRate;
    }
    if (s.buttons != n.prev) n.lastPadTime = now;
    n.prev = s.buttons;
    DWORD kbm = LastKeyboardMouseInput();
    bool kbmUsed = kbm != n.lastInputTime && now - n.lastPadTime > 250;  // (in case a pad also counts as input)
    n.lastInputTime = kbm;

    // Only the dialog the user is looking at reacts (the launcher is disabled while Settings is open). When a
    // dialog becomes current, the button that got us here (e.g. OK in Settings) may still be held.
    bool current = IsWindowEnabled(n.dlg) && GetForegroundWindow() == n.dlg;
    if (current != n.current) {
        n.current = current;
        n.waitRelease = true;
    }
    if (n.waitRelease) {
        if (s.buttons) pressed = 0;
        else n.waitRelease = false;
    }
    if (!current) return;
    if (pressed) {
        SetInput(n, PadInput(s.type));
        Act(n, pressed);
    } else if (kbmUsed) {
        SetInput(n, Input::Keyboard);
    }

    HWND focus = GetFocus();
    if (n.active && focus != n.focused) {
        HWND old = n.focused;
        n.focused = focus;
        RedrawAround(n.dlg, old);
        if (focus && IsChild(n.dlg, focus)) RedrawAround(n.dlg, focus);
    }
}

void Paint(Nav& n) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(n.dlg, &ps);
    HWND focus = GetFocus();
    if (focus && IsChild(n.dlg, focus) && IsWindowVisible(focus)) {
        RECT r = ChildRect(n.dlg, focus);
        InflateRect(&r, 3, 3);
        HBRUSH b = CreateSolidBrush(kHighlight);
        FrameRect(dc, &r, b);
        InflateRect(&r, -1, -1);
        FrameRect(dc, &r, b);
        DeleteObject(b);
    }
    EndPaint(n.dlg, &ps);
}
}  // namespace

void Attach(HWND dlg, Kind kind, int hintX, int hintY, int hintW, int hintH) {
    Nav* n = Find(nullptr);
    if (!n) return;
    if (g_openCount++ == 0) gamepad::Open(dlg);
    *n = Nav{};
    n->dlg = dlg;
    n->kind = kind;
    gamepad::State s = gamepad::Poll();
    n->prev = s.buttons;  // a button still held from the previous dialog must not fire here
    n->input = s.type == gamepad::Type::None ? Input::Keyboard : PadInput(s.type);
    n->lastInputTime = LastKeyboardMouseInput();

    // Hint line; grow the dialog if it lies below the client area.
    RECT r{hintX, hintY, hintX + hintW, hintY + hintH};
    MapDialogRect(dlg, &r);
    RECT client;
    GetClientRect(dlg, &client);
    if (r.bottom + 4 > client.bottom) {
        RECT wr;
        GetWindowRect(dlg, &wr);
        SetWindowPos(dlg, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top + r.bottom + 4 - client.bottom,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    n->hint = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE | WS_GROUP | SS_OWNERDRAW, r.left, r.top, r.right - r.left,
                              r.bottom - r.top, dlg, nullptr, GetModuleHandleA(nullptr), nullptr);
    SetTimer(dlg, kTimerId, kPollMs, nullptr);
}

bool OnMessage(HWND dlg, UINT msg, WPARAM wp, LPARAM lp, INT_PTR* result) {
    Nav* n = Find(dlg);
    if (!n) return false;
    switch (msg) {
        case WM_TIMER:
            if (wp != kTimerId) return false;
            Tick(*n);
            *result = TRUE;
            return true;
        case WM_DRAWITEM: {
            auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if (di->hwndItem != n->hint) return false;
            DrawHint(*n, *di);
            *result = TRUE;
            return true;
        }
        case WM_PAINT:
            if (!n->active) return false;
            Paint(*n);
            *result = TRUE;
            return true;
        case WM_DESTROY:
            KillTimer(dlg, kTimerId);
            *n = Nav{};
            if (--g_openCount == 0) gamepad::Close();
            return false;  // the game's proc still sees WM_DESTROY
        default:
            return false;
    }
}
}  // namespace launcherpad
