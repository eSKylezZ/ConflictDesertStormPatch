// OPTIONS > VIDEO (front-end state 0x4C): the launcher's settings in the game - its Detail Settings (resolution,
// shadows, lighting, vegetation, draw distance, shrubs, detail textures, decals, environment maps, texture resolution)
// and our Enhancements column (display mode, V-Sync, frame limit, anti-aliasing, filtering, HUD, split-screen layout),
// plus whether the launcher shows at all and APPLY & RESTART.
//
// When each takes effect (the launcher's OK, FUN_00448570, applies the game's ones once, at start-up):
//   live         frame limit (framecap.cpp), texture filtering (graphics.cpp), launcher at start
//   next level   draw distance (renderer +0x40A9C, read by the scene load 0x40B5BD), shrubs (+0x40A9D, density call
//                0x50AC60 at level load 0x40DFA9), texture resolution (+0x40AA0, texture loader 0x548F3C: ".ddl"
//                files), split-screen layout (views built before the load)
//   apply now    resolution / display mode / V-Sync / anti-aliasing: APPLY NOW (front end only) resets the device in
//                the new mode (graphics.cpp ChangeDisplayMode) and reloads the front end so every screen lays itself
//                out again; KEEP within 15 s or it goes back (a mode the monitor can't show)
//   restart      shadows (the mode's stencil),
//                per-pixel lighting / moving vegetation / detail textures / decals / environment maps (caps mask
//                copied to renderer +0x40680 at device creation), HUD scaling / size (sheets scaled at front-end load)
// The game's values are written to its own Device Settings key (the launcher reads them at the next start and writes
// them back on OK); next-level ones also go straight into the renderer.
//
// Rows are spinners: each choice item (0x68 B, ctor 0x4E5010) holds three texts - previous, current, next value - with
// the current one selected; Left / Right land on the previous / next text, which is taken as a step (Enter steps
// forward), so a row can have any number of values (the item itself holds at most 8). The screen is an Options
// sub-screen object as fixmenu.cpp's, same scrolling window.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/overlay.h"
#include "features/splitscreen_layout.h"

namespace {
constexpr uint32_t kSetState = 0x47EF20, kBackPressed = 0x478380, kListInput = 0x4E5D80;
constexpr uint32_t kAlloc = 0x4B83D0, kListCtor = 0x4E5B10, kListReserve = 0x4E56C0, kListAddToPool = 0x4E58C0;
constexpr uint32_t kChoiceCtor = 0x4E5010, kItemSet = 0x4E4FB0, kChoices = 0x4E5090;
constexpr uint32_t kListCallbacks = 0x4E59E0, kChoiceDraw = 0x402E20;
constexpr uint32_t kDrawPanel = 0x456EB0, kDrawTitle = 0x456F40;
constexpr uint32_t kLargeFont = 0x60EDB8, kHudSheet = 0x60EE18, kMainMenuScreen = 0x617A00, kRenderer = 0x63C924;
constexpr uint32_t kMainMenuEnter = 0x4153B0, kOptCtor = 0x409B90, kOptVtable = 0x5D8510, kOptSetup = 0x409C90;
constexpr uint32_t kGameWindow = 0x606A60, kLevelName = 0x606880;
constexpr uint32_t kStateOptions = 4;
constexpr int kVtableEntries = 21;

enum : uint32_t { kTextTitle = 0xC0120001, kTextRow0 = 0xC0120010, kTextVal0 = 0xC0120100 };

template <class T>
T& At(void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}
template <class T>
T& Global(uint32_t va) {
    return *reinterpret_cast<T*>(va);
}
void SetState(uint32_t s) { reinterpret_cast<void(__cdecl*)(uint32_t)>(kSetState)(s); }
void* ItemAt(void* list, int i) { return At<void**>(list, 0x2C)[i]; }
void AddToList(void* list, void* item) {
    reinterpret_cast<void(__thiscall*)(void*, void*)>((*reinterpret_cast<void***>(list))[1])(list, item);
}
using ItemSetFn = void(__thiscall*)(void*, int, uint32_t, int, int, uint32_t, int, uint32_t, int);

// ---- the game's device / mode tables (renderer = the DirectX sample framework's CD3DApplication) ----
// adapter = renderer + 0xE4 + i * 0x66D0 (device index +0x66C4, fullscreen flag +0x66C8), device = adapter + 0x440 +
// j * 0x13B4 (+0 type, +0xE0 caps, +0xE4 mode count, modes +0xE8 (0x20 B: +0 w, +4 h, +0x14 colour bits, +0x18 depth
// bits, +0x1C stencil bits), +0x13A8 current mode).
uint8_t* Device() {
    auto* r = Global<uint8_t*>(kRenderer);
    if (!r) return nullptr;
    const int adapter = At<int>(r, 0xE0);
    uint8_t* a = r + 0xE4 + adapter * 0x66D0;
    return a + 0x440 + At<int>(a, 0x66C4) * 0x13B4;
}

struct Mode {
    uint32_t index, w, h;
};
std::vector<Mode> g_modes;  // the current device's modes at its colour depth, one per size (most stencil bits)

void BuildModes() {
    g_modes.clear();
    uint8_t* d = Device();
    if (!d) return;
    const uint32_t count = At<uint32_t>(d, 0xE4), current = At<uint32_t>(d, 0x13A8);
    if (current >= count) return;
    auto mode = [&](uint32_t i) { return d + 0xE8 + i * 0x20; };
    const uint32_t bits = At<uint32_t>(mode(current), 0x14);
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t* m = mode(i);
        if (At<uint32_t>(m, 0x14) != bits) continue;
        const uint32_t w = At<uint32_t>(m, 0), h = At<uint32_t>(m, 4);
        auto same = std::find_if(g_modes.begin(), g_modes.end(), [&](const Mode& o) { return o.w == w && o.h == h; });
        if (same == g_modes.end()) g_modes.push_back({i, w, h});
        else if (same->index != current && (i == current || At<uint32_t>(m, 0x1C) > At<uint32_t>(mode(same->index), 0x1C)))
            same->index = i;
    }
    std::sort(g_modes.begin(), g_modes.end(), [](const Mode& a, const Mode& b) { return a.w != b.w ? a.w < b.w : a.h < b.h; });
}

