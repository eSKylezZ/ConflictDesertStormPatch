#include "features/graphics.h"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"

namespace graphics {
void (*g_adjustPresentParams)(D3DPRESENT_PARAMETERS*) = nullptr;
void (*g_onPresent)(IDirect3DDevice9*) = nullptr;
void (*g_onDeviceCreated)(int, int) = nullptr;
}  // namespace graphics

namespace {
constexpr uint32_t kCreateSlot = 0x5DA2C4;   // Direct3DCreate9 import slot

// IDirect3DDevice9 vtable slots
enum : int { kReset = 16, kPresent = 17, kSetRenderTarget = 37, kClear = 43, kSetTransform = 44, kSetViewport = 47,
             kGetViewport = 48, kSetScissorRect = 75, kDrawPrimitive = 81,
             kDrawIndexedPrimitive = 82, kDrawPrimitiveUP = 83, kDrawIndexedPrimitiveUP = 84 };

using CreateFn = IDirect3D9*(WINAPI*)(UINT);
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                   IDirect3DDevice9**);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ClearFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using SetViewportFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using GetViewportFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DVIEWPORT9*);
using SetRenderTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetTransformFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
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
SetTransformFn g_realSetTransform = nullptr;
GetViewportFn g_realGetViewport = nullptr;
SetRenderTargetFn g_realSetRenderTarget = nullptr;
DrawFn g_realDraw = nullptr;
DrawIndexedFn g_realDrawIndexed = nullptr;
DrawUPFn g_realDrawUP = nullptr;
DrawIndexedUPFn g_realDrawIndexedUP = nullptr;

// virtual screen: x' = ox + x * f, y' = oy + y * f
IDirect3DDevice9* g_device = nullptr;
bool g_virtual = false;
int g_realW = 0, g_realH = 0;
float g_f = 1.0f, g_ox = 0.0f, g_oy = 0.0f;
// render target 0 is the back buffer (only that is mapped - render-to-texture passes keep their own pixels)
bool g_onBackBuffer = true;
// first Present after the device was made (log: did the start-up draw anything?)
DWORD g_deviceTime = 0;
bool g_firstPresentLogged = false;
// the viewport as the game last set it (unmapped): GetViewport returns it, else a get / change / set round trip maps
// an already-mapped viewport again every frame (seen in fullscreen: the intro's viewport ran off the screen)
D3DVIEWPORT9 g_gameVp = {};
bool g_haveGameVp = false;

// Menu zoom (setting MenuZoom): the box around the front-end's monitor (778 x 585 at (251, 220) in the 1280 x 1024
// picture, centred on (640, 512)) fills the screen height; what lies outside that box is black anyway. Only while
// the monitor is drawn (3D on the virtual screen): the logo films and loading pictures use the whole picture, the zoom
// would crop them. Decided per frame at Present (on after a frame with 3D, off after 3 frames without).
constexpr float kZoomW = 820.0f, kZoomH = 620.0f, kZoomCx = 640.0f, kZoomCy = 512.0f;
bool g_zoomed = false, g_saw3d = false;
int g_framesWithout3d = 0;

// A mapped viewport larger than the screen (menu zoom) is clamped to it - D3D9 refuses viewports outside the target -
// and the projection gets a matching clip-space correction so 3D (the monitor, the globe) keeps its place and shape:
// x' = sx * x + tx * w, y' = sy * y + ty * w.
D3DMATRIX g_gameProj = {};
bool g_haveGameProj = false;
float g_fixSx = 1, g_fixSy = 1, g_fixTx = 0, g_fixTy = 0;
bool g_fixOn = false;

bool Mapping() { return g_virtual && g_onBackBuffer; }

void ComputeMapping() {
    if (g_zoomed) {
        g_f = std::min(g_realW / kZoomW, g_realH / kZoomH);
        g_ox = g_realW * 0.5f - kZoomCx * g_f;
        g_oy = g_realH * 0.5f - kZoomCy * g_f;
    } else {
        g_f = std::min(g_realW / float(graphics::kVirtualW), g_realH / float(graphics::kVirtualH));
        g_ox = (g_realW - graphics::kVirtualW * g_f) * 0.5f;
        g_oy = (g_realH - graphics::kVirtualH * g_f) * 0.5f;
    }
}

