// Menu bar / HUD colour (OPTIONS > DESERTSTORMFIX > MENU BAR COLOUR, HUD COLOUR). Two targets, each a set of images
// on one of the game's image sheets whose green is moved to the player's colour:
//   menu: the menu art sheet [0x60EE1C] (8060.img at 800x600 and up, 6448.img below; one page, same numbering): 7 / 8
//     IMAGE_BUTTON_END / MID (label bars, FUN_00474bb0 -> FUN_004749d0(x0, y0, x1, y1, 7, 8)) and 12 / 14
//     IMAGE_SELECTION_END / MID (the rimmed bars: main menu buttons, choice values, FUN_004749a0 -> 0xC / 0xE) - green
//     HSV 95 / 0.84 / 0.22 on both sheets. A bar is drawn in pieces (caps, halves) whose filtered edges reach past the
//     images' rectangles, so the whole page's bar green is recoloured; the olive panel (IMAGE_BACK_*, hue 50) and the
//     white outlines (*_HI) are outside the hue window and stay.
//   hud: the GWInt sheet [0x60EE18] (5 pages): 61 IMAGE_PLAYER (soldier panel backing), 53 IMAGE_NAME_BACK (name bar),
//     93 IMAGE_WEAPON_BG (weapon / ammo panel and the inventory rows: FUN_004515f0.. push 0x5D), 36 IMAGE_HEALTH_BAR
//     (the frame; the fill is a coloured quad and stays a health colour), 22 IMAGE_CLOCK (mission timer). All hue
//     ~94; reference = WEAPON_BG's 95 / 0.69 / 0.34. Item pictures on the sheet (grenades, keycard, medipack ...) are
//     other images and stay as they are, as do the map markers (MAP_ALLY / MAP_SELECTED tell sides apart).
//
// The sheet image draws bind their page texture through the renderer's SetTexture wrapper 0x544220 - the plain draw
// FUN_0053c9e0 at 0x53CC9B (a shared pse.hooks chain, features.h kGlyphBind*: our GlyphBind subscribes there) and the
// stretched / tiled / rotated variants at the ten sites in kBindSites (hooked here; the menu buttons use the stretched
// ones 0x53D868 / 0x53DBD7). Bind hands out a copy of the page in which only the target images' rectangles are recoloured (hue
// shifted, saturation and value scaled by target / reference, weighted by how green a pixel is, so shading and light
// edges stay); rewritten when the colour changes (live while the picker is open), dropped with its original when the
// sheet stops using it (level loads swap sheets) or the device changes.
//
// Also with the HUD colour: the info bar (weapon name / fire mode / pick-ups, top of the view; FUN_00507b30) is a
// filled rectangle FUN_004d2bb0(this, x, y, w, h, fill r g b a, edge r g b a, flag) ret 0x34 with the HUD green
// (52, 87, 27) / (88, 113, 69) as constants - its call 0x507BEA is wrapped and both colours moved like the art.
//
// Readable text: menu item text is the game's yellow / white, so a light bar colour made it hard to read. The text
// colour setter thiscall 0x539F80(font, 0xAARRGGBB) is wrapped at the calls of the menu item draws that sit on a bar
// (kMenuTextSites) and at the HUD's soldier name / info bar text (kHudTextSites): when the text's WCAG contrast with
// the target's colour is below 3:1 it becomes near-black or near-white (whichever reads better), alpha kept.
//
// The picker: a hue / saturation wheel, a brightness bar and a preview of the target's own art, drawn inside the
// DESERTSTORMFIX panel (fixmenu.cpp hides its list while the picker is open). Mouse drag, arrows / WASD / D-pad /
// left stick on the wheel, Q / E, Page Up / Down, mouse wheel, L1 / R1 or L2 / R2 for brightness; Enter / Cross
// accepts, Esc / Circle cancels, Delete / Square goes back to the game's colour.
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "core/texpixels.h"
#include "features/features.h"
#include "features/overlay.h"