// ---- rows ----
enum class When { Live, NextLevel, Display, Restart, Action };
struct Row {
    const char* label;
    When when;
    int (*count)();             // number of values
    int (*get)();               // current value index
    void (*set)(int);           // store value index
    std::string (*text)(int);   // value index -> text
    bool (*available)();        // false = shown as NOT SUPPORTED, no change (nullptr = always)
};

std::string OnOff(int v) { return v ? "ON" : "OFF"; }

// The game's Device Settings: the value shown is the saved one (applied at the next start / level).
uint32_t GameValue(const char* name, uint32_t fallback) {
    uint32_t v = fallback;
    settings::ReadDword(true, name, v);
    return v;
}
void SetGameValue(const char* name, uint32_t v) {
    const bool ok = settings::WriteDword(true, name, v);
    dslog::Write(ok ? "Video menu: %s = %u" : "[fail] Video menu: %s = %u (not saved)", name, v);
}

uint32_t Caps() {
    uint32_t stencil;
    return features::LauncherCaps(stencil);
}

void SaveSettings(const settings::Values& v, const char* what) {
    const bool ok = settings::Save(v);
    dslog::Write(ok ? "Video menu: %s changed" : "[fail] Video menu: %s (not saved)", what);
}

// RESOLUTION
int ResCount() { return std::max<int>(1, static_cast<int>(g_modes.size())); }
int ResGet() {
    uint8_t* d = Device();
    const uint32_t saved = GameValue("ResolutionIndex", d ? At<uint32_t>(d, 0x13A8) : 0);
    const uint8_t* m = d && saved < At<uint32_t>(d, 0xE4) ? d + 0xE8 + saved * 0x20 : nullptr;
    for (size_t i = 0; i < g_modes.size(); ++i)
        if (g_modes[i].index == saved || (m && g_modes[i].w == At<uint32_t>(const_cast<uint8_t*>(m), 0) &&
                                          g_modes[i].h == At<uint32_t>(const_cast<uint8_t*>(m), 4)))
            return static_cast<int>(i);
    return 0;
}
void ResSet(int i) {
    if (i >= 0 && i < static_cast<int>(g_modes.size())) SetGameValue("ResolutionIndex", g_modes[i].index);
}
std::string ResText(int i) {
    if (i < 0 || i >= static_cast<int>(g_modes.size())) return "-";
    char t[32];
    snprintf(t, sizeof t, "%u X %u", g_modes[i].w, g_modes[i].h);
    return t;
}

// DISPLAY MODE
const char* const kDisplay[] = {"FULLSCREEN", "WINDOWED", "BORDERLESS"};
int DispCount() { return 3; }
int DispGet() { return static_cast<int>(std::min<uint32_t>(settings::Get().displayMode, 2)); }
void DispSet(int i) {
    settings::Values v = settings::Get();
    v.displayMode = static_cast<uint32_t>(i);
    SaveSettings(v, "display mode");
}
std::string DispText(int i) { return kDisplay[i]; }

// V-SYNC
int Two() { return 2; }
int VsyncGet() { return settings::Get().vsync ? 1 : 0; }
void VsyncSet(int i) {
    settings::Values v = settings::Get();
    v.vsync = i != 0;
    SaveSettings(v, "V-Sync");
}

// FRAME RATE LIMIT (cap | toRefresh << 16, as the launcher's list)
const uint32_t kFps[] = {240 | 0x10000, 30, 60, 120, 144, 165, 180, 240};
int FpsCount() { return 8; }
int FpsGet() {
    const auto& s = settings::Get();
    const uint32_t key = s.fpsCap | (s.fpsCapToRefresh ? 0x10000u : 0u);
    for (int i = 0; i < 8; ++i)
        if (kFps[i] == key) return i;
    return 0;
}
void FpsSet(int i) {
    settings::Values v = settings::Get();
    v.fpsCap = kFps[i] & 0xFFFF;
    v.fpsCapToRefresh = (kFps[i] >> 16) != 0;
    SaveSettings(v, "frame rate limit");
    features::ApplyFrameCap();
}
std::string FpsText(int i) {
    if (kFps[i] >> 16) return "MONITOR (MAX 240)";
    return std::to_string(kFps[i]) + " FPS";
}

