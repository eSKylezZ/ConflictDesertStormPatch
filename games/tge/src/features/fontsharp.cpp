// Sharper text (tgefix.fontsharp, with HudScaling) - DesertStormFix's fontsharp.cpp for TGE. The GUI is laid out for
// 640 x 480 and drawn k times larger (guiscale.cpp: 2.25 at 1440p, times HudScalePercent), so every glyph is a
// bilinear blow-up of letters made for 640 x 480: soft, jaggy edges.
//
// Text is drawn glyph by glyph through the sprite draw 0x44AC60 (thiscall(sheet, image, x, y) ret 0xC), called from the
// text routines at 0x436177 0x436932 0x437054 0x4370FB 0x43727E 0x437829; it binds the glyph's page texture through
// 0x441940 (thiscall([0x79ACFC], stage, texture), call at 0x44AF11; page = sheet +0xC [record +0], texture = page +0x10
// - DS1's layout). Those text calls are wrapped (a flag while they run) and the bind is hooked: inside a text draw the
// page texture is replaced by our copy n = ceil(k x the sheet's own scale) (2..4) times larger - bilinearly enlarged
// with the alpha edge steepened back to ~1 px and more colour contrast (the letters' anti-aliasing is partly in the
// colour). Texture coordinates are fractions, so nothing else changes. We hold a reference to each original while its
// copies exist and drop both when the page hasn't been drawn for 10 s (a GUI / level unloaded its fonts).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "core/texpixels.h"
#include "features/features.h"
#include "loader.h"

namespace {
using namespace texpix;
constexpr uint32_t kBindSite = 0x44AF11, kBind = 0x441940;
constexpr uint32_t kSpriteDraw = 0x44AC60;
constexpr uint32_t kTextCalls[] = {0x436177, 0x436932, 0x437054, 0x4370FB, 0x43727E, 0x437829};
constexpr uint32_t kRenderer = 0x79ED20;   // renderer*: +0x40530 the D3D device
constexpr int kMaxSize = 4096;
constexpr float kContrast = 1.8f;
constexpr DWORD kKeepMs = 10000;

struct Copy {
    void* original;       // referenced by us while its copies exist
    void* sharp[5] = {};  // per factor 2..4
    bool tried[5] = {};   // made or failed - not retried
    DWORD used = 0;       // GetTickCount of the last draw
};
std::vector<Copy> g_copies;
const uint8_t* g_textSheet = nullptr;   // the sheet of the text draw in progress, else null

// n times larger: bilinear (premultiplied, as the GPU filters), then alpha pushed away from 0.5 so an edge ramps over
// ~1 output pixel instead of n, and the colour's contrast raised (outline vs fill).
std::vector<uint32_t> Enlarge(const std::vector<uint32_t>& src, uint32_t w, uint32_t h, int n) {
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
                v = uint32_t(sharpA * 255.0f + 0.5f) << 24;
                for (int c = 0; c < 3; ++c) {
                    const float col = std::clamp((std::clamp(rgb[c] / a, 0.0f, 255.0f) - 127.5f) * kContrast + 127.5f, 0.0f, 255.0f);
                    v |= uint32_t(col + 0.5f) << (16 - 8 * c);
                }
            }
            out[size_t(Y) * W + X] = v;
        }
    }
    return out;
}

