// Sharper text (port of DS1's fontsharp.cpp). The three fonts draw from their own bitmap sheets (font_m.img
// [0x62F448], font_s.img [0x62F444], font_l.img [0x62F44C]) made for 800x600 / 1280x1024; they are drawn larger by
// the HUD scale (sheet +0x24, missions) or by the front-end's virtual screen (graphics::VirtualScale, x1.4 at 1440p,
// x2.1 at 4K) - bilinear blow-ups with soft edges.
//
// Every glyph goes through the sheet image draw FUN_0052b7b0, which binds its page's texture with the renderer's
// SetTexture wrapper (thiscall 0x5364e0(stage, texture), call at 0x52BA5E; page = sheet +0xC [record +0], texture
// = page +0x10). That call is hooked: a font page texture is replaced by our copy, n = ceil(scale) (2..4) times
// larger - Lanczos-enlarged and lightly sharpened (see Enlarge). Texture coordinates are fractions, so the copy
// lines up as it is.
// We hold a reference to each original while its copy exists (its address can't be reused) and drop both once no
// font sheet uses it any more.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/graphics.h"

namespace {
constexpr uint32_t kSetTexture = 0x5364E0, kSetTextureSite = 0x52BA5E;
constexpr uint32_t kSheets[] = {0x62F448, 0x62F444, 0x62F44C};  // font_m, font_s, font_l
constexpr UINT kMaxSize = 4096;
constexpr float kSharpen = 0.6f;

struct Copy {
    IDirect3DTexture9* original;     // referenced by us while its copies exist
    IDirect3DTexture9* sharp[5] = {};  // per factor 2..4
    bool tried[5] = {};                // made or failed - not retried
};
std::vector<Copy> g_copies;

// RGBA (straight alpha) of level 0, or empty for formats we can't read.
std::vector<uint32_t> ReadPixels(IDirect3DTexture9* tex, const D3DSURFACE_DESC& d) {
    std::vector<uint32_t> out;
    D3DLOCKED_RECT lr{};
    if (FAILED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) return out;
    const uint32_t w = d.Width, h = d.Height;
    out.assign(size_t(w) * h, 0);
    auto row = [&](uint32_t y) { return static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch; };
    auto x5 = [](uint32_t v) { return (v << 3) | (v >> 2); };
    auto x6 = [](uint32_t v) { return (v << 2) | (v >> 4); };
    auto argb = [](uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return a << 24 | r << 16 | g << 8 | b; };
    auto rgb565 = [&](uint16_t c) { return argb(255, x5(c >> 11), x6((c >> 5) & 63), x5(c & 31)); };
    bool ok = true;
    switch (d.Format) {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
            for (uint32_t y = 0; y < h; ++y) {
                const auto* s = reinterpret_cast<const uint32_t*>(row(y));
                for (uint32_t x = 0; x < w; ++x) out[y * w + x] = d.Format == D3DFMT_X8R8G8B8 ? s[x] | 0xFF000000 : s[x];
            }
            break;
        case D3DFMT_R5G6B5:
        case D3DFMT_X1R5G5B5:
        case D3DFMT_A1R5G5B5:
        case D3DFMT_A4R4G4B4:
            for (uint32_t y = 0; y < h; ++y) {
                const auto* s = reinterpret_cast<const uint16_t*>(row(y));
                for (uint32_t x = 0; x < w; ++x) {
                    const uint16_t c = s[x];
                    uint32_t v;
                    if (d.Format == D3DFMT_R5G6B5) v = rgb565(c);
                    else if (d.Format == D3DFMT_A4R4G4B4)
                        v = argb((c >> 12) * 17, ((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17);
                    else
                        v = argb(d.Format == D3DFMT_A1R5G5B5 ? (c >> 15) * 255 : 255, x5((c >> 10) & 31),
                                 x5((c >> 5) & 31), x5(c & 31));
                    out[y * w + x] = v;
                }
            }
            break;
        case D3DFMT_A8:
        case D3DFMT_L8:
        case D3DFMT_A8L8:
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t* s = row(y);
                for (uint32_t x = 0; x < w; ++x) {
                    if (d.Format == D3DFMT_A8) out[y * w + x] = argb(s[x], 255, 255, 255);
                    else if (d.Format == D3DFMT_L8) out[y * w + x] = argb(255, s[x], s[x], s[x]);
                    else out[y * w + x] = argb(s[x * 2 + 1], s[x * 2], s[x * 2], s[x * 2]);
                }
            }
            break;
        case D3DFMT_DXT1:
        case D3DFMT_DXT2:
        case D3DFMT_DXT3:
        case D3DFMT_DXT4:
        case D3DFMT_DXT5: {
            const bool dxt1 = d.Format == D3DFMT_DXT1, explicitAlpha = d.Format == D3DFMT_DXT2 || d.Format == D3DFMT_DXT3;
            const uint32_t blockBytes = dxt1 ? 8 : 16;
            for (uint32_t by = 0; by < (h + 3) / 4; ++by) {
                const uint8_t* b = row(by);  // the pitch of a row of blocks
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
                        for (int s = 0; s < 24; s += 8) v |= ((((p >> s) & 255) * wp + ((q >> s) & 255) * wq) / div) << s;
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
    tex->UnlockRect(0);
    if (!ok) out.clear();
    return out;
}

// n times larger with a Lanczos-3 filter on premultiplied RGBA (separable), then a light unsharp mask: crisp edges
// for the GPU's bilinear stretch to sample, while the letters' soft drop shadow (semi-transparent) is kept - a
// thresholded alpha (DS1's way) wiped it.
float Lanczos(float x) {
    x = std::fabs(x);
    if (x < 1e-5f) return 1.0f;
    if (x >= 3.0f) return 0.0f;
    const float px = 3.14159265f * x;
    return 3.0f * std::sin(px) * std::sin(px / 3.0f) / (px * px);
}

std::vector<uint32_t> Enlarge(const std::vector<uint32_t>& src, uint32_t w, uint32_t h, int n) {
    const uint32_t W = w * n, H = h * n;
    // premultiplied float planes
    std::vector<float> in(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        const float a = float(src[i] >> 24) / 255.0f;
        in[i * 4 + 3] = a;
        for (int c = 0; c < 3; ++c) in[i * 4 + c] = float((src[i] >> (16 - 8 * c)) & 255) / 255.0f * a;
    }
    // weights per output coordinate (same for x and y up to the size)
    auto taps = [&](uint32_t outSize, uint32_t inSize, std::vector<int>& idx, std::vector<float>& wt) {
        idx.assign(size_t(outSize) * 6, 0);
        wt.assign(size_t(outSize) * 6, 0.0f);
        for (uint32_t o = 0; o < outSize; ++o) {
            const float c = (o + 0.5f) / n - 0.5f;
            const int base = int(std::floor(c)) - 2;
            float sum = 0;
            for (int k = 0; k < 6; ++k) {
                const float v = Lanczos(c - float(base + k));
                idx[o * 6 + k] = std::clamp(base + k, 0, int(inSize) - 1);
                wt[o * 6 + k] = v;
                sum += v;
            }
            for (int k = 0; k < 6; ++k) wt[o * 6 + k] /= sum;
        }
    };
    std::vector<int> ix, iy;
    std::vector<float> wx, wy;
    taps(W, w, ix, wx);
    taps(H, h, iy, wy);
    std::vector<float> mid(size_t(W) * h * 4);  // horizontal pass
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t X = 0; X < W; ++X)
            for (int c = 0; c < 4; ++c) {
                float v = 0;
                for (int k = 0; k < 6; ++k) v += in[(size_t(y) * w + ix[X * 6 + k]) * 4 + c] * wx[X * 6 + k];
                mid[(size_t(y) * W + X) * 4 + c] = v;
            }
    std::vector<float> big(size_t(W) * H * 4);  // vertical pass
    for (uint32_t Y = 0; Y < H; ++Y)
        for (uint32_t X = 0; X < W; ++X)
            for (int c = 0; c < 4; ++c) {
                float v = 0;
                for (int k = 0; k < 6; ++k) v += mid[(size_t(iy[Y * 6 + k]) * W + X) * 4 + c] * wy[Y * 6 + k];
                big[(size_t(Y) * W + X) * 4 + c] = v;
            }
    // unsharp mask (3x3 box) and back to straight alpha
    std::vector<uint32_t> out(size_t(W) * H);
    for (uint32_t Y = 0; Y < H; ++Y)
        for (uint32_t X = 0; X < W; ++X) {
            float p[4];
            for (int c = 0; c < 4; ++c) {
                float blur = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const uint32_t sx = std::clamp<int>(int(X) + dx, 0, int(W) - 1);
                        const uint32_t sy = std::clamp<int>(int(Y) + dy, 0, int(H) - 1);
                        blur += big[(size_t(sy) * W + sx) * 4 + c];
                    }
                const float v = big[(size_t(Y) * W + X) * 4 + c];
                p[c] = std::clamp(v + kSharpen * (v - blur / 9.0f), 0.0f, 1.0f);
            }
            const float a = p[3];
            uint32_t v = uint32_t(a * 255.0f + 0.5f) << 24;
            if (a > 1.0f / 512)
                for (int c = 0; c < 3; ++c) v |= uint32_t(std::clamp(p[c] / a, 0.0f, 1.0f) * 255.0f + 0.5f) << (16 - 8 * c);
            out[size_t(Y) * W + X] = v;
        }
    return out;
}

IDirect3DTexture9* MakeSharp(IDirect3DTexture9* original, int n) {
    D3DSURFACE_DESC d{};
    if (FAILED(original->GetLevelDesc(0, &d)) || d.Width * n > kMaxSize || d.Height * n > kMaxSize) return nullptr;
    const std::vector<uint32_t> src = ReadPixels(original, d);
    if (src.empty()) {
        dslog::Write("[fail] Sharp text: font texture %ux%u format 0x%X pool %u can't be read", d.Width, d.Height,
                     static_cast<unsigned>(d.Format), static_cast<unsigned>(d.Pool));
        return nullptr;
    }
    IDirect3DDevice9* dev = graphics::Device();
    IDirect3DTexture9* tex = nullptr;
    if (!dev || FAILED(dev->CreateTexture(d.Width * n, d.Height * n, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex,
                                          nullptr)) || !tex)
        return nullptr;
    const std::vector<uint32_t> big = Enlarge(src, d.Width, d.Height, n);
    D3DLOCKED_RECT lr{};
    if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) {
        tex->Release();
        return nullptr;
    }
    for (uint32_t y = 0; y < d.Height * n; ++y)
        std::memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, &big[size_t(y) * d.Width * n],
                    size_t(d.Width) * n * 4);
    tex->UnlockRect(0);
    dslog::Write("Sharp text: font texture %ux%u (format 0x%X) -> %ux%u", d.Width, d.Height,
                 static_cast<unsigned>(d.Format), d.Width * n, d.Height * n);
    return tex;
}

// The font sheet that has this texture as one of its pages, or null. Sheet: +0 header (+0xA page count), +0xC pages.
const uint8_t* FontSheetOf(void* tex) {
    if (!tex) return nullptr;
    for (uint32_t global : kSheets) {
        const auto* sheet = *reinterpret_cast<const uint8_t* const*>(global);
        if (!sheet) continue;
        const auto* header = *reinterpret_cast<const uint8_t* const*>(sheet);
        const auto* pages = *reinterpret_cast<const uint8_t* const* const*>(sheet + 0xC);
        if (!header || !pages) continue;
        const int count = *reinterpret_cast<const uint16_t*>(header + 0xA);
        for (int i = 0; i < count; ++i)
            if (pages[i] && *reinterpret_cast<void* const*>(pages[i] + 0x10) == tex) return sheet;
    }
    return nullptr;
}

// How much larger the glyphs land on the back buffer: sheet scale (if on) x the virtual screen's.
int Factor(const uint8_t* sheet) {
    float k = graphics::VirtualScale();
    if (*reinterpret_cast<const uint32_t*>(sheet + 0x20) & 1) k *= *reinterpret_cast<const float*>(sheet + 0x24);
    return k <= 1.05f ? 1 : std::clamp(static_cast<int>(std::ceil(k - 0.05f)), 2, 4);
}

// Copies of textures no font sheet uses any more (a level unloaded its fonts) are dropped.
void Prune() {
    for (auto it = g_copies.begin(); it != g_copies.end();) {
        if (FontSheetOf(it->original)) {
            ++it;
            continue;
        }
        for (auto* t : it->sharp)
            if (t) t->Release();
        it->original->Release();
        it = g_copies.erase(it);
    }
}

bool Enabled() {
#ifndef DS_DIST
    static const bool off = [] {  // dev: Dev\NoFontSharp = 1 compares against the game's own font texture
        DWORD v = 0, size = sizeof v;
        RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStorm2Fix\\Dev", "NoFontSharp", RRF_RT_REG_DWORD, nullptr, &v,
                     &size);
        return v != 0;
    }();
    if (off) return false;
#endif
    return settings::Get().hudScaling;
}