// the projection the device should have: the game's, corrected while a clamped viewport is in use
void ApplyProjection(IDirect3DDevice9* dev) {
    if (!g_haveGameProj || !g_realSetTransform) return;
    if (!g_fixOn) {
        g_realSetTransform(dev, D3DTS_PROJECTION, &g_gameProj);
        return;
    }
    D3DMATRIX r = g_gameProj;
    for (int i = 0; i < 4; ++i) {
        r.m[i][0] = g_gameProj.m[i][0] * g_fixSx + g_gameProj.m[i][3] * g_fixTx;
        r.m[i][1] = g_gameProj.m[i][1] * g_fixSy + g_gameProj.m[i][3] * g_fixTy;
    }
    g_realSetTransform(dev, D3DTS_PROJECTION, &r);
}

void SetFix(IDirect3DDevice9* dev, bool on, float sx = 1, float sy = 1, float tx = 0, float ty = 0) {
    if (on == g_fixOn && (!on || (sx == g_fixSx && sy == g_fixSy && tx == g_fixTx && ty == g_fixTy))) return;
    g_fixOn = on;
    g_fixSx = sx, g_fixSy = sy, g_fixTx = tx, g_fixTy = ty;
    ApplyProjection(dev);
}
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
void NoteDraw(IDirect3DDevice9* dev, int call, const char* name, void* ret) {
    if (!g_virtual) return;
    DWORD fvf = 0;
    dev->GetFVF(&fvf);
    for (int i = 0; i < g_seenDrawCount; ++i)
        if (g_seenDraws[i].call == call && g_seenDraws[i].fvf == fvf) return;
    if (g_seenDrawCount < 64) g_seenDraws[g_seenDrawCount++] = {call, fvf};
    dslog::Write("[dev] virtual screen: %s FVF 0x%03lX (caller %p)", name, static_cast<unsigned long>(fvf), ret);
}
D3DVIEWPORT9 g_seenVps[32];
int g_seenVpCount = 0;
void NoteViewport(const D3DVIEWPORT9* vp, void* caller) {
    for (int i = 0; i < g_seenVpCount; ++i)
        if (!std::memcmp(&g_seenVps[i], vp, sizeof *vp)) return;
    if (g_seenVpCount >= 32) return;
    g_seenVps[g_seenVpCount++] = *vp;
    char module[MAX_PATH] = "?";
    HMODULE m = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(caller), &m))
        GetModuleFileNameA(m, module, MAX_PATH);
    const char* base = std::strrchr(module, '\\');
    dslog::Write("[dev] virtual screen: SetViewport %lu,%lu %lux%lu (caller %p %s)", vp->X, vp->Y, vp->Width, vp->Height,
                 caller, base ? base + 1 : module);
}
// projection matrices set while the virtual screen is on (distinct ones, with the caller)
D3DMATRIX g_seenProj[16];
int g_seenProjCount = 0;
void NoteProjection(const D3DMATRIX* m, void* caller) {
    for (int i = 0; i < g_seenProjCount; ++i)
        if (!std::memcmp(&g_seenProj[i], m, sizeof *m)) return;
    if (g_seenProjCount >= 16) return;
    g_seenProj[g_seenProjCount++] = *m;
    dslog::Write("[dev] virtual screen: projection (caller %p) %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | "
                 "%.3f %.3f %.3f %.3f", caller, m->_11, m->_12, m->_13, m->_14, m->_21, m->_22, m->_23, m->_24, m->_31,
                 m->_32, m->_33, m->_34, m->_41, m->_42, m->_43, m->_44);
}
#endif

