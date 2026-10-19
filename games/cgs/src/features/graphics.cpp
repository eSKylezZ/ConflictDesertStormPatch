#include "features/graphics.h"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "core/patch.h"

namespace graphics {
void (*g_adjustPresentParams)(D3DPRESENT_PARAMETERS*) = nullptr;
void (*g_onPresent)(IDirect3DDevice9*) = nullptr;
void (*g_onDeviceCreated)(int, int) = nullptr;
}  // namespace graphics

namespace {
constexpr uint32_t kCreateSlot = 0x6C03B4;   // Direct3DCreate9 import slot

// IDirect3DDevice9 vtable slots
enum : int { kReset = 16, kPresent = 17, kClear = 43, kSetViewport = 47, kSetScissorRect = 75, kDrawPrimitive = 81,
             kDrawIndexedPrimitive = 82, kDrawPrimitiveUP = 83, kDrawIndexedPrimitiveUP = 84 };

using CreateFn = IDirect3D9*(WINAPI*)(UINT);
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                   IDirect3DDevice9**);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ClearFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SetViewportFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using SetScissorFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*);
using DrawFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawUPFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedUPFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*,
                                                    D3DFORMAT, const void*, UINT);

CreateFn g_realCreate = nullptr;
CreateDeviceFn g_realCreateDevice = nullptr;
ResetFn g_realReset = nullptr;
PresentFn g_realPresent = nullptr;
ClearFn g_realClear = nullptr;
SetViewportFn g_realSetViewport = nullptr;
SetScissorFn g_realSetScissor = nullptr;
DrawFn g_realDraw = nullptr;
DrawIndexedFn g_realDrawIndexed = nullptr;
DrawUPFn g_realDrawUP = nullptr;
DrawIndexedUPFn g_realDrawIndexedUP = nullptr;

// virtual screen: x' = ox + x * f, y' = oy + y * f
IDirect3DDevice9* g_device = nullptr;
bool g_virtual = false;
int g_realW = 0, g_realH = 0;
float g_f = 1.0f, g_ox = 0.0f, g_oy = 0.0f;
std::vector<uint8_t> g_vertices;