namespace {
constexpr uint32_t kSetTexture = 0x544220;
constexpr uint32_t kBindSites[] = {0x53D055, 0x53D2DA, 0x53D4E4, 0x53D868, 0x53DBD7,
                                   0x53DFF7, 0x53E218, 0x53E4E8, 0x53E81C, 0x53E940, 0x53EF56};
constexpr uint32_t kTextColour = 0x539F80, kBarFill = 0x4D2BB0, kInfoBarFillCall = 0x507BEA;
// text colour calls on menu bars: the list's own item text (0x4E6BDE in FUN_004e69b0: list +0x54 normal, +0x58
// selected, +0x5C disabled), item draws (plain 0x455A50, 0x455C70, choice 0x456040, 0x456350, 0x456770), the item
// text on the rimmed bars - main menu items, choice values - 0x456BAA in FUN_00456a70 (found with Dev\TextColourProbe),
// the Options screen's sliders / rows, KEY ASSIGNMENT
constexpr uint32_t kMenuTextSites[] = {0x456BAA, 0x4E6BDE, 0x455B2B, 0x455D83, 0x455FCB, 0x456123, 0x456437, 0x45664E, 0x45684E,
                                       0x4569E2, 0x4711E9, 0x47147E, 0x4714EB, 0x44608E, 0x44619D, 0x446204};
constexpr uint32_t kHudTextSites[] = {0x48E93C /* soldier panel name */, 0x507BFB /* info bar */};
constexpr uint32_t kRenderer = 0x63C924, kJoystickCount = 0x754BE8;
constexpr uint32_t kCursorX = 0x754C18, kCursorY = 0x754C1C, kMouseState = 0x754C24;  // DIMOUSESTATE: +8 lZ, +12 buttons
constexpr float kPi = 3.14159265f;

template <class T>
T& Global(uint32_t va) {
    return *reinterpret_cast<T*>(va);
}

// ---- colour maths ----
struct Hsv {
    float h, s, v;  // degrees 0..360, 0..1, 0..1
};

Hsv ToHsv(float r, float g, float b) {
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    Hsv o{0.0f, mx > 0 ? d / mx : 0.0f, mx};
    if (d > 0) {
        if (mx == r) o.h = 60.0f * std::fmod((g - b) / d + 6.0f, 6.0f);
        else if (mx == g) o.h = 60.0f * ((b - r) / d + 2.0f);
        else o.h = 60.0f * ((r - g) / d + 4.0f);
    }
    return o;
}

void ToRgb(Hsv c, float& r, float& g, float& b) {
    const float h = std::fmod(std::fmod(c.h, 360.0f) + 360.0f, 360.0f) / 60.0f;
    const float C = c.v * c.s, X = C * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f)), m = c.v - C;
    const int i = static_cast<int>(h);
    const float t[6][3] = {{C, X, 0}, {X, C, 0}, {0, C, X}, {0, X, C}, {X, 0, C}, {C, 0, X}};
    r = t[i % 6][0] + m, g = t[i % 6][1] + m, b = t[i % 6][2] + m;
}

uint32_t Pack(float r, float g, float b) {
    auto c = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return c(r) << 16 | c(g) << 8 | c(b);
}
uint32_t RgbOf(Hsv c) {
    float r, g, b;
    ToRgb(c, r, g, b);
    return Pack(r, g, b);
}
Hsv HsvOf(uint32_t rgb) {
    return ToHsv(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f);
}

// One pixel moved from the reference green to `to` (weighted by how close to it the pixel is, so edges blend).
uint32_t Recolour(uint32_t argb, Hsv ref, Hsv to) {
    const Hsv p = ToHsv(((argb >> 16) & 255) / 255.0f, ((argb >> 8) & 255) / 255.0f, (argb & 255) / 255.0f);
    const float dh = std::fmod(p.h - ref.h + 540.0f, 360.0f) - 180.0f;
    const float wHue = std::clamp((35.0f - std::fabs(dh)) / 15.0f, 0.0f, 1.0f);  // the olive panel (hue 50) is 45 away
    const float wSat = std::clamp((p.s - 0.12f) / 0.2f, 0.0f, 1.0f), w = wHue * wSat;
    if (w <= 0.0f) return argb;
    float r, g, b;
    ToRgb({to.h + dh, std::min(1.0f, p.s * to.s / ref.s), std::min(1.0f, p.v * to.v / ref.v)}, r, g, b);
    const float r0 = ((argb >> 16) & 255) / 255.0f, g0 = ((argb >> 8) & 255) / 255.0f, b0 = (argb & 255) / 255.0f;
    return (argb & 0xFF000000) | Pack(r0 + (r - r0) * w, g0 + (g - g0) * w, b0 + (b - b0) * w);
}

// ---- targets ----
struct Target {
    const char* name;
    uint32_t sheet;  // global holding the image sheet
    std::vector<int> images;  // find the pages; with wholePage false also the only rectangles recoloured
    bool wholePage;           // every reference-green pixel of the page (the menu bars are built from pieces whose
                              // samples reach past the images' own rectangles)
    Hsv ref;
    bool settings::Values::*custom;
    uint32_t settings::Values::*colour;
    uint32_t gameColour;  // the art's own colour (= the settings default)
};
const Target kTargets[] = {
    {"Menu bar colour", 0x60EE1C, {7, 8, 12, 14}, true, {95.0f, 0.842f, 0.2235f}, &settings::Values::menuBarCustom,
     &settings::Values::menuBarColour, 0x1D3909},
    {"HUD colour", 0x60EE18, {22, 36, 53, 61, 93}, false, {95.0f, 0.690f, 0.341f}, &settings::Values::hudColourCustom,
     &settings::Values::hudColour, 0x34571B},
};
constexpr int kTargetCount = static_cast<int>(sizeof kTargets / sizeof kTargets[0]);
constexpr int kHudPlayer = 61, kHudNameBack = 53, kHudWeaponBg = 93, kMenuEnd = 7, kMenuMid = 8;

// ---- the sheets ----
struct ImageRect {
    int page = -1, x = 0, y = 0, w = 0, h = 0;
};

const uint8_t* SheetOf(int target) { return Global<const uint8_t*>(kTargets[target].sheet); }

// Image i of the sheet (records +4, 24 bytes: +0 page, +0xA/+0xC texel x/y, +0xE/+0x10 size).
ImageRect ImageOf(const uint8_t* sheet, int i) {
    ImageRect r;
    if (!sheet) return r;
    const auto* header = *reinterpret_cast<const uint8_t* const*>(sheet);
    const auto* records = *reinterpret_cast<const uint8_t* const*>(sheet + 4);
    if (!header || !records || i >= *reinterpret_cast<const uint16_t*>(header + 6)) return r;
    const auto* rec = reinterpret_cast<const uint16_t*>(records + i * 0x18);
    r.page = rec[0], r.x = rec[5], r.y = rec[6], r.w = rec[7], r.h = rec[8];
    return r;
}

