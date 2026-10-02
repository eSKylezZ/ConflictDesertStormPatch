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
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/overlay.h"

namespace {
constexpr uint32_t kSetTexture = 0x544220, kSetTextureSite = 0x53CC9B;
constexpr uint32_t kFontSheet = 0x60EDB0, kDevice = 0x755050;
constexpr uint32_t kFonts[] = {0x60EDB4, 0x60EDB8};  // small, large: +0 sheet, +0x20 char -> glyph u16[], +0x24 size
constexpr int kMaxSize = 4096;
constexpr float kLetterContrast = 1.8f;

// Direct3D 8 (no d3d8.h in the SDK).
enum : int { kAddRef = 1, kRelease = 2, kCreateTexture = 20, kGetLevelDesc = 14, kLockRect = 16, kUnlockRect = 17 };
enum : uint32_t {
    kA8R8G8B8 = 21, kX8R8G8B8 = 22, kR5G6B5 = 23, kX1R5G5B5 = 24, kA1R5G5B5 = 25, kA4R4G4B4 = 26, kA8 = 28,
    kL8 = 50, kA8L8 = 51, kDxt1 = 0x31545844, kDxt2 = 0x32545844, kDxt3 = 0x33545844, kDxt4 = 0x34545844,
    kDxt5 = 0x35545844, kPoolManaged = 1, kLockReadOnly = 0x10,
};
struct SurfaceDesc {
    uint32_t format, type, usage, pool, size, multiSample, width, height;
};
struct Locked {
    INT pitch;
    void* bits;
};

void** Vt(void* obj) { return *static_cast<void***>(obj); }
template <class Fn>
Fn M(void* obj, int i) {
    return reinterpret_cast<Fn>(Vt(obj)[i]);
}
ULONG AddRef(void* o) { return M<ULONG(__stdcall*)(void*)>(o, kAddRef)(o); }
ULONG Release(void* o) { return M<ULONG(__stdcall*)(void*)>(o, kRelease)(o); }

struct Copy {
    void* original;       // referenced by us while its copies exist
    int page;             // of the font sheet
    void* sharp[5] = {};  // per factor 2..4
    bool tried[5] = {};   // made or failed - not retried
};
std::vector<Copy> g_copies;

// RGBA (straight alpha, 0..255 per channel) of a whole texture level, or empty for formats we can't read.
std::vector<uint32_t> ReadPixels(void* tex, const SurfaceDesc& d) {
    std::vector<uint32_t> out;
    Locked lr{};
    using Lock = HRESULT(__stdcall*)(void*, UINT, Locked*, const RECT*, DWORD);
    if (FAILED(M<Lock>(tex, kLockRect)(tex, 0, &lr, nullptr, kLockReadOnly))) return out;
    const uint32_t w = d.width, h = d.height;
    out.assign(size_t(w) * h, 0);
    auto row = [&](uint32_t y) { return static_cast<const uint8_t*>(lr.bits) + size_t(y) * lr.pitch; };
    auto x5 = [](uint32_t v) { return (v << 3) | (v >> 2); };
    auto x6 = [](uint32_t v) { return (v << 2) | (v >> 4); };
    auto argb = [](uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return a << 24 | r << 16 | g << 8 | b; };
    auto rgb565 = [&](uint16_t c) { return argb(255, x5(c >> 11), x6((c >> 5) & 63), x5(c & 31)); };
    bool ok = true;
    switch (d.format) {
        case kA8R8G8B8:
        case kX8R8G8B8:
            for (uint32_t y = 0; y < h; ++y) {
                const auto* s = reinterpret_cast<const uint32_t*>(row(y));
                for (uint32_t x = 0; x < w; ++x) out[y * w + x] = d.format == kX8R8G8B8 ? s[x] | 0xFF000000 : s[x];
            }
            break;
        case kR5G6B5:
        case kX1R5G5B5:
        case kA1R5G5B5:
        case kA4R4G4B4:
            for (uint32_t y = 0; y < h; ++y) {
                const auto* s = reinterpret_cast<const uint16_t*>(row(y));
                for (uint32_t x = 0; x < w; ++x) {
                    const uint16_t c = s[x];
                    uint32_t v;
                    if (d.format == kR5G6B5) v = rgb565(c);
                    else if (d.format == kA4R4G4B4)
                        v = argb((c >> 12) * 17, ((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17);
                    else
                        v = argb(d.format == kA1R5G5B5 ? (c >> 15) * 255 : 255, x5((c >> 10) & 31), x5((c >> 5) & 31),
                                 x5(c & 31));
                    out[y * w + x] = v;
                }
            }
            break;
        case kA8:
        case kL8:
        case kA8L8:
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t* s = row(y);
                for (uint32_t x = 0; x < w; ++x) {
                    if (d.format == kA8) out[y * w + x] = argb(s[x], 255, 255, 255);
                    else if (d.format == kL8) out[y * w + x] = argb(255, s[x], s[x], s[x]);
                    else out[y * w + x] = argb(s[x * 2 + 1], s[x * 2], s[x * 2], s[x * 2]);
                }
            }
            break;
        case kDxt1:
        case kDxt2:
        case kDxt3:
        case kDxt4:
        case kDxt5: {
            const bool dxt1 = d.format == kDxt1, explicitAlpha = d.format == kDxt2 || d.format == kDxt3;
            const uint32_t blockBytes = dxt1 ? 8 : 16;
            for (uint32_t by = 0; by < (h + 3) / 4; ++by) {
                const uint8_t* b = row(by);  // D3D reports the pitch of a row of blocks
                for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx, b += blockBytes) {
                    uint32_t alpha[16];
                    const uint8_t* c = dxt1 ? b : b + 8;
                    if (explicitAlpha) {
                        for (int i = 0; i < 16; ++i) alpha[i] = ((b[i / 2] >> ((i & 1) * 4)) & 15) * 17;
                    } else if (!dxt1) {
                        uint32_t a[8] = {b[0], b[1]};
                        if (a[0] > a[1])
                            for (int i = 1; i < 7; ++i) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
                        else {
                            for (int i = 1; i < 5; ++i) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5;
                            a[6] = 0, a[7] = 255;
                        }
                        uint64_t bits = 0;
                        for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
                        for (int i = 0; i < 16; ++i) alpha[i] = a[(bits >> (3 * i)) & 7];
                    }
                    const uint16_t c0 = uint16_t(c[0] | c[1] << 8), c1 = uint16_t(c[2] | c[3] << 8);
                    uint32_t pal[4] = {rgb565(c0), rgb565(c1)};
                    auto mix = [](uint32_t p, uint32_t q, int wp, int wq, int div) {
                        uint32_t v = 0xFF000000;
                        for (int s = 0; s < 24; s += 8)
                            v |= ((((p >> s) & 255) * wp + ((q >> s) & 255) * wq) / div) << s;
                        return v;
                    };
                    if (c0 > c1 || !dxt1) {
                        pal[2] = mix(pal[0], pal[1], 2, 1, 3);
                        pal[3] = mix(pal[0], pal[1], 1, 2, 3);
                    } else {
                        pal[2] = mix(pal[0], pal[1], 1, 1, 2);
                        pal[3] = 0;  // transparent black
                    }
                    const uint32_t idx = c[4] | c[5] << 8 | c[6] << 16 | uint32_t(c[7]) << 24;
                    for (int i = 0; i < 16; ++i) {
                        const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                        if (x >= w || y >= h) continue;
                        uint32_t v = pal[(idx >> (2 * i)) & 3];
                        if (!dxt1) v = (v & 0xFFFFFF) | alpha[i] << 24;
                        out[y * w + x] = v;
                    }
                }
            }
            break;
        }
        default:
            ok = false;
    }
    M<HRESULT(__stdcall*)(void*, UINT)>(tex, kUnlockRect)(tex, 0);
    if (!ok) out.clear();
    return out;
}

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
    const float k = *reinterpret_cast<const float*>(sheet + 0x24);
    return k <= 1.05f ? 1 : std::clamp(static_cast<int>(std::ceil(k - 0.05f)), 2, 4);
}

