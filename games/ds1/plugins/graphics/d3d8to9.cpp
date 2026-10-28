// Direct3D 8 on Direct3D 9 (see d3d8to9.h). One wrapper object per D3D9 object, created once and handed out again
// by every getter, so code comparing texture pointers (fontsharp, weaponicons, barcolour) keeps working.
//
// Layout rules: every class's virtual functions are declared in the D3D8 interface order and nothing else is
// virtual (no virtual destructors); base classes (Resource8 -> BaseTexture8 -> Texture8 ...) only append slots, as
// COM interface inheritance does. Wrappers carry a magic value so a foreign pointer is never unwrapped.
//
// Differences handled here: present parameters / surface descriptions / caps / adapter identifier layouts,
// EnumAdapterModes over all formats, sampler states split out of SetTextureStageState, ZBIAS -> DEPTHBIAS,
// SetVertexShader(FVF | handle), the base vertex index moved from SetIndices into DrawIndexedPrimitive,
// SetRenderTarget split into render target + depth, CopyRects, image surfaces, P8 reported unsupported (D3DX8 then
// converts palettised images itself), no PUREDEVICE (the plugin's overlay reads states back).
// Shaders are translated at the token level by shader8.cpp.
#include "render/d3d8to9.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "render/d3d8types.h"
#include <d3d9on12.h>
#include "render/presenter.h"
#include "render/shader8.h"
#include "render/water.h"
#include "core/settings.h"

namespace {

constexpr uint32_t kMagic = 0x38443344;  // "D3D8"
constexpr UINT kStages = 8, kStreams = 16;
constexpr uintptr_t kDispatchMode = 0x60EC8C;  // the game's frame dispatch: 1 = level load (progress frames inside one frame)

// clang-format off
const IID kIidDirect3D8        = {0x1DD9E8DA, 0x1C77, 0x4D40, {0xB0, 0xCF, 0x98, 0xFE, 0xFD, 0xFF, 0x95, 0x12}};
const IID kIidDevice8          = {0x7385E5DF, 0x8FE8, 0x41D5, {0x86, 0xB6, 0xD7, 0xB4, 0x85, 0x47, 0xB6, 0xCF}};
const IID kIidResource8        = {0x1B36BB7B, 0x09B7, 0x410A, {0xB4, 0x45, 0x7D, 0x14, 0x30, 0xD7, 0xB3, 0x3F}};
const IID kIidBaseTexture8     = {0xB4211CFA, 0x51B9, 0x4A9F, {0xAB, 0x78, 0xDB, 0x99, 0xB2, 0xBB, 0x67, 0x8E}};
const IID kIidTexture8         = {0xE4CDD575, 0x2866, 0x4F01, {0xB1, 0x2E, 0x7E, 0xEC, 0xE1, 0xEC, 0x93, 0x58}};
const IID kIidCubeTexture8     = {0x3EE5B968, 0x2ACA, 0x4C34, {0x8B, 0xB5, 0x7E, 0x0C, 0x3D, 0x19, 0xB7, 0x50}};
const IID kIidVolumeTexture8   = {0x4B8AAAFA, 0x140F, 0x42BA, {0x91, 0x31, 0x59, 0x7E, 0xAF, 0xAA, 0x2E, 0xAD}};
const IID kIidVertexBuffer8    = {0x8AEEEAC7, 0x05F9, 0x44D4, {0xB5, 0x91, 0x00, 0x0B, 0x0D, 0xF1, 0xCB, 0x95}};
const IID kIidIndexBuffer8     = {0x0E689C9A, 0x053D, 0x44A0, {0x9D, 0x92, 0xDB, 0x0E, 0x3D, 0x75, 0x0F, 0x86}};
const IID kIidSurface8         = {0xB96EEBCA, 0xB326, 0x4EA5, {0x88, 0x2F, 0x2F, 0xF5, 0xBA, 0xE0, 0x21, 0xDD}};
const IID kIidVolume8          = {0xBD7349F5, 0x14F1, 0x42E4, {0x9C, 0x79, 0x97, 0x23, 0x80, 0xDB, 0x40, 0xC0}};
const IID kIidDevice9On12      = {0xE7FDA234, 0xB589, 0x4049, {0x94, 0x0D, 0x88, 0x78, 0x97, 0x75, 0x31, 0xC8}};
const IID kIidSwapChain8       = {0x928C088B, 0x76B9, 0x4C6B, {0xA5, 0x36, 0xA5, 0x90, 0x85, 0x38, 0x76, 0xCD}};
// clang-format on

void LogOnce(const char* what) {
    static std::vector<const char*> seen;
    if (std::find(seen.begin(), seen.end(), what) != seen.end()) return;
    seen.push_back(what);
    dslog::Write("D3D8on9: %s", what);
}

void LockFailed(const char* what, HRESULT hr, DWORD flags, UINT offset, UINT size, DWORD usage, D3DPOOL pool, UINT total) {
    static int logged = 0;
    if (logged++ < 20)
        dslog::Write("[fail] D3D8on9: %s::Lock(offset %u, size %u, flags 0x%X) = 0x%08X (usage 0x%X, pool %u, %u bytes) - "
                     "scratch buffer used", what, offset, size, flags, static_cast<unsigned>(hr), usage, pool, total);
}

template <class T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// COM AddRef / Release through an object's vtable (slots 1 / 2) - used for surfaces whose life is their container's.
ULONG VAddRef(void* o) { return reinterpret_cast<ULONG(__stdcall*)(void*)>((*static_cast<void***>(o))[1])(o); }
ULONG VRelease(void* o) { return reinterpret_cast<ULONG(__stdcall*)(void*)>((*static_cast<void***>(o))[2])(o); }

bool IsBlock(D3DFORMAT f) {
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}

UINT BitsPerPixel(D3DFORMAT f) {
    switch (f) {
        case D3DFMT_R8G8B8: return 24;
        case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A2B10G10R10: case D3DFMT_A8B8G8R8:
        case D3DFMT_X8B8G8R8: case D3DFMT_G16R16: case D3DFMT_A2R10G10B10: case D3DFMT_X8L8V8U8:
        case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16: case D3DFMT_A2W10V10U10: case D3DFMT_D32: case D3DFMT_D24S8:
        case D3DFMT_D24X8: case D3DFMT_D24X4S4: return 32;
        case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8R3G3B2:
        case D3DFMT_X4R4G4B4: case D3DFMT_A8P8: case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L6V5U5:
        case D3DFMT_D16_LOCKABLE: case D3DFMT_D15S1: case D3DFMT_D16: case D3DFMT_UYVY: case D3DFMT_YUY2: return 16;
        case D3DFMT_R3G3B2: case D3DFMT_A8: case D3DFMT_P8: case D3DFMT_L8: case D3DFMT_A4L4: return 8;
        default: return 32;
    }
}

UINT SurfaceBytes(D3DFORMAT f, UINT w, UINT h, UINT d = 1) {
    if (IsBlock(f)) return ((w + 3) / 4) * ((h + 3) / 4) * (f == D3DFMT_DXT1 ? 8 : 16) * d;
    return w * h * d * BitsPerPixel(f) / 8;
}

void ToDesc8(const D3DSURFACE_DESC& d9, d3d8::SurfaceDesc* d8) {
    d8->Format = d9.Format;
    d8->Type = d9.Type;
    d8->Usage = d9.Usage;
    d8->Pool = d9.Pool;
    d8->Size = SurfaceBytes(d9.Format, d9.Width, d9.Height);
    d8->MultiSampleType = d9.MultiSampleType;
    d8->Width = d9.Width;
    d8->Height = d9.Height;
}

void ToDesc8(const D3DVOLUME_DESC& d9, d3d8::VolumeDesc* d8) {
    d8->Format = d9.Format;
    d8->Type = d9.Type;
    d8->Usage = d9.Usage;
    d8->Pool = d9.Pool;
    d8->Size = SurfaceBytes(d9.Format, d9.Width, d9.Height, d9.Depth);
    d8->Width = d9.Width;
    d8->Height = d9.Height;
    d8->Depth = d9.Depth;
}

// D3D8 present parameters -> D3D9. Windowed D3D8 presents never wait for the vertical blank (unless COPY_VSYNC);
// D3D9's windowed DEFAULT does, so it is made IMMEDIATE.
D3DPRESENT_PARAMETERS ToPresent9(const d3d8::PresentParameters& p) {
    D3DPRESENT_PARAMETERS q{};
    q.BackBufferWidth = p.BackBufferWidth;
    q.BackBufferHeight = p.BackBufferHeight;
    q.BackBufferFormat = p.BackBufferFormat;
    q.BackBufferCount = p.BackBufferCount;
    q.MultiSampleType = p.MultiSampleType;
    q.MultiSampleQuality = 0;
    q.SwapEffect = p.SwapEffect == d3d8::kSwapEffectCopyVsync ? D3DSWAPEFFECT_COPY : static_cast<D3DSWAPEFFECT>(p.SwapEffect);
    q.hDeviceWindow = p.hDeviceWindow;
    q.Windowed = p.Windowed;
    q.EnableAutoDepthStencil = p.EnableAutoDepthStencil;
    q.AutoDepthStencilFormat = p.AutoDepthStencilFormat;
    q.Flags = p.Flags;
    q.FullScreen_RefreshRateInHz = p.Windowed ? 0 : p.FullScreen_RefreshRateInHz;
    if (p.Windowed)
        q.PresentationInterval =
            p.SwapEffect == d3d8::kSwapEffectCopyVsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    else
        q.PresentationInterval = p.FullScreen_PresentationInterval;
    return q;
}

void BackToPresent8(const D3DPRESENT_PARAMETERS& q, d3d8::PresentParameters* p) {
    p->BackBufferWidth = q.BackBufferWidth;
    p->BackBufferHeight = q.BackBufferHeight;
    p->BackBufferFormat = q.BackBufferFormat;
    p->BackBufferCount = q.BackBufferCount;
}

class Device8;
class Direct3D8;
Device8* g_current = nullptr;  // the game's device (render::NoteHudStart)

// ---------------------------------------------------------------------------------------------------------------
// Surfaces / volumes: their life is their container's when they have one (texture levels, swap-chain and implicit
// back buffers), as in D3D8.
// ---------------------------------------------------------------------------------------------------------------
class Surface8 {
public:
    Surface8(Device8* dev, IDirect3DSurface9* s9, void* container);
    void Destroy();  // owner: releases the D3D9 surface and frees the wrapper

    virtual HRESULT __stdcall QueryInterface(REFIID riid, void** out) {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == kIidSurface8) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    virtual ULONG __stdcall AddRef();
    virtual ULONG __stdcall Release();
    virtual HRESULT __stdcall GetDevice(Device8** out);
    virtual HRESULT __stdcall SetPrivateData(REFGUID g, const void* data, DWORD size, DWORD flags) {
        return s9 ? s9->SetPrivateData(g, data, size, flags) : D3DERR_INVALIDCALL;
    }
    virtual HRESULT __stdcall GetPrivateData(REFGUID g, void* data, DWORD* size) {
        return s9 ? s9->GetPrivateData(g, data, size) : D3DERR_INVALIDCALL;
    }
    virtual HRESULT __stdcall FreePrivateData(REFGUID g) { return s9 ? s9->FreePrivateData(g) : D3DERR_INVALIDCALL; }
    virtual HRESULT __stdcall GetContainer(REFIID, void** out);
    virtual HRESULT __stdcall GetDesc(d3d8::SurfaceDesc* d) {
        D3DSURFACE_DESC d9;
        if (!d || !s9) return D3DERR_INVALIDCALL;
        HRESULT hr = s9->GetDesc(&d9);
        if (SUCCEEDED(hr)) ToDesc8(d9, d);
        return hr;
    }
    virtual HRESULT __stdcall LockRect(D3DLOCKED_RECT* locked, const RECT* r, DWORD flags) {
        return s9 ? s9->LockRect(locked, r, flags) : D3DERR_INVALIDCALL;
    }
    virtual HRESULT __stdcall UnlockRect() { return s9 ? s9->UnlockRect() : D3DERR_INVALIDCALL; }