void* PageTexture(const uint8_t* sheet, int page) {
    if (!sheet || page < 0) return nullptr;
    const auto* pages = *reinterpret_cast<const uint8_t* const* const*>(sheet + 0xC);
    return pages && pages[page] ? *reinterpret_cast<void* const*>(pages[page] + 0x10) : nullptr;
}

// The target images on the page that is this texture (empty = not one of the target's pages).
std::vector<ImageRect> RectsOn(int target, void* tex) {
    std::vector<ImageRect> rects;
    const uint8_t* sheet = SheetOf(target);
    for (int i : kTargets[target].images) {
        const ImageRect r = ImageOf(sheet, i);
        if (r.w > 0 && PageTexture(sheet, r.page) == tex) rects.push_back(r);
    }
    return rects;
}

struct Page {
    int target;
    void* device;
    void* original;  // AddRef'd while the copy exists
    void* copy = nullptr;
    uint32_t w = 0, h = 0, colour = 0xFFFFFFFF;
    DWORD written = 0;
    std::vector<uint32_t> src;
    std::vector<ImageRect> rects;
};
std::vector<Page> g_pages;
std::vector<void*> g_notOurs;  // textures already checked this frame and not a target page (cleared every frame)

void Release(void* o) { reinterpret_cast<ULONG(__stdcall*)(void*)>((*static_cast<void***>(o))[2])(o); }
void AddRef(void* o) { reinterpret_cast<ULONG(__stdcall*)(void*)>((*static_cast<void***>(o))[1])(o); }

void Write(Page& p, uint32_t colour) {
    std::vector<uint32_t> px = p.src;
    const Hsv to = HsvOf(colour), ref = kTargets[p.target].ref;
    // a 2-texel margin: filtering at a piece's edge samples the texels just outside its image
    constexpr int kMargin = 2;
    const int W = static_cast<int>(p.w), H = static_cast<int>(p.h);
    std::vector<uint8_t> done(px.size(), 0);
    for (const ImageRect& r : p.rects)
        for (int y = std::max(0, r.y - kMargin); y < std::min(H, r.y + r.h + kMargin); ++y)
            for (int x = std::max(0, r.x - kMargin); x < std::min(W, r.x + r.w + kMargin); ++x)
                if (!done[size_t(y) * W + x]++) px[size_t(y) * W + x] = Recolour(px[size_t(y) * W + x], ref, to);
    p.written = GetTickCount();
    if (overlay::UploadTexture(p.copy, px.data(), static_cast<int>(p.w), static_cast<int>(p.h))) {
        p.colour = colour;
        return;
    }
    dslog::Write("[fail] %s: recoloured page not written - the game's colour stays", kTargets[p.target].name);
    overlay::ReleaseTexture(p.copy);
    p.copy = nullptr;
}

// The page entry for a texture holding target images (made on first use), or null.
Page* PageFor(void* tex) {
    if (!tex) return nullptr;
    for (Page& p : g_pages)
        if (p.original == tex) return &p;
    if (std::find(g_notOurs.begin(), g_notOurs.end(), tex) != g_notOurs.end()) return nullptr;
    for (int t = 0; t < kTargetCount; ++t) {
        std::vector<ImageRect> rects = RectsOn(t, tex);
        if (rects.empty()) continue;
        Page p{t, overlay::CurrentDevice(), tex};
        p.src = texpix::ReadTexture(tex, p.w, p.h);
        if (kTargets[t].wholePage) rects = {ImageRect{rects[0].page, 0, 0, static_cast<int>(p.w), static_cast<int>(p.h)}};
        p.rects = std::move(rects);
        if (!p.src.empty()) p.copy = overlay::CreateTexture(static_cast<int>(p.w), static_cast<int>(p.h));
        if (!p.copy) dslog::Write("[fail] %s: art page %ux%u can't be copied", kTargets[t].name, p.w, p.h);
        else dslog::Write("%s: art page %ux%u copied (%u images)", kTargets[t].name, p.w, p.h, unsigned(p.rects.size()));
        AddRef(tex);
        g_pages.push_back(std::move(p));
        return &g_pages.back();
    }
    if (g_notOurs.size() < 256) g_notOurs.push_back(tex);
    return nullptr;
}

// ---- picker state ----
enum class Picker { Closed, Open, Closing };
Picker g_picker = Picker::Closed;
int g_target = 0;
Hsv g_pick{};
// OpenColourPicker (a pse.settings colour on a plugin's settings page): target kTargetCount, no art of its own
struct ExternalTarget {
    uint32_t def = 0;
    features::ColourDone done = nullptr;
    void* ctx = nullptr;
} g_ext;
bool External() { return g_target == kTargetCount; }
uint32_t GameColour() { return External() ? g_ext.def : kTargets[g_target].gameColour; }
bool g_waitRelease = false;
enum class Drag { None, Wheel, Bar } g_drag = Drag::None;
DWORD g_lastTick = 0;
bool g_prevAccept = false, g_prevCancel = false, g_prevReset = false;

// The colour a target is drawn in right now (picker preview first), or none = the game's own art.
bool CurrentColour(int target, uint32_t& colour) {
    if (g_picker == Picker::Open && target == g_target) {
        colour = RgbOf(g_pick);
        return true;
    }
    colour = settings::Get().*kTargets[target].colour;
    return settings::Get().*kTargets[target].custom;
}

