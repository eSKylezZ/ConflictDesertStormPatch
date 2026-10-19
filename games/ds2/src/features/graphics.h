#pragma once
#include <d3d9.h>

// Direct3D 9 hooks shared by the features (graphics.cpp): the exe's Direct3DCreate9 import slot (0x5DA2C4, called
// through the thunk at 0x566570) is wrapped, then IDirect3D9::CreateDevice and the device's methods.
namespace graphics {
void Install();

// dev background mode (devbg.cpp): present parameters before CreateDevice / Reset, and each Present
extern void (*g_adjustPresentParams)(D3DPRESENT_PARAMETERS* pp);
extern void (*g_onPresent)(IDirect3DDevice9* dev);
// after CreateDevice succeeded (front-end: the game sets its first viewport / projection right after it)
extern void (*g_onDeviceCreated)(int backBufferW, int backBufferH);

// Virtual screen (front-end, frontend.cpp): the game lays out and draws a 1280 x 1024 picture (the only size where its
// 3D monitor frames the pixel-sized menus), mapped onto the real back buffer (realW x realH) scaled by
// min(realW/1280, realH/1024) and centred - pre-transformed (XYZRHW) vertices of
// the user-pointer draws, viewports, scissor rectangles and clears. Reset keeps the real back-buffer size.
// The device's current viewport follows: a full real-screen one becomes the full mapped one and back.
constexpr int kVirtualW = 1280, kVirtualH = 1024;
void SetVirtualScreen(bool on, int realW = 0, int realH = 0);
bool VirtualScreenOn();
// a back-buffer pixel position -> the virtual screen's (inverse of the mapping)
void RealToVirtual(long& x, long& y);
// the real back-buffer size while the virtual screen is on
int RealWidth();
// the virtual screen's scale onto the back buffer (1 while it is off), and the game's device
float VirtualScale();
IDirect3DDevice9* Device();
int RealHeight();
}  // namespace graphics