HRESULT STDMETHODCALLTYPE HookSetTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m) {
    if (state != D3DTS_PROJECTION || !m) return g_realSetTransform(dev, state, m);
#ifndef DS_DIST
    if (g_virtual) NoteProjection(m, _ReturnAddress());
#endif
    g_gameProj = *m;
    g_haveGameProj = true;
    if (!g_fixOn) return g_realSetTransform(dev, state, m);
    ApplyProjection(dev);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE HookDrawUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT prims, const void* data,
                                     UINT stride) {
#ifndef DS_DIST
    NoteDraw(dev, 1, "DrawPrimitiveUP", _ReturnAddress());
#endif
    if (Mapping() && data && stride >= 8 && PreTransformed(dev))
        if (const UINT n = VertexCount(type, prims)) data = Mapped(data, n, stride);
    return g_realDrawUP(dev, type, prims, data, stride);
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT minIndex, UINT count,
                                            UINT prims, const void* indices, D3DFORMAT fmt, const void* data,
                                            UINT stride) {
#ifndef DS_DIST
    NoteDraw(dev, 2, "DrawIndexedPrimitiveUP", _ReturnAddress());
#endif
    if (Mapping() && data && stride >= 8 && PreTransformed(dev)) {
        // the indexed vertices start at minIndex: map [0, minIndex + count) so the indices stay valid
        data = Mapped(data, minIndex + count, stride);
    }
    return g_realDrawIndexedUP(dev, type, minIndex, count, prims, indices, fmt, data, stride);
}

HRESULT STDMETHODCALLTYPE HookDraw(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT start, UINT prims) {
#ifndef DS_DIST
    NoteVertexBufferDraw(dev, "DrawPrimitive");
    NoteDraw(dev, 3, "DrawPrimitive", _ReturnAddress());
#endif
    return g_realDraw(dev, type, start, prims);
}

HRESULT STDMETHODCALLTYPE HookDrawIndexed(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT base, UINT minIndex,
                                          UINT count, UINT start, UINT prims) {
#ifndef DS_DIST
    NoteVertexBufferDraw(dev, "DrawIndexedPrimitive");
    NoteDraw(dev, 4, "DrawIndexedPrimitive", _ReturnAddress());
#endif
    if (Mapping() && !PreTransformed(dev)) g_saw3d = true;
    return g_realDrawIndexed(dev, type, base, minIndex, count, start, prims);
}

HRESULT STDMETHODCALLTYPE HookSetViewport(IDirect3DDevice9* dev, const D3DVIEWPORT9* vp) {
    if (!Mapping() || !vp) return g_realSetViewport(dev, vp);
#ifndef DS_DIST
    NoteViewport(vp, _ReturnAddress());
#endif
    g_gameVp = *vp;
    g_haveGameVp = true;
    D3DVIEWPORT9 m = *vp;
    // the game also sets the full back-buffer viewport (0x537420, from the device's real size): the whole virtual screen
    if (vp->X == 0 && vp->Y == 0 && vp->Width == static_cast<DWORD>(g_realW) && vp->Height == static_cast<DWORD>(g_realH)) {
        m.Width = graphics::kVirtualW;
        m.Height = graphics::kVirtualH;
        vp = &m;
    }
    // the wanted rectangle (may reach past the screen with the menu zoom), then the part on the screen
    const float x = g_ox + vp->X * g_f, y = g_oy + vp->Y * g_f, w = vp->Width * g_f, h = vp->Height * g_f;
    const float x0 = std::clamp(std::floor(x + 0.5f), 0.0f, float(g_realW - 1));
    const float y0 = std::clamp(std::floor(y + 0.5f), 0.0f, float(g_realH - 1));
    const float x1 = std::clamp(std::floor(x + w + 0.5f), x0 + 1, float(g_realW));
    const float y1 = std::clamp(std::floor(y + h + 0.5f), y0 + 1, float(g_realH));
    m.X = DWORD(x0), m.Y = DWORD(y0), m.Width = DWORD(x1 - x0), m.Height = DWORD(y1 - y0);
    const float cw = x1 - x0, ch = y1 - y0;
    if (std::fabs(cw - w) > 1.0f || std::fabs(ch - h) > 1.0f)
        SetFix(dev, true, w / cw, h / ch, (2 * (x - x0) + w - cw) / cw, (ch - h - 2 * (y - y0)) / ch);
    else
        SetFix(dev, false);
    return g_realSetViewport(dev, &m);
}

HRESULT STDMETHODCALLTYPE HookGetViewport(IDirect3DDevice9* dev, D3DVIEWPORT9* vp) {
    if (Mapping() && vp && g_haveGameVp) {
        *vp = g_gameVp;
        return D3D_OK;
    }
    return g_realGetViewport(dev, vp);
}