    uint32_t magic = kMagic;
    LONG refs = 0;  // only used without a container
    Device8* dev;
    IDirect3DSurface9* s9;
    void* container;  // wrapper (texture / cube / swap chain / device) or nullptr = standalone
};

class Volume8 {
public:
    Volume8(Device8* dev, IDirect3DVolume9* v9, void* container) : dev(dev), v9(v9), container(container) {}
    void Destroy() {
        SafeRelease(v9);
        delete this;
    }

    virtual HRESULT __stdcall QueryInterface(REFIID riid, void** out) {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == kIidVolume8) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    virtual ULONG __stdcall AddRef() { return VAddRef(container); }
    virtual ULONG __stdcall Release() { return VRelease(container); }
    virtual HRESULT __stdcall GetDevice(Device8** out);
    virtual HRESULT __stdcall SetPrivateData(REFGUID g, const void* data, DWORD size, DWORD flags) {
        return v9->SetPrivateData(g, data, size, flags);
    }
    virtual HRESULT __stdcall GetPrivateData(REFGUID g, void* data, DWORD* size) { return v9->GetPrivateData(g, data, size); }
    virtual HRESULT __stdcall FreePrivateData(REFGUID g) { return v9->FreePrivateData(g); }
    virtual HRESULT __stdcall GetContainer(REFIID, void** out) {
        if (!out) return D3DERR_INVALIDCALL;
        *out = container;
        VAddRef(container);
        return S_OK;
    }
    virtual HRESULT __stdcall GetDesc(d3d8::VolumeDesc* d) {
        D3DVOLUME_DESC d9;
        if (!d) return D3DERR_INVALIDCALL;
        HRESULT hr = v9->GetDesc(&d9);
        if (SUCCEEDED(hr)) ToDesc8(d9, d);
        return hr;
    }
    virtual HRESULT __stdcall LockBox(D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) {
        return v9->LockBox(locked, box, flags);
    }
    virtual HRESULT __stdcall UnlockBox() { return v9->UnlockBox(); }

    uint32_t magic = kMagic;
    Device8* dev;
    IDirect3DVolume9* v9;
    void* container;
};

// ---------------------------------------------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------------------------------------------
class Resource8 {
public:
    Resource8(Device8* dev, IDirect3DResource9* r9, const IID& iid, void (*destroy)(Resource8*));

    virtual HRESULT __stdcall QueryInterface(REFIID riid, void** out) {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == kIidResource8 || riid == iid_ ||
            (riid == kIidBaseTexture8 && (iid_ == kIidTexture8 || iid_ == kIidCubeTexture8 || iid_ == kIidVolumeTexture8))) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    virtual ULONG __stdcall AddRef() { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    virtual ULONG __stdcall Release() {
        LONG n = InterlockedDecrement(&refs_);
        if (n == 0) destroy_(this);
        return static_cast<ULONG>(n);
    }
    virtual HRESULT __stdcall GetDevice(Device8** out);
    virtual HRESULT __stdcall SetPrivateData(REFGUID g, const void* data, DWORD size, DWORD flags) {
        return r9->SetPrivateData(g, data, size, flags);
    }
    virtual HRESULT __stdcall GetPrivateData(REFGUID g, void* data, DWORD* size) { return r9->GetPrivateData(g, data, size); }
    virtual HRESULT __stdcall FreePrivateData(REFGUID g) { return r9->FreePrivateData(g); }
    virtual DWORD __stdcall SetPriority(DWORD p) { return r9->SetPriority(p); }
    virtual DWORD __stdcall GetPriority() { return r9->GetPriority(); }
    virtual void __stdcall PreLoad() { r9->PreLoad(); }
    virtual D3DRESOURCETYPE __stdcall GetType() { return r9->GetType(); }

    uint32_t magic = kMagic;
    Device8* dev;
    IDirect3DResource9* r9;

protected:
    void Finish();  // shared part of every destroy: forget bindings, release the D3D9 object, drop the device

private:
    LONG refs_ = 1;
    IID iid_;
    void (*destroy_)(Resource8*);
};

class BaseTexture8 : public Resource8 {
public:
    BaseTexture8(Device8* dev, IDirect3DBaseTexture9* t9, const IID& iid, void (*destroy)(Resource8*))
        : Resource8(dev, t9, iid, destroy), bt9(t9) {}
    virtual DWORD __stdcall SetLOD(DWORD lod) { return bt9->SetLOD(lod); }
    virtual DWORD __stdcall GetLOD() { return bt9->GetLOD(); }
    virtual DWORD __stdcall GetLevelCount() { return bt9->GetLevelCount(); }

    IDirect3DBaseTexture9* bt9;
};

class Texture8 : public BaseTexture8 {
public:
    Texture8(Device8* dev, IDirect3DTexture9* t9)
        : BaseTexture8(dev, t9, kIidTexture8, &Texture8::Destroy), t9(t9), levels(t9->GetLevelCount(), nullptr) {}
    static void Destroy(Resource8* r) {
        auto* t = static_cast<Texture8*>(r);
        for (Surface8* s : t->levels)
            if (s) s->Destroy();
        t->Finish();
        delete t;
    }

    virtual HRESULT __stdcall GetLevelDesc(UINT level, d3d8::SurfaceDesc* d) {
        D3DSURFACE_DESC d9;
        if (!d) return D3DERR_INVALIDCALL;
        HRESULT hr = t9->GetLevelDesc(level, &d9);
        if (SUCCEEDED(hr)) ToDesc8(d9, d);
        return hr;
    }
    virtual HRESULT __stdcall GetSurfaceLevel(UINT level, Surface8** out) {
        if (!out || level >= levels.size()) return D3DERR_INVALIDCALL;
        if (!levels[level]) {
            IDirect3DSurface9* s9 = nullptr;
            HRESULT hr = t9->GetSurfaceLevel(level, &s9);
            if (FAILED(hr)) return hr;
            levels[level] = new Surface8(dev, s9, this);
        }
        *out = levels[level];
        AddRef();
        return S_OK;
    }
    virtual HRESULT __stdcall LockRect(UINT level, D3DLOCKED_RECT* locked, const RECT* r, DWORD flags) {
        return t9->LockRect(level, locked, r, flags);
    }
    virtual HRESULT __stdcall UnlockRect(UINT level) { return t9->UnlockRect(level); }
    virtual HRESULT __stdcall AddDirtyRect(const RECT* r) { return t9->AddDirtyRect(r); }

    IDirect3DTexture9* t9;
    std::vector<Surface8*> levels;
};

class CubeTexture8 : public BaseTexture8 {
public:
    CubeTexture8(Device8* dev, IDirect3DCubeTexture9* c9)
        : BaseTexture8(dev, c9, kIidCubeTexture8, &CubeTexture8::Destroy), c9(c9), faces(6 * c9->GetLevelCount(), nullptr) {}
    static void Destroy(Resource8* r) {
        auto* t = static_cast<CubeTexture8*>(r);
        for (Surface8* s : t->faces)
            if (s) s->Destroy();
        t->Finish();
        delete t;
    }

    virtual HRESULT __stdcall GetLevelDesc(UINT level, d3d8::SurfaceDesc* d) {
        D3DSURFACE_DESC d9;
        if (!d) return D3DERR_INVALIDCALL;
        HRESULT hr = c9->GetLevelDesc(level, &d9);
        if (SUCCEEDED(hr)) ToDesc8(d9, d);
        return hr;
    }
    virtual HRESULT __stdcall GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, Surface8** out) {
        size_t i = static_cast<size_t>(face) * c9->GetLevelCount() + level;
        if (!out || face > D3DCUBEMAP_FACE_NEGATIVE_Z || i >= faces.size()) return D3DERR_INVALIDCALL;
        if (!faces[i]) {
            IDirect3DSurface9* s9 = nullptr;
            HRESULT hr = c9->GetCubeMapSurface(face, level, &s9);
            if (FAILED(hr)) return hr;
            faces[i] = new Surface8(dev, s9, this);
        }
        *out = faces[i];
        AddRef();
        return S_OK;
    }
    virtual HRESULT __stdcall LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* locked, const RECT* r, DWORD flags) {
        return c9->LockRect(face, level, locked, r, flags);
    }
    virtual HRESULT __stdcall UnlockRect(D3DCUBEMAP_FACES face, UINT level) { return c9->UnlockRect(face, level); }
    virtual HRESULT __stdcall AddDirtyRect(D3DCUBEMAP_FACES face, const RECT* r) { return c9->AddDirtyRect(face, r); }

    IDirect3DCubeTexture9* c9;
    std::vector<Surface8*> faces;
};

class VolumeTexture8 : public BaseTexture8 {
public:
    VolumeTexture8(Device8* dev, IDirect3DVolumeTexture9* v9)
        : BaseTexture8(dev, v9, kIidVolumeTexture8, &VolumeTexture8::Destroy), v9(v9), levels(v9->GetLevelCount(), nullptr) {}
    static void Destroy(Resource8* r) {
        auto* t = static_cast<VolumeTexture8*>(r);
        for (Volume8* v : t->levels)
            if (v) v->Destroy();
        t->Finish();
        delete t;
    }

    virtual HRESULT __stdcall GetLevelDesc(UINT level, d3d8::VolumeDesc* d) {
        D3DVOLUME_DESC d9;
        if (!d) return D3DERR_INVALIDCALL;
        HRESULT hr = v9->GetLevelDesc(level, &d9);
        if (SUCCEEDED(hr)) ToDesc8(d9, d);
        return hr;
    }
    virtual HRESULT __stdcall GetVolumeLevel(UINT level, Volume8** out) {
        if (!out || level >= levels.size()) return D3DERR_INVALIDCALL;
        if (!levels[level]) {
            IDirect3DVolume9* l9 = nullptr;
            HRESULT hr = v9->GetVolumeLevel(level, &l9);
            if (FAILED(hr)) return hr;
            levels[level] = new Volume8(dev, l9, this);
        }
        *out = levels[level];
        AddRef();
        return S_OK;
    }
    virtual HRESULT __stdcall LockBox(UINT level, D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) {
        return v9->LockBox(level, locked, box, flags);
    }
    virtual HRESULT __stdcall UnlockBox(UINT level) { return v9->UnlockBox(level); }
    virtual HRESULT __stdcall AddDirtyBox(const D3DBOX* box) { return v9->AddDirtyBox(box); }

    IDirect3DVolumeTexture9* v9;
    std::vector<Volume8*> levels;
};