bool PreTransformed(IDirect3DDevice9* dev) {
    DWORD fvf = 0;
    return SUCCEEDED(dev->GetFVF(&fvf)) && (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
}

UINT VertexCount(D3DPRIMITIVETYPE type, UINT prims) {
    switch (type) {
        case D3DPT_POINTLIST: return prims;
        case D3DPT_LINELIST: return prims * 2;
        case D3DPT_LINESTRIP: return prims + 1;
        case D3DPT_TRIANGLELIST: return prims * 3;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN: return prims + 2;
        default: return 0;
    }
}

// menu squeeze (graphics::SetMenuSqueeze): x' = cx + (x - cx) * s for screen-space vertices, per 4-vertex sprite.
// Sprites reaching both screen edges (background film, frame bars, the translucent layers) keep their place.
// Text isn't stretched by the game (glyphs are drawn at their pixel size; the fonts' pages are of several sizes), so a
// run of glyph-like sprites - at least 2, on one line, small (height <= max(32, 4 % of the screen)), next to each other
// - is only moved: the run shifts to where the squeeze puts its centre. Square sprites (the cursor, the objectives map
// and its markers, portraits) are drawn in shape by the game already and are only moved too. Draws into a render target
// of another size than the screen (the post-processing passes) are never touched.
float g_squeeze = 0.0f, g_squeezeCx = 0.0f, g_squeezeW = 0.0f;
std::vector<uint8_t> g_squeezed;

bool ToScreenTarget(IDirect3DDevice9* dev) {
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return false;
    D3DSURFACE_DESC d = {};
    rt->GetDesc(&d);
    rt->Release();
    return static_cast<float>(d.Width) == g_squeezeW;
}

const void* Squeezed(IDirect3DDevice9* dev, const void* data, UINT count, UINT stride) {
    if (!ToScreenTarget(dev)) return data;
    g_squeezed.resize(static_cast<size_t>(count) * stride);
    std::memcpy(g_squeezed.data(), data, g_squeezed.size());
    auto X = [&](UINT i) -> float& { return *reinterpret_cast<float*>(g_squeezed.data() + static_cast<size_t>(i) * stride); };
    auto Y = [&](UINT i) { return *reinterpret_cast<const float*>(g_squeezed.data() + static_cast<size_t>(i) * stride + 4); };
    const UINT group = count % 4 == 0 ? 4 : count;
    struct Box { float x0, x1, y0, y1; };
    auto box = [&](UINT g) {
        Box b = {1e9f, -1e9f, 1e9f, -1e9f};
        for (UINT i = g; i < g + group; ++i) {
            b.x0 = std::min(b.x0, X(i)); b.x1 = std::max(b.x1, X(i));
            b.y0 = std::min(b.y0, Y(i)); b.y1 = std::max(b.y1, Y(i));
        }
        return b;
    };
    if (group == 4) {
        const float maxGlyph = std::max(32.0f, g_squeezeW * g_squeeze * 0.75f * 0.04f);   // screen height = W * s * 3 / 4
        for (UINT g = 0; g < count;) {
            const Box first = box(g);
            Box run = first, last = first;
            UINT end = g + 4;
            if (first.y1 - first.y0 <= maxGlyph)
                for (; end < count; end += 4) {
                    const Box b = box(end);
                    const float h = std::max(1.0f, last.y1 - last.y0);
                    if (b.y1 - b.y0 > maxGlyph || std::abs(b.y0 - last.y0) > h * 0.6f || b.x0 < last.x0 ||
                        b.x0 - last.x1 > h * 1.5f)
                        break;
                    run.x1 = std::max(run.x1, b.x1);
                    last = b;
                }
            if (end - g >= 8) {   // text: moved, not narrowed
                const float mid = (run.x0 + run.x1) * 0.5f, dx = (g_squeezeCx + (mid - g_squeezeCx) * g_squeeze) - mid;
                for (UINT i = g; i < end; ++i) X(i) += dx;
            } else if (std::abs((first.x1 - first.x0) - (first.y1 - first.y0)) <= 1.5f) {   // square: already in shape
                end = g + 4;
                const float mid = (first.x0 + first.x1) * 0.5f, dx = (g_squeezeCx + (mid - g_squeezeCx) * g_squeeze) - mid;
                for (UINT i = g; i < end; ++i) X(i) += dx;
            } else if (first.x0 > g_squeezeW * 0.03f || first.x1 < g_squeezeW * 0.97f) {   // art: squeezed (edge-to-edge pieces stay)
                end = g + 4;
                for (UINT i = g; i < end; ++i) X(i) = g_squeezeCx + (X(i) - g_squeezeCx) * g_squeeze;
            } else {
                end = g + 4;
            }
            g = end;
        }
        return g_squeezed.data();
    }
    for (UINT g = 0; g < count; g += group) {
        const Box b = box(g);
        if (b.x0 <= g_squeezeW * 0.03f && b.x1 >= g_squeezeW * 0.97f) continue;
        for (UINT i = g; i < g + group; ++i) X(i) = g_squeezeCx + (X(i) - g_squeezeCx) * g_squeeze;
    }
    return g_squeezed.data();
}

// copy of `count` vertices of `stride` bytes with x / y (the first two floats) mapped
const void* Mapped(const void* data, UINT count, UINT stride) {
    g_vertices.resize(static_cast<size_t>(count) * stride);
    std::memcpy(g_vertices.data(), data, g_vertices.size());
    for (UINT i = 0; i < count; ++i) {
        auto v = reinterpret_cast<float*>(g_vertices.data() + static_cast<size_t>(i) * stride);
        v[0] = g_ox + v[0] * g_f;
        v[1] = g_oy + v[1] * g_f;
    }
    return g_vertices.data();
}

#ifndef DS_DIST
// Dev\DrawProbe = 1: each distinct call path of a screen-space (XYZRHW) user-pointer draw is logged once - the first
// game return addresses on the stack, vertex count / stride, the vertices' bounding box and texture - which game
// function draws which HUD / text piece.
bool g_drawProbe = false;
struct ProbeSeen { uint32_t sig[4]; };
ProbeSeen g_probeSeen[256];
int g_probeCount = 0;
bool GameReturn(uint32_t v) {
    if (v < 0x401000 || v >= 0x6BF100) return false;
    const auto p = reinterpret_cast<const uint8_t*>(v);
    return p[-5] == 0xE8 || (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) || (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) ||
           (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10);
}
void __declspec(noinline) ProbeDraw(IDirect3DDevice9* dev, const char* what, const void* data, UINT count, UINT stride) {
    uint32_t sig[4] = {};
    int n = 0;
    const uint32_t* sp = reinterpret_cast<const uint32_t*>(_AddressOfReturnAddress());
    for (int i = 0; i < 600 && n < 4; ++i)
        if (GameReturn(sp[i])) sig[n++] = sp[i];
    for (int i = 0; i < g_probeCount; ++i)
        if (!std::memcmp(g_probeSeen[i].sig, sig, sizeof sig)) return;
    if (g_probeCount >= 256) return;
    std::memcpy(g_probeSeen[g_probeCount++].sig, sig, sizeof sig);
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (UINT i = 0; i < count; ++i) {
        auto v = reinterpret_cast<const float*>(static_cast<const uint8_t*>(data) + static_cast<size_t>(i) * stride);
        x0 = std::min(x0, v[0]); y0 = std::min(y0, v[1]); x1 = std::max(x1, v[0]); y1 = std::max(y1, v[1]);
    }
    IDirect3DBaseTexture9* tex = nullptr;
    dev->GetTexture(0, &tex);
    D3DSURFACE_DESC td = {};
    if (tex && tex->GetType() == D3DRTYPE_TEXTURE) static_cast<IDirect3DTexture9*>(tex)->GetLevelDesc(0, &td);
    if (tex) tex->Release();
    dslog::Write("[dev] DrawProbe %s n%u s%u box %.0f,%.0f - %.0f,%.0f tex %p %ux%u  from %08X %08X %08X %08X", what,
                 count, stride, x0, y0, x1, y1, static_cast<void*>(tex), td.Width, td.Height, sig[0], sig[1], sig[2], sig[3]);
}
bool g_loggedVbDraw = false;
void NoteVertexBufferDraw(IDirect3DDevice9* dev, const char* what) {
    if (g_virtual && !g_loggedVbDraw && PreTransformed(dev)) {
        g_loggedVbDraw = true;
        dslog::Write("[dev] virtual screen: pre-transformed %s from a vertex buffer - not mapped", what);
    }
}
// what the front-end draws: each distinct (call, FVF) and viewport once while the virtual screen is on
struct Seen { int call; DWORD fvf; };
Seen g_seenDraws[64];
int g_seenDrawCount = 0;
void NoteDraw(IDirect3DDevice9* dev, int call, const char* name) {
    if (!g_virtual) return;
    DWORD fvf = 0;
    dev->GetFVF(&fvf);
    for (int i = 0; i < g_seenDrawCount; ++i)
        if (g_seenDraws[i].call == call && g_seenDraws[i].fvf == fvf) return;
    if (g_seenDrawCount < 64) g_seenDraws[g_seenDrawCount++] = {call, fvf};
    void* ret = _ReturnAddress();
    dslog::Write("[dev] virtual screen: %s FVF 0x%03lX (caller %p)", name, static_cast<unsigned long>(fvf), ret);
}
D3DVIEWPORT9 g_seenVps[32];
int g_seenVpCount = 0;
void NoteViewport(const D3DVIEWPORT9* vp, void* caller) {
    for (int i = 0; i < g_seenVpCount; ++i)
        if (!std::memcmp(&g_seenVps[i], vp, sizeof *vp)) return;
    if (g_seenVpCount < 32) g_seenVps[g_seenVpCount++] = *vp;
    dslog::Write("[dev] virtual screen: SetViewport %lu,%lu %lux%lu (caller %p)", vp->X, vp->Y, vp->Width, vp->Height, caller);
}
#endif

HRESULT STDMETHODCALLTYPE HookDrawUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT prims, const void* data,
                                     UINT stride) {
#ifndef DS_DIST
    NoteDraw(dev, 1, "DrawPrimitiveUP");
    if (g_drawProbe && data && stride >= 8 && PreTransformed(dev)) ProbeDraw(dev, "UP", data, VertexCount(type, prims), stride);
#endif
    if (g_virtual && data && stride >= 8 && PreTransformed(dev))
        if (const UINT n = VertexCount(type, prims)) data = Mapped(data, n, stride);
    if (g_squeeze > 0.0f && data && stride >= 8 && PreTransformed(dev))
        if (const UINT n = VertexCount(type, prims)) data = Squeezed(dev, data, n, stride);
    return g_realDrawUP(dev, type, prims, data, stride);
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT minIndex, UINT count,
                                            UINT prims, const void* indices, D3DFORMAT fmt, const void* data,
                                            UINT stride) {
#ifndef DS_DIST
    NoteDraw(dev, 2, "DrawIndexedPrimitiveUP");
    if (g_drawProbe && data && stride >= 8 && PreTransformed(dev)) ProbeDraw(dev, "IUP", data, minIndex + count, stride);
#endif
    if (g_virtual && data && stride >= 8 && PreTransformed(dev)) {
        // the indexed vertices start at minIndex: map [0, minIndex + count) so the indices stay valid
        data = Mapped(data, minIndex + count, stride);
    }
    if (g_squeeze > 0.0f && data && stride >= 8 && PreTransformed(dev)) data = Squeezed(dev, data, minIndex + count, stride);
    return g_realDrawIndexedUP(dev, type, minIndex, count, prims, indices, fmt, data, stride);
}

