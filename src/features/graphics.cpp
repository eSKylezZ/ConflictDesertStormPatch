// Graphics quality: anti-aliasing (MSAA) and anisotropic texture filtering, neither of which the game offers.
//
// The game gets its IDirect3D8 from Direct3DCreate8 (import slot 0x5D8308, called through the thunk at 0x59B246).
// The slot is pointed at our function, which hooks IDirect3D8::CreateDevice in the object's vtable; CreateDevice
// then hooks the device's vtable. The rest of the plugin never touches D3D, so this works the same under any D3D8
// implementation (e.g. a wrapper DLL next to the exe).
//   CreateDevice  FUN_00542ac0 builds D3DPRESENT_PARAMETERS at renderer+0x84 (X8R8G8B8 / D24S8, DISCARD, not
//                 lockable) with MultiSampleType from the DirectX sample framework (always 0 here). We write the
//                 highest sample count <= the setting that the adapter supports for both the back buffer and the
//                 depth format; the struct stays in the renderer, so a later Reset keeps it. If the device cannot
//                 be created that way, it is created again without MSAA.
//   SetTextureStageState  MINFILTER LINEAR -> ANISOTROPIC and MAXANISOTROPY forced to the setting (clamped to the
//                 card's maximum); point-filtered textures (fonts, UI) stay as they are.
//   SetRenderTarget  a multisampled depth buffer can't be used with a non-multisampled target (render-to-texture
//                 with the main depth buffer, as 0x543b70 does) - such targets get our own plain depth buffer.
//   Reset         releases that depth buffer first (default-pool resources must be gone before a Reset).
//   DrawPrimitiveUP / DrawIndexedPrimitiveUP  with MSAA, D3D8 puts pixel 0's samples on both sides of x = 0, so a
//                 pre-transformed quad starting exactly at the left/top screen edge covered only half of the first
//                 column/row - the scene showed through cinematic bars, fades and menu backdrops there. Such vertices
//                 are moved to -1 (SetVertexShader tracks whether the FVF is XYZRHW).
// Settings are read when the device is created (after the launcher), so they apply from the next start of play.
#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "core/comhooks.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/framerec.h"
#include "features/overlay.h"
#include "loader.h"
#include "render/d3d8to9.h"

namespace {
constexpr uint32_t kIatDirect3DCreate8 = 0x5D8308;

// Direct3D 8 (the Windows SDK no longer ships d3d8.h): vtable indices and the values used here.
namespace d3d8 {
enum D3D : int { kGetAdapterModeCount = 6, kEnumAdapterModes = 7, kCheckDeviceMultiSampleType = 11, kCreateDevice = 15 };
struct DisplayMode {  // D3DDISPLAYMODE
    UINT width, height, refresh;
    uint32_t format;
};
constexpr uint32_t kIntervalOne = 1;
enum Device : int {
    kTestCooperativeLevel = 3, kGetDeviceCaps = 7, kReset = 14, kPresent = 15, kCreateDepthStencilSurface = 26, kEndScene = 35, kSetRenderTarget = 31, kGetRenderTarget = 32,
    kSetRenderState = 50, kSetTextureStageState = 63, kDrawPrimitiveUP = 72, kDrawIndexedPrimitiveUP = 73, kSetVertexShader = 76, kCreateTexture = 20,
#ifndef DS_DIST
    kGetBackBuffer = 16, kCreateRenderTarget = 25, kCopyRects = 28, kGetFrontBuffer = 30,
#endif
};
enum Surface : int { kRelease = 2, kGetDesc = 8 };
constexpr uint32_t kDevHal = 1;
constexpr uint32_t kTssMinFilter = 17, kTssMaxAnisotropy = 21;
constexpr uint32_t kTexfLinear = 2, kTexfAnisotropic = 3;
constexpr uint32_t kFilterCapsMinAnisotropic = 0x400;  // D3DPTFILTERCAPS_MINFANISOTROPY
constexpr uint32_t kUsageRenderTarget = 1;
constexpr int kMaxStages = 8;

struct PresentParams {  // D3DPRESENT_PARAMETERS
    uint32_t width, height, format, backBufferCount, multiSample, swapEffect;
    HWND window;
    BOOL windowed, autoDepthStencil;
    uint32_t depthFormat, flags, refresh, interval;
};
struct SurfaceDesc {  // D3DSURFACE_DESC
    uint32_t format, type, usage, pool, size, multiSample, width, height;
};
struct Caps {  // D3DCAPS8 (only the fields read here; the buffer is larger than the real struct)
    uint32_t fields[27];
    uint32_t maxAnisotropy;  // +0x6c
    uint8_t rest[256];
};
static_assert(offsetof(Caps, maxAnisotropy) == 0x6C);
constexpr size_t kTextureFilterCaps = 16;  // Caps::fields index (+0x40)
}  // namespace d3d8

using Direct3DCreate8Fn = void*(WINAPI*)(UINT);
using CheckMultiSampleFn = HRESULT(__stdcall*)(void*, UINT, uint32_t, uint32_t, BOOL, uint32_t);
using CreateDeviceFn = HRESULT(__stdcall*)(void*, UINT, uint32_t, HWND, DWORD, d3d8::PresentParams*, void**);
using GetCapsFn = HRESULT(__stdcall*)(void*, d3d8::Caps*);
using ResetFn = HRESULT(__stdcall*)(void*, d3d8::PresentParams*);
using TestCoopFn = HRESULT(__stdcall*)(void*);
using PresentFn = HRESULT(__stdcall*)(void*, const RECT*, const RECT*, HWND, const void*);
using CreateDepthFn = HRESULT(__stdcall*)(void*, UINT, UINT, uint32_t, uint32_t, void**);
using SetRenderTargetFn = HRESULT(__stdcall*)(void*, void*, void*);
using GetRenderTargetFn = HRESULT(__stdcall*)(void*, void**);
using SetTssFn = HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD);
using EndSceneFn = HRESULT(__stdcall*)(void*);
using DrawUpFn = HRESULT(__stdcall*)(void*, uint32_t, UINT, const void*, UINT);
using DrawIndexedUpFn = HRESULT(__stdcall*)(void*, uint32_t, UINT, UINT, UINT, const void*, uint32_t, const void*, UINT);
using SetVertexShaderFn = HRESULT(__stdcall*)(void*, DWORD);
using ReleaseFn = ULONG(__stdcall*)(void*);
using GetDescFn = HRESULT(__stdcall*)(void*, d3d8::SurfaceDesc*);