using SetTextureFn = void(__thiscall*)(void*, DWORD, void*);

bool Enabled() {
#ifndef DS_DIST
    static const bool off = [] {  // dev: Dev\NoFontSharp = 1 compares against the game's own font texture
        DWORD v = 0, size = sizeof v;
        RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "NoFontSharp", RRF_RT_REG_DWORD, nullptr, &v, &size);
        return v != 0;
    }();
    if (off) return false;
#endif
    return settings::Get().hudScaling;
}

void __fastcall SetTexture(void* renderer, void* /*edx*/, DWORD stage, void* tex) {
    if (void* icon = features::WeaponIconTexture(overlay::DrawingSheet(), overlay::DrawingImage())) {
        reinterpret_cast<SetTextureFn>(kSetTexture)(renderer, stage, icon);  // a mod weapon's picture
        return;
    }
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
    reinterpret_cast<SetTextureFn>(kSetTexture)(renderer, stage, tex);
}
}  // namespace

// Drops copies of textures the font sheet no longer uses (a level unloaded its fonts).
void features::OnFrameFontSharp() {
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

void features::ApplyFontSharp() {
    static bool hooked = false;
    if (hooked) return;
    hooked = patch::HookCall(kSetTextureSite, reinterpret_cast<const void*>(&SetTexture), kSetTexture);
    dslog::Write(hooked ? "Sharp text: glyph texture hook installed" : "[fail] Sharp text: glyph draw not recognised");
}
