// Diagnostics: the game's own messages in our log (development builds) and a performance overlay (all builds).
//
// Game messages (dev builds, DesertStormFix.log):
// - FUN_004c0a80 is the game's debug print ("Parsing %s", "Done - %.2f seconds", ...), compiled to a bare `ret` in the
//   shipped exe; its 18 call sites are redirected to GameLog.
// - Error / warning boxes: FUN_004b98d0(fmt, ...) "Error" (MB_ABORTRETRYIGNORE, e.g. "Duplicate weapon - %s for Grunt
//   ID - %d", "loadUnitItems:..."), FUN_004b9850(hwnd, fmt, ...) "Error", FUN_004b9890(hwnd, fmt, ...) "Warning" -
//   all go to the log first; Dev\NoErrorBoxes = 1 skips the box (scripted tests would otherwise wait on it).
// - File opens: FUN_004bebd0(name, flags) = FUN_004bebf0(name, [0x63c530] search paths, flags) -> file or 0; every
//   name is logged once with found / NOT FOUND (archives answer by name hash).
//
// Performance overlay (setting PerfOverlay, F11 toggles it in game): FPS, frame time (last / 1 s average / max, 1 %
// low), a frame-time graph (hitches stand out in red), level, zone, resolution and each player's position. Frame
// times are the intervals between the frame-cap hook's calls (the whole frame: update, render, present, cap wait).
// Drawn with GDI into a texture ~10 times a second and put over the back buffer at EndScene (overlay.cpp).
// Position: soldier (input block +0x310) +0x30, float x/y/z in cm (y up). Zone: soldier +0xE4 -> zone object +0x210 =
// its id (what the game's own leftover debug line "Player on %s, %d lights" printed as 0x%X; not a name hash).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/framerec.h"
#include "features/overlay.h"

