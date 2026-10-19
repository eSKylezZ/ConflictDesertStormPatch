// The PSE asset store in Conflict: Desert Storm (docs/sdk/asset-store.md, DESIGN.md §12 stage 1).
// Player setting: plugin dsfix.assetstore, "SharedAssetStore" (SHARED ASSET STORE, default OFF for now, restart) +
// "RebuildCache" (REBUILD CACHE AT NEXT START, switches itself off) - OPTIONS > DESERTSTORMFIX > PLUGINS. Dev builds:
// Dev\PseStore = 1 / 2 overrides it (below).
//
//   - pse_build (pse/build) builds / checks the content-addressed store from the player's own archives AND level sound
//     caches at start-up (<game>\PSE\Cache\assets, or %LOCALAPPDATA%\PSE\ds1-<hash>\assets): one pack + a manifest per
//     archive / sound cache. A build (first start, changed game files, REBUILD CACHE) runs on a worker thread behind a
//     small progress window (title, bar, current file, Cancel; Esc / pad Circle-B / Cross-A on Cancel) - before the
//     launcher. Cancel = store off for this run (asked again next start); a failure = off + log + mod report line.
//   - Sound: mods.cpp builds every level cache it hands the game (SoundCache) from the store's chunks and adds banks
//     from other levels (BorrowBank / BorrowKitBanks / FindBank) as store bank packs - no scanning / copying of .sch
//     files (features::AssetStoreSound*).
//   - The store is a pse.vfs provider ("ds1.store", pse/include/pse/pse_vfs.h): name (or name hash) + context archive ->
//     the bytes in store.pack.
//   - The game asks pse.vfs before its own archives: the current-archive lookup FUN_004be360 (used by the resolver
//     FUN_004be810 in archive mode and by the exists check FUN_004bfc80) and the opener FUN_004bd8a0 (archive branch:
//     FUN_004bd5c0 on the CURRENT slot [0x63B4CC]) are entry-hooked. A served file is opened through the opener's own
//     disk branch on store.pack, then the file object's base / size (+0x114 / +0x120) are set to the blob - its
//     read FUN_004bdd10 seeks to +0x114 + position, its size FUN_004bdc50 returns +0x120.
//   - Mods keep priority: the mod resolver (mods.cpp, entry hook of FUN_004be810) hands out Mods\ paths before any
//     archive lookup, and paths with a drive (absolute) are never ours.
//
// Search order for a file the game wants from archive X (slot objects 0x6242D8 + slot * 0x7AF8, +0 = path; slots
// 0 catalog.dat, 1 the level's archive, 2 chardata.dat; the game only ever looks in the CURRENT slot):
//   1. X's own manifest                                  (= the game's own answer, byte for byte)
//   2. Dev\PseStore = 1 only, the global fallback: the level archive (slot 1), catalog.dat, chardata.dat, then every
//      other archive's manifest in name order - with one exception: a texture name (.ddl/.dds/.png/.bmp/.tga) is not
//      fetched from elsewhere when X has the same name in another texture format (the game's loader tries the formats
//      in that order, and X's own .tga must still win over another level's .dds). Each fetch from elsewhere is logged.
// Dev\PseStore = 2: step 1 only (the store replaces the archive reads, no fallback).
// With the fallback on (the player setting, Dev\PseStore = 1) mods.cpp's Borrow* paths copy nothing: BorrowLevelFile /
// BorrowNamedFiles (vehicle swaps, .unit soldiers) are skipped - the store serves those files. Dev\PseNoBorrow = 1
// forces the same with PseStore = 2 (the control run: then they are simply missing).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include "core/gamepad.h"
#include "core/log.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "features/features.h"
#include "loader.h"
#include "pse/pse_vfs.h"
#include "pse_build.h"