// ANTI-ALIASING
const uint32_t kAa[] = {0, 2, 4, 8};
int AaCount() { return 4; }
int AaGet() {
    for (int i = 3; i >= 0; --i)
        if (settings::Get().antialiasing >= kAa[i]) return i;
    return 0;
}
void AaSet(int i) {
    settings::Values v = settings::Get();
    v.antialiasing = kAa[i];
    SaveSettings(v, "anti-aliasing");
}
std::string AaText(int i) { return i ? std::to_string(kAa[i]) + "X MSAA" : "OFF"; }

// RENDERER (src/render; read when the game creates its Direct3D object = restart)
int RendererCount() { return static_cast<int>(settings::Values::kMaxRenderer) + 1; }
int RendererGet() { return static_cast<int>(std::min(settings::Get().renderer, settings::Values::kMaxRenderer)); }
void RendererSet(int i) {
    settings::Values v = settings::Get();
    v.renderer = static_cast<uint32_t>(i);
    SaveSettings(v, "renderer");
}
std::string RendererText(int i) {
    static const char* const kText[] = {"DIRECT3D 8 (ORIGINAL)", "DIRECT3D 9", "DIRECT3D 12 (9ON12)", "DIRECT3D 12 + FLIP"};
    return kText[i];
}

// HDR (Renderer 3 only; src/render/presenter.cpp re-reads these every 120 frames = live)
bool HdrOk() { return settings::Get().renderer == 3; }
int HdrGet() { return settings::Get().hdr ? 1 : 0; }
void HdrSet(int i) {
    settings::Values v = settings::Get();
    v.hdr = i != 0;
    SaveSettings(v, "HDR");
}
std::string HdrText(int i) { return i ? "ON (HDR DISPLAYS)" : "OFF"; }
const uint32_t kPaper[] = {100, 150, 200, 250, 300, 400};
int PaperCount() { return 6; }
int PaperGet() {
    for (int i = 5; i >= 0; --i)
        if (settings::Get().hdrPaperWhite >= kPaper[i]) return i;
    return 0;
}
void PaperSet(int i) {
    settings::Values v = settings::Get();
    v.hdrPaperWhite = kPaper[i];
    SaveSettings(v, "HDR paper white");
}
std::string PaperText(int i) { return std::to_string(kPaper[i]) + " NITS"; }
const uint32_t kPeak[] = {0, 400, 600, 800, 1000, 1500, 2000};
int PeakCount() { return 7; }
int PeakGet() {
    for (int i = 6; i >= 1; --i)
        if (settings::Get().hdrPeak >= kPeak[i]) return i;
    return 0;
}
void PeakSet(int i) {
    settings::Values v = settings::Get();
    v.hdrPeak = kPeak[i];
    SaveSettings(v, "HDR peak");
}
std::string PeakText(int i) { return i ? std::to_string(kPeak[i]) + " NITS" : "DISPLAY'S OWN"; }
int HighCount() { return 5; }
int HighGet() { return static_cast<int>(std::min<uint32_t>(settings::Get().hdrHighlights, 100) / 25); }
void HighSet(int i) {
    settings::Values v = settings::Get();
    v.hdrHighlights = static_cast<uint32_t>(i) * 25;
    SaveSettings(v, "HDR highlights");
}
std::string HighText(int i) { return i ? std::to_string(i * 25) + "%" : "OFF (FLAT)"; }
int BloomCount() { return 3; }
int BloomGet() { return static_cast<int>(std::min<uint32_t>(settings::Get().bloom, 2)); }
void BloomSet(int i) {
    settings::Values v = settings::Get();
    v.bloom = static_cast<uint32_t>(i);
    SaveSettings(v, "bloom");
}
std::string BloomText(int i) {
    static const char* const kText[] = {"OFF", "SUBTLE", "STRONG"};
    return kText[i];
}

// TEXTURE FILTERING
const uint32_t kAf[] = {0, 2, 4, 8, 16};
int AfCount() { return 5; }
int AfGet() {
    for (int i = 4; i >= 0; --i)
        if (settings::Get().anisotropy >= kAf[i]) return i;
    return 0;
}
void AfSet(int i) {
    settings::Values v = settings::Get();
    v.anisotropy = kAf[i];
    SaveSettings(v, "texture filtering");
    features::ApplyAnisotropy();
}
std::string AfText(int i) { return i ? std::to_string(kAf[i]) + "X ANISOTROPIC" : "GAME (TRILINEAR)"; }

// Next-level values: registry + renderer
void RendererByte(uint32_t offset, uint8_t v) {
    if (auto* r = Global<uint8_t*>(kRenderer)) At<uint8_t>(r, offset) = v;
}
int TexGet() { return GameValue("TextureResolution", 0) ? 1 : 0; }
void TexSet(int i) {
    SetGameValue("TextureResolution", i);
    if (auto* r = Global<uint8_t*>(kRenderer)) At<uint32_t>(r, 0x40AA0) = i;
}
std::string TexText(int i) { return i ? "LOW" : "HIGH"; }
int DrawGet() { return GameValue("DrawDistance", 0) ? 1 : 0; }
void DrawSet(int i) {
    SetGameValue("DrawDistance", i);
    RendererByte(0x40A9C, static_cast<uint8_t>(i));
}
std::string DrawText(int i) { return i ? "EXTENDED" : "NORMAL"; }
int ShrubGet() { return GameValue("ShrubLevel", 0) ? 1 : 0; }
void ShrubSet(int i) {
    SetGameValue("ShrubLevel", i);
    RendererByte(0x40A9D, static_cast<uint8_t>(i));
}
std::string ShrubText(int i) { return i ? "FEWER" : "ALL"; }

