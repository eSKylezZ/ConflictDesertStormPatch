// Sharper text. Both game fonts (small [0x60EDB4], large [0x60EDB8]) draw from one bitmap sheet [0x60EDB0] made for
// 800x600; with HUD scaling the sheet is drawn k = min(W/800, H/600) times larger (2.4 at 2560x1440), so every
// glyph is a bilinear blow-up of ~13 px letters - soft edges on the loading screen, menus, HUD.
//
// Every glyph goes through the image draw FUN_0053c9e0, which binds its page's texture with the renderer's
// SetTexture wrapper (thiscall 0x544220(stage, texture), call at 0x53CC9B; page = sheet +0xC [record +0], texture
// = page +0x10). That call is hooked: a texture of the font sheet is replaced by our copy, n = ceil(k) (2..4) times
// larger - bilinearly enlarged with the alpha edge steepened back to ~1 px, i.e. the glyph outlines the GPU would
// draw, but crisp, and more colour contrast inside letter boxes (their anti-aliasing is partly in the colour). Texture coordinates are fractions of the texture, so the copy lines up without other changes.
// The game keeps owning its textures; we hold a reference to each original while its copy exists (so its address
// can't be reused by another texture) and drop both once the font sheet no longer uses it.
//
// Its own PSE plugin since 2026-10-24 (PSE\Plugins\dsfix.fontsharp.dll, on the dsfix kit and dsfix.services). Setting
// FeatSharpText in its own key; HudScaling is dsfix.dll's (DsfixSetting). Without the DLL: the game's soft text.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/texpixels.h"
#include "kit/kit.h"