// COM vtable hooks through the broker (core/comhooks.h): one record per COM class - the IDirect3D8 object's class and
// the device's (d3d8.dll's, or the D3D9 wrapper's own classes in src/render when Renderer >= 1). g_xxx(obj, ...) =
// the next function in that slot's chain for obj's class. Priority kD3dHookPriority (features.h): other plugins chain
// in front (higher) or behind (lower) at their own priority.
using DevTable = comhooks::Table<80>;
using D3dTable = comhooks::Table<16>;
DevTable g_devHooks;
D3dTable g_d3dHooks;
template <int I, class Fn>
using DevNext = comhooks::Next<DevTable, g_devHooks, I, Fn>;

Direct3DCreate8Fn g_create8 = nullptr;
const comhooks::Next<D3dTable, g_d3dHooks, d3d8::kCreateDevice, CreateDeviceFn> g_createDevice{};
const DevNext<d3d8::kReset, ResetFn> g_reset{};
const DevNext<d3d8::kTestCooperativeLevel, TestCoopFn> g_testCoop{};
bool g_forceReset = false;              // ChangeDisplayMode: TestCooperativeLevel answers DEVICENOTRESET once
d3d8::PresentParams g_fallback{};       // ChangeDisplayMode: the parameters to go back to if the new ones fail
bool g_haveFallback = false;
bool g_resetFailed = false;
const DevNext<d3d8::kPresent, PresentFn> g_present{};
const DevNext<d3d8::kSetRenderTarget, SetRenderTargetFn> g_setRenderTarget{};
const DevNext<d3d8::kSetTextureStageState, SetTssFn> g_setTss{};
const DevNext<d3d8::kEndScene, EndSceneFn> g_endScene{};
const DevNext<d3d8::kDrawPrimitiveUP, DrawUpFn> g_drawUp{};
const DevNext<d3d8::kDrawIndexedPrimitiveUP, DrawIndexedUpFn> g_drawIndexedUp{};
const DevNext<d3d8::kSetVertexShader, SetVertexShaderFn> g_setVertexShader{};
using SetRsFn = HRESULT(__stdcall*)(void*, DWORD, DWORD);
const DevNext<d3d8::kSetRenderState, SetRsFn> g_setRs{};
bool g_pretransformed = false;  // current vertex shader is an FVF with D3DFVF_XYZRHW

uint32_t g_samples = 0;     // MSAA sample count of the current device (0 = off)
uint32_t g_anisotropy = 0;  // anisotropy forced on linear-filtered textures (0 = game default)
uint32_t g_maxAnisotropy = 0;  // the device's limit (0 = no anisotropic minification)
void* g_device = nullptr;      // the device made by CreateDevice (OPTIONS > VIDEO changes the filtering live)
void* g_ownDepth = nullptr; // plain depth buffer for non-multisampled render targets
d3d8::SurfaceDesc g_ownDepthDesc{};

void** Vtable(void* obj) { return *static_cast<void***>(obj); }

template <class Fn>
Fn Method(void* obj, int index) {
    return reinterpret_cast<Fn>(Vtable(obj)[index]);
}

bool Desc(void* surface, d3d8::SurfaceDesc& d) {
    return surface && SUCCEEDED(Method<GetDescFn>(surface, d3d8::kGetDesc)(surface, &d));
}

void Release(void*& obj) {
    if (obj) Method<ReleaseFn>(obj, d3d8::kRelease)(obj);
    obj = nullptr;
}

HRESULT __stdcall SetTextureStageState(void* dev, DWORD stage, DWORD type, DWORD value) {
#ifndef DS_DIST
    // Which filter states the game uses (MAGFILTER 16 .. MAXANISOTROPY 21), once each.
    static uint8_t seen[6][8];
    if (type >= 16 && type <= 21 && value < 8 && !seen[type - 16][value]) {
        seen[type - 16][value] = 1;
        dslog::Write("Graphics: game sets texture stage state %lu = %lu (stage %lu)", type, value, stage);
    }
#endif
    if (g_anisotropy > 1) {
        if (type == d3d8::kTssMinFilter && value == d3d8::kTexfLinear) value = d3d8::kTexfAnisotropic;
        else if (type == d3d8::kTssMaxAnisotropy) value = g_anisotropy;
    }
    return g_setTss(dev, stage, type, value);
}

void SetAnisotropyLevels(void* dev) {
    if (g_anisotropy > 1)
        for (DWORD s = 0; s < d3d8::kMaxStages; ++s) g_setTss(dev, s, d3d8::kTssMaxAnisotropy, g_anisotropy);
}

HRESULT __stdcall SetRenderTarget(void* dev, void* target, void* depth) {
    if (g_samples && depth) {
        void* current = nullptr;
        if (!target && SUCCEEDED(Method<GetRenderTargetFn>(dev, d3d8::kGetRenderTarget)(dev, &current)))
            target = current;  // null = keep the target
        d3d8::SurfaceDesc t{}, z{};
        if (Desc(target, t) && Desc(depth, z) && t.multiSample == 0 && z.multiSample != 0) {
            if (g_ownDepth && (g_ownDepthDesc.width < t.width || g_ownDepthDesc.height < t.height ||
                               g_ownDepthDesc.format != z.format))
                Release(g_ownDepth);
            if (!g_ownDepth) {
                const UINT w = std::max(t.width, z.width), h = std::max(t.height, z.height);
                const auto create = Method<CreateDepthFn>(dev, d3d8::kCreateDepthStencilSurface);   // (not hooked)
                if (SUCCEEDED(create(dev, w, h, z.format, 0, &g_ownDepth)) && Desc(g_ownDepth, g_ownDepthDesc))
                    dslog::Write("Graphics: plain %ux%u depth buffer for render-to-texture (%ux%u target)", w, h,
                                 t.width, t.height);
                else
                    Release(g_ownDepth);
            }
            if (g_ownDepth) depth = g_ownDepth;
        }
        if (current) Release(current);
    }
    return g_setRenderTarget(dev, target, depth);
}