// The texture to bind for one of a target's pages: the recoloured copy, or the original.
void* Bind(void* original) {
    Page* p = PageFor(original);
    uint32_t colour;
    if (!p || !p->copy || !CurrentColour(p->target, colour)) return original;
    // live picker drags: at most ~40 rewrites a second
    if (p->colour != colour && (p->colour == 0xFFFFFFFF || GetTickCount() - p->written >= 25)) Write(*p, colour);
    return p->copy && p->colour != 0xFFFFFFFF ? p->copy : original;
}

// ---- readable text / info bar ----
float Luminance(uint32_t rgb) {
    auto c = [](uint32_t v) {
        const float f = v / 255.0f;
        return f <= 0.03928f ? f / 12.92f : std::pow((f + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * c((rgb >> 16) & 255) + 0.7152f * c((rgb >> 8) & 255) + 0.0722f * c(rgb & 255);
}
float Contrast(uint32_t a, uint32_t b) {
    const float la = Luminance(a), lb = Luminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}
// argb unchanged while it reads on `bar`, else near-black / near-white with its alpha
uint32_t Readable(uint32_t argb, uint32_t bar) {
    if (Contrast(argb & 0xFFFFFF, bar) >= 3.0f) return argb;
    constexpr uint32_t kDark = 0x141414, kLight = 0xF4F4F4;
    return (argb & 0xFF000000) | (Contrast(kDark, bar) >= Contrast(kLight, bar) ? kDark : kLight);
}

using TextColourFn = void(__thiscall*)(void*, uint32_t);
void __fastcall MenuTextColour(void* font, void* /*edx*/, uint32_t argb) {
    uint32_t bar;
    if (CurrentColour(0, bar)) argb = Readable(argb, bar);
    reinterpret_cast<TextColourFn>(kTextColour)(font, argb);
}
void __fastcall HudTextColour(void* font, void* /*edx*/, uint32_t argb) {
    uint32_t bar;
    if (CurrentColour(1, bar)) argb = Readable(argb, bar);
    reinterpret_cast<TextColourFn>(kTextColour)(font, argb);
}

#ifndef DS_DIST
// dev: Dev\TextColourProbe = 1 replaces the setter and logs each new caller while a front-end screen is up
bool TextProbeOn() {
    static const bool on = [] {
        DWORD v = 0, size = sizeof v;
        RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "TextColourProbe", RRF_RT_REG_DWORD, nullptr, &v,
                     &size);
        return v != 0;
    }();
    return on;
}
void __fastcall ProbeTextColour(void* font, void* /*edx*/, uint32_t argb) {
    static std::vector<uintptr_t> seen;
    const auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (std::find(seen.begin(), seen.end(), ret) == seen.end() && seen.size() < 400) {
        seen.push_back(ret);
        dslog::Write("[dev]  text colour %08X from 0x%08X (front-end state 0x%X)", argb, unsigned(ret - 5),
                     Global<uint32_t>(0x617C18));
    }
    // the original body: sheet = font +0, FUN_0053bd40(sheet, argb, 0)
    reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(0x53BD40)(*static_cast<void**>(font), argb, 0);
}
#endif

using BarFillFn = void(__thiscall*)(void*, int, int, int, int, int, int, int, int, int, int, int, int, int);
void __fastcall InfoBarFill(void* self, void* /*edx*/, int x, int y, int w, int h, int r, int g, int b, int a, int r2,
                            int g2, int b2, int a2, int flag) {
    uint32_t colour;
    if (CurrentColour(1, colour)) {
        const Hsv to = HsvOf(colour), ref = kTargets[1].ref;
        const uint32_t fill = Recolour(0xFF000000u | uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b), ref, to);
        const uint32_t edge = Recolour(0xFF000000u | uint32_t(r2) << 16 | uint32_t(g2) << 8 | uint32_t(b2), ref, to);
        r = (fill >> 16) & 255, g = (fill >> 8) & 255, b = fill & 255;
        r2 = (edge >> 16) & 255, g2 = (edge >> 8) & 255, b2 = edge & 255;
    }
    reinterpret_cast<BarFillFn>(kBarFill)(self, x, y, w, h, r, g, b, a, r2, g2, b2, a2, flag);
}

using SetTextureFn = void(__thiscall*)(void*, DWORD, void*);
void __fastcall SetTexture(void* renderer, void* /*edx*/, DWORD stage, void* tex) {
    if (tex) tex = Bind(tex);
    reinterpret_cast<SetTextureFn>(kSetTexture)(renderer, stage, tex);
}

// Subscriber of the plain image draw's bind 0x53CC9B (priority kGlyphBindBarColour: after a mod weapon's picture,
// before the sharp font pages).
SetTextureFn g_glyphNext = nullptr;
void __fastcall GlyphBind(void* renderer, void* /*edx*/, DWORD stage, void* tex) {
    if (tex) tex = Bind(tex);
    g_glyphNext(renderer, stage, tex);
}

// ---- picker layout (back-buffer pixels, inside the panel) ----
struct Layout {
    float u;               // 1 at 600 lines
    float cx, cy, r;       // wheel
    float bx, by, bw, bh;  // brightness bar
    float px, py, pw;      // preview (left, top, width)
    float textY;           // colour value line
};
Layout g_layout{};

Layout MakeLayout(void* screen) {
    const auto* rd = Global<const uint8_t*>(kRenderer);
    const float W = rd ? static_cast<float>(*reinterpret_cast<const int*>(rd + 0x40688)) : 800.0f;
    const float H = rd ? static_cast<float>(*reinterpret_cast<const int*>(rd + 0x4068C)) : 600.0f;
    Layout l{};
    l.u = H / 600.0f;
    // the panel (screen +0x10 top, +0x14 bottom, +0x18 left, +0x1C right); the title takes its top
    float top = H * 0.2f, bottom = H * 0.8f, left = W * 0.2f, right = W * 0.8f;
    if (screen) {
        const auto* s = static_cast<const uint8_t*>(screen);
        const float t = *reinterpret_cast<const float*>(s + 0x10), b = *reinterpret_cast<const float*>(s + 0x14);
        const float lf = *reinterpret_cast<const float*>(s + 0x18), rt = *reinterpret_cast<const float*>(s + 0x1C);
        if (b - t > 100.0f * l.u && rt - lf > 200.0f * l.u) top = t, bottom = b, left = lf, right = rt;
    }
    top += 70.0f * l.u;     // under the title
    bottom -= 50.0f * l.u;  // room for the prompt line
    const float width = right - left, height = bottom - top;
    l.r = std::max(40.0f * l.u, std::min(height * 0.5f, width * 0.22f));
    const float bar = 24.0f * l.u, gap = 30.0f * l.u, preview = std::min(width * 0.32f, 220.0f * l.u);
    const float total = 2 * l.r + gap + bar + gap + preview;
    const float x0 = left + (width - total) / 2;
    l.cx = x0 + l.r, l.cy = top + height / 2;
    l.bx = x0 + 2 * l.r + gap, l.by = l.cy - l.r, l.bw = bar, l.bh = 2 * l.r;
    l.px = l.bx + bar + gap, l.pw = preview, l.py = l.cy - l.r;
    return l;
}

// ---- picker textures: hue / saturation disc and the same disc in black (darkened by brightness) ----
void* g_wheel = nullptr;
void* g_shade = nullptr;
void* g_texDevice = nullptr;
constexpr int kWheelSize = 256;

void EnsureWheel() {
    void* dev = overlay::CurrentDevice();
    if (g_wheel && g_texDevice == dev) return;
    g_wheel = overlay::CreateTexture(kWheelSize, kWheelSize);
    g_shade = overlay::CreateTexture(kWheelSize, kWheelSize);
    g_texDevice = dev;
    if (!g_wheel || !g_shade) return;
    std::vector<uint32_t> wheel(kWheelSize * kWheelSize), shade(kWheelSize * kWheelSize);
    const float half = kWheelSize / 2.0f;
    for (int y = 0; y < kWheelSize; ++y)
        for (int x = 0; x < kWheelSize; ++x) {
            const float dx = (x + 0.5f - half) / half, dy = (y + 0.5f - half) / half, d = std::sqrt(dx * dx + dy * dy);
            const float a = std::clamp((1.0f - d) * half, 0.0f, 1.0f);
            const float hue = std::atan2(-dy, dx) * 180.0f / kPi;
            const uint32_t alpha = static_cast<uint32_t>(a * 255.0f + 0.5f) << 24;
            wheel[y * kWheelSize + x] = alpha | RgbOf({hue, std::min(d, 1.0f), 1.0f});
            shade[y * kWheelSize + x] = alpha;
        }
    overlay::UploadTexture(g_wheel, wheel.data(), kWheelSize, kWheelSize);
    overlay::UploadTexture(g_shade, shade.data(), kWheelSize, kWheelSize);
}

void Marker(float x, float y, float size, float u) {
    const float t = std::max(1.0f, 2.0f * u);
    for (int pass = 0; pass < 2; ++pass) {
        const float s = pass ? size - t : size;
        const uint32_t c = pass ? 0xFFFFFFFF : 0xFF000000;
        overlay::QueueLine(x - s, y - s, x + s, y - s, t, c);
        overlay::QueueLine(x + s, y - s, x + s, y + s, t, c);
        overlay::QueueLine(x + s, y + s, x - s, y + s, t, c);
        overlay::QueueLine(x - s, y + s, x - s, y - s, t, c);
    }
}

// One sheet image (texel rectangle r) at x0..x1 / y..y+h, from the texture the game would bind for it now.
void QueueImagePart(int target, const ImageRect& r, float x0, float x1, float y, float h, bool mirror) {
    void* original = PageTexture(SheetOf(target), r.page);
    if (!original) return;
    void* tex = Bind(original);
    const Page* p = PageFor(original);
    if (!p || !p->w || !p->h) return;
    const float tw = static_cast<float>(p->w), th = static_cast<float>(p->h);
    const float corners[4][2] = {{x0, y}, {x1, y}, {x0, y + h}, {x1, y + h}};
    float u0 = r.x / tw, u1 = (r.x + r.w) / tw;
    if (mirror) std::swap(u0, u1);
    overlay::QueueScreenQuad(tex, corners, u0, r.y / th, u1, (r.y + r.h) / th);
}

// Preview of the target's art in the current colour; returns its height.
float DrawPreview(const Layout& l) {
    if (External()) {  // a plain swatch of the colour
        const float h = l.pw * 0.35f;
        overlay::QueueRect(l.px - l.u, l.py - l.u, l.pw + 2 * l.u, h + 2 * l.u, 0xFF000000);
        overlay::QueueRect(l.px, l.py, l.pw, h, 0xFF000000 | RgbOf(g_pick));
        return h;
    }
    const uint8_t* sheet = SheetOf(g_target);
    if (g_target == 0) {  // a menu button: end cap, stretched middle, mirrored end cap
        const ImageRect end = ImageOf(sheet, kMenuEnd), mid = ImageOf(sheet, kMenuMid);
        if (end.h <= 0) return 0;
        const float scale = 2.0f * l.u * 22.0f / end.h;  // twice the 800x600 button height
        const float h = end.h * scale, capW = end.w * scale, x0 = l.px, x1 = l.px + l.pw;
        QueueImagePart(0, end, x0, x0 + capW, l.py, h, false);
        QueueImagePart(0, mid, x0 + capW, x1 - capW, l.py, h, false);
        QueueImagePart(0, end, x1 - capW, x1, l.py, h, true);
        return h;
    }
    // the HUD: soldier panel (backing + name bar) and the weapon panel next to it, as in a mission
    const ImageRect player = ImageOf(sheet, kHudPlayer), name = ImageOf(sheet, kHudNameBack),
                    weapon = ImageOf(sheet, kHudWeaponBg);
    if (player.w <= 0 || weapon.w <= 0) return 0;
    const float gap = 6.0f;  // texels
    const float scale = l.pw / (static_cast<float>(std::max(player.w, name.w)) + gap + weapon.w);
    float x = l.px;
    QueueImagePart(1, player, x, x + player.w * scale, l.py, player.h * scale, false);
    const float nameY = l.py + (player.h + 2) * scale;
    if (name.w > 0) QueueImagePart(1, name, x, x + name.w * scale, nameY, name.h * scale, false);
    x += (static_cast<float>(std::max(player.w, name.w)) + gap) * scale;
    QueueImagePart(1, weapon, x, x + weapon.w * scale, l.py, weapon.h * scale, false);
    return std::max(nameY + name.h * scale, l.py + weapon.h * scale) - l.py;
}

// ---- picker input ----
uint32_t PadBits() {
    uint32_t pad = 0;
    for (int j = 0, n = std::min(Global<int>(kJoystickCount), 8); j < n; ++j) pad |= features::ReadJoystick(j);
    return pad;
}
bool MouseDown() { return (Global<uint8_t>(kMouseState + 12) & 0x80) != 0; }

void Close(bool accept) {
    if (External()) {
        if (accept && g_ext.done) g_ext.done(RgbOf(g_pick), g_ext.ctx);
    } else if (accept) {
        const Target& t = kTargets[g_target];
        settings::Values v = settings::Get();
        v.*t.colour = RgbOf(g_pick);
        v.*t.custom = v.*t.colour != t.gameColour;
        const bool ok = settings::Save(v);
        dslog::Write(ok ? "DesertStormFix menu: %s #%06X%s" : "[fail] DesertStormFix menu: %s #%06X%s (not saved)",
                     t.name, v.*t.colour, v.*t.custom ? "" : " (the game's)");
    }
    g_picker = Picker::Closing;  // the list waits until the key / button is let go
    g_drag = Drag::None;
}

void LateText();
}  // namespace