// Restart toggles of the game: name + caps bit (0 = shadows: stencil bits of the mode)
#define GAME_TOGGLE(fn, name, cap)                                                   \
    int fn##Get() { return GameValue(name, 1) ? 1 : 0; }                             \
    void fn##Set(int i) { SetGameValue(name, i); }                                   \
    bool fn##Ok() {                                                                  \
        uint32_t stencil;                                                            \
        const uint32_t caps = features::LauncherCaps(stencil);                       \
        return (cap) ? (caps & (cap)) != 0 : stencil >= 8;                           \
    }
GAME_TOGGLE(Shadows, "Shadows", 0)
GAME_TOGGLE(PShaders, "PShaders", 0x400)
GAME_TOGGLE(VShaders, "VShaders", 0x200)
GAME_TOGGLE(Detail, "DetailTextures", 0x1)
GAME_TOGGLE(Decals, "Decals", 0x8)
GAME_TOGGLE(EnvMaps, "EnvironmentMaps", 0x1000)
#undef GAME_TOGGLE

// HUD
int HudGet() { return settings::Get().hudScaling ? 1 : 0; }
void HudSet(int i) {
    settings::Values v = settings::Get();
    v.hudScaling = i != 0;
    SaveSettings(v, "HUD scaling");
}
const uint32_t kHudSizes[] = {75, 85, 100, 115, 125, 150};
int HudSizeCount() { return 6; }
int HudSizeGet() {
    int best = 2;
    for (int i = 0; i < 6; ++i)
        if (std::abs(static_cast<int>(kHudSizes[i]) - static_cast<int>(settings::Get().hudScalePercent)) <
            std::abs(static_cast<int>(kHudSizes[best]) - static_cast<int>(settings::Get().hudScalePercent)))
            best = i;
    return best;
}
void HudSizeSet(int i) {
    settings::Values v = settings::Get();
    v.hudScalePercent = kHudSizes[i];
    SaveSettings(v, "HUD size");
}
std::string HudSizeText(int i) { return std::to_string(kHudSizes[i]) + "%"; }

// SPLIT SCREEN LAYOUT
int SplitGet() { return static_cast<int>(std::min<uint32_t>(settings::Get().splitScreenLayout, 1)); }
void SplitSet(int i) {
    settings::Values v = settings::Get();
    v.splitScreenLayout = static_cast<uint32_t>(i);
    SaveSettings(v, "split-screen layout");
}
std::string SplitText(int i) { return i ? "SIDE BY SIDE" : "TOP / BOTTOM"; }

// LAUNCHER AT START
int LauncherGet() { return settings::Get().skipLauncher ? 1 : 0; }
void LauncherSet(int i) {
    settings::Values v = settings::Get();
    v.skipLauncher = i != 0;
    SaveSettings(v, "launcher at start");
}
std::string LauncherText(int i) { return i ? "SKIP (SHIFT SHOWS IT)" : "SHOW"; }

// APPLY (one value; selecting it keeps / applies / restarts)
bool RestartPending();
bool DisplayPending();
int ConfirmSeconds();
// Menus (not a mission or the pause menu): the scene name [0x606880] - the front end loads intro.env.
bool InFrontEnd() {
    const char* level = &Global<char>(kLevelName);
    return !level[0] || _strnicmp(level, "FrontEnd", 8) == 0 || _strnicmp(level, "intro", 5) == 0;
}
int One() { return 1; }
int Zero() { return 0; }
void NoSet(int) {}
std::string ApplyText(int) {
    if (const int s = ConfirmSeconds()) return "KEEP THIS MODE (" + std::to_string(s) + ")";
    if (!DisplayPending() && !RestartPending()) return "NOTHING TO APPLY";
    if (!InFrontEnd()) return "IN MAIN MENU";
    return DisplayPending() ? "APPLY NOW" : "RESTART NOW";
}