using SetTextureFn = void(__thiscall*)(void*, DWORD, void*);

void __fastcall SetTexture(void* renderer, void* /*edx*/, DWORD stage, void* tex) {
    static unsigned calls = 0;
    if ((++calls & 1023) == 0) Prune();
    if (tex && Enabled()) {
        auto it = std::find_if(g_copies.begin(), g_copies.end(), [&](const Copy& c) { return c.original == tex; });
        const uint8_t* sheet = FontSheetOf(tex);
        if (it == g_copies.end() && sheet) {
            auto* t = static_cast<IDirect3DTexture9*>(tex);
            t->AddRef();
            g_copies.push_back({t});
            it = g_copies.end() - 1;
        }
        if (it != g_copies.end() && sheet) {
            const int n = Factor(sheet);
            if (n > 1 && !it->tried[n]) {
                it->tried[n] = true;
                it->sharp[n] = MakeSharp(it->original, n);
            }
            if (n > 1 && it->sharp[n]) tex = it->sharp[n];
        }
    }
    reinterpret_cast<SetTextureFn>(kSetTexture)(renderer, stage, tex);
}
}  // namespace

void features::ApplyFontSharp() {
    static bool hooked = false;
    if (hooked) return;
    hooked = patch::HookCall(kSetTextureSite, reinterpret_cast<const void*>(&SetTexture), kSetTexture);
    dslog::Write(hooked ? "[ok]   Sharp text: glyph texture hook" : "[fail] Sharp text: glyph draw not recognised");
}