DWORD g_vertexShader = 0;

HRESULT __stdcall SetVertexShader(void* dev, DWORD handle) {
    // FVF codes are even (shader handles from CreateVertexShader are odd); position type XYZRHW = 0x004.
    g_vertexShader = handle;
    g_pretransformed = (handle & 1) == 0 && (handle & 0x00E) == 0x004;
    return g_setVertexShader(dev, handle);
}

#ifndef DS_DIST
// Development builds: which UP draws touch the top/left screen edge (caller, shader/FVF, first vertex), once each.
void LogEdgeDraw(void* ret, const void* data, UINT count, UINT stride) {
    static void* seen[32];
    static int n = 0;
    if (!data || stride < 8 || n >= 32) return;
    bool edge = false;
    for (UINT i = 0; i < count && i < 64; ++i) {
        const float* p = reinterpret_cast<const float*>(static_cast<const uint8_t*>(data) + i * stride);
        if (p[0] < 1.0f || p[1] < 1.0f) edge = true;
    }
    if (!edge) return;
    for (int i = 0; i < n; ++i)
        if (seen[i] == ret) return;
    seen[n++] = ret;
    const float* p = static_cast<const float*>(data);
    dslog::Write("Graphics: edge draw from 0x%08X vs 0x%lX stride %u count %u v0 (%.2f, %.2f) samples %u",
                 reinterpret_cast<uint32_t>(ret), g_vertexShader, stride, count, p[0], p[1], g_samples);
}
#endif

// Copy of `count` vertices with x/y on the top/left screen edge moved to -1 (MSAA edge coverage, see the top).
// Returns null when no vertex needs it.
const void* ExtendScreenEdges(const void* data, UINT count, UINT stride) {
    if (!g_samples || !g_pretransformed || !data || stride < 8 || count == 0 || count > 4096) return nullptr;
    auto at = [&](const uint8_t* base, UINT i) { return reinterpret_cast<const float*>(base + i * stride); };
    const auto* src = static_cast<const uint8_t*>(data);
    auto onEdge = [](float v) { return v <= 0.0f && v > -1.0f; };
    UINT i = 0;
    while (i < count && !onEdge(at(src, i)[0]) && !onEdge(at(src, i)[1])) ++i;
    if (i == count) return nullptr;
    static uint8_t buffer[4096 * 64];
    if (static_cast<size_t>(count) * stride > sizeof buffer) return nullptr;
    memcpy(buffer, src, static_cast<size_t>(count) * stride);
    for (; i < count; ++i) {
        auto* p = reinterpret_cast<float*>(buffer + i * stride);
        if (onEdge(p[0])) p[0] = -1.0f;
        if (onEdge(p[1])) p[1] = -1.0f;
    }
    return buffer;
}

UINT VertexCount(uint32_t type, UINT prims) {
    switch (type) {
        case 1: return prims;      // point list
        case 2: return prims * 2;  // line list
        case 3: return prims + 1;  // line strip
        case 4: return prims * 3;  // triangle list
        case 5:                    // triangle strip
        case 6: return prims + 2;  // triangle fan
        default: return 0;
    }
}

HRESULT __stdcall DrawPrimitiveUP(void* dev, uint32_t type, UINT prims, const void* data, UINT stride) {
#ifndef DS_DIST
    LogEdgeDraw(_ReturnAddress(), data, VertexCount(type, prims), stride);
#endif
    if (const void* fixed = ExtendScreenEdges(data, VertexCount(type, prims), stride)) data = fixed;
    return g_drawUp(dev, type, prims, data, stride);
}

HRESULT __stdcall DrawIndexedPrimitiveUP(void* dev, uint32_t type, UINT minIndex, UINT vertices, UINT prims,
                                         const void* indices, uint32_t indexFormat, const void* data, UINT stride) {
#ifndef DS_DIST
    LogEdgeDraw(_ReturnAddress(), data, minIndex + vertices, stride);
#endif
    if (const void* fixed = ExtendScreenEdges(data, minIndex + vertices, stride)) data = fixed;
    return g_drawIndexedUp(dev, type, minIndex, vertices, prims, indices, indexFormat, data, stride);
}

// End of the frame: our button icons over whatever the game drew (overlay.cpp).
// Scene fog override (customise screen: the level behind the soldier fades to a flat colour). Active from
// features::SceneFogBegin (the frame's camera set-up) to EndScene, so the HUD / our 2D after it stay unfogged.
struct SceneFog {
    bool wanted = false, active = false;
    uint32_t colour = 0;
    float start = 0, end = 0;
    // The game's own values of the six states (D3D defaults until it sets them): put back on the device at EndScene and
    // when the override ends - the game sets its fog once and its renderer caches render states, so without this our
    // 3-5 m fog stayed on after the loadout screen closed (the whole mission looked fogged in).
    DWORD game[6] = {0, 0, 0, 0, 0x3F800000u, 0};
} g_fog;
constexpr DWORD kFogStates[6] = {28, 34, 35, 36, 37, 140};

int FogIndex(DWORD type) {
    for (int i = 0; i < 6; ++i)
        if (kFogStates[i] == type) return i;
    return -1;
}

DWORD FloatBits(float f) {
    DWORD d;
    memcpy(&d, &f, 4);
    return d;
}

bool FogOverride(DWORD type, DWORD& value) {
    if (!g_fog.active) return false;
    switch (type) {
    case 28: value = 1; return true;                        // FOGENABLE
    case 34: value = g_fog.colour; return true;             // FOGCOLOR
    case 35: value = 3; return true;                        // FOGTABLEMODE = LINEAR (per pixel, eye depth)
    case 36: value = FloatBits(g_fog.start); return true;   // FOGSTART
    case 37: value = FloatBits(g_fog.end); return true;     // FOGEND
    case 140: value = 0; return true;                       // FOGVERTEXMODE = NONE
    default: return false;
    }
}