void* MakeSharp(void* original, int n) {
    SurfaceDesc d{};
    if (FAILED(M<HRESULT(__stdcall*)(void*, UINT, SurfaceDesc*)>(original, kGetLevelDesc)(original, 0, &d))) return nullptr;
    if (d.width * n > kMaxSize || d.height * n > kMaxSize) return nullptr;
    const std::vector<uint32_t> src = ReadPixels(original, d);
    if (src.empty()) {
        dslog::Write("[fail] Sharp text: font texture %ux%u format 0x%X pool %u can't be read", d.width, d.height,
                     d.format, d.pool);
        return nullptr;
    }
    const auto* r = *reinterpret_cast<const uint8_t* const*>(kRenderer);
    void* dev = r ? *reinterpret_cast<void* const*>(r + 0x40530) : nullptr;
    void* tex = nullptr;
    using Create = HRESULT(__stdcall*)(void*, UINT, UINT, UINT, DWORD, uint32_t, uint32_t, void**);
    if (!dev || FAILED(M<Create>(dev, kCreateTexture)(dev, d.width * n, d.height * n, 1, 0, kA8R8G8B8, kPoolManaged,
                                                      &tex)) || !tex)
        return nullptr;
    const std::vector<uint32_t> big = Enlarge(src, d.width, d.height, n);
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

// How much larger than its texels the glyph is drawn: the GUI's scale x the sheet's own (+0x20 bit 0 = scaled).
int Factor(const uint8_t* sheet) {
    float k = features::GuiDrawScale();
    if (*reinterpret_cast<const uint32_t*>(sheet + 0x20) & 1)
        k *= std::max(*reinterpret_cast<const float*>(sheet + 0x24), *reinterpret_cast<const float*>(sheet + 0x28));
    return k <= 1.05f ? 1 : std::clamp(static_cast<int>(std::ceil(k - 0.05f)), 2, 4);
}

using BindFn = void(__thiscall*)(void*, DWORD, void*);
BindFn g_nextBind = nullptr;
void __fastcall Bind(void* state, void*, DWORD stage, void* tex) {
    if (tex && g_textSheet && features::Get().hudScaling) {
        auto it = std::find_if(g_copies.begin(), g_copies.end(), [&](const Copy& c) { return c.original == tex; });
        if (it == g_copies.end()) {
            AddRef(tex);
            g_copies.push_back({tex});
            it = g_copies.end() - 1;
        }
        it->used = GetTickCount();
        const int n = Factor(g_textSheet);
        if (n > 1 && !it->tried[n]) {
            it->tried[n] = true;
            it->sharp[n] = MakeSharp(tex, n);
        }
        if (n > 1 && it->sharp[n]) tex = it->sharp[n];
    }
    g_nextBind(state, stage, tex);
}

using SpriteDrawFn = int(__thiscall*)(void*, int, int, int);
int __fastcall TextGlyph(void* sheet, void*, int image, int x, int y) {
    const uint8_t* outer = g_textSheet;
    g_textSheet = static_cast<const uint8_t*>(sheet);
    const int r = reinterpret_cast<SpriteDrawFn>(kSpriteDraw)(sheet, image, x, y);
    g_textSheet = outer;
    return r;
}

// Copies of pages not drawn for a while: dropped (the original's reference too, so the game can free it).
int PSE_CALL DropUnused(PSEEventId, void*, void*) {
    const DWORD now = GetTickCount();
    for (auto it = g_copies.begin(); it != g_copies.end();) {
        if (now - it->used < kKeepMs) {
            ++it;
            continue;
        }
        for (void* t : it->sharp)
            if (t) Release(t);
        Release(it->original);
        it = g_copies.erase(it);
    }
    return 0;
}
}  // namespace

void features::ApplyFontSharp() {
    if (!Get().hudScaling) {
        dslog::Write("[off]  Sharp text: needs menu & HUD scaling");
        return;
    }
    int text = 0;
    for (uint32_t site : kTextCalls) text += patch::HookCall(site, reinterpret_cast<const void*>(&TextGlyph), kSpriteDraw);
    const bool bind = patch::SubscribeCall(kBindSite, reinterpret_cast<const void*>(&Bind), kBind, 0,
                                           reinterpret_cast<void**>(&g_nextBind));
    if (!bind || text != static_cast<int>(std::size(kTextCalls))) {
        ModLog("[fail] Sharp text: glyph draw not recognised (%d of %zu text calls, bind %d) - the game's soft text", text,
               std::size(kTextCalls), bind);
        return;
    }
    if (const PSEMessagingInterface* m = pse::MessagingInterface())
        m->Subscribe(pse_host::Find("tgefix.fontsharp"), PSE_EVENT_FRAME, &DropUnused, nullptr, 0);
    dslog::Write("[ok]   Sharp text: font pages enlarged while text is scaled up");
}
