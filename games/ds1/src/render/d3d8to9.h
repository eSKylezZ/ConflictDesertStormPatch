#pragma once
// Direct3D 8 implemented on Direct3D 9 (renderer setting 1) - step 1 of the modern-renderer plan (TODO.md "D3D9
// wrapper design"). The objects returned have the exact D3D8 vtable layout, so the game, the statically linked D3DX8
// and the plugin's own D3D8 code (graphics.cpp hooks, overlay, fontsharp, ...) use them unchanged.
#include <windows.h>

namespace render {
// An IDirect3D8 on top of IDirect3D9, or nullptr (d3d9.dll missing / failed) - then use the system's D3D8.
// on12: the IDirect3D9 comes from Direct3DCreate9On12 (Microsoft's D3D9-on-D3D12 layer); plain D3D9 if unavailable.
// present: our own D3D12 device + DXGI flip swap chain present the frames (presenter.h; needs on12).
void* CreateDirect3D8On9(UINT sdkVersion, bool on12, bool present);

// The game starts drawing a view's HUD (splitscreen.cpp's reticle wrapper, once per view and frame): the presenter
// copies the scene of the current viewport (HDR / bloom tell scene pixels from HUD pixels with it).
void NoteHudStart();
}  // namespace render
