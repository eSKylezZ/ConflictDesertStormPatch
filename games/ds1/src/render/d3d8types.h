#pragma once
// The Direct3D 8 declarations the wrapper needs that differ from Direct3D 9 (the Windows SDK has no d3d8.h). Types
// that are identical in both versions (D3DFORMAT, D3DPOOL, D3DDISPLAYMODE, D3DVIEWPORT8/9, D3DLIGHT, D3DMATERIAL,
// D3DLOCKED_RECT, D3DVERTEXBUFFER_DESC, ...) are taken from d3d9.h.
#include <d3d9.h>

#include <cstddef>

namespace d3d8 {

struct PresentParameters {  // D3DPRESENT_PARAMETERS (8): no MultiSampleQuality, interval semantics differ
    UINT BackBufferWidth, BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    DWORD SwapEffect;  // 1 DISCARD, 2 FLIP, 3 COPY, 4 COPY_VSYNC
    HWND hDeviceWindow;
    BOOL Windowed, EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz, FullScreen_PresentationInterval;
};
constexpr DWORD kSwapEffectCopyVsync = 4;

struct SurfaceDesc {  // D3DSURFACE_DESC (8): has Size, no MultiSampleQuality
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    UINT Width, Height;
};

struct VolumeDesc {  // D3DVOLUME_DESC (8)
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
    UINT Width, Height, Depth;
};

struct AdapterIdentifier {  // D3DADAPTER_IDENTIFIER8: no DeviceName
    char Driver[512];
    char Description[512];
    LARGE_INTEGER DriverVersion;
    DWORD VendorId, DeviceId, SubSysId, Revision;
    GUID DeviceIdentifier;
    DWORD WHQLLevel;
};

// D3DCAPS8 is D3DCAPS9 up to (not including) DevCaps2, field for field.
constexpr size_t kCapsSize = offsetof(D3DCAPS9, DevCaps2);
static_assert(offsetof(D3DCAPS9, MaxAnisotropy) == 0x6C, "D3DCAPS8 layout");
constexpr DWORD kRasterCapsZBias = 0x4000;  // D3DPRASTERCAPS_ZBIAS

// Render states that exist only in D3D8 (or moved in D3D9).
enum : DWORD {
    kRsLinePattern = 10, kRsZVisible = 30, kRsEdgeAntialias = 40, kRsZBias = 47, kRsSoftwareVertexProcessing = 153,
    kRsPatchSegments = 164,
};

// Texture stage states that became sampler states in D3D9: D3D8 type -> D3DSAMPLERSTATETYPE (0 = stays a TSS).
inline D3DSAMPLERSTATETYPE SamplerOf(DWORD tss) {
    switch (tss) {
        case 13: return D3DSAMP_ADDRESSU;
        case 14: return D3DSAMP_ADDRESSV;
        case 15: return D3DSAMP_BORDERCOLOR;
        case 16: return D3DSAMP_MAGFILTER;
        case 17: return D3DSAMP_MINFILTER;
        case 18: return D3DSAMP_MIPFILTER;
        case 19: return D3DSAMP_MIPMAPLODBIAS;
        case 20: return D3DSAMP_MAXMIPLEVEL;
        case 21: return D3DSAMP_MAXANISOTROPY;
        case 25: return D3DSAMP_ADDRESSW;
        default: return static_cast<D3DSAMPLERSTATETYPE>(0);
    }
}

// Vertex shader declaration tokens (D3DVSD_*).
enum : DWORD { kVsdNop = 0, kVsdStream = 1, kVsdStreamData = 2, kVsdTessellator = 3, kVsdConstMem = 4, kVsdExt = 5 };
constexpr DWORD kVsdEnd = 0xFFFFFFFF;

constexpr D3DFORMAT kFormatW11V11U10 = static_cast<D3DFORMAT>(65);  // D3D8 only

}  // namespace d3d8