// D3D9 resets the viewport to the whole target on SetRenderTarget(0, ...): back on the back buffer that is the whole
// (mapped) virtual screen, on a texture its own size
HRESULT STDMETHODCALLTYPE HookSetRenderTarget(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* surface) {
    if (index != 0) return g_realSetRenderTarget(dev, index, surface);
    bool back = false;
    IDirect3DSurface9* bb = nullptr;
    if (surface && SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        back = bb == surface;
        bb->Release();
    }
    // set before the call: d3d9's SetRenderTarget resets the viewport through the (hooked) SetViewport
    const bool before = g_onBackBuffer;
    g_onBackBuffer = back;
    if (!back) SetFix(dev, false);
    const HRESULT hr = g_realSetRenderTarget(dev, index, surface);
    if (FAILED(hr)) {
        g_onBackBuffer = before;
        return hr;
    }
    if (g_virtual && back) {
        D3DVIEWPORT9 full = {0, 0, static_cast<DWORD>(graphics::kVirtualW), static_cast<DWORD>(graphics::kVirtualH), 0.0f,
                             1.0f};
        HookSetViewport(dev, &full);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookSetScissor(IDirect3DDevice9* dev, const RECT* r) {
    if (!Mapping() || !r) return g_realSetScissor(dev, r);
    RECT m = {static_cast<LONG>(g_ox + r->left * g_f), static_cast<LONG>(g_oy + r->top * g_f),
              static_cast<LONG>(g_ox + r->right * g_f + 0.5f), static_cast<LONG>(g_oy + r->bottom * g_f + 0.5f)};
    m.left = std::clamp<LONG>(m.left, 0, g_realW), m.right = std::clamp<LONG>(m.right, 0, g_realW);
    m.top = std::clamp<LONG>(m.top, 0, g_realH), m.bottom = std::clamp<LONG>(m.bottom, 0, g_realH);
    return g_realSetScissor(dev, &m);
}

HRESULT STDMETHODCALLTYPE HookClear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags,
                                    D3DCOLOR colour, float z, DWORD stencil) {
    if (!Mapping()) return g_realClear(dev, count, rects, flags, colour, z, stencil);
    if (!count || !rects) {
        // a clear of the (mapped) viewport: clear the whole back buffer, so nothing stale stays around the picture
        D3DVIEWPORT9 vp, full = {0, 0, static_cast<DWORD>(g_realW), static_cast<DWORD>(g_realH), 0.0f, 1.0f};
        g_realGetViewport(dev, &vp);
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
        r.x1 = std::clamp<LONG>(r.x1, 0, g_realW), r.x2 = std::clamp<LONG>(r.x2, 0, g_realW);
        r.y1 = std::clamp<LONG>(r.y1, 0, g_realH), r.y2 = std::clamp<LONG>(r.y2, 0, g_realH);
    }
    return g_realClear(dev, count, m.data(), flags, colour, z, stencil);
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* c) {
    if (graphics::g_onPresent) graphics::g_onPresent(dev);
    const HRESULT hr = g_realPresent(dev, a, b, w, c);
    if (!g_firstPresentLogged) {   // tells from a log whether the start-up (logo films) drew anything
        g_firstPresentLogged = true;
        dslog::Write("Graphics: first Present %lu ms after CreateDevice -> 0x%08lX", GetTickCount() - g_deviceTime,
                     static_cast<unsigned long>(hr));
    }
    if (g_virtual) {
        g_framesWithout3d = g_saw3d ? 0 : g_framesWithout3d + 1;
        const bool zoom = settings::Get().menuZoom && (g_saw3d || (g_zoomed && g_framesWithout3d < 3));
        g_saw3d = false;
        if (zoom != g_zoomed) {
            g_zoomed = zoom;
            ComputeMapping();
            if (g_haveGameVp && g_onBackBuffer) {  // the game sets its full viewport once - re-map it
                const D3DVIEWPORT9 vp = g_gameVp;
                HookSetViewport(dev, &vp);
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (g_virtual && pp) {   // the game's fields say kVirtualW x kVirtualH while the virtual screen is on
        pp->BackBufferWidth = static_cast<UINT>(g_realW);
        pp->BackBufferHeight = static_cast<UINT>(g_realH);
    }
    if (graphics::g_adjustPresentParams) graphics::g_adjustPresentParams(pp);
    const HRESULT hr = g_realReset(dev, pp);
    if (SUCCEEDED(hr)) {   // back on the back buffer with a full-target viewport
        g_onBackBuffer = true;
        if (g_virtual) {
            D3DVIEWPORT9 full = {0, 0, static_cast<DWORD>(graphics::kVirtualW), static_cast<DWORD>(graphics::kVirtualH),
                                 0.0f, 1.0f};
            HookSetViewport(dev, &full);
        }
    }
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

// ---- The game window: the white box at start-up ----
// The window (FUN_0040e970: class "CDS II", black class brush, created hidden at 800 x 600 with its client area at the
// screen's top-left) never paints itself: its proc LAB_0040e7d0 answers WM_ERASEBKGND (0x14) with 1 without erasing and
// WM_PAINT with an empty BeginPaint / EndPaint. So until Direct3D presents into it, the desktop compositor shows the
// window's never-drawn surface - white where the window first existed (the 800 x 600 creation rectangle), black elsewhere.
// Nothing presents while the game's message loop FUN_00414ac0 idles: it runs frames (films included - the logo films
// draw + present from FUN_0042cbf0 inside the frame) only while the active flag [0x62F4BC] is set, and only WM_ACTIVATE
// (proc: wParam != 0 -> 1) sets it. The launcher dialog (DialogBoxParamA at 0x421AA7) is owned by the still hidden game
// window, so closing it hands the activation to another program; the fullscreen placement (0x411F80: style, topmost
// SetWindowPos, SetFocus) doesn't necessarily win it back -> the loop sits in WaitMessage (or, with the fullscreen device
// lost because its window isn't in front, in the TestCooperativeLevel / Sleep(100) branch of FUN_00541c50), black screen
// + white box, until a click activates the window. (From code reading; the user's fullscreen log showed no film draw
// before the menu. The log lines below - activation messages, foreground / flag after CreateDevice, first Present - tell.)
// Fix: the window paints itself black (subclass), is brought to the foreground before the fullscreen device is made,
// and the game's active flag is brought in line with the real activation for the first seconds.
constexpr uint32_t kActiveFlag = 0x62F4BC;   // message loop: run frames (set / cleared by WM_ACTIVATE)
constexpr UINT_PTR kActiveTimer = 0x44533246;   // "DS2F"
HWND g_window = nullptr;
WNDPROC g_gameProc = nullptr;
int g_activeChecks = 0;

int& ActiveFlag() { return *reinterpret_cast<int*>(kActiveFlag); }

void PaintBlack(HWND window, HDC dc) {
    RECT rc;
    if (GetClientRect(window, &rc)) FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
}

LRESULT CALLBACK WindowProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:   // the game's proc returns 1 without erasing - erase to black
            PaintBlack(window, reinterpret_cast<HDC>(wp));
            return 1;
        case WM_ACTIVATE:
        case WM_ACTIVATEAPP:
            dslog::Write("Graphics: window %s %s (foreground %s)", msg == WM_ACTIVATE ? "WM_ACTIVATE" : "WM_ACTIVATEAPP",
                         LOWORD(wp) ? "on" : "off", GetForegroundWindow() == window ? "yes" : "no");
            break;
        case WM_TIMER:
            if (wp != kActiveTimer) break;
            // the window has the foreground but the game still thinks it is inactive (never got its WM_ACTIVATE):
            // hand it the activation it missed
            if (GetForegroundWindow() == window && !ActiveFlag() && !IsIconic(window)) {
                dslog::Write("Graphics: window in front but the game's active flag is off - activating");
                CallWindowProcA(g_gameProc, window, WM_ACTIVATE, WA_ACTIVE, 0);
            }
            if (++g_activeChecks >= 40) KillTimer(window, kActiveTimer);   // 10 s
            return 0;
        default:
            break;
    }
    return CallWindowProcA(g_gameProc, window, msg, wp, lp);
}

void SubclassWindow(HWND window) {
    if (!window || g_window == window) return;
    auto old = reinterpret_cast<WNDPROC>(GetWindowLongPtrA(window, GWLP_WNDPROC));
    if (!old || old == &WindowProc) return;
    g_gameProc = old;
    g_window = window;
    SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc));
}

// What the compositor shows of the window before the first Present: black (GDI fill into its surface now).
void FillWindowBlack(HWND window) {
    if (HDC dc = GetDC(window)) {
        PaintBlack(window, dc);
        ReleaseDC(window, dc);
    }
    ValidateRect(window, nullptr);
}

// The window has to be the foreground window for a fullscreen device (else it is lost from the start) and for the
// game's loop to run. Same as DS1's +level start: attach to the foreground thread's input, then SetForegroundWindow;
// an Alt tap as the last resort (lifts the foreground lock).
void BringToFront(HWND window) {
    if (GetForegroundWindow() == window) return;
    HWND fg = GetForegroundWindow();
    const DWORD me = GetCurrentThreadId(), other = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const bool attached = other && other != me && AttachThreadInput(other, me, TRUE);
    ShowWindow(window, SW_SHOW);
    BringWindowToTop(window);
    SetForegroundWindow(window);
    if (attached) AttachThreadInput(other, me, FALSE);
    if (GetForegroundWindow() != window) {
        INPUT in[2]{};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[1].ki.wVk = VK_MENU;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        SetForegroundWindow(window);
    }
    dslog::Write("Graphics: game window was not in front - %s", GetForegroundWindow() == window ? "brought to the front"
                                                                                               : "still behind");
}

// DS1 resizes its window to the mode after the launcher; here, before a fullscreen device is made: borderless, over
// the adapter's monitor, black, in front.
void FitFullscreenWindow(IDirect3D9* d3d, UINT adapter, HWND window) {
    if (!window) return;
    MONITORINFO mi = {sizeof mi};
    HMONITOR monitor = d3d->GetAdapterMonitor(adapter);
    if (!monitor || !GetMonitorInfoA(monitor, &mi)) return;
    SetClassLongPtrA(window, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH)));
    SetWindowLongPtrA(window, GWL_STYLE, WS_POPUP | (GetWindowLongPtrA(window, GWL_STYLE) & WS_VISIBLE));
    const RECT& r = mi.rcMonitor;
    SetWindowPos(window, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    FillWindowBlack(window);
    BringToFront(window);
    dslog::Write("Graphics: fullscreen window fitted to the monitor (%ld,%ld %ldx%ld)", r.left, r.top, r.right - r.left,
                 r.bottom - r.top);
}