namespace {
using namespace texpix;
// the shared texture bind of the game's image draw FUN_0053c9e0 (dsfix.dll features.h kGlyphBind*)
constexpr uint32_t kSetTextureSite = 0x53CC9B, kGlyphBindTarget = 0x544220;
constexpr int32_t kGlyphBindFontSharp = 0;
constexpr uint32_t kFontSheet = 0x60EDB0, kDevice = 0x755050;
constexpr uint32_t kFonts[] = {0x60EDB4, 0x60EDB8};  // small, large: +0 sheet, +0x20 char -> glyph u16[], +0x24 size
constexpr int kMaxSize = 4096;
constexpr float kLetterContrast = 1.8f;
bool g_settingOn = true, g_hudScaling = true;

struct Copy {
    void* original;       // referenced by us while its copies exist
    int page;             // of the font sheet
    void* sharp[5] = {};  // per factor 2..4
    bool tried[5] = {};   // made or failed - not retried
};
std::vector<Copy> g_copies;

// n times larger: bilinear (premultiplied, like the GPU would filter it), then alpha pushed away from 0.5 so an
// edge ramps over ~1 output pixel instead of n. The letters also carry their anti-aliasing in the colour (grey
// edges inside a dark outline), so inside letter boxes (mask) the colour gets more contrast too; other images on
// the sheet (pad buttons) keep their shading.
std::vector<uint32_t> Enlarge(const std::vector<uint32_t>& src, uint32_t w, uint32_t h, int n,
                              const std::vector<uint8_t>& letters) {
    const uint32_t W = w * n, H = h * n;
    std::vector<uint32_t> out(size_t(W) * H);
    auto ch = [&](uint32_t x, uint32_t y, int s) { return float((src[size_t(y) * w + x] >> s) & 255); };
    const float steep = float(n);
    for (uint32_t Y = 0; Y < H; ++Y) {
        const float sy = std::clamp((Y + 0.5f) / n - 0.5f, 0.0f, float(h - 1));
        const uint32_t y0 = uint32_t(sy), y1 = std::min(y0 + 1, h - 1);
        const float fy = sy - y0;
        for (uint32_t X = 0; X < W; ++X) {
            const float sx = std::clamp((X + 0.5f) / n - 0.5f, 0.0f, float(w - 1));
            const uint32_t x0 = uint32_t(sx), x1 = std::min(x0 + 1, w - 1);
            const float fx = sx - x0;
            const float wts[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
            const uint32_t xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
            float a = 0, rgb[3] = {};
            for (int k = 0; k < 4; ++k) {
                const float pa = ch(xs[k], ys[k], 24) * wts[k];
                a += pa;
                for (int c = 0; c < 3; ++c) rgb[c] += ch(xs[k], ys[k], 16 - 8 * c) * pa;
            }
            uint32_t v = 0;
            if (a > 0) {
                const float sharpA = std::clamp((a / 255.0f - 0.5f) * steep + 0.5f, 0.0f, 1.0f);
                const bool letter = letters[size_t(uint32_t(sy + 0.5f)) * w + uint32_t(sx + 0.5f)] != 0;
                v = uint32_t(sharpA * 255.0f + 0.5f) << 24;
                for (int c = 0; c < 3; ++c) {
                    float col = std::clamp(rgb[c] / a, 0.0f, 255.0f);
                    if (letter) col = std::clamp((col - 127.5f) * kLetterContrast + 127.5f, 0.0f, 255.0f);
                    v |= uint32_t(col + 0.5f) << (16 - 8 * c);
                }
            }
            out[size_t(Y) * W + X] = v;
        }
    }
    return out;
}

void* MakeSharp(void* original, int n, const std::vector<uint8_t>& (*letters)(uint32_t, uint32_t)) {
    SurfaceDesc d{};
    if (FAILED(M<HRESULT(__stdcall*)(void*, UINT, SurfaceDesc*)>(original, kGetLevelDesc)(original, 0, &d))) return nullptr;
    if (d.width * n > kMaxSize || d.height * n > kMaxSize) return nullptr;
    const std::vector<uint32_t> src = ReadPixels(original, d);
    if (src.empty()) {
        dslog::Write("[fail] Sharp text: font texture %ux%u format 0x%X pool %u can't be read", d.width, d.height,
                     d.format, d.pool);
        return nullptr;
    }
    void* dev = *reinterpret_cast<void**>(kDevice);
    void* tex = nullptr;
    using Create = HRESULT(__stdcall*)(void*, UINT, UINT, UINT, DWORD, uint32_t, uint32_t, void**);
    if (!dev || FAILED(M<Create>(dev, kCreateTexture)(dev, d.width * n, d.height * n, 1, 0, kA8R8G8B8, kPoolManaged,
                                                      &tex)) || !tex)
        return nullptr;
    const std::vector<uint32_t> big = Enlarge(src, d.width, d.height, n, letters(d.width, d.height));
    Locked lr{};
    using Lock = HRESULT(__stdcall*)(void*, UINT, Locked*, const RECT*, DWORD);
    if (FAILED(M<Lock>(tex, kLockRect)(tex, 0, &lr, nullptr, 0))) {
        Release(tex);
        return nullptr;
    }
    for (uint32_t y = 0; y < d.height * n; ++y)
        memcpy(static_cast<uint8_t*>(lr.bits) + size_t(y) * lr.pitch, &big[size_t(y) * d.width * n], d.width * n * 4);
    M<HRESULT(__stdcall*)(void*, UINT)>(tex, kUnlockRect)(tex, 0);
    dslog::Write("Sharp text: font texture %ux%u (format 0x%X) -> %ux%u", d.width, d.height, d.format, d.width * n,
                 d.height * n);
    return tex;
}

const uint8_t* FontSheet() { return *reinterpret_cast<const uint8_t* const*>(kFontSheet); }

// The font sheet's page holding this texture, or -1. (The sheet has one or two pages.)
int FontPage(const uint8_t* sheet, void* tex) {
    if (!sheet || !tex) return -1;
    const auto* header = *reinterpret_cast<const uint8_t* const*>(sheet);
    const auto* records = *reinterpret_cast<const uint8_t* const*>(sheet + 4);
    const auto* pages = *reinterpret_cast<const uint8_t* const* const*>(sheet + 0xC);
    if (!header || !records || !pages) return -1;
    const int images = *reinterpret_cast<const uint16_t*>(header + 6);
    int lastPage = -1;
    for (int i = 0; i < images; ++i) {
        const int page = *reinterpret_cast<const uint16_t*>(records + i * 0x18);
        if (page == lastPage) continue;
        lastPage = page;
        if (pages[page] && *reinterpret_cast<void* const*>(pages[page] + 0x10) == tex) return page;
    }
    return -1;
}

// Pixels of page g_maskPage inside a printable character's glyph box of either font (+1 px for the outline).
// Records: +0 page, +0xa/+0xc texel x/y, +0xe/+0x10 width/height (u16).
int g_maskPage = 0;
const std::vector<uint8_t>& LetterMask(uint32_t w, uint32_t h) {
    static std::vector<uint8_t> mask;
    mask.assign(size_t(w) * h, 0);
    const uint8_t* sheet = FontSheet();
    const auto* header = *reinterpret_cast<const uint8_t* const*>(sheet);
    const auto* records = *reinterpret_cast<const uint8_t* const*>(sheet + 4);
    const int images = *reinterpret_cast<const uint16_t*>(header + 6);
    for (uint32_t global : kFonts) {
        const uint8_t* font = *reinterpret_cast<const uint8_t* const*>(global);
        if (!font || *reinterpret_cast<const uint8_t* const*>(font) != sheet) continue;
        const auto* table = *reinterpret_cast<const uint16_t* const*>(font + 0x20);
        const int size = *reinterpret_cast<const uint16_t*>(font + 0x24);
        for (int c = 33; c < std::min(size, 256); ++c) {
            const int g = table[c];
            if (g >= images) continue;
            const auto* r = reinterpret_cast<const uint16_t*>(records + g * 0x18);
            if (r[0] != g_maskPage) continue;
            const int x0 = std::max(0, r[5] - 1), y0 = std::max(0, r[6] - 1);
            const int x1 = std::min<int>(w, r[5] + r[7] + 1), y1 = std::min<int>(h, r[6] + r[8] + 1);
            for (int y = y0; y < y1; ++y)
                if (x1 > x0) memset(&mask[size_t(y) * w + x0], 1, x1 - x0);
        }
    }
    return mask;
}

int Factor(const uint8_t* sheet) {
    if (!(*reinterpret_cast<const uint32_t*>(sheet + 0x20) & 1)) return 1;  // sheet scale switched off
    // the larger scale: textfit.cpp condenses long menu texts by lowering only X (+0x24) for one draw
    const float k = std::max(*reinterpret_cast<const float*>(sheet + 0x24), *reinterpret_cast<const float*>(sheet + 0x28));
    return k <= 1.05f ? 1 : std::clamp(static_cast<int>(std::ceil(k - 0.05f)), 2, 4);
}

using SetTextureFn = void(__thiscall*)(void*, DWORD, void*);

bool Enabled() {
    static const bool off = kit::DevDword("NoFontSharp") != 0;  // dev: compare against the game's own font texture
    if (off) return false;
    const bool on = g_hudScaling && g_settingOn;
    static int logged = -1;
    if (logged != static_cast<int>(on)) {   // live: OPTIONS > DESERTSTORMFIX (FeatSharpText); needs HUD scaling
        logged = on;
        dslog::Write(on ? "[ok]   Sharp text: enlarged font texture" : "[off]  Sharp text: the game's font texture");
    }
    return on;
}

SetTextureFn g_next = nullptr;  // pse.hooks chain at 0x53CC9B: the next subscriber or the SetTexture wrapper

// Last subscriber of the glyph texture bind (features.h kGlyphBind*): a mod weapon's picture (weaponicons.cpp) or a
// recoloured page (barcolour.cpp) arrive here already swapped in and are no font pages, so they pass through.
void __fastcall SetTexture(void* renderer, void* /*edx*/, DWORD stage, void* tex) {
    if (tex && Enabled()) {
        const uint8_t* sheet = FontSheet();
        auto it = std::find_if(g_copies.begin(), g_copies.end(), [&](const Copy& c) { return c.original == tex; });
        if (it == g_copies.end()) {
            const int page = FontPage(sheet, tex);
            if (page >= 0) {
                AddRef(tex);
                g_copies.push_back({tex, page});
                it = g_copies.end() - 1;
            }
        }
        // The loading screen draws its text 1.25 x larger than the menus, so a page can need two sizes.
        if (it != g_copies.end() && sheet) {
            const int n = Factor(sheet);
            if (n > 1 && !it->tried[n]) {
                it->tried[n] = true;
                g_maskPage = it->page;
                it->sharp[n] = MakeSharp(tex, n, &LetterMask);
            }
            if (n > 1 && it->sharp[n]) tex = it->sharp[n];
        }
    }
    g_next(renderer, stage, tex);
}

// Drops copies of textures the font sheet no longer uses (a level unloaded its fonts).
void DropUnused() {
    const uint8_t* sheet = FontSheet();
    for (auto it = g_copies.begin(); it != g_copies.end();) {
        if (FontPage(sheet, it->original) >= 0) {
            ++it;
            continue;
        }
        for (void* t : it->sharp)
            if (t) Release(t);
        Release(it->original);
        it = g_copies.erase(it);
    }
}

const DsfixServicesV1* g_services = nullptr;

// HudScaling is dsfix.dll's setting (the launcher's checkbox): re-read when the launcher saves.
void PSE_CALL ReadHudScaling() { g_hudScaling = g_services->DsfixSetting("HudScaling") != 0; }

bool Load() {
    g_services = kit::Services();
    if (!g_services) return false;
    if (!kit::DeclareBool("FeatSharpText", true, "SHARPER TEXT", "menu and HUD text drawn crisp at high resolutions",
                          "GRAPHICS", [](bool on) { g_settingOn = on; }))
        return false;
    g_settingOn = kit::GetBool("FeatSharpText");
    ReadHudScaling();
    g_services->SetSettingsListener(kit::Self(), &ReadHudScaling);
    // Last subscriber of the shared glyph texture bind (dsfix.dll's weaponicons.cpp 200, barcolour.cpp 100).
    const bool hooked = patch::SubscribeCall(kSetTextureSite, reinterpret_cast<const void*>(&SetTexture),
                                             kGlyphBindTarget, kGlyphBindFontSharp, reinterpret_cast<void**>(&g_next));
    dslog::Write(hooked ? "Sharp text: glyph texture hook installed" : "[fail] Sharp text: glyph draw not recognised");
    return hooked && kit::OnFrame(&DropUnused);
}
}  // namespace

DSFIX_KIT_PLUGIN("dsfix.fontsharp", "Sharper text", PSE_PLUGIN_CLIENT_ONLY, Load)