HRESULT __stdcall SetRenderState(void* dev, DWORD type, DWORD value) {
    if (int i = FogIndex(type); i >= 0) g_fog.game[i] = value;
    FogOverride(type, value);
    return g_setRs(dev, type, value);
}

// Ends the scene fog override: the game's fog states back on the device.
void EndSceneFog(void* dev) {
    if (!g_fog.active) return;
    g_fog.active = false;
    if (dev && g_setRs)
        for (int i = 0; i < 6; ++i) g_setRs(dev, kFogStates[i], g_fog.game[i]);
}

HRESULT __stdcall EndScene(void* dev) {
    EndSceneFog(dev);
    overlay::Render(dev);
    return g_endScene(dev);
}

void StartVsyncCheck();  // V-Sync section below
HRESULT __stdcall Present(void* dev, const RECT* src, const RECT* dst, HWND wnd, const void* dirty);

HRESULT __stdcall Reset(void* dev, d3d8::PresentParams* pp) {
    Release(g_ownDepth);
    HRESULT hr = g_reset(dev, pp);
    if (FAILED(hr) && g_haveFallback) {  // a mode change the device refused: back to what worked (same struct)
        dslog::Write("Graphics: %ux%u %s refused (0x%08lX) - back to %ux%u", pp->width, pp->height,
                     pp->windowed ? "windowed" : "fullscreen", static_cast<unsigned long>(hr), g_fallback.width,
                     g_fallback.height);
        *pp = g_fallback;
        g_resetFailed = true;
        hr = g_reset(dev, pp);
    }
    if (SUCCEEDED(hr)) SetAnisotropyLevels(dev);  // Reset returns every state to its default
    if (SUCCEEDED(hr)) StartVsyncCheck();          // re-time: the mode may differ after alt-tab
    dslog::Write("Graphics: device reset %s (0x%08lX, %u samples)", SUCCEEDED(hr) ? "ok" : "FAILED",
                 static_cast<unsigned long>(hr), pp->multiSample);
    return hr;
}

using CreateTextureFn = HRESULT(__stdcall*)(void*, UINT, UINT, UINT, DWORD, uint32_t, uint32_t, void**);
const DevNext<d3d8::kCreateTexture, CreateTextureFn> g_createTexture{};

#ifndef DS_DIST
// Development builds: report the calls that would not work with a multisampled back buffer.
using GetBackBufferFn = HRESULT(__stdcall*)(void*, UINT, uint32_t, void**);
using CreateRenderTargetFn = HRESULT(__stdcall*)(void*, UINT, UINT, uint32_t, uint32_t, BOOL, void**);
using CopyRectsFn = HRESULT(__stdcall*)(void*, void*, const RECT*, UINT, void*, const POINT*);
using GetFrontBufferFn = HRESULT(__stdcall*)(void*, void*);
const DevNext<d3d8::kGetBackBuffer, GetBackBufferFn> g_getBackBuffer{};
const DevNext<d3d8::kCreateRenderTarget, CreateRenderTargetFn> g_createRenderTarget{};
const DevNext<d3d8::kCopyRects, CopyRectsFn> g_copyRects{};
const DevNext<d3d8::kGetFrontBuffer, GetFrontBufferFn> g_getFrontBuffer{};
int g_reports = 0;

template <class... Args>
void Report(const char* fmt, void* ret, Args... args) {
    if (g_reports >= 40) return;
    ++g_reports;
    char line[160];
    snprintf(line, sizeof line, fmt, args...);
    dslog::Write("Graphics: %s (from 0x%08X)", line, reinterpret_cast<uint32_t>(ret));
}

HRESULT __stdcall GetBackBuffer(void* dev, UINT i, uint32_t type, void** out) {
    Report("GetBackBuffer(%u)", _ReturnAddress(), i);
    return g_getBackBuffer(dev, i, type, out);
}
HRESULT __stdcall CreateRenderTarget(void* dev, UINT w, UINT h, uint32_t fmt, uint32_t ms, BOOL lockable, void** out) {
    Report("CreateRenderTarget %ux%u format %u", _ReturnAddress(), w, h, fmt);
    return g_createRenderTarget(dev, w, h, fmt, ms, lockable, out);
}
HRESULT __stdcall CopyRects(void* dev, void* src, const RECT* rects, UINT n, void* dst, const POINT* pts) {
    d3d8::SurfaceDesc d{};
    if (Desc(src, d) && d.multiSample) Report("CopyRects from a multisampled surface", _ReturnAddress());
    return g_copyRects(dev, src, rects, n, dst, pts);
}
HRESULT __stdcall GetFrontBuffer(void* dev, void* dst) {
    Report("GetFrontBuffer", _ReturnAddress());
    return g_getFrontBuffer(dev, dst);
}
#endif

// Every build: texture creation is timed for the frame recorder (level loads, streaming).
HRESULT __stdcall CreateTexture(void* dev, UINT w, UINT h, UINT levels, DWORD usage, uint32_t fmt, uint32_t pool,
                                void** out) {
#ifndef DS_DIST
    if (usage & d3d8::kUsageRenderTarget) Report("render-target texture %ux%u format %u", _ReturnAddress(), w, h, fmt);
#endif
    const double t0 = framerec::Now();
    const HRESULT hr = g_createTexture(dev, w, h, levels, usage, fmt, pool, out);
    framerec::AddTexture(w, h, fmt, framerec::Now() - t0);
    if (SUCCEEDED(hr) && out) features::NoteTexture(*out, w, h, levels, fmt, pool);
    return hr;
}

// FUN_00548920 (the message loop's device check, 0x4116A3) resets the device - releasing and re-making its render
// targets, back buffer / depth surfaces and render-state cache - when this answers DEVICENOTRESET; ChangeDisplayMode
// borrows that path.
HRESULT __stdcall TestCooperativeLevel(void* dev) {
    if (g_forceReset) {
        g_forceReset = false;
        return static_cast<HRESULT>(0x88760869);  // D3DERR_DEVICENOTRESET
    }
    return g_testCoop(dev);
}

