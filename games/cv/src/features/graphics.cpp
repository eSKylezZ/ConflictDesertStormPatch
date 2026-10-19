#include "features/graphics.h"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "core/patch.h"

namespace graphics {
void (*g_adjustPresentParams)(D3DPRESENT_PARAMETERS*) = nullptr;
void (*g_onPresent)(IDirect3DDevice9*) = nullptr;
void (*g_onDeviceCreated)(int, int) = nullptr;
bool g_retryLostCreate = false;
}  // namespace graphics

namespace {
constexpr uint32_t kCreateSlot = 0x69F33C;   // Direct3DCreate9 import slot

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
int g_realW = 0, g_realH = 0, g_virtW = 800, g_virtH = 600;
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

#ifndef DS_DIST
void FirstCall(const char* what, bool& seen) {
    if (!seen) {
        seen = true;
        dslog::Write("[dev] graphics: first %s", what);
    }
}
bool g_seenClear = false, g_seenPresent = false, g_seenSwapPresent = false, g_seenReset = false;
#define DS_FIRST(what, flag) FirstCall(what, flag)
#else
#define DS_FIRST(what, flag) ((void)0)
#endif

#ifndef DS_DIST
// dev: TestCooperativeLevel results (the game's loop renders only while it says OK) - each change logged
using TestCoopFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
TestCoopFn g_realTestCoop = nullptr;
HRESULT g_lastCoop = 1;
HRESULT STDMETHODCALLTYPE HookTestCoop(IDirect3DDevice9* dev) {
    const HRESULT hr = g_realTestCoop(dev);
    if (hr != g_lastCoop) {
        g_lastCoop = hr;
        dslog::Write("[dev] graphics: TestCooperativeLevel -> 0x%08lX", static_cast<unsigned long>(hr));
    }
    return hr;
}
#endif

HRESULT STDMETHODCALLTYPE HookDrawUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT prims, const void* data,
                                     UINT stride) {
#ifndef DS_DIST
    NoteDraw(dev, 1, "DrawPrimitiveUP");
#endif
    if (g_virtual && data && stride >= 8 && PreTransformed(dev))
        if (const UINT n = VertexCount(type, prims)) data = Mapped(data, n, stride);
    return g_realDrawUP(dev, type, prims, data, stride);
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT minIndex, UINT count,
                                            UINT prims, const void* indices, D3DFORMAT fmt, const void* data,
                                            UINT stride) {
#ifndef DS_DIST
    NoteDraw(dev, 2, "DrawIndexedPrimitiveUP");
#endif
    if (g_virtual && data && stride >= 8 && PreTransformed(dev)) {
        // the indexed vertices start at minIndex: map [0, minIndex + count) so the indices stay valid
        data = Mapped(data, minIndex + count, stride);
    }
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
    // the game also sets the full back-buffer viewport (0x537420, from the device's real size): the whole virtual screen
    if (vp->X == 0 && vp->Y == 0 && vp->Width == static_cast<DWORD>(g_realW) && vp->Height == static_cast<DWORD>(g_realH)) {
        m.Width = static_cast<DWORD>(g_virtW);
        m.Height = static_cast<DWORD>(g_virtH);
        vp = &m;
    }
    m.X = static_cast<DWORD>(g_ox + vp->X * g_f + 0.5f);
    m.Y = static_cast<DWORD>(g_oy + vp->Y * g_f + 0.5f);
    m.Width = std::min<DWORD>(static_cast<DWORD>(vp->Width * g_f + 0.5f), static_cast<DWORD>(g_realW) - m.X);
    m.Height = std::min<DWORD>(static_cast<DWORD>(vp->Height * g_f + 0.5f), static_cast<DWORD>(g_realH) - m.Y);
    return g_realSetViewport(dev, &m);
}

HRESULT STDMETHODCALLTYPE HookSetScissor(IDirect3DDevice9* dev, const RECT* r) {
    if (!g_virtual || !r) return g_realSetScissor(dev, r);
    RECT m = {static_cast<LONG>(g_ox + r->left * g_f), static_cast<LONG>(g_oy + r->top * g_f),
              static_cast<LONG>(g_ox + r->right * g_f + 0.5f), static_cast<LONG>(g_oy + r->bottom * g_f + 0.5f)};
    return g_realSetScissor(dev, &m);
}

HRESULT STDMETHODCALLTYPE HookClear(IDirect3DDevice9* dev, DWORD count, const D3DRECT* rects, DWORD flags,
                                    D3DCOLOR colour, float z, DWORD stencil) {
    DS_FIRST("Clear", g_seenClear);
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

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* c) {
    DS_FIRST("device Present", g_seenPresent);
    if (graphics::g_onPresent) graphics::g_onPresent(dev);
    return g_realPresent(dev, a, b, w, c);
}

// Vietnam presents through the implicit swap chain (IDirect3DSwapChain9::Present, vtable 3), not the device's Present
using SwapPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
SwapPresentFn g_realSwapPresent = nullptr;
HRESULT STDMETHODCALLTYPE HookSwapPresent(IDirect3DSwapChain9* sc, const RECT* a, const RECT* b, HWND w, const RGNDATA* c,
                                          DWORD flags) {
    DS_FIRST("swap chain Present", g_seenSwapPresent);
    IDirect3DDevice9* dev = nullptr;
    if (graphics::g_onPresent && SUCCEEDED(sc->GetDevice(&dev)) && dev) {
        graphics::g_onPresent(dev);
        dev->Release();
    }
    return g_realSwapPresent(sc, a, b, w, c, flags);
}

HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    DS_FIRST("Reset", g_seenReset);
    if (g_virtual && pp) {   // the game's fields say the virtual size while it is on
        pp->BackBufferWidth = static_cast<UINT>(g_realW);
        pp->BackBufferHeight = static_cast<UINT>(g_realH);
    }
    if (graphics::g_adjustPresentParams) graphics::g_adjustPresentParams(pp);
    return g_realReset(dev, pp);
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

void HookDevice(IDirect3DDevice9* dev);

HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                           D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    if (graphics::g_adjustPresentParams) graphics::g_adjustPresentParams(pp);
    HRESULT hr = g_realCreateDevice(d3d, adapter, type, focus, flags, pp, out);
    // dev background runs: a windowed device can't be made while the display is unavailable (locked / asleep / another
    // program in exclusive full screen) - D3DERR_DEVICELOST; wait for it instead of letting the game quit
    for (int i = 0; hr == D3DERR_DEVICELOST && graphics::g_retryLostCreate && i < 120; ++i) {
        if (i == 0) dslog::Write("Graphics: CreateDevice -> device lost (display unavailable?) - retrying every second");
        Sleep(1000);
        hr = g_realCreateDevice(d3d, adapter, type, focus, flags, pp, out);
    }
    dslog::Write("Graphics: CreateDevice %ux%u %s -> 0x%08lX", pp->BackBufferWidth, pp->BackBufferHeight,
                 pp->Windowed ? "windowed" : "fullscreen", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr) && *out) {
        g_device = *out;
#ifndef DS_DIST
        dslog::Write("[dev] graphics: device %p vt %p", static_cast<void*>(g_device), *reinterpret_cast<void**>(g_device));
#endif
        HookDevice(g_device);
        if (graphics::g_onDeviceCreated)
            graphics::g_onDeviceCreated(static_cast<int>(pp->BackBufferWidth), static_cast<int>(pp->BackBufferHeight));
    }
    return hr;
}

// Vietnam's Direct3D runtime keeps a per-device copy of the vtable and rewrites entries after creation (seen: every
// hooked slot back to the runtime's own function by the first film frame) - so the hooks are put back every frame
// (graphics::Rehook from the frame hook). HookVtable keeps the latest runtime function as the one to call.
void HookDevice(IDirect3DDevice9* dev) {
    g_device = dev;
    HookVtable(dev, kReset, reinterpret_cast<void*>(&HookReset), reinterpret_cast<void**>(&g_realReset));
    HookVtable(dev, kPresent, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_realPresent));
#ifndef DS_DIST
    HookVtable(dev, 3, reinterpret_cast<void*>(&HookTestCoop), reinterpret_cast<void**>(&g_realTestCoop));
#endif
    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
        HookVtable(sc, 3, reinterpret_cast<void*>(&HookSwapPresent), reinterpret_cast<void**>(&g_realSwapPresent));
        sc->Release();
    }
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

IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdk) {
    IDirect3D9* d3d = g_realCreate(sdk);
    if (d3d)
        HookVtable(d3d, 16, reinterpret_cast<void*>(&HookCreateDevice), reinterpret_cast<void**>(&g_realCreateDevice));
    return d3d;
}
}  // namespace

void graphics::Install() {
    if (g_realCreate) return;
    g_realCreate = *reinterpret_cast<CreateFn*>(kCreateSlot);
    auto mine = &HookDirect3DCreate9;
    patch::WriteValue(kCreateSlot, mine);
}

void graphics::SetVirtualScreen(bool on, int realW, int realH, int virtW, int virtH) {
    D3DVIEWPORT9 vp = {};
    const bool haveVp = g_device && g_realSetViewport && SUCCEEDED(g_device->GetViewport(&vp));
    if (on && realW > 0 && realH > 0) {
        if (g_virtual) return;
        g_realW = realW;
        g_realH = realH;
        g_virtW = virtW;
        g_virtH = virtH;
        g_f = std::min(static_cast<float>(realW) / virtW, static_cast<float>(realH) / virtH);
        g_ox = (realW - virtW * g_f) * 0.5f;
        g_oy = (realH - virtH * g_f) * 0.5f;
        g_virtual = true;
        // a full-screen viewport already set becomes the full virtual one (the game doesn't set it again)
        if (haveVp && vp.X == 0 && vp.Y == 0 && vp.Width == static_cast<DWORD>(realW) && vp.Height == static_cast<DWORD>(realH)) {
            vp.Width = static_cast<DWORD>(virtW);
            vp.Height = static_cast<DWORD>(virtH);
            HookSetViewport(g_device, &vp);
        }
        dslog::Write("Graphics: virtual %dx%d screen on %dx%d (x%.3f, offset %.0f, %.0f)", virtW, virtH, realW, realH, g_f,
                     g_ox, g_oy);
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

void graphics::Rehook() {
    if (g_device) HookDevice(g_device);
}