class VertexBuffer8 : public Resource8 {
public:
    VertexBuffer8(Device8* dev, IDirect3DVertexBuffer9* b9) : Resource8(dev, b9, kIidVertexBuffer8, &VertexBuffer8::Destroy), b9(b9) {}
    static void Destroy(Resource8* r) {
        auto* b = static_cast<VertexBuffer8*>(r);
        b->Finish();
        delete b;
    }
    virtual HRESULT __stdcall Lock(UINT offset, UINT size, BYTE** data, DWORD flags) {
        if (!data) return D3DERR_INVALIDCALL;
        HRESULT hr = b9->Lock(offset, size, reinterpret_cast<void**>(data), flags);
        if (SUCCEEDED(hr) && *data) return hr;
        // The game never checks a lock (a null pointer crashed the skinning code at 0x5404FC): hand out a scratch
        // buffer - that mesh misses one frame instead of the game crashing.
        if (SUCCEEDED(hr)) b9->Unlock();
        D3DVERTEXBUFFER_DESC d{};
        b9->GetDesc(&d);
        LockFailed("VertexBuffer8", hr, flags, offset, size, d.Usage, d.Pool, d.Size);
        scratch.resize(size ? size : d.Size);
        *data = scratch.data();
        scratchLocked = true;
        return D3D_OK;
    }
    virtual HRESULT __stdcall Unlock() {
        if (scratchLocked) {
            scratchLocked = false;
            return D3D_OK;
        }
        return b9->Unlock();
    }
    virtual HRESULT __stdcall GetDesc(D3DVERTEXBUFFER_DESC* d) { return b9->GetDesc(d); }

    std::vector<BYTE> scratch;
    bool scratchLocked = false;

    IDirect3DVertexBuffer9* b9;
};

class IndexBuffer8 : public Resource8 {
public:
    IndexBuffer8(Device8* dev, IDirect3DIndexBuffer9* b9) : Resource8(dev, b9, kIidIndexBuffer8, &IndexBuffer8::Destroy), b9(b9) {}
    static void Destroy(Resource8* r) {
        auto* b = static_cast<IndexBuffer8*>(r);
        b->Finish();
        delete b;
    }
    virtual HRESULT __stdcall Lock(UINT offset, UINT size, BYTE** data, DWORD flags) {
        if (!data) return D3DERR_INVALIDCALL;
        HRESULT hr = b9->Lock(offset, size, reinterpret_cast<void**>(data), flags);
        if (SUCCEEDED(hr) && *data) return hr;
        // The game never checks a lock (a null pointer crashed the skinning code at 0x5404FC): hand out a scratch
        // buffer - that mesh misses one frame instead of the game crashing.
        if (SUCCEEDED(hr)) b9->Unlock();
        D3DINDEXBUFFER_DESC d{};
        b9->GetDesc(&d);
        LockFailed("IndexBuffer8", hr, flags, offset, size, d.Usage, d.Pool, d.Size);
        scratch.resize(size ? size : d.Size);
        *data = scratch.data();
        scratchLocked = true;
        return D3D_OK;
    }
    virtual HRESULT __stdcall Unlock() {
        if (scratchLocked) {
            scratchLocked = false;
            return D3D_OK;
        }
        return b9->Unlock();
    }
    virtual HRESULT __stdcall GetDesc(D3DINDEXBUFFER_DESC* d) { return b9->GetDesc(d); }

    std::vector<BYTE> scratch;
    bool scratchLocked = false;

    IDirect3DIndexBuffer9* b9;
};

class SwapChain8 {
public:
    SwapChain8(Device8* dev, IDirect3DSwapChain9* sc9);

    virtual HRESULT __stdcall QueryInterface(REFIID riid, void** out) {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == kIidSwapChain8) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    virtual ULONG __stdcall AddRef() { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    virtual ULONG __stdcall Release();
    virtual HRESULT __stdcall Present(const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
        return sc9->Present(src, dst, wnd, dirty, 0);
    }
    virtual HRESULT __stdcall GetBackBuffer(UINT i, D3DBACKBUFFER_TYPE type, Surface8** out) {
        if (!out) return D3DERR_INVALIDCALL;
        if (i >= buffers_.size()) buffers_.resize(i + 1, nullptr);
        if (!buffers_[i]) {
            IDirect3DSurface9* s9 = nullptr;
            HRESULT hr = sc9->GetBackBuffer(i, type, &s9);
            if (FAILED(hr)) return hr;
            buffers_[i] = new Surface8(dev, s9, this);
        }
        *out = buffers_[i];
        AddRef();
        return S_OK;
    }

    uint32_t magic = kMagic;
    Device8* dev;
    IDirect3DSwapChain9* sc9;

private:
    LONG refs_ = 1;
    std::vector<Surface8*> buffers_;
};

template <class T>
T* Unwrap(void* p) {  // our wrapper or nullptr (foreign pointers are logged and treated as nullptr)
    if (!p) return nullptr;
    auto* t = static_cast<T*>(p);
    if (t->magic == kMagic) return t;
    LogOnce("a non-wrapper object was passed to the device - ignored");
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// IDirect3D8
// ---------------------------------------------------------------------------------------------------------------
class Direct3D8 {
public:
    Direct3D8(IDirect3D9* d9, IUnknown* device12, IUnknown* queue12) : d9(d9), device12(device12), queue12(queue12) {
        // D3D8 lists the modes of every format in one list; D3D9 per format.
        const D3DFORMAT formats[] = {D3DFMT_X8R8G8B8, D3DFMT_R5G6B5, D3DFMT_X1R5G5B5};
        UINT n = d9->GetAdapterCount();
        modes_.resize(n);
        for (UINT a = 0; a < n; ++a)
            for (D3DFORMAT f : formats)
                for (UINT i = 0, c = d9->GetAdapterModeCount(a, f); i < c; ++i) {
                    D3DDISPLAYMODE m;
                    if (SUCCEEDED(d9->EnumAdapterModes(a, f, i, &m))) modes_[a].push_back(m);
                }
    }

    virtual HRESULT __stdcall QueryInterface(REFIID riid, void** out) {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == kIidDirect3D8) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    virtual ULONG __stdcall AddRef() { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    virtual ULONG __stdcall Release() {
        LONG n = InterlockedDecrement(&refs_);
        if (n == 0) {
            d9->Release();
            if (queue12) queue12->Release();
            if (device12) device12->Release();
            delete this;
        }
        return static_cast<ULONG>(n);
    }
    virtual HRESULT __stdcall RegisterSoftwareDevice(void* init) { return d9->RegisterSoftwareDevice(init); }
    virtual UINT __stdcall GetAdapterCount() { return d9->GetAdapterCount(); }
    virtual HRESULT __stdcall GetAdapterIdentifier(UINT adapter, DWORD, d3d8::AdapterIdentifier* id) {
        D3DADAPTER_IDENTIFIER9 i9;
        if (!id) return D3DERR_INVALIDCALL;
        HRESULT hr = d9->GetAdapterIdentifier(adapter, 0, &i9);  // no WHQL query (slow; D3D8's flag meant "skip it")
        if (FAILED(hr)) return hr;
        memcpy(id->Driver, i9.Driver, sizeof id->Driver);
        memcpy(id->Description, i9.Description, sizeof id->Description);
        id->DriverVersion = i9.DriverVersion;
        id->VendorId = i9.VendorId;
        id->DeviceId = i9.DeviceId;
        id->SubSysId = i9.SubSysId;
        id->Revision = i9.Revision;
        id->DeviceIdentifier = i9.DeviceIdentifier;
        id->WHQLLevel = i9.WHQLLevel;
        return hr;
    }
    virtual UINT __stdcall GetAdapterModeCount(UINT adapter) {
        return adapter < modes_.size() ? static_cast<UINT>(modes_[adapter].size()) : 0;
    }
    virtual HRESULT __stdcall EnumAdapterModes(UINT adapter, UINT i, D3DDISPLAYMODE* m) {
        if (!m || adapter >= modes_.size() || i >= modes_[adapter].size()) return D3DERR_INVALIDCALL;
        *m = modes_[adapter][i];
        return S_OK;
    }
    virtual HRESULT __stdcall GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* m) { return d9->GetAdapterDisplayMode(adapter, m); }
    virtual HRESULT __stdcall CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT back, BOOL windowed) {
        return d9->CheckDeviceType(adapter, type, display, back, windowed);
    }
    virtual HRESULT __stdcall CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat, DWORD usage,
                                                D3DRESOURCETYPE rtype, D3DFORMAT format) {
        if (format == D3DFMT_P8 || format == D3DFMT_A8P8 || format == d3d8::kFormatW11V11U10) return D3DERR_NOTAVAILABLE;
        return d9->CheckDeviceFormat(adapter, type, adapterFormat, usage, rtype, format);
    }
    virtual HRESULT __stdcall CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed,
                                                         D3DMULTISAMPLE_TYPE ms) {
        return d9->CheckDeviceMultiSampleType(adapter, type, format, windowed, ms, nullptr);
    }
    virtual HRESULT __stdcall CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat, D3DFORMAT rt,
                                                     D3DFORMAT ds) {
        return d9->CheckDepthStencilMatch(adapter, type, adapterFormat, rt, ds);
    }
    virtual HRESULT __stdcall GetDeviceCaps(UINT adapter, D3DDEVTYPE type, void* caps) {
        D3DCAPS9 c9;
        if (!caps) return D3DERR_INVALIDCALL;
        HRESULT hr = d9->GetDeviceCaps(adapter, type, &c9);
        if (SUCCEEDED(hr)) ToCaps8(c9, caps);
        return hr;
    }
    virtual HMONITOR __stdcall GetAdapterMonitor(UINT adapter) { return d9->GetAdapterMonitor(adapter); }
    virtual HRESULT __stdcall CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                           d3d8::PresentParameters* pp, Device8** out);

    static void ToCaps8(D3DCAPS9 c, void* out) {
        c.RasterCaps |= d3d8::kRasterCapsZBias;  // emulated with DEPTHBIAS
        // Shaders: at most what D3D8 knew (vs 1.1, ps 1.4); translated by shader8.cpp.
        if ((c.VertexShaderVersion & 0xFFFF) > 0x0101) c.VertexShaderVersion = D3DVS_VERSION(1, 1);
        if ((c.PixelShaderVersion & 0xFFFF) > 0x0104) c.PixelShaderVersion = D3DPS_VERSION(1, 4);
        memcpy(out, &c, d3d8::kCapsSize);
    }

    IDirect3D9* d9;
    IUnknown* device12;  // our D3D12 device + queue (Renderer 3), else nullptr
    IUnknown* queue12;

private:
    LONG refs_ = 1;
    std::vector<std::vector<D3DDISPLAYMODE>> modes_;
};

// ---------------------------------------------------------------------------------------------------------------
// IDirect3DDevice8
// ---------------------------------------------------------------------------------------------------------------
struct VertexShader {
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    std::vector<DWORD> declTokens, function;
    struct Constants {
        DWORD reg;
        std::vector<float> values;  // 4 per register
    };
    std::vector<Constants> constants;  // D3DVSD_CONST: loaded whenever the shader is set
};
struct PixelShader {
    IDirect3DPixelShader9* ps = nullptr;
    std::vector<DWORD> function;
};