void HookDevice(void* dev) {
    patch::OwnerScope owner(pse_host::Find("dsfix.graphics"));  // run-time hooks: owned by this feature's PSE plugin
    auto hook = [dev](int index, const void* fn) { return g_devHooks.Hook(dev, index, fn, features::kD3dHookPriority); };
    bool ok = hook(d3d8::kTestCooperativeLevel, reinterpret_cast<const void*>(&TestCooperativeLevel));
    ok = hook(d3d8::kSetRenderState, reinterpret_cast<const void*>(&SetRenderState)) && ok;
    ok = hook(d3d8::kSetTextureStageState, reinterpret_cast<const void*>(&SetTextureStageState)) && ok;
    ok = hook(d3d8::kSetRenderTarget, reinterpret_cast<const void*>(&SetRenderTarget)) && ok;
    ok = hook(d3d8::kReset, reinterpret_cast<const void*>(&Reset)) && ok;
    ok = hook(d3d8::kPresent, reinterpret_cast<const void*>(&Present)) && ok;
    ok = hook(d3d8::kCreateTexture, reinterpret_cast<const void*>(&CreateTexture)) && ok;
    ok = hook(d3d8::kEndScene, reinterpret_cast<const void*>(&EndScene)) && ok;
    ok = hook(d3d8::kSetVertexShader, reinterpret_cast<const void*>(&SetVertexShader)) && ok;
    ok = hook(d3d8::kDrawPrimitiveUP, reinterpret_cast<const void*>(&DrawPrimitiveUP)) && ok;
    ok = hook(d3d8::kDrawIndexedPrimitiveUP, reinterpret_cast<const void*>(&DrawIndexedPrimitiveUP)) && ok;
#ifndef DS_DIST
    hook(d3d8::kGetBackBuffer, reinterpret_cast<const void*>(&GetBackBuffer));
    hook(d3d8::kCreateRenderTarget, reinterpret_cast<const void*>(&CreateRenderTarget));
    hook(d3d8::kCopyRects, reinterpret_cast<const void*>(&CopyRects));
    hook(d3d8::kGetFrontBuffer, reinterpret_cast<const void*>(&GetFrontBuffer));
#endif
    if (!ok) {
        dslog::Write("[fail] Graphics: device hooks");
        g_anisotropy = 0;
        return;
    }
    SetAnisotropyLevels(dev);
}

// Highest sample count <= wanted that the adapter supports for the back buffer and the depth buffer.
uint32_t PickSamples(void* d3d, UINT adapter, uint32_t type, const d3d8::PresentParams& pp, uint32_t wanted) {
    const auto check = Method<CheckMultiSampleFn>(d3d, d3d8::kCheckDeviceMultiSampleType);   // (not hooked)
    for (uint32_t s = wanted; s >= 2; --s) {
        if (SUCCEEDED(check(d3d, adapter, type, pp.format, pp.windowed, s)) &&
            (!pp.autoDepthStencil || SUCCEEDED(check(d3d, adapter, type, pp.depthFormat, pp.windowed, s))))
            return s;
    }
    return 0;
}

bool g_vsync = false;  // the device presents with V-Sync (fullscreen + setting)
uint32_t g_refresh = 0;

// Some drivers ignore the D3D8 presentation interval (measured on the dev PC: ~750 fps with INTERVAL_ONE at 180 Hz -
// e.g. V-Sync forced off in the driver's settings). The first second after the device is (re)created is timed; if
// frames come faster than the refresh, Present waits for the monitor's vertical blank itself
// (D3DKMTWaitForVerticalBlankEvent, gdi32) - one frame per refresh, the flip in the blank, so no tearing either.
struct KmtOpen {
    HDC hdc;
    UINT adapter;
    LUID luid;
    UINT source;
};
struct KmtWait {
    UINT adapter, device, source;
};
using KmtOpenFn = LONG(APIENTRY*)(KmtOpen*);
using KmtWaitFn = LONG(APIENTRY*)(const KmtWait*);
KmtWaitFn g_kmtWait = nullptr;
KmtWait g_vblank{};
bool g_softVsync = false, g_decided = false;
bool g_unsynced = false;  // neither the driver nor our wait syncs: the frame cap has to pace
LONGLONG g_detectStart = 0;
int g_detectFrames = 0;