HRESULT STDMETHODCALLTYPE HookDraw(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT start, UINT prims) {
#ifndef DS_DIST
    NoteVertexBufferDraw(dev, "DrawPrimitive");
    NoteDraw(dev, 3, "DrawPrimitive");
#endif
    return g_realDraw(dev, type, start, prims);
}

HRESULT STDMETHODCALLTYPE HookDrawIndexed(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT base, UINT minIndex,
                                          UINT count, UINT start, UINT prims) {
#ifndef DS_DIST
    NoteVertexBufferDraw(dev, "DrawIndexedPrimitive");
    NoteDraw(dev, 4, "DrawIndexedPrimitive");
#endif
    return g_realDrawIndexed(dev, type, base, minIndex, count, start, prims);
}

HRESULT STDMETHODCALLTYPE HookSetViewport(IDirect3DDevice9* dev, const D3DVIEWPORT9* vp) {
    if (!g_virtual || !vp) return g_realSetViewport(dev, vp);
#ifndef DS_DIST
    NoteViewport(vp, _ReturnAddress());
#endif
    D3DVIEWPORT9 m = *vp;
    m.X = static_cast<DWORD>(g_ox + vp->X * g_f + 0.5f);
    m.Y = static_cast<DWORD>(g_oy + vp->Y * g_f + 0.5f);
    m.Width = std::min<DWORD>(static_cast<DWORD>(vp->Width * g_f + 0.5f), static_cast<DWORD>(g_realW) - m.X);
    m.Height = std::min<DWORD>(static_cast<DWORD>(vp->Height * g_f + 0.5f), static_cast<DWORD>(g_realH) - m.Y);
    return g_realSetViewport(dev, &m);
}