void features::OnFrameBarColour() {
    g_notOurs.clear();
    void* dev = overlay::CurrentDevice();
    for (auto it = g_pages.begin(); it != g_pages.end();) {
        if (it->device == dev && !RectsOn(it->target, it->original).empty()) {
            ++it;
            continue;
        }
        if (it->copy) overlay::ReleaseTexture(it->copy);
        if (it->device == dev) Release(it->original);  // a lost device took its textures with it
        it = g_pages.erase(it);
    }
}

void features::ApplyBarColour() {
    static bool hooked = false;
    if (hooked) return;
    hooked = true;
    if (!patch::SubscribeCall(features::kGlyphBindSite, reinterpret_cast<const void*>(&GlyphBind),
                              features::kGlyphBindTarget, features::kGlyphBindBarColour,
                              reinterpret_cast<void**>(&g_glyphNext)))
        dslog::Write("[fail] Menu / HUD colour: plain image texture bind 0x%08X not hooked", features::kGlyphBindSite);
    int ok = 0;
    for (uint32_t site : kBindSites)
        if (patch::HookCall(site, reinterpret_cast<const void*>(&SetTexture), kSetTexture)) ++ok;
    dslog::Write(ok == static_cast<int>(sizeof kBindSites / sizeof kBindSites[0])
                     ? "Menu / HUD colour: image texture binds hooked (%d)"
                     : "[fail] Menu / HUD colour: only %d image texture binds hooked",
                 ok);
    int text = 0, textSites = 0;
    for (uint32_t site : kMenuTextSites)
        ++textSites, text += patch::HookCall(site, reinterpret_cast<const void*>(&MenuTextColour), kTextColour) ? 1 : 0;
    for (uint32_t site : kHudTextSites)
        ++textSites, text += patch::HookCall(site, reinterpret_cast<const void*>(&HudTextColour), kTextColour) ? 1 : 0;
#ifndef DS_DIST
    if (TextProbeOn())  // dev: replaces the setter (an entry hook that never runs the original)
        patch::HookFunction(kTextColour, reinterpret_cast<const void*>(&ProbeTextColour), nullptr);
#endif
    const bool bar = patch::HookCall(kInfoBarFillCall, reinterpret_cast<const void*>(&InfoBarFill), kBarFill);
    dslog::Write(text == textSites && bar ? "Menu / HUD colour: text colours (%d) + info bar hooked"
                                          : "[fail] Menu / HUD colour: text colours %d of %d, info bar %d",
                 text, textSites, bar ? 1 : 0);
}