void OpenVblank(HWND wnd) {
    g_vblank = {};
    static KmtOpenFn open = nullptr;
    if (!open) {
        HMODULE gdi = GetModuleHandleA("gdi32.dll");
        open = reinterpret_cast<KmtOpenFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc"));
        g_kmtWait = reinterpret_cast<KmtWaitFn>(GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent"));
    }
    MONITORINFOEXA mi{};
    mi.cbSize = sizeof mi;
    if (!open || !g_kmtWait || !GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
    HDC dc = CreateDCA(nullptr, mi.szDevice, nullptr, nullptr);
    KmtOpen o{dc};
    if (dc && open(&o) == 0) g_vblank = {o.adapter, 0, o.source};
    if (dc) DeleteDC(dc);
}

void StartVsyncCheck() {
    g_softVsync = g_decided = g_unsynced = false;
    g_detectStart = 0;
    g_detectFrames = 0;
}

// Frames are shown on every n-th refresh so the rate never exceeds 240 (or the player's frame limit): 180 Hz -> every
// refresh, 360 Hz -> 2nd (180), 480 Hz -> 2nd (240), 1000 Hz -> 5th (200); 60 fps limit on 180 Hz -> 3rd. Hardware
// intervals go up to 4 (D3DPRESENT_INTERVAL_FOUR); beyond that, or when the driver ignores the interval, Present
// waits for the vertical blanks itself.
constexpr uint32_t kMaxSyncedFps = 240;
uint32_t g_every = 1;
LONGLONG g_lastPresent = 0;

uint32_t SyncTarget() {
    const uint32_t cap = settings::Get().fpsCap;
    return cap ? std::min(cap, kMaxSyncedFps) : kMaxSyncedFps;
}

void WaitForVblanks() {
    LARGE_INTEGER f, now;
    QueryPerformanceFrequency(&f);
    // Wait blanks until (n - 1/2) refresh periods have passed since the last frame went out: the right blank even when
    // rendering took longer than one refresh.
    const double need = (g_every - 0.5) / g_refresh;
    for (int i = 0; i < 16; ++i) {
        g_kmtWait(&g_vblank);
        QueryPerformanceCounter(&now);
        if (!g_lastPresent || double(now.QuadPart - g_lastPresent) / double(f.QuadPart) >= need) break;
    }
    g_lastPresent = now.QuadPart;
}

HRESULT __stdcall Present(void* dev, const RECT* src, const RECT* dst, HWND wnd, const void* dirty) {
    if (features::LockstepSkipRender()) return 0;   // online catch-up frame / lost device: nothing was drawn
    if (g_vsync && !g_decided) {
        LARGE_INTEGER now, f;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&f);
        if (!g_detectStart) g_detectStart = now.QuadPart;
        ++g_detectFrames;
        const double s = double(now.QuadPart - g_detectStart) / double(f.QuadPart);
        if (s >= 1.0) {
            // Decided only by a clear window: too fast (the driver ignores the interval) or on target (it syncs).
            // Slower windows (loading screens, heavy scenes) say nothing - measure the next second.
            const double fps = (g_detectFrames - 1) / s, expected = g_refresh ? double(g_refresh) / g_every : 0;
            const bool tooFast = expected > 0 && fps > expected * 1.1;
            if (tooFast || (expected > 0 && fps > expected * 0.9)) {
                g_decided = true;
                g_softVsync = tooFast && g_vblank.adapter;
                g_unsynced = tooFast && !g_vblank.adapter;
                dslog::Write("Graphics: %.0f fps at %u Hz, every %u. refresh - %s", fps, g_refresh, g_every,
                             g_softVsync  ? "the driver doesn't sync, waiting for the vertical blank ourselves"
                             : g_unsynced ? "the driver doesn't sync and the blank wait is unavailable - frame cap"
                                          : "V-Sync works");
            } else {
                g_detectStart = now.QuadPart;
                g_detectFrames = 1;
            }
        }
    }
    const double t0 = framerec::Now();
    if (g_softVsync) WaitForVblanks();
    const double t1 = framerec::Now();
    features::PhotoBeforePresent(dev);   // photo mode: the finished frame (overlay included) read back
    const HRESULT hr = g_present(dev, src, dst, wnd, dirty);
    framerec::AddPresent(t1 - t0, framerec::Now() - t1);
    features::PhotoAfterPresent(dev);    // (system D3D8 + MSAA: from the front buffer)
    return hr;
}

uint32_t DesktopRefresh(HWND wnd) {
    MONITORINFOEXA mi{};
    mi.cbSize = sizeof mi;
    DEVMODEA dm{};
    dm.dmSize = sizeof dm;
    return GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY), &mi) &&
                   EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)
               ? dm.dmDisplayFrequency
               : 0;
}

// Fullscreen V-Sync: the game leaves FullScreen_PresentationInterval at DEFAULT and the refresh at 0 ("default") -
// measured 240 fps on a 180 Hz monitor, i.e. no sync (3 of 4 frames shown: judder + tearing). With the setting on,
// the device runs at the highest refresh the adapter offers for this resolution and presents on every n-th refresh.
// The struct lives in the renderer (+0x84), so Reset (alt-tab) keeps it. Windowed / borderless: DWM composes without
// tearing, and D3D8's windowed sync (COPY_VSYNC) can't be combined with MSAA - left to the frame cap.
void ApplyVsync(void* d3d, UINT adapter, d3d8::PresentParams* pp) {
    g_vsync = false;
    g_every = 1;
    if (pp->windowed || !settings::Get().vsync) return;
    if (!pp->refresh) {
        using CountFn = UINT(__stdcall*)(void*, UINT);
        using EnumFn = HRESULT(__stdcall*)(void*, UINT, UINT, d3d8::DisplayMode*);
        const UINT n = Method<CountFn>(d3d, d3d8::kGetAdapterModeCount)(d3d, adapter);
        UINT best = 0;
        for (UINT i = 0; i < n; ++i) {
            d3d8::DisplayMode m{};
            if (SUCCEEDED(Method<EnumFn>(d3d, d3d8::kEnumAdapterModes)(d3d, adapter, i, &m)) && m.width == pp->width &&
                m.height == pp->height && m.format == pp->format)
                best = std::max(best, m.refresh);
        }
        pp->refresh = best;  // 0 if none matched: default as before
    }
    const uint32_t hz = pp->refresh ? pp->refresh : DesktopRefresh(pp->window);
    const uint32_t target = SyncTarget();
    g_every = hz ? std::max<uint32_t>(1, (hz + target - 1) / target) : 1;
    static const uint32_t kInterval[] = {1, 1, 2, 4, 8};  // D3DPRESENT_INTERVAL_ONE / TWO / THREE / FOUR
    pp->interval = g_every <= 4 ? kInterval[g_every] : 1;
    g_vsync = true;
    OpenVblank(pp->window);
    StartVsyncCheck();
    if (g_every > 4) {  // only our own wait can skip that many
        g_decided = true;
        g_softVsync = g_vblank.adapter != 0;
        g_unsynced = !g_softVsync;
    }
}

void NoteRefresh(void* dev, const d3d8::PresentParams* pp) {
    g_refresh = pp->refresh;
    if (!g_refresh) {  // default refresh: ask the device what it got
        using ModeFn = HRESULT(__stdcall*)(void*, d3d8::DisplayMode*);
        d3d8::DisplayMode m{};
        if (SUCCEEDED(Method<ModeFn>(dev, 8 /* GetDisplayMode */)(dev, &m))) g_refresh = m.refresh;
    }
}