namespace {

constexpr uint32_t kArchiveLookup = 0x4BE360;   // int cdecl(name): the name is in the CURRENT archive slot
constexpr uint8_t kArchiveLookupEntry[] = {0x8B, 0x44, 0x24, 0x04, 0x8D, 0x94, 0x24, 0xFC, 0xFE, 0xFF, 0xFF};
constexpr uint32_t kOpen = 0x4BD8A0;            // thiscall(file, path) ret 4
constexpr uint8_t kOpenEntry[] = {0x8B, 0x44, 0x24, 0x04, 0x81, 0xEC, 0x08, 0x02, 0x00, 0x00};
constexpr uint32_t kFileFlags = 0x63B4C8;       // bit 0 = archive mode
constexpr uint32_t kCurrentSlot = 0x63B4CC;     // archive slot object the opener / lookups use
constexpr uint32_t kSlots = 0x6242D8, kSlotSize = 0x7AF8;   // +0 = the archive's path
constexpr uint32_t kSlotNames = 0x5F7E94;       // default names: catalog.dat, mission.dat, chardata.dat
constexpr int32_t kPriority = 100;              // before mods.cpp's opener subscriber (plain paths only)

using LookupFn = int(__cdecl*)(const char*);
using OpenFn = int(__thiscall*)(void*, const char*);
LookupFn g_lookup = nullptr;
OpenFn g_open = nullptr;

pse::build::Store g_store;
bool g_on = false, g_fallback = false, g_noBorrow = false;
const PSEVfsInterface* g_vfs = nullptr;
PSEPluginHandle g_self = 0;
std::string g_packUtf8, g_packAnsi;   // store.pack for the provider (UTF-8) and the game's fopen (ANSI)
std::vector<std::string> g_fallbackOrder;   // other manifests, name order
std::mutex g_logMutex;
std::set<std::pair<std::string, uint32_t>> g_logged;
uint32_t g_served = 0, g_borrowed = 0;

#ifndef DS_DIST
DWORD DevDword(const char* name) {
    DWORD v = 0, size = sizeof v;
    RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", name, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}
#endif

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Base(const char* path) {
    const char* b = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') b = p + 1;
    return Lower(b);
}

bool CopySlotPath(const uint8_t* slot, char* out, size_t len) {   // (no C++ objects: __try)
    __try {
        memcpy(out, slot, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The archive of slot object `slot` (its path at +0; an empty path = the slot's default name, table 0x5F7E94).
std::string SlotArchive(const uint8_t* slot) {
    if (!slot) return {};
    char path[0x104];
    if (!CopySlotPath(slot, path, sizeof path)) return {};
    path[sizeof path - 1] = 0;
    if (!path[0]) {
        const uintptr_t index = (reinterpret_cast<uintptr_t>(slot) - kSlots) / kSlotSize;
        if (reinterpret_cast<uintptr_t>(slot) >= kSlots && index < 3)
            return Base(*reinterpret_cast<const char* const*>(kSlotNames + index * 4));
        return {};
    }
    return Base(path);
}
std::string CurrentArchive() { return SlotArchive(*reinterpret_cast<uint8_t**>(kCurrentSlot)); }
std::string LevelArchive() { return SlotArchive(reinterpret_cast<uint8_t*>(kSlots + 1 * kSlotSize)); }

bool IsTexture(const std::string& ext) {
    return ext == "ddl" || ext == "dds" || ext == "png" || ext == "bmp" || ext == "tga";
}

// X has `name` (a texture) in another texture format: its own copy wins, no fallback.
bool OwnTextureSibling(const pse::build::Store::Manifest* m, const char* name) {
    if (!m || !name) return false;
    std::string base = Base(name);
    const size_t dot = base.rfind('.');
    if (dot == std::string::npos || !IsTexture(base.substr(dot + 1))) return false;
    const std::string stem = base.substr(0, dot);
    for (const char* ext : {"ddl", "dds", "png", "bmp", "tga"})
        if (m->byHash.count(pse::build::Ds1NameHash((stem + "." + ext).c_str()))) return true;
    return false;
}

void Fill(PSEVfsLocation* out, uint32_t blob, const std::string& archive) {
    const pse::build::Blob& b = g_store.Blobs()[blob];
    strncpy_s(out->path, g_packUtf8.c_str(), _TRUNCATE);
    out->offset = b.offset;
    out->length = b.size;
    strncpy_s(out->source, archive.c_str(), _TRUNCATE);
}

// pse.vfs provider "ds1.store".
int PSE_CALL Locate(const PSEVfsQuery* q, PSEVfsLocation* out, void*) {
    if (!g_store.IsOpen()) return 0;
    const std::string context = q->context ? Base(q->context) : CurrentArchive();
    const pse::build::Store::Manifest* own = g_store.Find(context);
    if (own) {
        auto it = own->byHash.find(q->nameHash);
        if (it != own->byHash.end()) {
            Fill(out, it->second, own->archive);
            return 1;
        }
    }
    if (!g_fallback || (q->flags & PSE_VFS_EXACT) || OwnTextureSibling(own, q->name)) return 0;
    std::vector<std::string> order;
    const std::string level = LevelArchive();
    if (!level.empty() && level != context) order.push_back(level);
    for (const char* g : {"catalog.dat", "chardata.dat"})
        if (context != g) order.push_back(g);
    for (const std::string& a : g_fallbackOrder)
        if (a != context && std::find(order.begin(), order.end(), a) == order.end()) order.push_back(a);
    for (const std::string& a : order) {
        const int32_t blob = g_store.Lookup(a, q->nameHash);
        if (blob < 0) continue;
        Fill(out, static_cast<uint32_t>(blob), a);
        {
            std::lock_guard lock(g_logMutex);
            if (g_logged.insert({context, q->nameHash}).second) {
                ++g_borrowed;
                const std::string name = q->name ? q->name : g_store.NameOf(q->nameHash);
                dslog::Write("Asset store: %s (%08X) for %s from %s (global fallback)",
                             name.empty() ? "?" : name.c_str(), q->nameHash, context.c_str(), a.c_str());
            }
        }
        return 1;
    }
    return 0;
}

uint32_t PSE_CALL NameHash(const char* name) { return pse::build::Ds1NameHash(name); }

bool OurName(const char* name) {
    return g_on && g_vfs && name && *name && !strchr(name, ':') && (*reinterpret_cast<uint32_t*>(kFileFlags) & 1);
}

// FUN_004be360: "is `name` in the current archive" - pse.vfs first.
int __cdecl ArchiveLookup(const char* name) {
    if (OurName(name)) {
        PSEVfsQuery q{sizeof q, name, 0, nullptr, 0};
        if (g_vfs->Exists(g_self, &q)) return 1;
    }
    return g_lookup(name);
}

// FUN_004bd8a0: a file pse.vfs serves is opened on its disk file (the opener's disk branch) at its offset.
int __fastcall Open(uint8_t* file, void*, const char* path) {
    if (OurName(path)) {
        PSEVfsQuery q{sizeof q, path, 0, nullptr, 0};
        PSEVfsLocation loc{};
        loc.size = sizeof loc;
        if (g_vfs->ResolvePath(g_self, &q, &loc) == PSE_OK && loc.path[0] && loc.offset + loc.length < 0x7FFFFFFFull) {
            const std::string& disk = loc.path == g_packUtf8 ? g_packAnsi : std::string(loc.path);
            auto* flags = reinterpret_cast<uint32_t*>(kFileFlags);
            const uint32_t saved = *flags;
            *flags &= ~1u;   // the disk branch (fopen "rb")
            const int r = g_open(file, disk.c_str());
            *flags = saved | (*flags & ~saved & ~1u);
            if (r) {
                *reinterpret_cast<uint32_t*>(file + 0x114) = static_cast<uint32_t>(loc.offset);   // base
                *reinterpret_cast<uint32_t*>(file + 0x120) = loc.length;                         // size
                *reinterpret_cast<uint32_t*>(file + 0x11C) = 0;                                  // position
                if (++g_served % 250 == 0)
                    dslog::Write("Asset store: %u files served from the store so far (last: %s from %s)", g_served, path,
                                 loc.source);
                return r;
            }
            dslog::Write("[fail] Asset store: can't open %s for %s - the game's archive is used", disk.c_str(), path);
        }
    }
    return g_open(file, path);
}

std::string AnsiPath(const std::wstring& w) {
    BOOL lossy = FALSE;
    const int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), -1, nullptr, 0, nullptr, &lossy);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), -1, s.data(), n, nullptr, &lossy);
    if (!lossy) return s;
    wchar_t shortPath[MAX_PATH];
    if (GetShortPathNameW(w.c_str(), shortPath, MAX_PATH)) return pse::build::Narrow(shortPath);
    return s;
}

std::wstring GameDir() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    return dir.substr(0, dir.find_last_of(L"\\/"));
}

// ---- player settings (plugin dsfix.assetstore, shown in OPTIONS > DESERTSTORMFIX > PLUGINS) ----
constexpr const char* kPlugin = "dsfix.assetstore";
constexpr const char* kSetOn = "SharedAssetStore";
constexpr const char* kSetRebuild = "RebuildCache";

PSEPluginHandle Self() {
    static PSEPluginHandle h = pse_host::Find(kPlugin);
    return h;
}

void DeclareSettings() {
    static bool done = false;
    const PSESettingsInterface* set = pse::SettingsInterface();
    if (done || !set || !Self()) return;
    done = true;
    PSESettingDesc d{};
    d.size = sizeof d;
    d.key = kSetOn;
    d.type = PSE_SETTING_BOOL;
    d.flags = 0;   // restart
    d.defaultInt = 0;   // OFF until the user decides (docs/sdk/asset-store.md "Before it is on by default")
    d.minInt = 0;
    d.maxInt = 1;
    d.label = "SHARED ASSET STORE";
    d.help = "one copy of the game's files and sounds for every level (PSE\\Cache, ~350 MB, built at the next start)";
    if (set->Declare(Self(), &d) != PSE_OK) dslog::Write("[fail] Asset store: setting %s not declared", kSetOn);
    d.key = kSetRebuild;
    d.label = "REBUILD CACHE AT NEXT START";
    d.help = "builds the shared asset store again from the game files at the next start, then switches itself off";
    if (set->Declare(Self(), &d) != PSE_OK) dslog::Write("[fail] Asset store: setting %s not declared", kSetRebuild);
}

int32_t Setting(const char* key) {
    const PSESettingsInterface* set = pse::SettingsInterface();
    return set && Self() ? set->GetInt(Self(), key) : 0;
}

bool BackgroundRun() {   // dev test harness (tools/bg.py): windows stay off-screen and are never activated
#ifndef DS_DIST
    return DevDword("RunInBackground") != 0;
#else
    return false;
#endif
}

// ---- the first-boot progress window ----
// A plain Win32 window (the game has none yet: this runs at the exe's entry point, before the launcher). The build
// runs on a worker thread; this thread pumps messages, polls the pads (core/gamepad) and paints.
struct BuildJob {
    pse::build::StoreOptions options;
    pse::build::StoreStats stats;
    std::string error;
    bool ok = false;
    std::atomic<uint32_t> permille{0};
    std::atomic<bool> cancel{false};
    std::mutex textMutex;
    std::string current;
};

constexpr wchar_t kWindowClass[] = L"PSEAssetStoreBuild";
constexpr int kCancelId = IDCANCEL;
BuildJob* g_job = nullptr;
HFONT g_font = nullptr, g_bold = nullptr;

void RequestCancel(HWND hwnd) {
    if (!g_job || g_job->cancel.exchange(true)) return;
    dslog::Write("Asset store: build cancelled by the player");
    EnableWindow(GetDlgItem(hwnd, kCancelId), FALSE);
    SetWindowTextW(GetDlgItem(hwnd, kCancelId), L"Cancelling...");
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Draws the client area (not the Cancel button) into `target` - through a memory bitmap, so it doesn't flicker.
void Paint(HWND hwnd, HDC target) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    const int w = rc.right;
    HGDIOBJ old = SelectObject(dc, g_bold);
    RECT t = {16, 14, w - 16, 36};
    DrawTextW(dc, L"Preparing the game data (first start only)", -1, &t, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, g_font);
    t = {16, 38, w - 16, 74};
    DrawTextW(dc,
              L"The shared asset store is built once from your own game files "
              L"(PSE\\Cache, about 350 MB). Cancel starts the game without it.",
              -1, &t, DT_LEFT | DT_WORDBREAK);
    // the bar
    const uint32_t pm = g_job ? g_job->permille.load() : 0;
    RECT bar = {16, 82, w - 16, 102};
    FrameRect(dc, &bar, GetSysColorBrush(COLOR_BTNSHADOW));
    RECT fill = {bar.left + 2, bar.top + 2, bar.left + 2 + static_cast<LONG>((bar.right - bar.left - 4) * pm / 1000), bar.bottom - 2};
    HBRUSH green = CreateSolidBrush(RGB(0x3C, 0x8C, 0x2E));
    if (fill.right > fill.left) FillRect(dc, &fill, green);
    DeleteObject(green);
    std::string cur;
    if (g_job) {
        std::lock_guard lock(g_job->textMutex);
        cur = g_job->current;
    }
    wchar_t line[200];
    if (g_job && g_job->cancel.load())
        swprintf_s(line, L"Cancelling...");
    else
        swprintf_s(line, L"%S  -  %u %%", cur.empty() ? "starting" : cur.c_str(), pm / 10);
    t = {16, 108, w - 16, 128};
    DrawTextW(dc, line, -1, &t, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    t = {16, 140, w - 120, 162};
    DrawTextW(dc, L"Esc / pad Circle or B: Cancel", -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, old);
    // the Cancel button's area is the button's own (WS_CLIPCHILDREN on a real paint; PrintWindow draws it after us)
    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            Paint(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:   // PrintWindow (test screenshots)
            Paint(hwnd, reinterpret_cast<HDC>(wp));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_COMMAND:
            if (LOWORD(wp) == kCancelId) RequestCancel(hwnd);
            return 0;
        case WM_CLOSE:
            RequestCancel(hwnd);
            return 0;
        case WM_TIMER: {
            static uint32_t held = ~0u;   // pad buttons already down when the window opened don't count
            const gamepad::State s = gamepad::Poll();
            const uint32_t pressed = s.buttons & ~held;
            held = s.buttons;
            if (pressed & (gamepad::kBack | gamepad::kAccept)) RequestCancel(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI BuildThread(void* p) {
    auto* job = static_cast<BuildJob*>(p);
    job->ok = pse::build::BuildStore(job->options, &job->stats, &job->error);
    return 0;
}

// Runs the build behind the progress window. Returns when it ended (built, failed or cancelled).
void BuildWithWindow(BuildJob& job) {
    job.options.progress = [&job](uint32_t permille, const std::string& what) {
        job.permille = permille;
        std::lock_guard lock(job.textMutex);
        job.current = what;
    };
    job.options.cancelled = [&job] { return job.cancel.load(); };
    g_job = &job;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = &WindowProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));   // the game's icon if it has one under that id
    wc.lpszClassName = kWindowClass;
    RegisterClassW(&wc);
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_BOLD;
    g_bold = CreateFontIndirectW(&bold);
    RECT r = {0, 0, 480, 176};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    AdjustWindowRect(&r, style, FALSE);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    const bool bg = BackgroundRun();
    const int x = bg ? -8000 : (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, y = bg ? -8000 : (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    HWND hwnd = CreateWindowExW(bg ? WS_EX_NOACTIVATE : WS_EX_APPWINDOW, kWindowClass,
                                L"Conflict: Desert Storm - preparing game data", style, x, y, ww, wh, nullptr, nullptr,
                                inst, nullptr);
    HANDLE thread = CreateThread(nullptr, 0, &BuildThread, &job, 0, nullptr);
    if (!thread) {   // no thread: build right here, without the window
        if (hwnd) DestroyWindow(hwnd);
        BuildThread(&job);
        g_job = nullptr;
        return;
    }
    if (hwnd) {
        HWND button = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                      480 - 16 - 96, 136, 96, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelId)),
                                      inst, nullptr);
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        gamepad::Open(hwnd);
        SetTimer(hwnd, 1, 33, nullptr);
        ShowWindow(hwnd, bg ? SW_SHOWNOACTIVATE : SW_SHOW);
        if (!bg) {
            SetForegroundWindow(hwnd);
            SetFocus(button);
        }
        UpdateWindow(hwnd);
        dslog::Write("Asset store: progress window shown%s", bg ? " (off-screen: background test run)" : "");
    }
    for (;;) {
        const DWORD w = MsgWaitForMultipleObjects(1, &thread, FALSE, INFINITE, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0) break;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (hwnd && m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) RequestCancel(hwnd);
            if (!hwnd || !IsDialogMessageW(hwnd, &m)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
    }
    CloseHandle(thread);
    if (hwnd) {
        KillTimer(hwnd, 1);
        gamepad::Close();
        DestroyWindow(hwnd);
        MSG m;   // the window's last messages
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    }
    UnregisterClassW(kWindowClass, inst);
    DeleteObject(g_font);
    DeleteObject(g_bold);
    g_font = g_bold = nullptr;
    g_job = nullptr;
}

// ---- opening the store (lazily, once: the first of ApplyMods' sound work / ApplyAssetStore) ----
enum class Prep { NotYet, Off, Ready };
Prep g_prep = Prep::NotYet;
std::wstring g_dir;

bool Prepare() {
    if (g_prep != Prep::NotYet) return g_prep == Prep::Ready;
    g_prep = Prep::Off;
    DeclareSettings();
    DWORD mode = Setting(kSetOn) ? 1 : 0;
    bool force = Setting(kSetRebuild) != 0;
#ifndef DS_DIST
    if (const DWORD dev = DevDword("PseStore")) mode = dev;   // dev override: 1 store + fallback, 2 exact only
    force = force || DevDword("PseRebuild") >= 2;            // DESIGN.md §12.3: 2 = assets too
    g_noBorrow = DevDword("PseNoBorrow") != 0;
#endif
    if (mode == 0) {
        dslog::Write("Asset store: off (SHARED ASSET STORE in OPTIONS > DESERTSTORMFIX > PLUGINS)");
        return false;
    }
    g_fallback = mode == 1;
    g_noBorrow = g_noBorrow || g_fallback;   // the fallback serves what Borrow* used to copy
    const std::wstring game = GameDir();
    g_dir = pse::build::DefaultStoreDir(game, "ds1");
    std::string why;
    const auto t0 = std::chrono::steady_clock::now();
    if (force || !pse::build::StampValid(g_dir, game, pse::build::Ds1Archives(), &why)) {
        dslog::Write("Asset store: build needed (%s) in %s", force ? "rebuild requested" : why.c_str(),
                     pse::build::Narrow(g_dir).c_str());
        BuildJob job;
        job.options.gameDir = game;
        job.options.outDir = g_dir;
        job.options.force = true;
        job.options.log = [](const std::string& s) { dslog::Write("Asset store: %s", s.c_str()); };
        BuildWithWindow(job);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (job.stats.cancelled) {
            dslog::Write("Asset store: build cancelled after %lld ms - off for this run (built at the next start)",
                         static_cast<long long>(ms));
            features::ModLog("Asset store: the first-start build was cancelled - off for this run");
            return false;
        }
        if (!job.ok) {
            dslog::Write("[fail] Asset store: build failed (%s) after %lld ms - off", job.error.c_str(),
                         static_cast<long long>(ms));
            features::ModLog("[fail] Asset store: couldn't build the shared asset store (%s) - the game runs without it",
                             job.error.c_str());
            return false;
        }
        dslog::Write("Asset store: first-start build finished in %lld ms (with the window)", static_cast<long long>(ms));
        if (Setting(kSetRebuild) && pse::SettingsInterface()) pse::SettingsInterface()->SetInt(Self(), kSetRebuild, 0);
    }
    std::string err;
    if (!g_store.Open(g_dir, &err)) {
        dslog::Write("[fail] Asset store: can't open %s (%s) - off", pse::build::Narrow(g_dir).c_str(), err.c_str());
        features::ModLog("[fail] Asset store: can't open the shared asset store (%s) - the game runs without it", err.c_str());
        return false;
    }
    g_prep = Prep::Ready;
    dslog::Write("Asset store: open - %zu archives, %zu sound caches, %zu blobs (%lld ms)", g_store.Manifests().size(),
                 g_store.SoundCaches().size(), g_store.Blobs().size(),
                 static_cast<long long>(
                     std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
    return true;
}

}  // namespace

bool features::AssetStoreOn() { return g_on; }
bool features::AssetStoreServesBorrows() { return g_on && g_noBorrow; }   // (with PseStore = 2: the control run)

bool features::AssetStoreHasFile(const char* name) {   // some archive has it and the fallback serves it to any level
    if (!g_on || !g_fallback || !name || !*name) return false;
    const uint32_t h = pse::build::Ds1NameHash(name);
    for (const auto& m : g_store.Manifests())
        if (m.byHash.count(h)) return true;
    return false;
}

bool features::AssetStoreSound() { return Prepare() && !g_store.SoundCaches().empty(); }

bool features::AssetStoreSoundCache(const char* cache, std::string* bytes) {
    if (!AssetStoreSound() || !cache) return false;
    const auto t0 = std::chrono::steady_clock::now();
    if (!g_store.RebuildSound(cache, bytes)) return false;
    dslog::Write("Asset store: sound cache %s from the store (%zu bytes, %lld ms)", cache, bytes->size(),
                 static_cast<long long>(
                     std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
    return true;
}

bool features::AssetStoreHasBank(uint32_t key, std::string* cache) { return AssetStoreSound() && g_store.HasSoundBank(key, cache); }

bool features::AssetStoreBankPack(uint32_t key, std::string* pack, std::string* cache) {
    return AssetStoreSound() && g_store.SoundBankPack(key, pack, cache);
}

void features::ApplyAssetStore() {
    if (g_on) return;   // start-up only (the store isn't switched live)
    if (!Prepare()) return;
    g_packUtf8 = pse::build::Narrow(g_store.PackPath());
    g_packAnsi = AnsiPath(g_store.PackPath());
    for (const auto& m : g_store.Manifests()) g_fallbackOrder.push_back(m.archive);
    std::sort(g_fallbackOrder.begin(), g_fallbackOrder.end());

    g_self = Self();
    const PSELoadInterface* li = pse::LoadInterfaceFor(g_self ? g_self : pse_host::Core());
    g_vfs = li && li->Query ? static_cast<const PSEVfsInterface*>(li->Query(li->plugin, PSE_VFS_INTERFACE, 1)) : nullptr;
    if (!g_vfs) {
        dslog::Write("[fail] Asset store: the loader has no pse.vfs (old pse_loader.dll?) - archive reads off");
        return;
    }
    if (!g_self) g_self = li->plugin;
    if (g_vfs->SetNameHash(g_self, &NameHash) != PSE_OK) dslog::Write("Asset store: pse.vfs already has a name hash");
    PSEVfsProvider p{sizeof p, "ds1.store", &Locate, nullptr};
    if (g_vfs->RegisterProvider(g_self, &p, 0, nullptr, nullptr) != PSE_OK) {
        dslog::Write("[fail] Asset store: provider not registered - archive reads off");
        return;
    }
    if (!patch::HookFunction(kArchiveLookup, reinterpret_cast<const void*>(&ArchiveLookup), reinterpret_cast<void**>(&g_lookup),
                             kArchiveLookupEntry, sizeof kArchiveLookupEntry, 0, kPriority) ||
        !patch::HookFunction(kOpen, reinterpret_cast<const void*>(&Open), reinterpret_cast<void**>(&g_open), kOpenEntry,
                             sizeof kOpenEntry, 0, kPriority)) {
        dslog::Write("[fail] Asset store: the archive hooks failed - archive reads off");
        return;
    }
    g_on = true;
    dslog::Write("Asset store: ON (%s) - %zu archives, %zu sound caches, %zu blobs%s, pack %s",
                 g_fallback ? "global fallback" : "exact", g_store.Manifests().size(), g_store.SoundCaches().size(),
                 g_store.Blobs().size(), g_noBorrow ? ", Borrow* copies off" : "", g_packAnsi.c_str());
}