// After the device: the game's active flag must match the real activation (checked now and for 10 s on a timer).
void SyncActive(HWND window) {
    if (!window || g_window != window) return;
    dslog::Write("Graphics: after CreateDevice - foreground %s, game active flag %d",
                 GetForegroundWindow() == window ? "yes" : "no", ActiveFlag());
    if (GetForegroundWindow() == window && !ActiveFlag() && !IsIconic(window))
        CallWindowProcA(g_gameProc, window, WM_ACTIVATE, WA_ACTIVE, 0);
    g_activeChecks = 0;
    SetTimer(window, kActiveTimer, 250, nullptr);
}

HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                           D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    if (graphics::g_adjustPresentParams) graphics::g_adjustPresentParams(pp);
    HWND window = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    SubclassWindow(window);
    if (!pp->Windowed) FitFullscreenWindow(d3d, adapter, window);
    HRESULT hr = g_realCreateDevice(d3d, adapter, type, focus, flags, pp, out);
    dslog::Write("Graphics: CreateDevice %ux%u %s -> 0x%08lX", pp->BackBufferWidth, pp->BackBufferHeight,
                 pp->Windowed ? "windowed" : "fullscreen", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && *out) {
        IDirect3DDevice9* dev = *out;
        g_device = dev;
        g_deviceTime = GetTickCount();
        g_firstPresentLogged = false;
        if (!pp->Windowed) SyncActive(window);
        HookVtable(dev, kReset, reinterpret_cast<void*>(&HookReset), reinterpret_cast<void**>(&g_realReset));
        HookVtable(dev, kPresent, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_realPresent));
        HookVtable(dev, kClear, reinterpret_cast<void*>(&HookClear), reinterpret_cast<void**>(&g_realClear));
        HookVtable(dev, kSetViewport, reinterpret_cast<void*>(&HookSetViewport),
                   reinterpret_cast<void**>(&g_realSetViewport));
        HookVtable(dev, kSetTransform, reinterpret_cast<void*>(&HookSetTransform),
                   reinterpret_cast<void**>(&g_realSetTransform));
        HookVtable(dev, kGetViewport, reinterpret_cast<void*>(&HookGetViewport),
                   reinterpret_cast<void**>(&g_realGetViewport));
        HookVtable(dev, kSetRenderTarget, reinterpret_cast<void*>(&HookSetRenderTarget),
                   reinterpret_cast<void**>(&g_realSetRenderTarget));
        HookVtable(dev, kSetScissorRect, reinterpret_cast<void*>(&HookSetScissor),
                   reinterpret_cast<void**>(&g_realSetScissor));
        HookVtable(dev, kDrawPrimitive, reinterpret_cast<void*>(&HookDraw), reinterpret_cast<void**>(&g_realDraw));
        HookVtable(dev, kDrawIndexedPrimitive, reinterpret_cast<void*>(&HookDrawIndexed),
                   reinterpret_cast<void**>(&g_realDrawIndexed));
        HookVtable(dev, kDrawPrimitiveUP, reinterpret_cast<void*>(&HookDrawUP), reinterpret_cast<void**>(&g_realDrawUP));
        HookVtable(dev, kDrawIndexedPrimitiveUP, reinterpret_cast<void*>(&HookDrawIndexedUP),
                   reinterpret_cast<void**>(&g_realDrawIndexedUP));
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
}  // namespace