HRESULT __stdcall CreateDevice(void* d3d, UINT adapter, uint32_t type, HWND wnd, DWORD flags,
                               d3d8::PresentParams* pp, void** out) {
    const settings::Values& v = settings::Get();
    ApplyVsync(d3d, adapter, pp);
    const uint32_t gameSamples = pp->multiSample;
    g_samples = 0;
    if (v.antialiasing >= 2 && pp->swapEffect == 1 /* DISCARD */ && !(pp->flags & 1 /* lockable back buffer */)) {
        g_samples = PickSamples(d3d, adapter, type, *pp, v.antialiasing);
        if (g_samples) pp->multiSample = g_samples;
    }
    Release(g_ownDepth);
    HRESULT hr = g_createDevice(d3d, adapter, type, wnd, flags, pp, out);
    if (FAILED(hr) && g_samples) {
        dslog::Write("Graphics: device with %ux MSAA failed (0x%08lX) - creating it without", g_samples,
                     static_cast<unsigned long>(hr));
        g_samples = 0;
        pp->multiSample = gameSamples;
        hr = g_createDevice(d3d, adapter, type, wnd, flags, pp, out);
    }
    if (FAILED(hr)) return hr;

    void* dev = *out;
    NoteRefresh(dev, pp);
    g_device = dev;
    g_maxAnisotropy = 0;
    d3d8::Caps caps{};
    if (SUCCEEDED(Method<GetCapsFn>(dev, d3d8::kGetDeviceCaps)(dev, &caps)) &&
        (caps.fields[d3d8::kTextureFilterCaps] & d3d8::kFilterCapsMinAnisotropic))
        g_maxAnisotropy = std::max<uint32_t>(caps.maxAnisotropy, 1);
    g_anisotropy = v.anisotropy >= 2 && g_maxAnisotropy ? std::min<uint32_t>(v.anisotropy, g_maxAnisotropy) : 0;
    HookDevice(dev);
    dslog::Write("Graphics: %ux%u %s, MSAA %ux (setting %u), anisotropic filtering %ux (setting %u), V-Sync %s at %u Hz",
                 pp->width, pp->height, pp->windowed ? "windowed" : "fullscreen", g_samples, v.antialiasing,
                 g_anisotropy, v.anisotropy, g_vsync ? "on" : "off", pp->refresh);
    return hr;
}

void* WINAPI Direct3DCreate8Hook(UINT sdkVersion) {
    // Renderer 1 / 2 / 3: our D3D8-on-D3D9 objects (src/render; 2 = D3D9 on D3D12, 3 = + own DXGI presenter); the hooks below then wrap them like the system's D3D8.
    void* d3d = settings::Get().renderer >= 1 ? render::CreateDirect3D8On9(sdkVersion, settings::Get().renderer >= 2, settings::Get().renderer == 3) : nullptr;
    if (!d3d) d3d = g_create8(sdkVersion);
    if (!d3d) return d3d;
    patch::OwnerScope owner(pse_host::Find("dsfix.graphics"));
    if (!g_d3dHooks.Hook(d3d, d3d8::kCreateDevice, reinterpret_cast<const void*>(&CreateDevice), features::kD3dHookPriority))
        dslog::Write("[fail] Graphics: IDirect3D8::CreateDevice hook");
    return d3d;
}
}  // namespace

// OPTIONS > VIDEO > TEXTURE FILTERING: the new level from now on. The game sets each stage's MINFILTER per material,
// so the next draws pick it up; MAXANISOTROPY is set on every stage here (as after a Reset).
void features::ApplyAnisotropy() {
    const uint32_t want = settings::Get().anisotropy;
    const uint32_t now = want >= 2 && g_maxAnisotropy ? std::min<uint32_t>(want, g_maxAnisotropy) : 0;
    if (now == g_anisotropy || !g_device || !g_setTss) {
        g_anisotropy = now;
        return;
    }
    if (!now)  // back to the game's filtering: plain linear until the game sets the stage again
        for (DWORD s = 0; s < d3d8::kMaxStages; ++s) g_setTss(g_device, s, d3d8::kTssMinFilter, d3d8::kTexfLinear);
    g_anisotropy = now;
    SetAnisotropyLevels(g_device);
    dslog::Write("Graphics: anisotropic filtering now %ux (setting %u)", g_anisotropy, want);
}