HRESULT STDMETHODCALLTYPE HookSetScissor(IDirect3DDevice9* dev, const RECT* r) {
#ifndef DS_DIST
    if (g_drawProbe && r) dslog::Write("[dev] DrawProbe scissor %ld,%ld - %ld,%ld", r->left, r->top, r->right, r->bottom);
#endif
    if (g_squeeze > 0.0f && r && ToScreenTarget(dev)) {   // clip windows follow the squeezed art
        RECT m = *r;
        m.left = static_cast<LONG>(g_squeezeCx + (r->left - g_squeezeCx) * g_squeeze);
        m.right = static_cast<LONG>(g_squeezeCx + (r->right - g_squeezeCx) * g_squeeze + 0.5f);
        return g_realSetScissor(dev, &m);
    }
    if (!g_virtual || !r) return g_realSetScissor(dev, r);
    RECT m = {static_cast<LONG>(g_ox + r->left * g_f), static_cast<LONG>(g_oy + r->top * g_f),
              static_cast<LONG>(g_ox + r->right * g_f + 0.5f), static_cast<LONG>(g_oy + r->bottom * g_f + 0.5f)};
    return g_realSetScissor(dev, &m);
}

HRESULT STDMETHODCALLTYPE HookClear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags,
                                    D3DCOLOR colour, float z, DWORD stencil) {
    if (!g_virtual) return g_realClear(dev, count, rects, flags, colour, z, stencil);
    if (!count || !rects) {
        // a clear of the (mapped) viewport: clear the whole back buffer, so nothing stale stays around the picture
        D3DVIEWPORT9 vp, full = {0, 0, static_cast<DWORD>(g_realW), static_cast<DWORD>(g_realH), 0.0f, 1.0f};
        dev->GetViewport(&vp);
        full.MinZ = vp.MinZ;
        full.MaxZ = vp.MaxZ;
        g_realSetViewport(dev, &full);
        HRESULT hr = g_realClear(dev, 0, nullptr, flags, colour, z, stencil);
        g_realSetViewport(dev, &vp);
        return hr;
    }
    std::vector<D3DRECT> m(rects, rects + count);
    for (auto& r : m) {
        r.x1 = static_cast<LONG>(g_ox + r.x1 * g_f);
        r.y1 = static_cast<LONG>(g_oy + r.y1 * g_f);
        r.x2 = static_cast<LONG>(g_ox + r.x2 * g_f + 0.5f);
        r.y2 = static_cast<LONG>(g_oy + r.y2 * g_f + 0.5f);
    }
    return g_realClear(dev, count, m.data(), flags, colour, z, stencil);
}

using TestCoopFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
TestCoopFn g_realTestCoop = nullptr;
HRESULT g_lastCoop = 1;
HRESULT STDMETHODCALLTYPE HookTestCoop(IDirect3DDevice9* dev) {
    const HRESULT hr = g_realTestCoop(dev);
    if (hr != g_lastCoop) {
        g_lastCoop = hr;
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        dev->GetCreationParameters(&cp);
        RECT r = {};
        GetWindowRect(cp.hFocusWindow, &r);
        dslog::Write("Graphics: TestCooperativeLevel -> 0x%08lX (focus window %p %ld,%ld %ldx%ld style 0x%08lX iconic %d)",
                     static_cast<unsigned long>(hr), cp.hFocusWindow, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     static_cast<unsigned long>(GetWindowLongA(cp.hFocusWindow, GWL_STYLE)), IsIconic(cp.hFocusWindow));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* c) {
    if (graphics::g_onPresent) graphics::g_onPresent(dev);
    return g_realPresent(dev, a, b, w, c);
}

HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (g_virtual && pp) {   // the game's fields say 800 x 600 while the virtual screen is on
        pp->BackBufferWidth = static_cast<UINT>(g_realW);
        pp->BackBufferHeight = static_cast<UINT>(g_realH);
    }
    if (graphics::g_adjustPresentParams) graphics::g_adjustPresentParams(pp);
    const HRESULT hr = g_realReset(dev, pp);
    dslog::Write("Graphics: Reset %ux%u %s -> 0x%08lX", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
                 pp && pp->Windowed ? "windowed" : "fullscreen", static_cast<unsigned long>(hr));
    return hr;
}