// Dev builds: a shader D3D9 refused is written next to the exe (original + converted) for a look with a disassembler.
void DumpShader(const char* kind, unsigned n, const DWORD* original, const std::vector<DWORD>& converted) {
#ifndef DS_DIST
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    for (int pass = 0; pass < 2; ++pass) {
        snprintf(slash + 1, MAX_PATH - (slash + 1 - path), "DesertStormFix-%s%u-%s.bin", kind, n, pass ? "d3d9" : "d3d8");
        if (FILE* f = fopen(path, "wb")) {
            if (pass) fwrite(converted.data(), 4, converted.size(), f);
            else fwrite(original, 4, render::ShaderLength(original), f);
            fclose(f);
        }
    }
#else
    (void)kind, (void)n, (void)original, (void)converted;
#endif
}

// Renderer 3: D3D9 renders windowed for a hidden window; our presenter shows the frames in the game window.
void ToHiddenWindow(D3DPRESENT_PARAMETERS& q) {
    q.hDeviceWindow = render::HiddenDeviceWindow();
    q.Windowed = TRUE;
    q.FullScreen_RefreshRateInHz = 0;
    q.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    q.SwapEffect = D3DSWAPEFFECT_DISCARD;
    q.BackBufferCount = 1;
}

class Device8 {
public:
    Device8(Direct3D8* parent, IDirect3DDevice9* d9, const D3DPRESENT_PARAMETERS& pp, HWND gameWindow)
        : d9(d9), parent_(parent), gameWindow_(gameWindow) {
        parent_->AddRef();
        if (gameWindow_ && parent_->queue12)
            presenter_ = render::Presenter::Create(d9, parent_->queue12, gameWindow_, pp.BackBufferWidth, pp.BackBufferHeight);
        autoDepth_ = pp.EnableAutoDepthStencil != FALSE;
        FetchImplicit();
        MakeFloatTarget();
        rt_ = backBuffers_.empty() ? nullptr : backBuffers_[0];
        ds_ = depth_;
    }

    // Internal reference counting (resources hold the device) - not the vtable, so no hooks run.
    void Ref() { InterlockedIncrement(&refs_); }
    void Unref() {
        if (InterlockedDecrement(&refs_) == 0) Destroy();
    }

    // A wrapper is going away: forget every binding that points at it.
    void Forget(const void* w) {
        for (auto& t : textures_)
            if (t == w) t = nullptr;
        for (auto& s : streams_)
            if (s.vb == w) s.vb = nullptr;
        if (indices_ == w) indices_ = nullptr;
        if (rt_ == w) rt_ = nullptr;
        if (ds_ == w) ds_ = nullptr;
    }