void features::OpenBarColourPicker(int target) {
    g_target = std::clamp(target, 0, kTargetCount - 1);
    g_pick = HsvOf(settings::Get().*kTargets[g_target].colour);
    g_picker = Picker::Open;
    g_waitRelease = true;  // the Enter / Cross / click that opened it
    g_drag = Drag::None;
    g_lastTick = GetTickCount();
    overlay::AddLateDraw(&LateText);
}

void features::OpenColourPicker(uint32_t colour, uint32_t defaultColour, ColourDone done, void* ctx) {
    g_target = kTargetCount;
    g_ext.def = defaultColour & 0xFFFFFF;
    g_ext.done = done;
    g_ext.ctx = ctx;
    g_pick = HsvOf(colour & 0xFFFFFF);
    g_picker = Picker::Open;
    g_waitRelease = true;
    g_drag = Drag::None;
    g_lastTick = GetTickCount();
    overlay::AddLateDraw(&LateText);
}

// Dev server (tests): the wheel without input - "set" a colour, "ok" (= Enter) or "cancel" (= Esc). False = not open.
bool features::ColourPickerDev(const char* op, uint32_t colour) {
    if (g_picker != Picker::Open || !op) return false;
    g_waitRelease = false;
    if (std::strcmp(op, "set") == 0) g_pick = HsvOf(colour & 0xFFFFFF);
    else if (std::strcmp(op, "ok") == 0) Close(true);
    else if (std::strcmp(op, "cancel") == 0) Close(false);
    else return false;
    // no key closed it: nothing to wait for (in background runs the stale mouse / key state can look held for ever)
    if (g_picker == Picker::Closing) g_picker = Picker::Closed;
    return true;
}
uint32_t features::ColourPickerColour() { return RgbOf(g_pick); }