const Row kRows[] = {
    {"RESOLUTION", When::Display, ResCount, ResGet, ResSet, ResText, nullptr},
    {"DISPLAY MODE", When::Display, DispCount, DispGet, DispSet, DispText, nullptr},
    {"V-SYNC", When::Display, Two, VsyncGet, VsyncSet, OnOff, nullptr},
    {"FRAME RATE LIMIT", When::Live, FpsCount, FpsGet, FpsSet, FpsText, nullptr},
    {"ANTI-ALIASING", When::Display, AaCount, AaGet, AaSet, AaText, nullptr},
    {"RENDERER", When::Restart, RendererCount, RendererGet, RendererSet, RendererText, nullptr},
    {"HDR", When::Live, Two, HdrGet, HdrSet, HdrText, HdrOk},
    {"HDR PAPER WHITE", When::Live, PaperCount, PaperGet, PaperSet, PaperText, HdrOk},
    {"HDR PEAK BRIGHTNESS", When::Live, PeakCount, PeakGet, PeakSet, PeakText, HdrOk},
    {"HDR HIGHLIGHTS", When::Live, HighCount, HighGet, HighSet, HighText, HdrOk},
    {"BLOOM", When::Live, BloomCount, BloomGet, BloomSet, BloomText, HdrOk},
    {"TEXTURE FILTERING", When::Live, AfCount, AfGet, AfSet, AfText, nullptr},
    {"TEXTURE RESOLUTION", When::NextLevel, Two, TexGet, TexSet, TexText, nullptr},
    {"DRAW DISTANCE", When::NextLevel, Two, DrawGet, DrawSet, DrawText, nullptr},
    {"SHRUBS", When::NextLevel, Two, ShrubGet, ShrubSet, ShrubText, nullptr},
    {"VOLUMETRIC SHADOWS", When::Restart, Two, ShadowsGet, ShadowsSet, OnOff, ShadowsOk},
    {"PER-PIXEL LIGHTING", When::Restart, Two, PShadersGet, PShadersSet, OnOff, PShadersOk},
    {"MOVING VEGETATION", When::Restart, Two, VShadersGet, VShadersSet, OnOff, VShadersOk},
    {"DETAIL TEXTURES", When::Restart, Two, DetailGet, DetailSet, OnOff, DetailOk},
    {"DECALS", When::Restart, Two, DecalsGet, DecalsSet, OnOff, DecalsOk},
    {"ENVIRONMENT MAPS", When::Restart, Two, EnvMapsGet, EnvMapsSet, OnOff, EnvMapsOk},
    {"HUD & MENU SCALING", When::Restart, Two, HudGet, HudSet, OnOff, nullptr},
    {"HUD SIZE", When::Restart, HudSizeCount, HudSizeGet, HudSizeSet, HudSizeText, nullptr},
    {"SPLIT SCREEN LAYOUT", When::NextLevel, Two, SplitGet, SplitSet, SplitText, nullptr},
    {"LAUNCHER AT START", When::Live, Two, LauncherGet, LauncherSet, LauncherText, nullptr},
    {"APPLY", When::Action, One, Zero, NoSet, ApplyText, nullptr},
};
constexpr int kRowCount = static_cast<int>(sizeof kRows / sizeof kRows[0]);
constexpr int kVisible = 8;
constexpr uint16_t kFirstId = 0x8A0;  // visible row k = kFirstId + k

int g_atStart[kRowCount];  // restart / display rows: the values the game is running with
bool g_haveStart = false;
int g_offset = 0, g_lastRow = 0;
int g_choicesFor[kVisible];  // row << 1 | spinner, whose choices each item holds (-1 = none)
uint8_t g_screen[0x20];
void* g_vtable[kVtableEntries];
bool g_built = false;

bool Available(int r) { return !kRows[r].available || kRows[r].available(); }
bool Spinner(int r) { return kRows[r].when != When::Action && Available(r) && kRows[r].count() > 1; }

bool Pending(When when) {
    for (int r = 0; r < kRowCount; ++r)
        if (kRows[r].when == when && kRows[r].get() != g_atStart[r]) return true;
    return false;
}
bool RestartPending() { return Pending(When::Restart); }
bool DisplayPending() { return Pending(When::Display); }

// ---- display changes without a restart ----
struct DisplayValues {
    int row[kRowCount];  // the display rows' values (others unused)
};
enum class Step { None, Apply, Reload, Confirm, Revert };
Step g_step = Step::None;
DisplayValues g_previous{};   // what to go back to if the new mode isn't kept
bool g_reverting = false;     // the reload after a revert (no confirmation)
DWORD g_confirmEnd = 0;

int ConfirmSeconds() {
    if (g_step != Step::Confirm) return 0;
    const DWORD now = GetTickCount();
    return now >= g_confirmEnd ? 0 : static_cast<int>((g_confirmEnd - now + 999) / 1000);
}

// The device in the mode the display rows now hold (settings / ResolutionIndex saved).
bool ApplyNewDisplay() {
    uint32_t mode = 0;
    if (!settings::ReadDword(true, "ResolutionIndex", mode)) return false;
    return features::ChangeDisplayMode(mode, settings::Get().displayMode);
}

std::string Marked(int r, int value) {
    if (!Available(r)) return "NOT SUPPORTED";
    std::string t = kRows[r].text(value);
    if (kRows[r].when == When::Restart) t += " *";
    else if (kRows[r].when == When::NextLevel) t += " +";
    return t;
}