// OPTIONS > VIDEO: resolution / display mode / anti-aliasing / V-Sync without a restart. The new mode goes where
// the sample framework's device creation (FUN_00542AC0) would have put it - present parameters (renderer +0x84), mode
// index (device +0x13A8), fullscreen flag (adapter +0x66C8), windowed flag (renderer +0x40528), back-buffer size and
// bits (+0x40688 ..) - then the game's own device-lost path resets the device (as after alt-tab). A refused mode
// falls back to the old parameters inside the Reset hook. Only between frames (the frame-cap hook).
bool features::ChangeDisplayMode(uint32_t modeIndex, uint32_t displayMode) {
    displayMode = features::EffectiveDisplayMode(displayMode);
    constexpr uint32_t kRendererPtr = 0x63C924, kDeviceCheck = 0x548920;
    auto* r = *reinterpret_cast<uint8_t**>(kRendererPtr);
    if (!r || !g_testCoop || !g_device) return false;
    const int adapterIndex = *reinterpret_cast<int*>(r + 0xE0);
    uint8_t* adapter = r + 0xE4 + adapterIndex * 0x66D0;
    uint8_t* device = adapter + 0x440 + *reinterpret_cast<int*>(adapter + 0x66C4) * 0x13B4;
    if (modeIndex >= *reinterpret_cast<uint32_t*>(device + 0xE4)) return false;
    const uint32_t* m = reinterpret_cast<const uint32_t*>(device + 0xE8 + modeIndex * 0x20);
    auto* pp = reinterpret_cast<d3d8::PresentParams*>(r + 0x84);
    const bool windowed = displayMode != 0;

    struct Saved {
        uint32_t mode, fullscreen, windowedFlag, wh[2], bits[3];
    } old{*reinterpret_cast<uint32_t*>(device + 0x13A8), *reinterpret_cast<uint32_t*>(adapter + 0x66C8),
          *reinterpret_cast<uint32_t*>(r + 0x40528), {*reinterpret_cast<uint32_t*>(r + 0x40688),
          *reinterpret_cast<uint32_t*>(r + 0x4068C)}, {*reinterpret_cast<uint32_t*>(r + 0x40694),
          *reinterpret_cast<uint32_t*>(r + 0x40698), *reinterpret_cast<uint32_t*>(r + 0x4069C)}};
    g_fallback = *pp;
    g_haveFallback = true;
    g_resetFailed = false;

    features::PlaceGameWindow(m[0], m[1], displayMode);
    pp->width = m[0];
    pp->height = m[1];
    pp->windowed = windowed;
    pp->format = windowed ? *reinterpret_cast<uint32_t*>(adapter + 0x438) : m[2];  // desktop format when windowed
    pp->depthFormat = m[4];
    pp->refresh = 0;
    pp->interval = 0;
    pp->multiSample = 0;
    auto* d3d = *reinterpret_cast<void**>(r + 0x4052C);
    const settings::Values& v = settings::Get();
    ApplyVsync(d3d, adapterIndex, pp);
    g_samples = v.antialiasing >= 2 && pp->swapEffect == 1 && !(pp->flags & 1)
                    ? PickSamples(d3d, adapterIndex, d3d8::kDevHal, *pp, v.antialiasing)
                    : 0;
    pp->multiSample = g_samples;
    *reinterpret_cast<uint32_t*>(device + 0x13A8) = modeIndex;
    *reinterpret_cast<uint32_t*>(device + 0x13AC) = windowed;
    *reinterpret_cast<uint32_t*>(adapter + 0x66C8) = windowed ? 0 : 1;
    *reinterpret_cast<uint32_t*>(r + 0x40528) = windowed;
    *reinterpret_cast<uint32_t*>(r + 0x40688) = m[0];
    *reinterpret_cast<uint32_t*>(r + 0x4068C) = m[1];
    for (int i = 0; i < 3; ++i) *reinterpret_cast<uint32_t*>(r + 0x40694 + i * 4) = m[5 + i];

    g_forceReset = true;
    reinterpret_cast<int(__thiscall*)(void*)>(kDeviceCheck)(r);
    g_forceReset = false;
    g_haveFallback = false;
    if (g_resetFailed) {  // the old mode is back on the device: the game's fields too
        *reinterpret_cast<uint32_t*>(device + 0x13A8) = old.mode;
        *reinterpret_cast<uint32_t*>(device + 0x13AC) = old.windowedFlag;
        *reinterpret_cast<uint32_t*>(adapter + 0x66C8) = old.fullscreen;
        *reinterpret_cast<uint32_t*>(r + 0x40528) = old.windowedFlag;
        *reinterpret_cast<uint32_t*>(r + 0x40688) = old.wh[0];
        *reinterpret_cast<uint32_t*>(r + 0x4068C) = old.wh[1];
        for (int i = 0; i < 3; ++i) *reinterpret_cast<uint32_t*>(r + 0x40694 + i * 4) = old.bits[i];
        features::PlaceGameWindow(old.wh[0], old.wh[1], old.fullscreen ? 0 : displayMode);
        g_samples = g_fallback.multiSample;
        return false;
    }
    dslog::Write("Graphics: display now %ux%u %s, MSAA %ux, V-Sync %s", m[0], m[1],
                 displayMode == 0 ? "fullscreen" : displayMode == 1 ? "windowed" : "borderless", g_samples,
                 g_vsync ? "on" : "off");
    return true;
}

void features::ApplyGraphics() {
    if (g_create8) return;  // installed once; the settings are read when the device is created
    auto slot = reinterpret_cast<Direct3DCreate8Fn*>(kIatDirect3DCreate8);
    HMODULE d3d8 = GetModuleHandleA("d3d8.dll");
    auto real = d3d8 ? reinterpret_cast<Direct3DCreate8Fn>(GetProcAddress(d3d8, "Direct3DCreate8")) : nullptr;
    // (The game layer, pse.game.ds1, hooks the slot first - PSE's D3D events; then it holds the layer's hook and the
    // broker knows the slot: this hook chains behind it, its next is still the real function.)
    if (!real || (*slot != real && !pse::HooksInterface()->OwnerOf(kIatDirect3DCreate8))) {
        dslog::Write("[fail] Graphics: Direct3DCreate8 import not found - different exe version? Not applied.");
        return;
    }
    // IAT slot pointer hook: g_create8 = the chain's next (the real function unless another hook joined).
    if (patch::HookSlot(kIatDirect3DCreate8, reinterpret_cast<const void*>(&Direct3DCreate8Hook),
                        reinterpret_cast<void**>(&g_create8), reinterpret_cast<uint32_t>(real)))
        dslog::Write("[ok]   Graphics: anti-aliasing / anisotropic filtering hooks");
    else
        g_create8 = nullptr;
}

// Frame rate V-Sync holds the game at (0 = not synced: the frame cap paces). Not while our own wait would be needed
// but isn't available.
uint32_t features::VsyncFps() {
    if (!g_vsync || !g_refresh || !g_decided || g_unsynced) return 0;
    return g_refresh / g_every;
}
// While V-Sync is being checked: the frame cap to use (twice the synced rate), else 0.
uint32_t features::VsyncProbeFps() {
    if (!g_vsync || !g_refresh || g_decided) return 0;
    return std::min<uint32_t>(2 * g_refresh / g_every, 500);
}

void features::SetSceneFog(bool on, uint32_t colour, float start, float end) {
    g_fog.wanted = on;
    g_fog.colour = colour, g_fog.start = start, g_fog.end = end;
    if (!on) EndSceneFog(*reinterpret_cast<void**>(0x755050));
}

// Called when the frame's 3-D camera is set: forces the fog states now (the game may not set them again this frame).
void features::SceneFogBegin() {
    if (!g_fog.wanted || !g_setRs) return;
    void* dev = *reinterpret_cast<void**>(0x755050);
    if (!dev) return;
    g_fog.active = true;
    for (DWORD type : kFogStates) {
        DWORD value = 0;
        FogOverride(type, value);
        g_setRs(dev, type, value);
    }
}
