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
// Settings are read when the device is created (after the launcher), so they apply from the next start of play.
#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/overlay.h"

namespace {
constexpr uint32_t kIatDirect3DCreate8 = 0x5D8308;

// Direct3D 8 (the Windows SDK no longer ships d3d8.h): vtable indices and the values used here.
namespace d3d8 {
enum D3D : int { kCheckDeviceMultiSampleType = 11, kCreateDevice = 15 };
enum Device : int {
    kGetDeviceCaps = 7, kReset = 14, kCreateDepthStencilSurface = 26, kEndScene = 35, kSetRenderTarget = 31, kGetRenderTarget = 32,
    kSetTextureStageState = 63,
#ifndef DS_DIST
    kGetBackBuffer = 16, kCreateTexture = 20, kCreateRenderTarget = 25, kCopyRects = 28, kGetFrontBuffer = 30,
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
using CreateDepthFn = HRESULT(__stdcall*)(void*, UINT, UINT, uint32_t, uint32_t, void**);
using SetRenderTargetFn = HRESULT(__stdcall*)(void*, void*, void*);
using GetRenderTargetFn = HRESULT(__stdcall*)(void*, void**);
using SetTssFn = HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD);
using EndSceneFn = HRESULT(__stdcall*)(void*);
using ReleaseFn = ULONG(__stdcall*)(void*);
using GetDescFn = HRESULT(__stdcall*)(void*, d3d8::SurfaceDesc*);

Direct3DCreate8Fn g_create8 = nullptr;
CheckMultiSampleFn g_checkMultiSample = nullptr;
CreateDeviceFn g_createDevice = nullptr;
ResetFn g_reset = nullptr;
CreateDepthFn g_createDepth = nullptr;
SetRenderTargetFn g_setRenderTarget = nullptr;
GetRenderTargetFn g_getRenderTarget = nullptr;
SetTssFn g_setTss = nullptr;
EndSceneFn g_endScene = nullptr;

uint32_t g_samples = 0;     // MSAA sample count of the current device (0 = off)
uint32_t g_anisotropy = 0;  // anisotropy forced on linear-filtered textures (0 = game default)
void* g_ownDepth = nullptr; // plain depth buffer for non-multisampled render targets
d3d8::SurfaceDesc g_ownDepthDesc{};

void** Vtable(void* obj) { return *static_cast<void***>(obj); }

template <class Fn>
Fn Method(void* obj, int index) {
    return reinterpret_cast<Fn>(Vtable(obj)[index]);
}

// Points vtable slot `index` of `obj` at `hook`; returns the previous function (or the saved one when the slot is
// already ours, e.g. for a second device - vtables are per class, not per object).
template <class Fn>
bool HookSlot(void* obj, int index, void* hook, Fn& original) {
    void** slot = &Vtable(obj)[index];
    if (*slot == hook) return original != nullptr;
    original = reinterpret_cast<Fn>(*slot);
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return false;
    *slot = hook;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return true;
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
        if (!target && SUCCEEDED(g_getRenderTarget(dev, &current))) target = current;  // null = keep the target
        d3d8::SurfaceDesc t{}, z{};
        if (Desc(target, t) && Desc(depth, z) && t.multiSample == 0 && z.multiSample != 0) {
            if (g_ownDepth && (g_ownDepthDesc.width < t.width || g_ownDepthDesc.height < t.height ||
                               g_ownDepthDesc.format != z.format))
                Release(g_ownDepth);
            if (!g_ownDepth) {
                const UINT w = std::max(t.width, z.width), h = std::max(t.height, z.height);
                if (SUCCEEDED(g_createDepth(dev, w, h, z.format, 0, &g_ownDepth)) && Desc(g_ownDepth, g_ownDepthDesc))
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

// End of the frame: our button icons over whatever the game drew (overlay.cpp).
HRESULT __stdcall EndScene(void* dev) {
    overlay::Render(dev);
    return g_endScene(dev);
}

HRESULT __stdcall Reset(void* dev, d3d8::PresentParams* pp) {
    Release(g_ownDepth);
    HRESULT hr = g_reset(dev, pp);
    if (SUCCEEDED(hr)) SetAnisotropyLevels(dev);  // Reset returns every state to its default
    dslog::Write("Graphics: device reset %s (0x%08lX, %u samples)", SUCCEEDED(hr) ? "ok" : "FAILED",
                 static_cast<unsigned long>(hr), pp->multiSample);
    return hr;
}

#ifndef DS_DIST
// Development builds: report the calls that would not work with a multisampled back buffer.
using GetBackBufferFn = HRESULT(__stdcall*)(void*, UINT, uint32_t, void**);
using CreateTextureFn = HRESULT(__stdcall*)(void*, UINT, UINT, UINT, DWORD, uint32_t, uint32_t, void**);
using CreateRenderTargetFn = HRESULT(__stdcall*)(void*, UINT, UINT, uint32_t, uint32_t, BOOL, void**);
using CopyRectsFn = HRESULT(__stdcall*)(void*, void*, const RECT*, UINT, void*, const POINT*);
using GetFrontBufferFn = HRESULT(__stdcall*)(void*, void*);
GetBackBufferFn g_getBackBuffer = nullptr;
CreateTextureFn g_createTexture = nullptr;
CreateRenderTargetFn g_createRenderTarget = nullptr;
CopyRectsFn g_copyRects = nullptr;
GetFrontBufferFn g_getFrontBuffer = nullptr;
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
HRESULT __stdcall CreateTexture(void* dev, UINT w, UINT h, UINT levels, DWORD usage, uint32_t fmt, uint32_t pool,
                                void** out) {
    if (usage & d3d8::kUsageRenderTarget) Report("render-target texture %ux%u format %u", _ReturnAddress(), w, h, fmt);
    return g_createTexture(dev, w, h, levels, usage, fmt, pool, out);
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

void HookDevice(void* dev) {
    bool ok = HookSlot(dev, d3d8::kSetTextureStageState, reinterpret_cast<void*>(&SetTextureStageState), g_setTss) &&
              HookSlot(dev, d3d8::kSetRenderTarget, reinterpret_cast<void*>(&SetRenderTarget), g_setRenderTarget) &&
              HookSlot(dev, d3d8::kReset, reinterpret_cast<void*>(&Reset), g_reset) &&
              HookSlot(dev, d3d8::kEndScene, reinterpret_cast<void*>(&EndScene), g_endScene);
    g_getRenderTarget = Method<GetRenderTargetFn>(dev, d3d8::kGetRenderTarget);
    g_createDepth = Method<CreateDepthFn>(dev, d3d8::kCreateDepthStencilSurface);
#ifndef DS_DIST
    HookSlot(dev, d3d8::kGetBackBuffer, reinterpret_cast<void*>(&GetBackBuffer), g_getBackBuffer);
    HookSlot(dev, d3d8::kCreateTexture, reinterpret_cast<void*>(&CreateTexture), g_createTexture);
    HookSlot(dev, d3d8::kCreateRenderTarget, reinterpret_cast<void*>(&CreateRenderTarget), g_createRenderTarget);
    HookSlot(dev, d3d8::kCopyRects, reinterpret_cast<void*>(&CopyRects), g_copyRects);
    HookSlot(dev, d3d8::kGetFrontBuffer, reinterpret_cast<void*>(&GetFrontBuffer), g_getFrontBuffer);
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
    for (uint32_t s = wanted; s >= 2; --s) {
        if (SUCCEEDED(g_checkMultiSample(d3d, adapter, type, pp.format, pp.windowed, s)) &&
            (!pp.autoDepthStencil || SUCCEEDED(g_checkMultiSample(d3d, adapter, type, pp.depthFormat, pp.windowed, s))))
            return s;
    }
    return 0;
}

HRESULT __stdcall CreateDevice(void* d3d, UINT adapter, uint32_t type, HWND wnd, DWORD flags,
                               d3d8::PresentParams* pp, void** out) {
    const settings::Values& v = settings::Get();
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
    g_anisotropy = 0;
    if (v.anisotropy >= 2) {
        d3d8::Caps caps{};
        if (SUCCEEDED(Method<GetCapsFn>(dev, d3d8::kGetDeviceCaps)(dev, &caps)) &&
            (caps.fields[d3d8::kTextureFilterCaps] & d3d8::kFilterCapsMinAnisotropic))
            g_anisotropy = std::min<uint32_t>(v.anisotropy, std::max<uint32_t>(caps.maxAnisotropy, 1));
    }
    HookDevice(dev);
    dslog::Write("Graphics: %ux%u, MSAA %ux (setting %u), anisotropic filtering %ux (setting %u)", pp->width,
                 pp->height, g_samples, v.antialiasing, g_anisotropy, v.anisotropy);
    return hr;
}

void* WINAPI Direct3DCreate8Hook(UINT sdkVersion) {
    void* d3d = g_create8(sdkVersion);
    if (!d3d) return d3d;
    g_checkMultiSample = Method<CheckMultiSampleFn>(d3d, d3d8::kCheckDeviceMultiSampleType);
    if (!HookSlot(d3d, d3d8::kCreateDevice, reinterpret_cast<void*>(&CreateDevice), g_createDevice))
        dslog::Write("[fail] Graphics: IDirect3D8::CreateDevice hook");
    return d3d;
}
}  // namespace

void features::ApplyGraphics() {
    if (g_create8) return;  // installed once; the settings are read when the device is created
    auto slot = reinterpret_cast<Direct3DCreate8Fn*>(kIatDirect3DCreate8);
    HMODULE d3d8 = GetModuleHandleA("d3d8.dll");
    auto real = d3d8 ? reinterpret_cast<Direct3DCreate8Fn>(GetProcAddress(d3d8, "Direct3DCreate8")) : nullptr;
    if (!real || *slot != real) {
        dslog::Write("[fail] Graphics: Direct3DCreate8 import not found - different exe version? Not applied.");
        return;
    }
    g_create8 = real;
    auto hook = &Direct3DCreate8Hook;
    if (patch::WriteValue(kIatDirect3DCreate8, hook))
        dslog::Write("[ok]   Graphics: anti-aliasing / anisotropic filtering hooks");
    else
        g_create8 = nullptr;
}