// ---- restart ----
void Restart() {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    std::wstring cmd = GetCommandLineW();
    const size_t old = cmd.find(L" +restart ");
    if (old != std::wstring::npos) cmd.erase(old);
    cmd += L" +restart " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        dslog::Write("[fail] Video menu: restart - CreateProcess error %lu", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    dslog::Write("Video menu: restarting (new process %lu)", pi.dwProcessId);
    PostMessageA(Global<HWND>(kGameWindow), WM_CLOSE, 0, 0);  // the game's WM_DESTROY unloads the level and quits
}

// ---- screen ----
int VisibleRows() { return std::min(kVisible, kRowCount); }
int MaxOffset() { return std::max(0, kRowCount - kVisible); }

bool __fastcall Create(void* screen, void*) {
    using Alloc = void*(__cdecl*)(uint32_t, uint32_t);
    void* list = reinterpret_cast<Alloc>(kAlloc)(0xA4, 0x1E);
    if (!list) return false;
    reinterpret_cast<void*(__thiscall*)(void*, void*, void*, int)>(kListCtor)(
        list, Global<void*>(kLargeFont), Global<void*>(kHudSheet), 1);
    At<void*>(screen, 8) = list;
    if (!reinterpret_cast<int(__thiscall*)(void*, int)>(kListReserve)(list, VisibleRows())) return false;
    for (int i = 0; i < VisibleRows(); ++i) {
        void* item = reinterpret_cast<Alloc>(kAlloc)(0x68, 0x1E);
        if (item) reinterpret_cast<void*(__thiscall*)(void*)>(kChoiceCtor)(item);
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kListAddToPool)(list, item);
    }
    return true;
}

// Each visible row k shows kRows[g_offset + k]; a spinner's item holds prev / current / next (texts kTextVal0 + k * 4 +
// 0..2, looked up every frame) with the current one selected.
void RefreshRows(void* list) {
    using ChoicesFn = void(__thiscall*)(void*, uint32_t, uint32_t, int, int, int, int, int, int);
    for (int k = 0; k < VisibleRows(); ++k) {
        void* item = ItemAt(list, k);
        if (!item) continue;
        const int r = g_offset + k;
        const bool spin = Spinner(r);
        const int key = r << 1 | (spin ? 1 : 0);
        if (g_choicesFor[k] != key) {  // only when it changes (fixmenu.cpp: every frame swallowed Up / Down)
            g_choicesFor[k] = key;
            const uint32_t t = kTextVal0 + k * 4;
            if (spin) reinterpret_cast<ChoicesFn>(kChoices)(item, t, t + 1, t + 2, -1, -1, -1, -1, -1);
            else reinterpret_cast<ChoicesFn>(kChoices)(item, t + 1, static_cast<uint32_t>(-1), -1, -1, -1, -1, -1, -1);
        }
        At<int8_t>(item, 0x24) = spin ? 1 : 0;
    }
}

void __fastcall Setup(void* screen, void*) {
    BuildModes();
    dslog::Write("Video menu: opened (scene \"%s\", %zu sizes)", &Global<char>(kLevelName), g_modes.size());
    if (!g_haveStart) {  // first visit: what the game started with (nothing here changed it before)
        for (int r = 0; r < kRowCount; ++r) g_atStart[r] = kRows[r].get();
        g_haveStart = true;
    }
    g_offset = 0;
    g_lastRow = 0;
    for (int& c : g_choicesFor) c = -1;
    reinterpret_cast<void(__thiscall*)(void*)>(kOptSetup)(screen);
    void* list = At<void*>(screen, 8);
    using Callbacks = void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, int, int, int, int, int);
    reinterpret_cast<Callbacks>(kListCallbacks)(list, 0x478440, 0x478480, 0x478530, 0x4785E0, 0x478690, 0, 0, 0, 0, 0);
    At<void*>(list, 0x90) = screen;
    const int rows = VisibleRows();
    for (int k = 0; k < rows; ++k)
        reinterpret_cast<ItemSetFn>(kItemSet)(ItemAt(list, k), kFirstId + k, 0xFFFFFFFF, 0, 0x24, kTextRow0 + k, 0,
                                              kChoiceDraw, 0);
    RefreshRows(list);
    for (int k = 2; k < rows; ++k) AddToList(list, ItemAt(list, k));
    At<int16_t>(list, 0x28) = 0;
    if (g_step != Step::None) {  // back after APPLY NOW: the cursor on APPLY, ready to keep the new mode
        g_offset = MaxOffset();
        g_lastRow = rows - 1;
        At<int16_t>(list, 0x28) = static_cast<int16_t>(rows - 1);
        for (int& c : g_choicesFor) c = -1;
        RefreshRows(list);
    }
    At<uint16_t>(list, 0x30) = static_cast<uint16_t>(kFirstId);
    At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + (rows - 2) * At<int16_t>(list, 0x38));
    At<int16_t>(list, 0x42) = static_cast<int16_t>(rows);
    using Layout = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, kTextTitle, 0, 0, 0x7FFF);
}

// Scrolling and W / S as in fixmenu.cpp.
constexpr uint32_t kJoystickCount = 0x754BE8, kMouseWheel = 0x754C24 + 8;

void Scroll(void* list) {
    const int vis = VisibleRows();
    int16_t& row = At<int16_t>(list, 0x28);
    const int prev = g_lastRow, now = row;
    if (prev == vis - 1 && now == 0) {
        if (g_offset < MaxOffset()) {
            ++g_offset;
            row = static_cast<int16_t>(vis - 1);
        } else {
            g_offset = 0;
        }
    } else if (prev == 0 && now == vis - 1) {
        if (g_offset > 0) {
            --g_offset;
            row = 0;
        } else {
            g_offset = MaxOffset();
        }
    }
    g_lastRow = row;
}

int WasdStep() {
    static int dir = 0;
    static DWORD since = 0, last = 0;
    const int now = features::KeyHeld('W') ? -1 : features::KeyHeld('S') ? 1 : 0;
    const DWORD t = GetTickCount();
    if (now != dir) {
        dir = now;
        since = last = t;
        return now;
    }
    if (now && t - since >= 400 && t - last >= 120) {
        last = t;
        return now;
    }
    return 0;
}