    // ---- interface, in D3D8 order ----
    virtual HRESULT __stdcall QueryInterface(REFIID riid, void** out) {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == kIidDevice8) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    virtual ULONG __stdcall AddRef() { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    virtual ULONG __stdcall Release() {
        LONG n = InterlockedDecrement(&refs_);
        if (n == 0) Destroy();
        return static_cast<ULONG>(n);
    }
    virtual HRESULT __stdcall TestCooperativeLevel() { return d9->TestCooperativeLevel(); }
    virtual UINT __stdcall GetAvailableTextureMem() { return d9->GetAvailableTextureMem(); }
    virtual HRESULT __stdcall ResourceManagerDiscardBytes(DWORD) { return d9->EvictManagedResources(); }
    virtual HRESULT __stdcall GetDirect3D(Direct3D8** out) {
        if (!out) return D3DERR_INVALIDCALL;
        *out = parent_;
        parent_->AddRef();
        return S_OK;
    }
    virtual HRESULT __stdcall GetDeviceCaps(void* caps) {
        D3DCAPS9 c9;
        if (!caps) return D3DERR_INVALIDCALL;
        HRESULT hr = d9->GetDeviceCaps(&c9);
        if (SUCCEEDED(hr)) Direct3D8::ToCaps8(c9, caps);
        return hr;
    }
    virtual HRESULT __stdcall GetDisplayMode(D3DDISPLAYMODE* m) { return d9->GetDisplayMode(0, m); }
    virtual HRESULT __stdcall GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p) { return d9->GetCreationParameters(p); }
    virtual HRESULT __stdcall SetCursorProperties(UINT x, UINT y, void* surface) {
        Surface8* s = Unwrap<Surface8>(surface);
        return d9->SetCursorProperties(x, y, s ? s->s9 : nullptr);
    }
    virtual void __stdcall SetCursorPosition(int x, int y, DWORD flags) { d9->SetCursorPosition(x, y, flags); }
    virtual BOOL __stdcall ShowCursor(BOOL show) { return d9->ShowCursor(show); }
    virtual HRESULT __stdcall CreateAdditionalSwapChain(d3d8::PresentParameters* pp, SwapChain8** out) {
        if (!pp || !out) return D3DERR_INVALIDCALL;
        D3DPRESENT_PARAMETERS q = ToPresent9(*pp);
        IDirect3DSwapChain9* sc = nullptr;
        HRESULT hr = d9->CreateAdditionalSwapChain(&q, &sc);
        if (FAILED(hr)) return hr;
        BackToPresent8(q, pp);
        *out = new SwapChain8(this, sc);
        return hr;
    }
    virtual HRESULT __stdcall Reset(d3d8::PresentParameters* pp) {
        if (!pp) return D3DERR_INVALIDCALL;
        D3DPRESENT_PARAMETERS q = ToPresent9(*pp);
        if (gameWindow_) ToHiddenWindow(q);
        ReleaseImplicit();  // D3D9 needs every reference to the old back buffer / depth surface gone
        ClearBindings();
        if (presenter_) presenter_->BeforeReset();
        render::WaterBeforeReset();   // its scene copy is a D3DPOOL_DEFAULT texture
        HRESULT hr = d9->Reset(&q);
        if (SUCCEEDED(hr) && presenter_) presenter_->AfterReset(q.BackBufferWidth, q.BackBufferHeight);
        autoDepth_ = q.EnableAutoDepthStencil != FALSE;
        if (SUCCEEDED(hr)) {
            BackToPresent8(q, pp);
            FetchImplicit();
            MakeFloatTarget();
            rt_ = backBuffers_.empty() ? nullptr : backBuffers_[0];
            ds_ = depth_;
        } else {
            dslog::Write("D3D8on9: Reset failed (0x%08X)", static_cast<unsigned>(hr));
        }
        return hr;
    }
    virtual HRESULT __stdcall Present(const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
#ifndef DS_DIST
        if (++presents_ % 15 == 0) DevShot();
#endif
        if (presenter_)
            return presenter_->Present(backBuffers_.empty() ? nullptr : backBuffers_[0]->s9, settings::Get().vsync,
                                       *reinterpret_cast<const uint32_t*>(kDispatchMode) == 1);
        // D3D9 renders for a hidden window when it was meant for our presenter: show it in the game window
        return d9->Present(src, dst, wnd ? wnd : gameWindow_, dirty);
    }
    virtual HRESULT __stdcall GetBackBuffer(UINT i, D3DBACKBUFFER_TYPE, Surface8** out) {
        if (!out || i >= backBuffers_.size() || !backBuffers_[i] || !backBuffers_[i]->s9) return D3DERR_INVALIDCALL;
        *out = backBuffers_[i];
        Ref();
        return S_OK;
    }
    virtual HRESULT __stdcall GetRasterStatus(D3DRASTER_STATUS* s) { return d9->GetRasterStatus(0, s); }
    virtual void __stdcall SetGammaRamp(DWORD flags, const D3DGAMMARAMP* ramp) { d9->SetGammaRamp(0, flags, ramp); }
    virtual void __stdcall GetGammaRamp(D3DGAMMARAMP* ramp) { d9->GetGammaRamp(0, ramp); }
    virtual HRESULT __stdcall CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                            Texture8** out) {
        if (!out) return D3DERR_INVALIDCALL;
        IDirect3DTexture9* t = nullptr;
        HRESULT hr = d9->CreateTexture(w, h, levels, usage, format, pool, &t, nullptr);
        if (FAILED(hr)) return hr;
        *out = new Texture8(this, t);
        return hr;
    }
    virtual HRESULT __stdcall CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT format,
                                                  D3DPOOL pool, VolumeTexture8** out) {
        if (!out) return D3DERR_INVALIDCALL;
        IDirect3DVolumeTexture9* t = nullptr;
        HRESULT hr = d9->CreateVolumeTexture(w, h, d, levels, usage, format, pool, &t, nullptr);
        if (FAILED(hr)) return hr;
        *out = new VolumeTexture8(this, t);
        return hr;
    }
    virtual HRESULT __stdcall CreateCubeTexture(UINT size, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                                CubeTexture8** out) {
        if (!out) return D3DERR_INVALIDCALL;
        IDirect3DCubeTexture9* t = nullptr;
        HRESULT hr = d9->CreateCubeTexture(size, levels, usage, format, pool, &t, nullptr);
        if (FAILED(hr)) return hr;
        *out = new CubeTexture8(this, t);
        return hr;
    }
    virtual HRESULT __stdcall CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, VertexBuffer8** out) {
        if (!out) return D3DERR_INVALIDCALL;
        IDirect3DVertexBuffer9* b = nullptr;
        HRESULT hr = d9->CreateVertexBuffer(length, usage, fvf, pool, &b, nullptr);
        if (FAILED(hr)) return hr;
        *out = new VertexBuffer8(this, b);
        return hr;
    }
    virtual HRESULT __stdcall CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IndexBuffer8** out) {
        if (!out) return D3DERR_INVALIDCALL;
        IDirect3DIndexBuffer9* b = nullptr;
        HRESULT hr = d9->CreateIndexBuffer(length, usage, format, pool, &b, nullptr);
        if (FAILED(hr)) return hr;
        *out = new IndexBuffer8(this, b);
        return hr;
    }
    virtual HRESULT __stdcall CreateRenderTarget(UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms, BOOL lockable,
                                                 Surface8** out) {
        if (!out) return D3DERR_INVALIDCALL;
        IDirect3DSurface9* s = nullptr;
        HRESULT hr = d9->CreateRenderTarget(w, h, format, ms, 0, lockable, &s, nullptr);
        if (FAILED(hr)) return hr;
        *out = NewStandalone(s);
        return hr;
    }
    virtual HRESULT __stdcall CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms,
                                                        Surface8** out) {
        if (!out) return D3DERR_INVALIDCALL;
        IDirect3DSurface9* s = nullptr;
        HRESULT hr = d9->CreateDepthStencilSurface(w, h, format, ms, 0, FALSE, &s, nullptr);
        if (FAILED(hr)) return hr;
        *out = NewStandalone(s);
        return hr;
    }
    virtual HRESULT __stdcall CreateImageSurface(UINT w, UINT h, D3DFORMAT format, Surface8** out) {
        if (!out) return D3DERR_INVALIDCALL;
        IDirect3DSurface9* s = nullptr;
        HRESULT hr = d9->CreateOffscreenPlainSurface(w, h, format, D3DPOOL_SYSTEMMEM, &s, nullptr);
        if (FAILED(hr)) return hr;
        *out = NewStandalone(s);
        return hr;
    }
    virtual HRESULT __stdcall CopyRects(void* src, const RECT* rects, UINT count, void* dst, const POINT* points);
    virtual HRESULT __stdcall UpdateTexture(void* src, void* dst) {
        BaseTexture8* s = Unwrap<BaseTexture8>(src);
        BaseTexture8* d = Unwrap<BaseTexture8>(dst);
        if (!s || !d) return D3DERR_INVALIDCALL;
        return d9->UpdateTexture(s->bt9, d->bt9);
    }
    virtual HRESULT __stdcall GetFrontBuffer(void* dst) {
        Surface8* d = Unwrap<Surface8>(dst);
        if (!d) return D3DERR_INVALIDCALL;
        return d9->GetFrontBufferData(0, d->s9);
    }
    virtual HRESULT __stdcall SetRenderTarget(void* target, void* depth) {
        Surface8* rt = Unwrap<Surface8>(target);
        Surface8* ds = Unwrap<Surface8>(depth);
        if (rt) {  // D3D8: a null target keeps the current one
            HRESULT hr = d9->SetRenderTarget(0, rt->s9);
            if (FAILED(hr)) return hr;
            rt_ = rt;
        }
        HRESULT hr = d9->SetDepthStencilSurface(ds ? ds->s9 : nullptr);
        if (SUCCEEDED(hr)) ds_ = ds;
        return hr;
    }
    virtual HRESULT __stdcall GetRenderTarget(Surface8** out) {
        if (!out) return D3DERR_INVALIDCALL;
        *out = rt_;
        if (!rt_) return D3DERR_NOTFOUND;
        rt_->AddRef();
        return S_OK;
    }
    virtual HRESULT __stdcall GetDepthStencilSurface(Surface8** out) {
        if (!out) return D3DERR_INVALIDCALL;
        *out = ds_;
        if (!ds_) return D3DERR_NOTFOUND;
        ds_->AddRef();
        return S_OK;
    }
    virtual HRESULT __stdcall BeginScene() { return d9->BeginScene(); }
    virtual HRESULT __stdcall EndScene() { return d9->EndScene(); }
    virtual HRESULT __stdcall Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR colour, float z, DWORD stencil) {
        return d9->Clear(count, rects, flags, colour, z, stencil);
    }
    virtual HRESULT __stdcall SetTransform(D3DTRANSFORMSTATETYPE t, const D3DMATRIX* m) { return d9->SetTransform(t, m); }
    virtual HRESULT __stdcall GetTransform(D3DTRANSFORMSTATETYPE t, D3DMATRIX* m) { return d9->GetTransform(t, m); }
    virtual HRESULT __stdcall MultiplyTransform(D3DTRANSFORMSTATETYPE t, const D3DMATRIX* m) { return d9->MultiplyTransform(t, m); }
    virtual HRESULT __stdcall SetViewport(const D3DVIEWPORT9* v) { return d9->SetViewport(v); }
    virtual HRESULT __stdcall GetViewport(D3DVIEWPORT9* v) { return d9->GetViewport(v); }
    virtual HRESULT __stdcall SetMaterial(const D3DMATERIAL9* m) { return d9->SetMaterial(m); }
    virtual HRESULT __stdcall GetMaterial(D3DMATERIAL9* m) { return d9->GetMaterial(m); }
    virtual HRESULT __stdcall SetLight(DWORD i, const D3DLIGHT9* l) { return d9->SetLight(i, l); }
    virtual HRESULT __stdcall GetLight(DWORD i, D3DLIGHT9* l) { return d9->GetLight(i, l); }
    virtual HRESULT __stdcall LightEnable(DWORD i, BOOL on) { return d9->LightEnable(i, on); }
    virtual HRESULT __stdcall GetLightEnable(DWORD i, BOOL* on) { return d9->GetLightEnable(i, on); }
    virtual HRESULT __stdcall SetClipPlane(DWORD i, const float* p) { return d9->SetClipPlane(i, p); }
    virtual HRESULT __stdcall GetClipPlane(DWORD i, float* p) { return d9->GetClipPlane(i, p); }
    virtual HRESULT __stdcall SetRenderState(DWORD state, DWORD value) {
        switch (state) {
            case d3d8::kRsLinePattern: case d3d8::kRsZVisible: case d3d8::kRsPatchSegments:
                return S_OK;  // gone in D3D9
            case d3d8::kRsEdgeAntialias:
                return d9->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, value);
            case d3d8::kRsZBias: {
                zbias_ = value;
                // DXVK's d3d8 scale: one ZBIAS step = one D16 depth step towards the viewer.
                float bias = static_cast<float>(value) * (-1.0f / 65535.0f);
                DWORD bits;
                memcpy(&bits, &bias, 4);
                return d9->SetRenderState(D3DRS_DEPTHBIAS, bits);
            }
            case d3d8::kRsSoftwareVertexProcessing:
                return d9->SetSoftwareVertexProcessing(value);
            default:
                return d9->SetRenderState(static_cast<D3DRENDERSTATETYPE>(state), value);
        }
    }
    virtual HRESULT __stdcall GetRenderState(DWORD state, DWORD* value) {
        if (!value) return D3DERR_INVALIDCALL;
        switch (state) {
            case d3d8::kRsLinePattern: case d3d8::kRsZVisible: case d3d8::kRsPatchSegments:
                *value = 0;
                return S_OK;
            case d3d8::kRsEdgeAntialias:
                return d9->GetRenderState(D3DRS_ANTIALIASEDLINEENABLE, value);
            case d3d8::kRsZBias:
                *value = zbias_;
                return S_OK;
            case d3d8::kRsSoftwareVertexProcessing:
                *value = d9->GetSoftwareVertexProcessing();
                return S_OK;
            default:
                return d9->GetRenderState(static_cast<D3DRENDERSTATETYPE>(state), value);
        }
    }
    // State blocks: token = the IDirect3DStateBlock9 pointer (32-bit). The wrapper's own state (vertex shader
    // handle, ZBIAS, base vertex index, bound wrappers) is not captured - the game uses no state blocks.
    virtual HRESULT __stdcall BeginStateBlock() { return d9->BeginStateBlock(); }
    virtual HRESULT __stdcall EndStateBlock(DWORD* token) {
        if (!token) return D3DERR_INVALIDCALL;
        IDirect3DStateBlock9* sb = nullptr;
        HRESULT hr = d9->EndStateBlock(&sb);
        *token = reinterpret_cast<DWORD>(sb);
        return hr;
    }
    virtual HRESULT __stdcall ApplyStateBlock(DWORD token) {
        return token ? reinterpret_cast<IDirect3DStateBlock9*>(token)->Apply() : D3DERR_INVALIDCALL;
    }
    virtual HRESULT __stdcall CaptureStateBlock(DWORD token) {
        return token ? reinterpret_cast<IDirect3DStateBlock9*>(token)->Capture() : D3DERR_INVALIDCALL;
    }
    virtual HRESULT __stdcall DeleteStateBlock(DWORD token) {
        if (!token) return D3DERR_INVALIDCALL;
        reinterpret_cast<IDirect3DStateBlock9*>(token)->Release();
        return S_OK;
    }
    virtual HRESULT __stdcall CreateStateBlock(D3DSTATEBLOCKTYPE type, DWORD* token) {
        if (!token) return D3DERR_INVALIDCALL;
        IDirect3DStateBlock9* sb = nullptr;
        HRESULT hr = d9->CreateStateBlock(type, &sb);
        *token = reinterpret_cast<DWORD>(sb);
        return hr;
    }
    virtual HRESULT __stdcall SetClipStatus(const D3DCLIPSTATUS9* s) { return d9->SetClipStatus(s); }
    virtual HRESULT __stdcall GetClipStatus(D3DCLIPSTATUS9* s) { return d9->GetClipStatus(s); }
    virtual HRESULT __stdcall GetTexture(DWORD stage, BaseTexture8** out) {
        if (!out || stage >= kStages) return D3DERR_INVALIDCALL;
        *out = textures_[stage];
        if (*out) (*out)->AddRef();
        return S_OK;
    }
    virtual HRESULT __stdcall SetTexture(DWORD stage, void* texture) {
        if (stage >= kStages) return D3DERR_INVALIDCALL;
        BaseTexture8* t = Unwrap<BaseTexture8>(texture);
        HRESULT hr = d9->SetTexture(stage, t ? t->bt9 : nullptr);
        if (SUCCEEDED(hr)) textures_[stage] = t;
        return hr;
    }
    virtual HRESULT __stdcall GetTextureStageState(DWORD stage, DWORD type, DWORD* value) {
        if (D3DSAMPLERSTATETYPE s = d3d8::SamplerOf(type)) return d9->GetSamplerState(stage, s, value);
        return d9->GetTextureStageState(stage, static_cast<D3DTEXTURESTAGESTATETYPE>(type), value);
    }
    virtual HRESULT __stdcall SetTextureStageState(DWORD stage, DWORD type, DWORD value) {
        if (D3DSAMPLERSTATETYPE s = d3d8::SamplerOf(type)) {
            if ((s == D3DSAMP_MAGFILTER || s == D3DSAMP_MINFILTER || s == D3DSAMP_MIPFILTER) && value > D3DTEXF_ANISOTROPIC)
                value = D3DTEXF_LINEAR;  // D3D8's FLATCUBIC / GAUSSIANCUBIC
            return d9->SetSamplerState(stage, s, value);
        }
        return d9->SetTextureStageState(stage, static_cast<D3DTEXTURESTAGESTATETYPE>(type), value);
    }
    virtual HRESULT __stdcall ValidateDevice(DWORD* passes) { return d9->ValidateDevice(passes); }
    virtual HRESULT __stdcall GetInfo(DWORD, void*, DWORD) { return S_FALSE; }
    virtual HRESULT __stdcall SetPaletteEntries(UINT n, const PALETTEENTRY* e) {
        d9->SetPaletteEntries(n, e);  // P8 is reported unsupported; harmless either way
        return S_OK;
    }
    virtual HRESULT __stdcall GetPaletteEntries(UINT n, PALETTEENTRY* e) { return d9->GetPaletteEntries(n, e); }
    virtual HRESULT __stdcall SetCurrentTexturePalette(UINT n) {
        d9->SetCurrentTexturePalette(n);
        return S_OK;
    }
    virtual HRESULT __stdcall GetCurrentTexturePalette(UINT* n) { return d9->GetCurrentTexturePalette(n); }
    virtual HRESULT __stdcall DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT count) {
        NoteDraw();
        return d9->DrawPrimitive(type, start, count);
    }
    virtual HRESULT __stdcall DrawIndexedPrimitive(D3DPRIMITIVETYPE type, UINT minIndex, UINT vertices, UINT startIndex,
                                                   UINT count) {
        NoteDraw();
        return d9->DrawIndexedPrimitive(type, static_cast<INT>(baseVertex_), minIndex, vertices, startIndex, count);
    }
    virtual HRESULT __stdcall DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride) {
        NoteDraw();
        HRESULT hr = d9->DrawPrimitiveUP(type, count, data, stride);
        streams_[0] = {};  // UP draws leave stream 0 and the indices unset, as in D3D8
        indices_ = nullptr;
        return hr;
    }
    virtual HRESULT __stdcall DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT vertices, UINT count,
                                                     const void* indexData, D3DFORMAT indexFormat, const void* data,
                                                     UINT stride) {
        NoteDraw();
        HRESULT hr = d9->DrawIndexedPrimitiveUP(type, minIndex, vertices, count, indexData, indexFormat, data, stride);
        streams_[0] = {};
        indices_ = nullptr;
        return hr;
    }
    virtual HRESULT __stdcall ProcessVertices(UINT srcStart, UINT dstIndex, UINT count, void* dst, DWORD flags) {
        VertexBuffer8* b = Unwrap<VertexBuffer8>(dst);
        if (!b) return D3DERR_INVALIDCALL;
        return d9->ProcessVertices(srcStart, dstIndex, count, b->b9, nullptr, flags);
    }
    virtual HRESULT __stdcall CreateVertexShader(const DWORD* decl, const DWORD* function, DWORD* handle, DWORD usage);
    virtual HRESULT __stdcall SetVertexShader(DWORD handle) {
        HRESULT hr;
        if (!(handle & 1)) {  // an FVF (bit 0 is reserved in FVF codes)
            d9->SetVertexShader(nullptr);
            hr = d9->SetFVF(handle);
        } else {
            VertexShader* s = Shader(handle);
            if (!s) return D3DERR_INVALIDCALL;
            d9->SetVertexDeclaration(s->decl);
            hr = d9->SetVertexShader(s->vs);
            for (const auto& c : s->constants)
                d9->SetVertexShaderConstantF(c.reg, c.values.data(), static_cast<UINT>(c.values.size() / 4));
        }
        if (SUCCEEDED(hr)) {
            vsHandle_ = handle;
            pretransformed_ = !(handle & 1) && (handle & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
        }
        return hr;
    }
    virtual HRESULT __stdcall GetVertexShader(DWORD* handle) {
        if (!handle) return D3DERR_INVALIDCALL;
        *handle = vsHandle_;
        return S_OK;
    }
    virtual HRESULT __stdcall DeleteVertexShader(DWORD handle) {
        VertexShader* s = Shader(handle);
        if (!s) return D3DERR_INVALIDCALL;
        if (vsHandle_ == handle) {
            d9->SetVertexShader(nullptr);
            vsHandle_ = 0;
        }
        SafeRelease(s->vs);
        SafeRelease(s->decl);
        delete s;
        shaders_[handle >> 1] = nullptr;
        return S_OK;
    }
    virtual HRESULT __stdcall SetVertexShaderConstant(DWORD reg, const void* data, DWORD count) {
        return d9->SetVertexShaderConstantF(reg, static_cast<const float*>(data), count);
    }
    virtual HRESULT __stdcall GetVertexShaderConstant(DWORD reg, void* data, DWORD count) {
        return d9->GetVertexShaderConstantF(reg, static_cast<float*>(data), count);
    }
    virtual HRESULT __stdcall GetVertexShaderDeclaration(DWORD handle, void* data, DWORD* size) {
        VertexShader* s = Shader(handle);
        return s ? CopyTokens(s->declTokens, data, size) : D3DERR_INVALIDCALL;
    }
    virtual HRESULT __stdcall GetVertexShaderFunction(DWORD handle, void* data, DWORD* size) {
        VertexShader* s = Shader(handle);
        return s ? CopyTokens(s->function, data, size) : D3DERR_INVALIDCALL;
    }
    virtual HRESULT __stdcall SetStreamSource(UINT stream, void* buffer, UINT stride) {
        if (stream >= kStreams) return D3DERR_INVALIDCALL;
        VertexBuffer8* b = Unwrap<VertexBuffer8>(buffer);
        HRESULT hr = d9->SetStreamSource(stream, b ? b->b9 : nullptr, 0, stride);
        if (SUCCEEDED(hr)) streams_[stream] = {b, stride};
        return hr;
    }
    virtual HRESULT __stdcall GetStreamSource(UINT stream, VertexBuffer8** out, UINT* stride) {
        if (!out || !stride || stream >= kStreams) return D3DERR_INVALIDCALL;
        *out = streams_[stream].vb;
        *stride = streams_[stream].stride;
        if (*out) (*out)->AddRef();
        return S_OK;
    }
    virtual HRESULT __stdcall SetIndices(void* buffer, UINT baseVertex) {
        IndexBuffer8* b = Unwrap<IndexBuffer8>(buffer);
        HRESULT hr = d9->SetIndices(b ? b->b9 : nullptr);
        if (SUCCEEDED(hr)) {
            indices_ = b;
            baseVertex_ = baseVertex;
        }
        return hr;
    }
    virtual HRESULT __stdcall GetIndices(IndexBuffer8** out, UINT* baseVertex) {
        if (!out || !baseVertex) return D3DERR_INVALIDCALL;
        *out = indices_;
        *baseVertex = baseVertex_;
        if (*out) (*out)->AddRef();
        return S_OK;
    }
    virtual HRESULT __stdcall CreatePixelShader(const DWORD* function, DWORD* handle);
    virtual HRESULT __stdcall SetPixelShader(DWORD handle) {
        PixelShader* s = handle ? PShader(handle) : nullptr;
        if (handle && !s) return D3DERR_INVALIDCALL;
        HRESULT hr = d9->SetPixelShader(s ? s->ps : nullptr);
        if (SUCCEEDED(hr)) psHandle_ = handle;
        return hr;
    }
    virtual HRESULT __stdcall GetPixelShader(DWORD* handle) {
        if (!handle) return D3DERR_INVALIDCALL;
        *handle = psHandle_;
        return S_OK;
    }
    virtual HRESULT __stdcall DeletePixelShader(DWORD handle) {
        PixelShader* s = PShader(handle);
        if (!s) return D3DERR_INVALIDCALL;
        if (psHandle_ == handle) {
            d9->SetPixelShader(nullptr);
            psHandle_ = 0;
        }
        SafeRelease(s->ps);
        delete s;
        pixelShaders_[handle >> 1] = nullptr;
        return S_OK;
    }
    virtual HRESULT __stdcall SetPixelShaderConstant(DWORD reg, const void* data, DWORD count) {
        return d9->SetPixelShaderConstantF(reg, static_cast<const float*>(data), count);
    }
    virtual HRESULT __stdcall GetPixelShaderConstant(DWORD reg, void* data, DWORD count) {
        return d9->GetPixelShaderConstantF(reg, static_cast<float*>(data), count);
    }
    virtual HRESULT __stdcall GetPixelShaderFunction(DWORD handle, void* data, DWORD* size) {
        PixelShader* s = PShader(handle);
        return s ? CopyTokens(s->function, data, size) : D3DERR_INVALIDCALL;
    }
    virtual HRESULT __stdcall DrawRectPatch(UINT, const float*, const void*) { return D3DERR_INVALIDCALL; }
    virtual HRESULT __stdcall DrawTriPatch(UINT, const float*, const void*) { return D3DERR_INVALIDCALL; }
    virtual HRESULT __stdcall DeletePatch(UINT) { return D3DERR_INVALIDCALL; }

    IDirect3DDevice9* d9;

private:
    struct Stream {
        VertexBuffer8* vb = nullptr;
        UINT stride = 0;
    };

    void NoteDraw() {
#ifndef DS_DIST
        LogDrawState();
#endif
    }

public:
    // HDR / bloom: the HUD of the current viewport starts (render::NoteHudStart) - the presenter copies the scene of
    // that viewport. Once per view and frame: copying at every 3D -> 2D switch meant dozens of MSAA sub-rectangle
    // resolves per frame during explosions, and 9On12 then hung the GPU (DXGI_ERROR_DEVICE_HUNG, 2026-10-16).
    void HudStart() {
        if (!presenter_ || !presenter_->WantsScene() || backBuffers_.empty() || !backBuffers_[0]->s9) return;
        D3DVIEWPORT9 vp;
        if (FAILED(d9->GetViewport(&vp))) return;
        RECT r{static_cast<LONG>(vp.X), static_cast<LONG>(vp.Y), static_cast<LONG>(vp.X + vp.Width),
               static_cast<LONG>(vp.Y + vp.Height)};
        presenter_->SnapshotScene(backBuffers_[0]->s9, r);
    }

private:

#ifndef DS_DIST
    // Dev (Dev\R9DrawStates = 1): each new combination of the states that matter for blending / rasterising is logged
    // once with the draw count so far - states that only appear with an effect (an explosion) stand out.
    void LogDrawState() {
        static int enabled = -1;
        if (enabled < 0) {
            DWORD v = 0, size = sizeof v;
            enabled = RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "R9DrawStates", RRF_RT_REG_DWORD, nullptr,
                                   &v, &size) == ERROR_SUCCESS && v;
        }
        if (!enabled) return;
        static const D3DRENDERSTATETYPE kStates[] = {D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP,
                                                     D3DRS_ALPHATESTENABLE, D3DRS_FOGENABLE, D3DRS_POINTSPRITEENABLE,
                                                     D3DRS_POINTSCALEENABLE, D3DRS_ZWRITEENABLE, D3DRS_CULLMODE, D3DRS_FILLMODE,
                                                     D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_COLORWRITEENABLE, D3DRS_STENCILENABLE};
        DWORD v[16] = {};
        uint64_t key = vsHandle_ * 1000003ull + psHandle_;
        for (size_t i = 0; i < std::size(kStates); ++i) {
            d9->GetRenderState(kStates[i], &v[i]);
            key = key * 131 + v[i];
        }
        static std::vector<uint64_t> seen;
        static unsigned draws = 0;
        ++draws;
        if (std::find(seen.begin(), seen.end(), key) != seen.end() || seen.size() > 300) return;
        seen.push_back(key);
        float psize = 0;
        d9->GetRenderState(D3DRS_POINTSIZE, reinterpret_cast<DWORD*>(&psize));
        dslog::Write("D3D8on9 state #%u (draw %u, %s): vs 0x%X ps %u | blend %u src %u dst %u op %u | atest %u fog %u | "
                     "psprite %u pscale %u psize %.1f | zwrite %u cull %u fill %u sepalpha %u cwrite 0x%X stencil %u",
                     static_cast<unsigned>(seen.size()), draws, pretransformed_ ? "2D" : "3D", vsHandle_, psHandle_, v[0], v[1],
                     v[2], v[3], v[4], v[5], v[6], v[7], psize, v[8], v[9], v[10], v[11], v[12], v[13]);
    }