bool features::BarColourPickerOpen() { return g_picker != Picker::Closed; }
int features::BarColourPickerTarget() { return g_picker == Picker::Open ? g_target : -1; }

void features::UpdateBarColourPicker() {
    const bool front = features::GameFocused();
    const uint32_t pad = front ? PadBits() : 0;
    auto key = [&](int vk) { return front && features::KeyHeld(vk); };
    const bool accept = key(VK_RETURN) || key(VK_SPACE) || (pad & 1u << 2);
    const bool cancel = key(VK_ESCAPE) || key(VK_BACK) || (pad & 1u << 1);
    const bool reset = key(VK_DELETE) || (pad & 1u << 3);
    const bool mouse = front && MouseDown();
    const bool anyHeld = accept || cancel || reset || mouse || (pad & 1u << 0);
    if (g_picker == Picker::Closing) {
        if (!anyHeld) g_picker = Picker::Closed;
        return;
    }
    if (g_picker != Picker::Open) return;
    const DWORD now = GetTickCount();
    const float dt = std::min(0.1f, (now - g_lastTick) / 1000.0f);
    g_lastTick = now;
    if (g_waitRelease) {
        g_waitRelease = anyHeld;
        g_prevAccept = accept, g_prevCancel = cancel, g_prevReset = reset;
        return;
    }
    const bool pressAccept = accept && !g_prevAccept, pressCancel = cancel && !g_prevCancel;
    const bool pressReset = reset && !g_prevReset;
    g_prevAccept = accept, g_prevCancel = cancel, g_prevReset = reset;
    if (pressCancel) return Close(false);
    if (pressAccept) return Close(true);
    if (pressReset) g_pick = HsvOf(GameColour());

    const Layout& l = g_layout;
    if (l.r <= 0) return;
    // mouse: a press on the wheel / bar starts a drag
    const float mx = static_cast<float>(Global<int>(kCursorX)), my = static_cast<float>(Global<int>(kCursorY));
    if (!mouse) g_drag = Drag::None;
    else if (g_drag == Drag::None) {
        const float dx = mx - l.cx, dy = my - l.cy;
        if (dx * dx + dy * dy <= (l.r + 6 * l.u) * (l.r + 6 * l.u)) g_drag = Drag::Wheel;
        else if (mx >= l.bx - 8 * l.u && mx <= l.bx + l.bw + 8 * l.u && my >= l.by - 8 * l.u && my <= l.by + l.bh + 8 * l.u)
            g_drag = Drag::Bar;
    }
    // point on the wheel (unit disc, y down)
    float px = std::cos(g_pick.h * kPi / 180.0f) * g_pick.s, py = -std::sin(g_pick.h * kPi / 180.0f) * g_pick.s;
    if (g_drag == Drag::Wheel) px = (mx - l.cx) / l.r, py = (my - l.cy) / l.r;
    else if (g_drag == Drag::Bar) g_pick.v = std::clamp(1.0f - (my - l.by) / l.bh, 0.0f, 1.0f);
    const float sx = static_cast<float>((key(VK_RIGHT) || key('D') || (pad & (1u << 13 | 1u << 17))) -
                                        (key(VK_LEFT) || key('A') || (pad & (1u << 15 | 1u << 19))));
    const float sy = static_cast<float>((key(VK_DOWN) || key('S') || (pad & (1u << 14 | 1u << 18))) -
                                        (key(VK_UP) || key('W') || (pad & (1u << 12 | 1u << 16))));
    px += sx * 0.8f * dt, py += sy * 0.8f * dt;
    const float d = std::sqrt(px * px + py * py);
    if (d > 1.0f) px /= d, py /= d;
    if (g_drag == Drag::Wheel || sx || sy) {
        g_pick.s = std::min(d, 1.0f);
        if (g_pick.s > 0.002f) g_pick.h = std::fmod(std::atan2(-py, px) * 180.0f / kPi + 360.0f, 360.0f);
    }
    const float sv = static_cast<float>((key('E') || key(VK_PRIOR) || (pad & (1u << 5 | 1u << 7))) -
                                        (key('Q') || key(VK_NEXT) || (pad & (1u << 4 | 1u << 6))));
    const int wheel = front ? Global<int>(kMouseState + 8) : 0;  // lZ, 120 a notch
    g_pick.v = std::clamp(g_pick.v + sv * 0.6f * dt + wheel / 120.0f * 0.04f, 0.0f, 1.0f);
}