void graphics::Install() {
    if (g_realCreate) return;
#ifdef DS2_PSE_HOST
    // PSE host plugin build (src/pse/ds2fix.cpp): the import slot through the broker, chained with the DS2 game
    // layer's own Direct3DCreate9 hook (g_realCreate = the next one in the chain).
    patch::HookSlot(kCreateSlot, reinterpret_cast<const void*>(&HookDirect3DCreate9), reinterpret_cast<void**>(&g_realCreate));
#else
    g_realCreate = *reinterpret_cast<CreateFn*>(kCreateSlot);
    auto mine = &HookDirect3DCreate9;
    patch::WriteValue(kCreateSlot, mine);
#endif
}

void graphics::SetVirtualScreen(bool on, int realW, int realH) {
    D3DVIEWPORT9 vp = {};
    // the device's own viewport (not HookGetViewport's game copy)
    const bool haveVp = g_device && g_realSetViewport && g_realGetViewport && SUCCEEDED(g_realGetViewport(g_device, &vp));
    if (on && realW > 0 && realH > 0) {
        if (g_virtual) return;
        g_realW = realW;
        g_realH = realH;
        g_zoomed = false;
        g_framesWithout3d = 0;
        ComputeMapping();
        g_virtual = true;
        // a full-screen viewport already set becomes the full virtual one (the game doesn't set it again)
        if (haveVp && vp.X == 0 && vp.Y == 0 && vp.Width == static_cast<DWORD>(realW) && vp.Height == static_cast<DWORD>(realH)) {
            vp.Width = graphics::kVirtualW;
            vp.Height = graphics::kVirtualH;
            HookSetViewport(g_device, &vp);
        }
        dslog::Write("Graphics: virtual %dx%d screen on %dx%d (x%.3f, offset %.0f, %.0f)", graphics::kVirtualW, graphics::kVirtualH, realW, realH, g_f, g_ox, g_oy);
    } else if (!on && g_virtual) {
        g_virtual = false;
        g_haveGameVp = false;
        if (g_device) SetFix(g_device, false);
        // the full virtual viewport (clamped to the screen) becomes the full real one again
        if (haveVp && vp.X == static_cast<DWORD>(std::max(0.0f, g_ox) + 0.5f) &&
            vp.Y == static_cast<DWORD>(std::max(0.0f, g_oy) + 0.5f)) {
            D3DVIEWPORT9 full = {0, 0, static_cast<DWORD>(g_realW), static_cast<DWORD>(g_realH), vp.MinZ, vp.MaxZ};
            g_realSetViewport(g_device, &full);
        }
        dslog::Write("Graphics: virtual screen off");
    }
}

bool graphics::VirtualScreenOn() { return g_virtual; }

void graphics::RealToVirtual(long& x, long& y) {
    if (!g_virtual || g_f <= 0.0f) return;
    x = static_cast<long>((x - g_ox) / g_f);
    y = static_cast<long>((y - g_oy) / g_f);
}

int graphics::RealWidth() { return g_realW; }
int graphics::RealHeight() { return g_realH; }

float graphics::VirtualScale() { return g_virtual ? g_f : 1.0f; }
IDirect3DDevice9* graphics::Device() { return g_device; }