#endif

    Surface8* NewStandalone(IDirect3DSurface9* s) {
        auto* w = new Surface8(this, s, nullptr);
        w->refs = 1;
        return w;
    }

    // Back buffers + the automatic depth surface: wrappers live as long as the device (their references count on
    // the device, as in D3D8); a Reset swaps the D3D9 surfaces inside them.
    void FetchImplicit() {
        IDirect3DSwapChain9* sc = nullptr;
        UINT count = 1;
        if (SUCCEEDED(d9->GetSwapChain(0, &sc))) {
            D3DPRESENT_PARAMETERS p;
            if (SUCCEEDED(sc->GetPresentParameters(&p))) count = std::max(1u, p.BackBufferCount);
            sc->Release();
        }
        if (backBuffers_.size() < count) backBuffers_.resize(count, nullptr);
        for (UINT i = 0; i < backBuffers_.size(); ++i) {
            IDirect3DSurface9* s = nullptr;
            if (i < count) d9->GetBackBuffer(0, i, D3DBACKBUFFER_TYPE_MONO, &s);
            if (!backBuffers_[i]) backBuffers_[i] = new Surface8(this, s, this);
            else backBuffers_[i]->s9 = s;
        }
        IDirect3DSurface9* z = nullptr;
        if (autoDepth_) d9->GetDepthStencilSurface(&z);
        if (!depth_) depth_ = new Surface8(this, z, this);
        else depth_->s9 = z;
    }
    // HDR (Renderer 3): an A16B16G16R16F render target (same size / MSAA as the back buffer) takes the back buffer's
    // place in the wrapper - the game, D3DX and the plugin draw into it and additive effects can exceed 1.0. The real
    // D3D9 back buffer (hidden window) is kept aside; the presenter reads the float target.
    void MakeFloatTarget() {
        bool forceFloat = false;
#ifndef DS_DIST
        DWORD ff = 0, ffSize = sizeof ff;   // dev: float target without HDR (test: does FP16 alone hang the GPU?)
        forceFloat = RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "R9FloatAlways", RRF_RT_REG_DWORD, nullptr,
                                  &ff, &ffSize) == ERROR_SUCCESS && ff;