bool features::DrawBarColourPicker(void* screen) {
    if (g_picker != Picker::Open) return false;
    Layout l = MakeLayout(screen);
    EnsureWheel();
    if (g_wheel && g_shade) {
        overlay::QueueImage(g_wheel, l.cx - l.r, l.cy - l.r, 2 * l.r, 2 * l.r);
        overlay::QueueImage(g_shade, l.cx - l.r, l.cy - l.r, 2 * l.r, 2 * l.r, 1.0f, 1.0f,
                            (1.0f - g_pick.v) * overlay::MenuAlpha());
    }
    const float a = g_pick.h * kPi / 180.0f;
    Marker(l.cx + std::cos(a) * g_pick.s * l.r, l.cy - std::sin(a) * g_pick.s * l.r, 7 * l.u, l.u);
    // brightness: the picked hue / saturation from full (top) to black
    constexpr int kSteps = 32;
    for (int i = 0; i < kSteps; ++i) {
        const float v = 1.0f - (i + 0.5f) / kSteps;
        overlay::QueueRect(l.bx, l.by + l.bh * i / kSteps, l.bw, l.bh / kSteps + 1.0f,
                           0xFF000000 | RgbOf({g_pick.h, g_pick.s, v}));
    }
    const float my = l.by + (1.0f - g_pick.v) * l.bh, t = std::max(1.0f, 2.0f * l.u);
    overlay::QueueRect(l.bx - 4 * l.u, my - t * 1.5f, l.bw + 8 * l.u, t * 3, 0xFF000000);
    overlay::QueueRect(l.bx - 3 * l.u, my - t * 0.5f, l.bw + 6 * l.u, t, 0xFFFFFFFF);
    l.textY = l.py + DrawPreview(l) + 16.0f * l.u;
    g_layout = l;
    return true;
}

namespace {
// Text over the shapes: the colour's value and how to change it.
void LateText() {
    if (g_picker != Picker::Open || g_layout.r <= 0) return;
    void* font = overlay::MenuFont();
    if (!font) return;
    const Layout& l = g_layout;
    const float lineH = 26.0f * l.u;
    char hex[64];
    const uint32_t c = RgbOf(g_pick);
    std::snprintf(hex, sizeof hex, "#%06X%s", c, c == GameColour() ? (External() ? "  DEFAULT" : "  GAME COLOUR") : "");
    overlay::DrawLabel(font, hex, static_cast<int>(l.px), static_cast<int>(l.textY + lineH * 0.6f), 0xFFE0D878);

    const bool kb = features::KeyboardInUse();
    const bool xb = features::PadStyleXbox();
    const bool glyphs = overlay::GlyphsReady();
    auto pad = [&](int b) {  // the pad icon block: the game's buttons 0-15, then D-pad pairs, 18 = left stick
        return overlay::IconChar(static_cast<overlay::Icon>(
            static_cast<int>(xb ? overlay::Icon::XbTriangle : overlay::Icon::PsTriangle) + b));
    };
    auto icon = [&](overlay::Icon kbIcon, int padButton, const char* plain) -> std::string {
        if (!glyphs) return plain;
        return kb ? overlay::IconChar(kbIcon) : pad(padButton);
    };
    const std::string hints[] = {
        icon(overlay::Icon::KbArrows, 18, "ARROWS") + " COLOUR",
        (glyphs ? (kb ? std::string(overlay::IconChar(overlay::Icon::KbQ)) + overlay::IconChar(overlay::Icon::KbE)
                      : std::string(pad(6)) + pad(7))
                : std::string("Q / E")) + " BRIGHTNESS",
        icon(overlay::Icon::KbDelete, 3, "DELETE") + (External() ? " DEFAULT" : " GAME COLOUR"),
        icon(overlay::Icon::KbEnter, 2, "ENTER") + " OK   " + icon(overlay::Icon::KbEscape, 1, "ESC") + " CANCEL",
    };
    float y = std::max(l.textY + lineH * 2.0f, l.py + 2 * l.r - lineH * 3.4f);
    for (const std::string& text : hints) {
        overlay::DrawLabel(font, text.c_str(), static_cast<int>(l.px), static_cast<int>(y), 0xFFFFFFFF);
        y += lineH;
    }
}
}  // namespace