void HookVtable(void* obj, int index, void* fn, void** original) {
    void** vt = *static_cast<void***>(obj);
    if (vt[index] == fn) return;
    *original = vt[index];
    DWORD old;
    VirtualProtect(&vt[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vt[index] = fn;
    VirtualProtect(&vt[index], sizeof(void*), old, &old);
}

// The device's vtable is a heap copy that d3d9 replaces after creation (seen on Windows 11), so the hooks are put back
// whenever they are missing - after CreateDevice and every frame (graphics::OnFrame).
void HookDevice(IDirect3DDevice9* dev) {
    HookVtable(dev, 3, reinterpret_cast<void*>(&HookTestCoop), reinterpret_cast<void**>(&g_realTestCoop));
    HookVtable(dev, kReset, reinterpret_cast<void*>(&HookReset), reinterpret_cast<void**>(&g_realReset));
    HookVtable(dev, kPresent, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_realPresent));
    HookVtable(dev, kClear, reinterpret_cast<void*>(&HookClear), reinterpret_cast<void**>(&g_realClear));
    HookVtable(dev, kSetViewport, reinterpret_cast<void*>(&HookSetViewport),
               reinterpret_cast<void**>(&g_realSetViewport));
    HookVtable(dev, kSetScissorRect, reinterpret_cast<void*>(&HookSetScissor),
               reinterpret_cast<void**>(&g_realSetScissor));
    HookVtable(dev, kDrawPrimitive, reinterpret_cast<void*>(&HookDraw), reinterpret_cast<void**>(&g_realDraw));
    HookVtable(dev, kDrawIndexedPrimitive, reinterpret_cast<void*>(&HookDrawIndexed),
               reinterpret_cast<void**>(&g_realDrawIndexed));
    HookVtable(dev, kDrawPrimitiveUP, reinterpret_cast<void*>(&HookDrawUP), reinterpret_cast<void**>(&g_realDrawUP));
    HookVtable(dev, kDrawIndexedPrimitiveUP, reinterpret_cast<void*>(&HookDrawIndexedUP),
               reinterpret_cast<void**>(&g_realDrawIndexedUP));
}

HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                           D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    if (graphics::g_adjustPresentParams) graphics::g_adjustPresentParams(pp);
    HRESULT hr = g_realCreateDevice(d3d, adapter, type, focus, flags, pp, out);
    // a windowed device can't be made while another program holds exclusive full screen: dev background runs wait
    for (int i = 0; hr == D3DERR_DEVICELOST && graphics::g_adjustPresentParams && i < 240; ++i) {
        if (i % 20 == 0) dslog::Write("Graphics: CreateDevice -> device lost (another full-screen program?) - retrying");
        Sleep(500);
        hr = g_realCreateDevice(d3d, adapter, type, focus, flags, pp, out);
    }
    dslog::Write("Graphics: CreateDevice %ux%u %s -> 0x%08lX", pp->BackBufferWidth, pp->BackBufferHeight,
                 pp->Windowed ? "windowed" : "fullscreen", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && *out) {
        g_device = *out;
        HookDevice(*out);
        if (graphics::g_onDeviceCreated)
            graphics::g_onDeviceCreated(static_cast<int>(pp->BackBufferWidth), static_cast<int>(pp->BackBufferHeight));
    }
    return hr;
}

IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdk) {
    IDirect3D9* d3d = g_realCreate(sdk);
    if (d3d)
        HookVtable(d3d, 16, reinterpret_cast<void*>(&HookCreateDevice), reinterpret_cast<void**>(&g_realCreateDevice));
    return d3d;
}

