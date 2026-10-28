#pragma once
// Own presentation for the 9On12 renderer (Renderer = 3): the game draws into its D3D9 back buffer as always, but D3D9
// never presents. Present resolves the back buffer into our texture, unwraps its D3D12 resource and puts it into a
// DXGI flip-model swap chain on the game window (borderless, tearing allowed = VRR, instant Alt+Tab):
//   SDR: a copy into a B8G8R8A8 swap chain.
//   HDR (setting Hdr + the window's monitor in HDR mode): a pass (hdr.hlsl) into an FP16 scRGB swap chain - SDR white
//        at paper white, scene highlights stretched towards the peak, HUD / menus left at paper white.
#include <windows.h>

struct IDirect3DDevice9;
struct IDirect3DSurface9;
struct IUnknown;

namespace render {

// Our D3D12 device + direct queue for Direct3DCreate9On12 (both AddRef'd into the out pointers). False = unavailable.
bool CreateD3D12(IUnknown** device, IUnknown** queue);

// A hidden window for the D3D9 device, so D3D9 never owns (or loses) the game window.
HWND HiddenDeviceWindow();

class Presenter {
public:
    // window = the game window; w / h = the back buffer size.
    static Presenter* Create(IDirect3DDevice9* d9, IUnknown* queue, HWND window, UINT w, UINT h);
    ~Presenter();

    // vsync: every n-th refresh for the frame limit. loading: a level-load progress frame - never waits for the display
    // (the game presents ~1000 of them inside one frame) and at most one per 16 ms is shown.
    HRESULT Present(IDirect3DSurface9* backBuffer, bool vsync, bool loading);
    void BeforeReset();               // D3D9 default-pool objects go before a D3D9 Reset
    void AfterReset(UINT w, UINT h);  // resizes the swap chain to the new back buffer
    bool Tearing() const;
    void RequestShot(const char* path);  // dev: the next presented frame read back from D3D12 (.bmp; HDR: raw FP16)

    // HDR: the device calls SnapshotScene at each switch from 3D to pre-transformed (2D) drawing with the current
    // viewport - the scene copy that tells HUD pixels from scene pixels. Cheap no-op while HDR is off.
    bool WantsScene() const;
    // HDR output is on: the device gives the game an FP16 render target instead of the 8-bit back buffer, so
    // additive effects can go brighter than white.
    bool WantsFloatScene() const;
    void SnapshotScene(IDirect3DSurface9* backBuffer, const RECT& r);

    struct State;

private:
    State* s_ = nullptr;
};

}  // namespace render