void Navigate(void* list) {
    const int vis = VisibleRows();
    const bool front = features::GameFocused();
    int16_t& row = At<int16_t>(list, 0x28);
    const int step = front ? WasdStep() : 0;
    if (step) row = static_cast<int16_t>((row + step + vis) % vis);
    uint32_t pad = 0;
    for (int j = 0, n = std::min(Global<int>(kJoystickCount), 8); front && j < n; ++j) pad |= features::ReadJoystick(j);
    const bool nav = front && (step || features::KeyHeld(VK_UP) || features::KeyHeld(VK_DOWN) ||
                               (pad & (1u << 12 | 1u << 16 | 1u << 14 | 1u << 18)));
    if (nav) Scroll(list);
    else g_lastRow = row;
    // the real foreground only: background test runs never poll the mouse, so lZ keeps a stale value there
    const int wheel = GetForegroundWindow() == Global<HWND>(0x606A60) ? Global<int>(kMouseWheel) : 0;
    if (wheel < 0 && g_offset < MaxOffset()) ++g_offset;
    else if (wheel > 0 && g_offset > 0) --g_offset;
}

int __fastcall Update(void* screen, void*) {
    void* list = At<void*>(screen, 8);
    Navigate(list);
    RefreshRows(list);
    const int16_t id = reinterpret_cast<int16_t(__thiscall*)(void*)>(kListInput)(list);
    const int k = id - kFirstId;
    if (k >= 0 && k < VisibleRows()) {
        const int r = g_offset + k;
        void* item = ItemAt(list, k);
        if (kRows[r].when == When::Action) {
            if (g_step == Step::Confirm) {
                g_step = Step::None;
                dslog::Write("Video menu: new display mode kept");
            } else if (InFrontEnd() && g_step == Step::None) {
                if (DisplayPending()) g_step = Step::Apply;  // between frames (OnFrameVideoMenu)
                else if (RestartPending()) Restart();
            }
        } else if (item && Spinner(r)) {
            const int choice = At<int8_t>(item, 0x24), n = kRows[r].count();
            const int step = choice == 0 ? -1 : 1;  // Left landed on "previous"; Right / Enter / click: next
            kRows[r].set((kRows[r].get() + step + n) % n);
            At<int8_t>(item, 0x24) = 1;
        }
    }
    if (reinterpret_cast<int(__cdecl*)(int)>(kBackPressed)(0)) SetState(kStateOptions);
    return 0;
}

void DrawScrollbar(void* list) {
    const int vis = VisibleRows();
    if (kRowCount <= vis) return;
    const float right = static_cast<float>(At<int16_t>(list, 0x3A) + At<int16_t>(list, 0x32));
    const float pitch = At<int16_t>(list, 0x38), top = At<int16_t>(list, 0x3C);
    const auto* sheet = Global<const uint8_t*>(kHudSheet);
    const float k = sheet ? *reinterpret_cast<const float*>(sheet + 0x24) : 1.0f;
    const float w = 6.0f * k, x = right + 10.0f * k;
    const float trackH = pitch * vis - 4.0f * k, y = top + 2.0f * k;
    const float thumbH = std::max(trackH * vis / kRowCount, 8.0f * k);
    const float thumbY = y + (trackH - thumbH) * g_offset / std::max(1, MaxOffset());
    overlay::QueueRect(x, y, w, trackH, 0x80202A10);
    overlay::QueueRect(x, thumbY, w, thumbH, 0xFFE0D878);
}

void DrawFooter() {
    const auto* r = Global<const uint8_t*>(kRenderer);
    void* font = overlay::MenuFont();
    if (!r || !font) return;
    const int W = *reinterpret_cast<const int*>(r + 0x40688), H = *reinterpret_cast<const int*>(r + 0x4068C);
    const int confirm = ConfirmSeconds();
    const bool pending = confirm || DisplayPending() || RestartPending();
    char keep[64];
    snprintf(keep, sizeof keep, "KEEP THIS MODE? SELECT APPLY - GOING BACK IN %d", confirm);
    const char* text = confirm            ? keep
                       : DisplayPending() ? (InFrontEnd() ? "DISPLAY CHANGES: SELECT APPLY" : "APPLY IN THE MAIN MENU")
                       : RestartPending() ? (InFrontEnd() ? "RESTART THE GAME TO APPLY - SELECT APPLY"
                                                          : "RESTART THE GAME TO APPLY (AFTER THE MISSION)")
                                          : "* AFTER RESTARTING THE GAME     + FROM THE NEXT LEVEL";
    const int y = static_cast<int>(H * 0.92f);
    overlay::DrawLabel(font, text, (W - overlay::TextWidth(font, text)) / 2, y, pending ? 0xFFE0D878 : 0xFFC8C8C8);
}

void __fastcall Draw(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(kDrawPanel)(screen, 6, 0);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(kDrawTitle)(screen, kTextTitle, 1);
    void* list = At<void*>(screen, 8);
    reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(list))[2])(list);
    DrawScrollbar(list);
    DrawFooter();
}