namespace {
// ---- game messages ----
#ifndef DS_DIST
constexpr uint32_t kDebugPrint = 0x4C0A80;
constexpr uint32_t kDebugPrintSites[] = {0x40B38A, 0x40B729, 0x40BE51, 0x40C1D1, 0x40C8C4, 0x40C91B,
                                         0x40CEAE, 0x40CEB8, 0x4363D1, 0x47581C, 0x4CD2C6, 0x4E3D84,
                                         0x4E3E09, 0x4E3F2A, 0x4F8990, 0x4F8D47, 0x4FAE02, 0x4FEC65};
#endif
constexpr uint32_t kErrorBox = 0x4B98D0, kErrorBoxWnd = 0x4B9850, kWarningBoxWnd = 0x4B9890;
constexpr uint32_t kOpenFile = 0x4BEBD0, kOpenFileIn = 0x4BEBF0, kSearchPaths = 0x63C530;

bool g_noErrorBoxes = false;

bool Readable(const void* p) {
    MEMORY_BASIC_INFORMATION mbi;
    return p && VirtualQuery(p, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
           !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

// The game's format strings come with the game's arguments; a bad pointer must not take the game down with it.
bool SafeFormat(char* out, size_t n, const char* fmt, va_list args) {
    if (!Readable(fmt)) return false;
    __try {
        vsnprintf(out, n, fmt, args);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Trim(char* s) {
    for (size_t n = strlen(s); n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '); --n) s[n - 1] = 0;
}

#ifndef DS_DIST
void __cdecl GameLog(const char* fmt, ...) {
    char text[1024];
    va_list args;
    va_start(args, fmt);
    const bool ok = SafeFormat(text, sizeof text, fmt, args);
    va_end(args);
    if (!ok) return;
    Trim(text);
    if (text[0]) dslog::Write("[game] %s", text);
}

#endif

void Box(HWND wnd, const char* caption, UINT type, const char* fmt, va_list args) {
    char text[4096];
    if (!SafeFormat(text, sizeof text, fmt, args)) strcpy_s(text, "(unformattable message)");
    Trim(text);
    // Our loadouts keep a kit's item count with 0-magazine copies of the pistol (loadout.cpp); the game refuses the
    // copies as duplicates (they stay unowned, like an item a script took away) - no box for those.
    const bool ours = features::LoadoutUsesFillers() && strncmp(text, "Duplicate weapon - ", 19) == 0;
    const bool skip = g_noErrorBoxes || ours;
    dslog::Write("[game %s] %s%s", caption, text, skip ? "  (box skipped)" : "");
    if (!skip) MessageBoxA(wnd, text, caption, type);
}
void __cdecl ErrorBox(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    Box(GetActiveWindow(), "Error", MB_ABORTRETRYIGNORE | MB_ICONERROR, fmt, a);
    va_end(a);
}
void __cdecl ErrorBoxWnd(HWND wnd, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    Box(wnd, "Error", MB_ABORTRETRYIGNORE | MB_ICONERROR, fmt, a);
    va_end(a);
}
void __cdecl WarningBoxWnd(HWND wnd, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    Box(wnd, "Warning", MB_ICONWARNING, fmt, a);
    va_end(a);
}

// Every build: file opens are timed for the frame recorder; development builds also log each name once.
void* __cdecl GameOpenFile(const char* name, int flags) {
    using Fn = void*(__cdecl*)(const char*, int, int);
    const double t0 = framerec::Now();
    void* f = reinterpret_cast<Fn>(kOpenFileIn)(name, *reinterpret_cast<int*>(kSearchPaths), flags);
    if (!Readable(name)) return f;
    framerec::AddFile(name, framerec::Now() - t0, f != nullptr);
#ifndef DS_DIST
    static std::unordered_set<std::string> seen;
    if (seen.insert(name).second) dslog::Write("[file] %s%s", name, f ? "" : "  - NOT FOUND");
#endif
    return f;
}

#ifndef DS_DIST

DWORD DevDword(const char* name) {
    DWORD v = 0, size = sizeof v;
    RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", name, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}
#endif

// ---- performance overlay ----
constexpr uint32_t kRenderer = 0x63C924;   // W +0x40688, H +0x4068c
constexpr uint32_t kGameWindow = 0x606A60;
constexpr uint32_t kLevelName = 0x606880;  // "Mission4.env"
constexpr uint32_t kBlocks = 0x60F5B8, kBlockSize = 0x478, kBlockSoldier = 0x310;
constexpr int kTexW = 1024, kTexH = 512;
constexpr int kHistory = 240;   // frames in the graph
constexpr double kRedrawSeconds = 0.1;

LARGE_INTEGER g_freq;
LONGLONG g_last = 0;
float g_times[kHistory] = {};  // ms, ring
int g_head = 0, g_count = 0;
bool g_on = false, g_keyDown = false;
LONGLONG g_lastDraw = 0;
void* g_tex = nullptr;
void* g_texDevice = nullptr;
int g_w = 0, g_h = 0;  // content size in the texture
std::vector<uint32_t> g_pixels;

template <class T>
T Read(uintptr_t a) { return *reinterpret_cast<const T*>(a); }

struct Stats {
    float last = 0, avg = 0, max = 0, low1 = 0;  // ms; low1 = the 99th percentile frame time
    int fps = 0;
};

Stats Measure() {
    Stats s;
    if (!g_count) return s;
    s.last = g_times[(g_head + kHistory - 1) % kHistory];
    // FPS over the last second of frames (at most the history)
    float sum = 0;
    std::vector<float> window;
    for (int i = 1; i <= g_count && sum < 1000.0f; ++i) {
        const float t = g_times[(g_head + kHistory - i) % kHistory];
        sum += t;
        window.push_back(t);
    }
    s.avg = sum / window.size();
    s.max = *std::max_element(window.begin(), window.end());
    s.fps = sum > 0 ? static_cast<int>(window.size() * 1000.0f / sum + 0.5f) : 0;
    std::sort(window.begin(), window.end());
    s.low1 = window[std::min(window.size() - 1, window.size() * 99 / 100)];
    return s;
}

COLORREF FrameColour(float ms, float avg) {
    if (ms > avg * 2.5f && ms > 20.0f) return RGB(255, 70, 60);   // hitch
    if (ms > avg * 1.5f && ms > 10.0f) return RGB(255, 200, 60);  // uneven
    return RGB(90, 220, 110);
}

void Draw(int screenH, bool full) {
    const int px = std::clamp(screenH / 60, 12, 28);  // text height
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), kTexW, -kTexH, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        DeleteDC(dc);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HFONT font = CreateFontA(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    memset(bits, 0, size_t(kTexW) * kTexH * 4);

    const Stats s = Measure();
    const int pad = px / 2, lineH = px + px / 4;
    int y = pad, w = 0;
    auto line = [&](COLORREF c, const char* fmt, auto... args) {
        char t[160];
        snprintf(t, sizeof t, fmt, args...);
        SetTextColor(dc, c);
        TextOutA(dc, pad, y, t, static_cast<int>(strlen(t)));
        SIZE sz;
        GetTextExtentPoint32A(dc, t, static_cast<int>(strlen(t)), &sz);
        w = std::max(w, static_cast<int>(sz.cx));
        y += lineH;
    };
    if (framerec::Recording()) {
        line(RGB(255, 80, 70), "REC  frame recording (Ctrl+F11 stops)");
    }
    if (!full) {
        g_w = std::min(kTexW, w + pad * 2);
        g_h = std::min(kTexH, y + pad - lineH + px);
    }
    if (full) {
    line(RGB(255, 255, 255), "%3d FPS  %6.2f ms   avg %.2f  max %.1f  1%% low %d FPS", s.fps, s.last, s.avg, s.max,
         s.low1 > 0 ? static_cast<int>(1000.0f / s.low1 + 0.5f) : 0);

    // frame-time graph: one bar per frame, newest on the right; guide lines at 16.7 and 33.3 ms
    const int gw = std::max(w, px * 20), gh = px * 3, gx = pad, gy = y + px / 4;
    const float top = std::max(40.0f, s.max * 1.1f);
    HPEN guide = CreatePen(PS_DOT, 1, RGB(110, 110, 110));
    HGDIOBJ oldPen = SelectObject(dc, guide);
    for (float g : {16.667f, 33.333f}) {
        const int ly = gy + gh - static_cast<int>(g / top * gh);
        MoveToEx(dc, gx, ly, nullptr);
        LineTo(dc, gx + gw, ly);
    }
    SelectObject(dc, oldPen);
    DeleteObject(guide);
    const int bars = std::min(g_count, kHistory);
    const float bw = static_cast<float>(gw) / kHistory;
    for (int i = 0; i < bars; ++i) {
        const float t = g_times[(g_head + kHistory - bars + i) % kHistory];
        const int bh = std::max(1, static_cast<int>(std::min(t, top) / top * gh));
        const int x0 = gx + static_cast<int>((kHistory - bars + i) * bw), x1 = std::max(x0 + 1, gx + static_cast<int>((kHistory - bars + i + 1) * bw));
        RECT r{x0, gy + gh - bh, x1, gy + gh};
        HBRUSH b = CreateSolidBrush(FrameColour(t, s.avg));
        FillRect(dc, &r, b);
        DeleteObject(b);
    }
    y = gy + gh + px / 2;
    w = std::max(w, gw);

    char level[32] = {};
    memcpy(level, reinterpret_cast<const char*>(kLevelName), sizeof level - 1);
    const auto* r = Read<const uint8_t*>(kRenderer);
    const int W = r ? Read<int>(uintptr_t(r) + 0x40688) : 0, H = r ? Read<int>(uintptr_t(r) + 0x4068C) : 0;
    const bool frontEnd = _strnicmp(level, "FrontEnd", 8) == 0 || !level[0];
    line(RGB(200, 200, 200), "%s  %dx%d  %s", level[0] ? level : "-", W, H, frontEnd ? "menus" : "");
    if (!frontEnd) {
        const int players = std::max(1, features::SplitScreenPlayers());
        for (int p = 0; p < players && p < 4; ++p) {
            const auto soldier = Read<uintptr_t>(kBlocks + p * kBlockSize + kBlockSoldier);
            if (!soldier) continue;
            const float* pos = reinterpret_cast<const float*>(soldier + 0x30);
            const auto zone = Read<uintptr_t>(soldier + 0xE4);
            const uint32_t zoneId = zone ? Read<uint32_t>(zone + 0x210) : 0;
            line(RGB(200, 200, 200), "P%d  %8.1f %7.1f %8.1f m  zone %08X", p + 1, pos[0] / 100, pos[1] / 100,
                 pos[2] / 100, zoneId);
        }
    }
    g_w = std::min(kTexW, w + pad * 2);
    g_h = std::min(kTexH, y + pad - lineH + px);
    }

    // GDI leaves alpha 0: panel = translucent black, text / bars opaque by their brightness
    auto* px32 = static_cast<uint32_t*>(bits);
    g_pixels.assign(size_t(g_w) * g_h, 0);
    for (int yy = 0; yy < g_h; ++yy)
        for (int xx = 0; xx < g_w; ++xx) {
            const uint32_t c = px32[size_t(yy) * kTexW + xx] & 0xFFFFFF;
            const uint32_t lum = std::max({c >> 16 & 255, c >> 8 & 255, c & 255});
            const uint32_t a = 0xA0 + (0xFF - 0xA0) * lum / 255;
            g_pixels[size_t(yy) * g_w + xx] = a << 24 | c;
        }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}
}  // namespace

void features::ApplyDiagnostics() {
    QueryPerformanceFrequency(&g_freq);
    g_on = settings::Get().perfOverlay;
    static bool hooked = false;
    if (hooked) return;
    hooked = true;
    // Error / warning boxes in every build: the same boxes, plus the log and the loadout exception above.
    static const uint8_t boxEntry[] = {0x8B, 0x4C, 0x24, 0x04, 0x8D, 0x44, 0x24, 0x08};
    static const uint8_t wndEntry[] = {0x8B, 0x4C, 0x24, 0x08, 0x8D, 0x44, 0x24, 0x0C};
    const bool boxes = patch::Matches(kErrorBox, boxEntry, sizeof boxEntry) &&
                       patch::Matches(kErrorBoxWnd, wndEntry, sizeof wndEntry) &&
                       patch::Matches(kWarningBoxWnd, wndEntry, sizeof wndEntry) &&
                       patch::WriteJump(kErrorBox, reinterpret_cast<const void*>(&ErrorBox)) &&
                       patch::WriteJump(kErrorBoxWnd, reinterpret_cast<const void*>(&ErrorBoxWnd)) &&
                       patch::WriteJump(kWarningBoxWnd, reinterpret_cast<const void*>(&WarningBoxWnd));
    static const uint8_t openEntry[] = {0x8B, 0x44, 0x24, 0x08, 0x8B, 0x0D, 0x30, 0xC5, 0x63, 0x00};
    const bool files = patch::Matches(kOpenFile, openEntry, sizeof openEntry) &&
                       patch::WriteJump(kOpenFile, reinterpret_cast<const void*>(&GameOpenFile));
    framerec::ApplyHooks();
#ifdef DS_DIST
    (void)boxes, (void)files;
#else
    g_noErrorBoxes = DevDword("NoErrorBoxes") != 0;
    int sites = 0;
    for (uint32_t s : kDebugPrintSites) sites += patch::HookCall(s, reinterpret_cast<const void*>(&GameLog), kDebugPrint);
    dslog::Write("[dev]  Diagnostics: game debug print %d/%d sites, error boxes %s%s, file opens %s", sites,
                 static_cast<int>(std::size(kDebugPrintSites)), boxes ? "logged" : "FAILED",
                 g_noErrorBoxes ? " (boxes skipped)" : "", files ? "logged" : "FAILED");
#endif
}

void features::OnFrameDiagnostics() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_last) {
        g_times[g_head] = static_cast<float>(double(now.QuadPart - g_last) * 1000.0 / double(g_freq.QuadPart));
        g_head = (g_head + 1) % kHistory;
        g_count = std::min(g_count + 1, kHistory);
    }
    g_last = now.QuadPart;
#ifndef DS_DIST
    // Dev\FrameLog = 1: in a mission, 480 frames of real frame time vs the game's frame time (main timer [0x63C954]
    // +0xA8, seconds - read before this frame's update, so it is the previous frame's value).
    static const bool frameLog = DevDword("FrameLog") != 0;
    static int logged = -1;
    static float real[480], game[480], game2[480];
    static int int1[480], int2[480];
    if (frameLog && logged < 480) {
        char level[16] = {};
        memcpy(level, reinterpret_cast<const char*>(kLevelName), sizeof level - 1);
        const auto timer = Read<uintptr_t>(0x63C954);
        const bool mission = level[0] && _strnicmp(level, "FrontEnd", 8) != 0 && timer;
        static LONGLONG since = 0;
        if (!mission) since = 0, logged = -1;
        else if (!since) since = now.QuadPart;
        else if (logged < 0 && double(now.QuadPart - since) / double(g_freq.QuadPart) > 5.0) logged = 0;
        else if (logged >= 0) {
            real[logged] = g_times[(g_head + kHistory - 1) % kHistory];
            const auto timer2 = Read<uintptr_t>(0x63C958);
            game[logged] = Read<float>(timer + 0xA8) * 1000.0f;
            game2[logged] = timer2 ? Read<float>(timer2 + 0xA8) * 1000.0f : -1.0f;
            int1[logged] = Read<int>(timer + 0x14), int2[logged] = timer2 ? Read<int>(timer2 + 0x14) : -1;
            if (++logged == 480) {
                double sr = 0, sg = 0, dr = 0, dg = 0;
                for (int i = 1; i < 480; ++i) {
                    sr += real[i - 1], sg += game[i];
                    dr += std::abs(real[i] - real[i - 1]), dg += std::abs(game[i] - game[i - 1]);
                }
                dslog::Write("[dev]  FrameLog: real avg %.3f ms (mean frame-to-frame change %.3f), game avg %.3f ms "
                             "(change %.3f)", sr / 479, dr / 479, sg / 479, dg / 479);
                for (int i = 1; i < 480; i += 8) {
                    char t[1024];
                    int n = 0;
                    for (int k = i; k < i + 8 && k < 480; ++k)
                        n += snprintf(t + n, sizeof t - n, " %5.2f/%5.2f %d/%5.2f %d", real[k - 1], game[k], int1[k],
                                      game2[k], int2[k]);
                    dslog::Write("[dev]  FrameLog real/954 f,i/958 f,i:%s", t);
                }
            }
        }
    }
#endif

#ifndef DS_DIST
    static bool autoRecord = DevDword("FrameRecord") != 0;  // Dev\FrameRecord = 1: record from the first frame
    if (autoRecord && overlay::CurrentDevice()) {
        autoRecord = false;
        if (!framerec::Recording()) framerec::Toggle();
    }
#endif
    // F11 toggles the overlay, Ctrl+F11 the frame recording (only while the game window is in front)
    const HWND wnd = Read<HWND>(kGameWindow);
    const bool down = wnd && GetForegroundWindow() == wnd && (GetAsyncKeyState(VK_F11) & 0x8000);
    if (down && !g_keyDown) {
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) framerec::Toggle();
        else g_on = !g_on;
        g_lastDraw = 0;
    }
    g_keyDown = down;
    const bool rec = framerec::Recording();
    if (!g_on && !rec) return;

    void* dev = overlay::CurrentDevice();
    if (!dev) return;
    if (dev != g_texDevice) {  // first use or a new device (managed textures survive Reset)
        g_tex = overlay::CreateTexture(kTexW, kTexH);
        g_texDevice = g_tex ? dev : nullptr;
        g_lastDraw = 0;
        if (!g_tex) return;
    }
    if (!g_lastDraw || double(now.QuadPart - g_lastDraw) / double(g_freq.QuadPart) >= kRedrawSeconds) {
        g_lastDraw = now.QuadPart;
        const auto* r = Read<const uint8_t*>(kRenderer);
        const double t0 = framerec::Now();
        Draw(r ? Read<int>(uintptr_t(r) + 0x4068C) : 720, g_on);
        if (g_w > 0 && g_h > 0) overlay::UploadTexture(g_tex, g_pixels.data(), g_w, g_h);
        framerec::AddOverlay(framerec::Now() - t0);
    }
    if (g_w > 0 && g_h > 0) {
        const float m = 8.0f;
        overlay::QueueScreenImage(g_tex, m, m, static_cast<float>(g_w), static_cast<float>(g_h),
                                  static_cast<float>(g_w) / kTexW, static_cast<float>(g_h) / kTexH);
    }
}