#endif
        if (!presenter_ || (!presenter_->WantsFloatScene() && !forceFloat) || backBuffers_.empty() || !backBuffers_[0]->s9) return;
#ifndef DS_DIST
        DWORD noFloat = 0, size = sizeof noFloat;
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "R9NoFloat", RRF_RT_REG_DWORD, nullptr, &noFloat,
                         &size) == ERROR_SUCCESS && noFloat)
            return;
#endif
        IDirect3DSurface9* bb = backBuffers_[0]->s9;
        D3DSURFACE_DESC d;
        bb->GetDesc(&d);
        D3DDEVICE_CREATION_PARAMETERS cp;
        d9->GetCreationParameters(&cp);
        IDirect3D9* d3d = nullptr;
        d9->GetDirect3D(&d3d);
        const D3DFORMAT f = D3DFMT_A16B16G16R16F;
        bool ok = d3d && SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, D3DFMT_X8R8G8B8,
                                                         D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING,
                                                         D3DRTYPE_SURFACE, f));
        if (ok && d.MultiSampleType != D3DMULTISAMPLE_NONE)
            ok = SUCCEEDED(d3d->CheckDeviceMultiSampleType(cp.AdapterOrdinal, cp.DeviceType, f, TRUE, d.MultiSampleType, nullptr));
        if (d3d) d3d->Release();
        IDirect3DSurface9* rt = nullptr;
        if (!ok || FAILED(d9->CreateRenderTarget(d.Width, d.Height, f, d.MultiSampleType, d.MultiSampleQuality, FALSE, &rt,
                                                 nullptr))) {
            dslog::Write("D3D8on9: no FP16 render target (MSAA %u) - HDR highlights stop at white", d.MultiSampleType);
            return;
        }
        realBackBuffer_ = bb;
        backBuffers_[0]->s9 = rt;
        d9->SetRenderTarget(0, rt);
        dslog::Write("D3D8on9: game draws into an FP16 render target %ux%u (MSAA %u) for HDR", d.Width, d.Height,
                     d.MultiSampleType);
    }

    void ReleaseImplicit() {
        SafeRelease(realBackBuffer_);
        for (Surface8* s : backBuffers_)
            if (s) SafeRelease(s->s9);
        if (depth_) SafeRelease(depth_->s9);
    }
    void ClearBindings() {
        for (auto& t : textures_) t = nullptr;
        for (auto& s : streams_) s = {};
        indices_ = nullptr;
        baseVertex_ = 0;
        vsHandle_ = 0;
        psHandle_ = 0;
        zbias_ = 0;
    }

    VertexShader* Shader(DWORD handle) {
        size_t i = handle >> 1;
        return (handle & 1) && i < shaders_.size() ? shaders_[i] : nullptr;
    }
    PixelShader* PShader(DWORD handle) {
        size_t i = handle >> 1;
        return (handle & 1) && i < pixelShaders_.size() ? pixelShaders_[i] : nullptr;
    }
    static HRESULT CopyTokens(const std::vector<DWORD>& tokens, void* data, DWORD* size) {
        if (!size) return D3DERR_INVALIDCALL;
        DWORD bytes = static_cast<DWORD>(tokens.size() * sizeof(DWORD));
        if (!data) {
            *size = bytes;
            return S_OK;
        }
        if (*size < bytes) return D3DERR_MOREDATA;
        memcpy(data, tokens.data(), bytes);
        *size = bytes;
        return S_OK;
    }

#ifndef DS_DIST
    // Dev: HKCU\Software\DesertStormFix\Dev\R9Shot = a .bmp path -> the back buffer (as presented next) is saved
    // there and the value deleted. Test scripts use it where PrintWindow can't read the window (9On12 flip model).
    void DevShot() {
        char path[MAX_PATH] = {};
        DWORD size = sizeof path - 1;
        constexpr const char* kDev = "Software\\DesertStormFix\\Dev";
        if (RegGetValueA(HKEY_CURRENT_USER, kDev, "R9Shot", RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS || !path[0])
            return;
        RegDeleteKeyValueA(HKEY_CURRENT_USER, kDev, "R9Shot");
        if (presenter_) {  // what our presenter puts in the swap chain (read back from D3D12)
            presenter_->RequestShot(path);
            return;
        }
        if (backBuffers_.empty() || !backBuffers_[0]->s9) return;
        IDirect3DSurface9* bb = backBuffers_[0]->s9;
        D3DSURFACE_DESC d;
        bb->GetDesc(&d);
        IDirect3DSurface9 *resolved = nullptr, *sys = nullptr;
        if (d.MultiSampleType != D3DMULTISAMPLE_NONE) {
            if (FAILED(d9->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)))
                return;
            d9->StretchRect(bb, nullptr, resolved, nullptr, D3DTEXF_NONE);
        }
        bool ok = SUCCEEDED(d9->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
                  SUCCEEDED(d9->GetRenderTargetData(resolved ? resolved : bb, sys));
        D3DLOCKED_RECT lr;
        if (ok && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            if (FILE* f = fopen(path, "wb")) {
                BITMAPINFOHEADER bih{sizeof bih, static_cast<LONG>(d.Width), -static_cast<LONG>(d.Height), 1, 32};
                BITMAPFILEHEADER bfh{0x4D42, 0, 0, 0, sizeof bfh + sizeof bih};
                bfh.bfSize = bfh.bfOffBits + d.Width * d.Height * 4;
                fwrite(&bfh, sizeof bfh, 1, f);
                fwrite(&bih, sizeof bih, 1, f);
                for (UINT y = 0; y < d.Height; ++y) fwrite(static_cast<BYTE*>(lr.pBits) + y * lr.Pitch, 4, d.Width, f);
                fclose(f);
            }
            sys->UnlockRect();
        }
        SafeRelease(sys);
        SafeRelease(resolved);
    }
    unsigned presents_ = 0;
#endif

    void Destroy() {
        if (g_current == this) g_current = nullptr;
        delete presenter_;
        presenter_ = nullptr;
        SafeRelease(realBackBuffer_);
        for (Surface8* s : backBuffers_)
            if (s) s->Destroy();
        if (depth_) depth_->Destroy();
        for (VertexShader* s : shaders_)
            if (s) {
                SafeRelease(s->vs);
                SafeRelease(s->decl);
                delete s;
            }
        for (PixelShader* s : pixelShaders_)
            if (s) {
                SafeRelease(s->ps);
                delete s;
            }
        d9->Release();
        parent_->Release();
        delete this;
    }

    LONG refs_ = 1;
    Direct3D8* parent_;
    HWND gameWindow_;  // Renderer 3: the window the game meant (D3D9 got a hidden one), else nullptr
    render::Presenter* presenter_ = nullptr;
    IDirect3DSurface9* realBackBuffer_ = nullptr;  // HDR: the D3D9 back buffer while the game draws into the float target
    bool pretransformed_ = false;  // current FVF is XYZRHW (2D)
    bool autoDepth_ = false;
    std::vector<Surface8*> backBuffers_;
    Surface8* depth_ = nullptr;
    Surface8* rt_ = nullptr;
    Surface8* ds_ = nullptr;
    BaseTexture8* textures_[kStages] = {};
    Stream streams_[kStreams];
    IndexBuffer8* indices_ = nullptr;
    UINT baseVertex_ = 0;
    DWORD vsHandle_ = 0;
    DWORD zbias_ = 0;
    std::vector<VertexShader*> shaders_ = {nullptr};  // index 0 unused: handle = index * 2 + 1
    std::vector<PixelShader*> pixelShaders_ = {nullptr};  // same scheme (D3D8 pixel shader handle 0 = none)
    DWORD psHandle_ = 0;
};

// ---- out-of-line members (need the complete classes) ----

Surface8::Surface8(Device8* dev, IDirect3DSurface9* s9, void* container) : dev(dev), s9(s9), container(container) {
    if (!container) dev->Ref();
}
void Surface8::Destroy() {
    SafeRelease(s9);
    delete this;
}
ULONG Surface8::AddRef() {
    if (container == dev) {
        dev->Ref();
        return 1;
    }
    if (container) return VAddRef(container);
    return static_cast<ULONG>(InterlockedIncrement(&refs));
}
ULONG Surface8::Release() {
    if (container == dev) {
        dev->Unref();
        return 1;
    }
    if (container) return VRelease(container);
    LONG n = InterlockedDecrement(&refs);
    if (n == 0) {
        Device8* d = dev;
        d->Forget(this);
        Destroy();
        d->Unref();
    }
    return static_cast<ULONG>(n);
}
HRESULT Surface8::GetDevice(Device8** out) {
    if (!out) return D3DERR_INVALIDCALL;
    *out = dev;
    dev->AddRef();
    return S_OK;
}
HRESULT Surface8::GetContainer(REFIID, void** out) {
    if (!out) return D3DERR_INVALIDCALL;
    *out = container ? container : this;
    if (container == dev) dev->AddRef();
    else VAddRef(*out);
    return S_OK;
}

HRESULT Volume8::GetDevice(Device8** out) {
    if (!out) return D3DERR_INVALIDCALL;
    *out = dev;
    dev->AddRef();
    return S_OK;
}

Resource8::Resource8(Device8* dev, IDirect3DResource9* r9, const IID& iid, void (*destroy)(Resource8*))
    : dev(dev), r9(r9), iid_(iid), destroy_(destroy) {
    dev->Ref();
}
HRESULT Resource8::GetDevice(Device8** out) {
    if (!out) return D3DERR_INVALIDCALL;
    *out = dev;
    dev->AddRef();
    return S_OK;
}
void Resource8::Finish() {
    Device8* d = dev;
    d->Forget(this);
    SafeRelease(r9);
    d->Unref();
}

SwapChain8::SwapChain8(Device8* dev, IDirect3DSwapChain9* sc9) : dev(dev), sc9(sc9) { dev->Ref(); }
ULONG SwapChain8::Release() {
    LONG n = InterlockedDecrement(&refs_);
    if (n == 0) {
        for (Surface8* s : buffers_)
            if (s) {
                dev->Forget(s);
                s->Destroy();
            }
        sc9->Release();
        Device8* d = dev;
        delete this;
        d->Unref();
    }
    return static_cast<ULONG>(n);
}