// D3DXMatrixPerspectiveFovLH (import 0x6C03FC): each distinct (fov, aspect) the game asks for is logged once - whether
// the 3D view follows the screen's shape.
using PerspFn = void*(WINAPI*)(void*, float, float, float, float);
PerspFn g_realPersp = nullptr;
float g_seenPersp[32][2];
int g_seenPerspCount = 0;
void* WINAPI HookPerspective(void* out, float fovY, float aspect, float zn, float zf) {
    bool seen = false;
    for (int i = 0; i < g_seenPerspCount; ++i)
        if (g_seenPersp[i][0] == fovY && g_seenPersp[i][1] == aspect) seen = true;
    if (!seen && g_seenPerspCount < 32) {
        g_seenPersp[g_seenPerspCount][0] = fovY;
        g_seenPersp[g_seenPerspCount++][1] = aspect;
        dslog::Write("Graphics: perspective fovY %.4f (%.1f deg) aspect %.4f near %.1f far %.1f (caller %p)", fovY,
                     fovY * 57.29578f, aspect, zn, zf, _ReturnAddress());
    }
    return g_realPersp(out, fovY, aspect, zn, zf);
}
}  // namespace

void graphics::SetMenuSqueeze(float s, float centreX, float screenW) {
    g_squeeze = s;
    g_squeezeCx = centreX;
    g_squeezeW = screenW;
}

void graphics::OnFrame() {
    if (g_device) HookDevice(g_device);
}

void graphics::Install() {
    if (g_realCreate) return;
#ifndef DS_DIST
    DWORD v = 0, size = sizeof v;
    g_drawProbe = RegGetValueA(HKEY_CURRENT_USER, "Software\\GlobalStormFix\\Dev", "DrawProbe", RRF_RT_REG_DWORD, nullptr,
                               &v, &size) == ERROR_SUCCESS && v;
#endif
    g_realCreate = *reinterpret_cast<CreateFn*>(kCreateSlot);
    auto mine = &HookDirect3DCreate9;
    patch::WriteValue(kCreateSlot, mine);
    g_realPersp = *reinterpret_cast<PerspFn*>(0x6C03FC);
    auto persp = &HookPerspective;
    patch::WriteValue(0x6C03FC, persp);
}

void graphics::SetVirtualScreen(bool on, int realW, int realH) {
    D3DVIEWPORT9 vp = {};
    const bool haveVp = g_device && g_realSetViewport && SUCCEEDED(g_device->GetViewport(&vp));
    if (on && realW > 0 && realH > 0) {
        if (g_virtual) return;
        g_realW = realW;
        g_realH = realH;
        g_f = std::min(realW / 800.0f, realH / 600.0f);
        g_ox = (realW - 800.0f * g_f) * 0.5f;
        g_oy = (realH - 600.0f * g_f) * 0.5f;
        g_virtual = true;
        // a full-screen viewport already set becomes the full virtual one (the game doesn't set it again)
        if (haveVp && vp.X == 0 && vp.Y == 0 && vp.Width == static_cast<DWORD>(realW) && vp.Height == static_cast<DWORD>(realH)) {
            vp.Width = 800;
            vp.Height = 600;
            HookSetViewport(g_device, &vp);
        }
        dslog::Write("Graphics: virtual 800x600 screen on %dx%d (x%.3f, offset %.0f, %.0f)", realW, realH, g_f, g_ox, g_oy);
    } else if (!on && g_virtual) {
        g_virtual = false;
        // the full virtual viewport becomes the full real one again
        if (haveVp && vp.X == static_cast<DWORD>(g_ox + 0.5f) && vp.Y == static_cast<DWORD>(g_oy + 0.5f)) {
            D3DVIEWPORT9 full = {0, 0, static_cast<DWORD>(g_realW), static_cast<DWORD>(g_realH), vp.MinZ, vp.MaxZ};
            g_realSetViewport(g_device, &full);
        }
        dslog::Write("Graphics: virtual screen off");
    }
}

bool graphics::VirtualScreenOn() { return g_virtual; }
