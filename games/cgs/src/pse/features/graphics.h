#pragma once
#include <d3d9.h>

// Direct3D 9 hooks shared by the features (graphics.cpp): the exe's Direct3DCreate9 import slot (0x6C03B4, called
// through the thunk at 0x6B7918) is wrapped, then IDirect3D9::CreateDevice and the device's methods.
namespace graphics {
void Install();
void OnFrame();   // per game frame (frame.cpp): re-hooks the device if d3d9 swapped its vtable

// dev background mode (devbg.cpp): present parameters before CreateDevice / Reset, and each Present
extern void (*g_adjustPresentParams)(D3DPRESENT_PARAMETERS* pp);
extern void (*g_onPresent)(IDirect3DDevice9* dev);
// after CreateDevice succeeded (front-end: the game sets its first viewport / projection right after it)
extern void (*g_onDeviceCreated)(int backBufferW, int backBufferH);

// Virtual screen (front-end, frontend.cpp): the game lays out and draws an 800 x 600 picture, mapped onto the real
// back buffer (realW x realH) scaled by min(realW/800, realH/600) and centred - pre-transformed (XYZRHW) vertices of
// the user-pointer draws, viewports, scissor rectangles and clears. Reset keeps the real back-buffer size.
// The device's current viewport follows: a full real-screen one becomes the full mapped one and back.
// Menu squeeze: screen-space (XYZRHW) user-pointer draws get x' = centreX + (x - centreX) * s, per 4-vertex sprite;
// sprites spanning the full width (screenW) are left alone. s = 0 turns it off.
void SetMenuSqueeze(float s, float centreX = 0.0f, float screenW = 0.0f);

void SetVirtualScreen(bool on, int realW = 0, int realH = 0);
bool VirtualScreenOn();
}  // namespace graphics