HRESULT Direct3D8::CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags, d3d8::PresentParameters* pp,
                                Device8** out) {
    if (!pp || !out) return D3DERR_INVALIDCALL;
    *out = nullptr;
    D3DPRESENT_PARAMETERS q = ToPresent9(*pp);
    HWND gameWindow = nullptr;
    if (queue12) {
        gameWindow = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
        ToHiddenWindow(q);  // only the device window: the focus window stays the game's (D3D9 may activate the focus
                            // window - the hidden one took the keyboard from the game)
    }
    // No PUREDEVICE: the plugin's overlay reads render / stage states back (Get* is invalid on a pure device).
    DWORD flags9 = flags & ~D3DCREATE_PUREDEVICE;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d9->CreateDevice(adapter, type, focus, flags9, &q, &dev);
    dslog::Write("D3D8on9: CreateDevice(adapter %u, type %u, flags 0x%X -> 0x%X, %ux%u fmt %u, %s, ms %u, interval 0x%X) "
                 "= 0x%08X",
                 adapter, type, flags, flags9, q.BackBufferWidth, q.BackBufferHeight, q.BackBufferFormat,
                 q.Windowed ? "windowed" : "fullscreen", q.MultiSampleType, q.PresentationInterval,
                 static_cast<unsigned>(hr));
    if (FAILED(hr)) return hr;
    BackToPresent8(q, pp);
    IUnknown* on12 = nullptr;  // IDirect3DDevice9On12: the device really runs on D3D12
    if (SUCCEEDED(dev->QueryInterface(kIidDevice9On12, reinterpret_cast<void**>(&on12)))) {
        dslog::Write("D3D8on9: device runs on Direct3D 12 (9On12)");
        on12->Release();
    }
    *out = new Device8(this, dev, q, gameWindow);
    g_current = *out;
    return hr;
}

HRESULT Device8::CreateVertexShader(const DWORD* decl, const DWORD* function, DWORD* handle, DWORD) {
    if (!decl || !handle) return D3DERR_INVALIDCALL;
    static const WORD kSize[8] = {4, 8, 12, 16, 4, 4, 4, 8};
    auto* s = new VertexShader;
    std::vector<D3DVERTEXELEMENT9> elements;
    std::vector<DWORD> inputs;
    WORD stream = 0, offset = 0;
    for (const DWORD* t = decl;; ++t) {
        DWORD tok = *t;
        s->declTokens.push_back(tok);
        if (tok == d3d8::kVsdEnd) break;
        switch (tok >> 29) {
            case d3d8::kVsdStream:
                stream = static_cast<WORD>(tok & 0xF);
                offset = 0;
                break;
            case d3d8::kVsdStreamData:
                if (tok & 0x10000000) {  // skip n dwords
                    offset = static_cast<WORD>(offset + ((tok >> 16) & 0xF) * 4);
                } else {
                    DWORD type = (tok >> 16) & 0xF;
                    D3DVERTEXELEMENT9 e{stream, offset, static_cast<BYTE>(type), D3DDECLMETHOD_DEFAULT, 0, 0};
                    if (type < 8 && render::UsageOfRegister(tok & 0x1F, e.Usage, e.UsageIndex)) {
                        elements.push_back(e);
                        inputs.push_back(tok & 0x1F);
                    }
                    offset = static_cast<WORD>(offset + (type < 8 ? kSize[type] : 0));
                }
                break;
            case d3d8::kVsdConstMem: {  // constants for programmable shaders
                DWORD n = ((tok >> 25) & 0xF) * 4;
                VertexShader::Constants c{tok & 0x7F, {}};
                for (DWORD i = 0; i < n; ++i) {
                    s->declTokens.push_back(*++t);
                    float f;
                    memcpy(&f, t, 4);
                    c.values.push_back(f);
                }
                if (n) s->constants.push_back(std::move(c));
                break;
            }
            case d3d8::kVsdExt: {
                DWORD n = (tok >> 24) & 0x1F;
                for (DWORD i = 0; i < n; ++i) s->declTokens.push_back(*++t);
                break;
            }
            default:
                break;
        }
    }
    elements.push_back(D3DDECL_END());
    HRESULT hr = d9->CreateVertexDeclaration(elements.data(), &s->decl);
    if (FAILED(hr)) {
        dslog::Write("D3D8on9: vertex declaration refused (0x%08X)", static_cast<unsigned>(hr));
        delete s;
        return hr;
    }
    if (function) {
        std::string note;
        std::vector<DWORD> code = render::ConvertShader8(function, inputs, note);
        s->function.assign(function, function + render::ShaderLength(function));
        hr = code.empty() ? D3DERR_INVALIDCALL : d9->CreateVertexShader(code.data(), &s->vs);
        unsigned n = static_cast<unsigned>(shaders_.size());
        dslog::Write("D3D8on9: vertex shader %u: %s, %u inputs -> 0x%08X", n, note.c_str(),
                     static_cast<unsigned>(inputs.size()), static_cast<unsigned>(hr));
        if (FAILED(hr)) {
            dslog::Write("D3D8on9:   validator: %s", render::ValidateShader9(code).c_str());
            DumpShader("vs", n, function, code);
            SafeRelease(s->decl);
            delete s;
            return hr;
        }
    }
    shaders_.push_back(s);
    *handle = static_cast<DWORD>((shaders_.size() - 1) * 2 + 1);
    return S_OK;
}

HRESULT Device8::CreatePixelShader(const DWORD* function, DWORD* handle) {
    if (!function || !handle) return D3DERR_INVALIDCALL;
    std::string note;
    std::vector<DWORD> code = render::ConvertShader8(function, {}, note);
    auto* s = new PixelShader;
    s->function.assign(function, function + render::ShaderLength(function));
    HRESULT hr = code.empty() ? D3DERR_INVALIDCALL : d9->CreatePixelShader(code.data(), &s->ps);
    unsigned n = static_cast<unsigned>(pixelShaders_.size());
    dslog::Write("D3D8on9: pixel shader %u: %s -> 0x%08X", n, note.c_str(), static_cast<unsigned>(hr));
    if (FAILED(hr)) {
        dslog::Write("D3D8on9:   validator: %s", render::ValidateShader9(code).c_str());
        DumpShader("ps", n, function, code);
        delete s;
        return hr;
    }
    pixelShaders_.push_back(s);
    *handle = static_cast<DWORD>((pixelShaders_.size() - 1) * 2 + 1);
    return S_OK;
}

HRESULT Device8::CopyRects(void* src, const RECT* rects, UINT count, void* dst, const POINT* points) {
    Surface8* s = Unwrap<Surface8>(src);
    Surface8* d = Unwrap<Surface8>(dst);
    if (!s || !d || !s->s9 || !d->s9 || s == d) return D3DERR_INVALIDCALL;
    D3DSURFACE_DESC sd, dd;
    s->s9->GetDesc(&sd);
    d->s9->GetDesc(&dd);
    if (sd.Format != dd.Format) return D3DERR_INVALIDCALL;
    RECT whole{0, 0, static_cast<LONG>(sd.Width), static_cast<LONG>(sd.Height)};
    if (!count) {
        rects = &whole;
        count = 1;
        points = nullptr;
    }
    for (UINT i = 0; i < count; ++i) {
        RECT r = rects[i];
        POINT p = points ? points[i] : POINT{r.left, r.top};
        RECT dr{p.x, p.y, p.x + (r.right - r.left), p.y + (r.bottom - r.top)};
        HRESULT hr = E_FAIL;
        if (sd.Pool == D3DPOOL_SYSTEMMEM && dd.Pool == D3DPOOL_DEFAULT) {
            hr = d9->UpdateSurface(s->s9, &r, d->s9, &p);
        } else if (sd.Pool == D3DPOOL_DEFAULT && dd.Pool == D3DPOOL_DEFAULT) {
            hr = d9->StretchRect(s->s9, &r, d->s9, &dr, D3DTEXF_NONE);
        } else if (sd.Pool == D3DPOOL_DEFAULT && dd.Pool == D3DPOOL_SYSTEMMEM && (sd.Usage & D3DUSAGE_RENDERTARGET) &&
                   sd.Width == dd.Width && sd.Height == dd.Height) {
            hr = d9->GetRenderTargetData(s->s9, d->s9);
        }
        if (SUCCEEDED(hr)) continue;
        // CPU copy (managed / system memory, or the fast paths refused).
        D3DLOCKED_RECT sl, dl;
        if (FAILED(s->s9->LockRect(&sl, &r, D3DLOCK_READONLY))) return D3DERR_INVALIDCALL;
        if (FAILED(d->s9->LockRect(&dl, &dr, 0))) {
            s->s9->UnlockRect();
            return D3DERR_INVALIDCALL;
        }
        UINT w = r.right - r.left, h = r.bottom - r.top;
        UINT rows = IsBlock(sd.Format) ? (h + 3) / 4 : h;
        UINT rowBytes = SurfaceBytes(sd.Format, w, IsBlock(sd.Format) ? 4 : 1);
        for (UINT y = 0; y < rows; ++y)
            memcpy(static_cast<BYTE*>(dl.pBits) + y * dl.Pitch, static_cast<const BYTE*>(sl.pBits) + y * sl.Pitch, rowBytes);
        d->s9->UnlockRect();
        s->s9->UnlockRect();
    }
    return S_OK;
}

}  // namespace

IDirect3DDevice9* render::Device9For(void* dev8) {
    return g_current && dev8 == static_cast<void*>(g_current) ? g_current->d9 : nullptr;
}

IDirect3DTexture9* render::Texture9For(void* tex8) {
    if (!g_current || !tex8) return nullptr;
    auto* t = static_cast<Texture8*>(tex8);
    return t->magic == kMagic ? t->t9 : nullptr;
}

void render::NoteHudStart() {
    if (g_current) g_current->HudStart();
}

void* render::CreateDirect3D8On9(UINT sdkVersion, bool on12, bool present) {
    HMODULE lib = LoadLibraryA("d3d9.dll");
    IDirect3D9* d9 = nullptr;
    IUnknown *device12 = nullptr, *queue12 = nullptr;
    const char* kind = "Direct3D 9";
    if (on12 && lib) {
        auto create12 = reinterpret_cast<PFN_Direct3DCreate9On12>(GetProcAddress(lib, "Direct3DCreate9On12"));
        D3D9ON12_ARGS args{};
        args.Enable9On12 = TRUE;  // without a device / queue 9On12 makes its own D3D12 device
        if (present && render::CreateD3D12(&device12, &queue12)) {
            args.pD3D12Device = device12;
            args.ppD3D12Queues[0] = queue12;
            args.NumQueues = 1;
        }
        d9 = create12 ? create12(D3D_SDK_VERSION, &args, 1) : nullptr;
        if (d9) kind = queue12 ? "Direct3D 9 on 12 + own presenter" : "Direct3D 9 on 12";
        else dslog::Write("[fail] D3D8on9: Direct3DCreate9On12 unavailable - plain Direct3D 9");
    }
    if (!d9) {
        if (queue12) queue12->Release();
        if (device12) device12->Release();
        queue12 = device12 = nullptr;
        auto create = lib ? reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(lib, "Direct3DCreate9")) : nullptr;
        d9 = create ? create(D3D_SDK_VERSION) : nullptr;
    }
    if (!d9) {
        dslog::Write("[fail] D3D8on9: Direct3DCreate9 unavailable - using the game's Direct3D 8");
        return nullptr;
    }
    auto* d3d = new Direct3D8(d9, device12, queue12);
    dslog::Write("D3D8on9: %s renderer active (game asked for SDK %u; %u adapter(s), %u mode(s) on adapter 0)", kind,
                 sdkVersion, d3d->GetAdapterCount(), d3d->GetAdapterModeCount(0));
    return d3d;
}