void Build() {
    reinterpret_cast<void(__thiscall*)(void*)>(kOptCtor)(g_screen);
    std::memcpy(g_vtable, reinterpret_cast<void*>(kOptVtable), sizeof g_vtable);
    At<void**>(g_screen, 0) = g_vtable;
    g_vtable[6] = reinterpret_cast<void*>(kMainMenuEnter);
    g_vtable[1] = reinterpret_cast<void*>(&Create);
    g_vtable[3] = reinterpret_cast<void*>(&Setup);
    g_vtable[4] = reinterpret_cast<void*>(&Update);
    g_vtable[5] = reinterpret_cast<void*>(&Draw);
    g_built = Create(g_screen, nullptr);
    dslog::Write(g_built ? "Video menu: screen built" : "[fail] Video menu: screen not built");
}

DWORD g_restartedFrom = 0;

void SnapshotDisplay(DisplayValues& d) {
    for (int r = 0; r < kRowCount; ++r) d.row[r] = kRows[r].when == When::Display ? kRows[r].get() : 0;
}
void RestoreDisplay(const DisplayValues& d) {
    for (int r = 0; r < kRowCount; ++r)
        if (kRows[r].when == When::Display && kRows[r].get() != d.row[r]) kRows[r].set(d.row[r]);
}
void MarkApplied() {
    for (int r = 0; r < kRowCount; ++r)
        if (kRows[r].when == When::Display) g_atStart[r] = kRows[r].get();
}

// Front end rebuilt at the new size: back to this page once the main menu is up and the state setter is free.
constexpr uint32_t kFrontEndState = 0x617C18, kStateBusy = 0x617C20, kDispatchMode = 0x60EC8C;
bool BackOnPage() {
    if (Global<int8_t>(kDispatchMode) != 0 || !InFrontEnd()) return false;
    const uint32_t state = Global<uint32_t>(kFrontEndState);
    if (state == 0x4C) return true;
    if (state == 1 && Global<int>(kStateBusy) == 0) SetState(0x4C);
    return false;
}
}  // namespace

void* features::VideoMenuScreen() {
    if (!g_built && Global<void*>(kMainMenuScreen)) Build();
    return g_built ? g_screen : nullptr;
}

const char* features::VideoMenuText(uint32_t hash) {
    if (hash == kTextTitle) return "VIDEO";
    if (hash >= kTextRow0 && hash < kTextRow0 + kVisible) {
        const int r = g_offset + static_cast<int>(hash - kTextRow0);
        return r < kRowCount ? kRows[r].label : "";
    }
    if (hash >= kTextVal0 && hash < kTextVal0 + kVisible * 4) {
        const int k = static_cast<int>(hash - kTextVal0) / 4, j = static_cast<int>(hash - kTextVal0) % 4;
        const int r = g_offset + k;
        if (r >= kRowCount || j > 2) return "";
        static std::string texts[kVisible][3];  // one buffer per text: the game keeps the pointer for the frame
        const int n = kRows[r].count(), cur = kRows[r].get();
        texts[k][j] = Marked(r, Spinner(r) ? (cur + j - 1 + n) % n : cur);
        return texts[k][j].c_str();
    }
    return nullptr;
}

// Display changes run here, between frames (not inside the menu's update, which runs while the frame is built).
void features::OnFrameVideoMenu() {
    switch (g_step) {
        case Step::Apply:
            for (int r = 0; r < kRowCount; ++r) g_previous.row[r] = g_atStart[r];
            if (!ApplyNewDisplay()) {
                dslog::Write("[fail] Video menu: the new display mode didn't work - settings put back");
                RestoreDisplay(g_previous);
                g_step = Step::None;
                break;
            }
            MarkApplied();
            g_reverting = false;
            g_step = Step::Reload;
            features::ConsoleStartLevel("FrontEnd.dll");
            break;
        case Step::Reload:
            if (BackOnPage()) {
                if (g_reverting) {
                    g_step = Step::None;
                } else {
                    g_step = Step::Confirm;
                    g_confirmEnd = GetTickCount() + 15000;
                }
            }
            break;
        case Step::Confirm:
            if (!ConfirmSeconds()) g_step = Step::Revert;
            break;
        case Step::Revert:
            dslog::Write("Video menu: new display mode not kept - going back");
            RestoreDisplay(g_previous);
            ApplyNewDisplay();
            MarkApplied();
            g_reverting = true;
            g_step = Step::Reload;
            features::ConsoleStartLevel("FrontEnd.dll");
            break;
        case Step::None:
            break;
    }
}

// "+restart <pid>" (APPLY & RESTART): wait for the old process to finish quitting before this one goes on, and start
// without the launcher (the player has just picked the settings in the game).
void features::ApplyRestart() {
    const char* cmd = GetCommandLineA();
    const char* at = cmd ? strstr(cmd, " +restart ") : nullptr;
    if (!at) return;
    g_restartedFrom = strtoul(at + 10, nullptr, 10);
    if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, g_restartedFrom)) {
        const DWORD r = WaitForSingleObject(old, 20000);
        CloseHandle(old);
        dslog::Write("Video menu: restarted from process %lu (%s)", g_restartedFrom,
                     r == WAIT_OBJECT_0 ? "it has quit" : "still running after 20 s");
    }
}

bool features::Restarting() { return g_restartedFrom != 0; }
