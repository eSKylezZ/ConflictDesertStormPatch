#pragma once
#include <d3d9.h>

// Direct3D 9 hooks shared by the features (graphics.cpp): the exe's Direct3DCreate9 import slot (0x69F33C, called
// through the thunk at 0x566570) is wrapped, then IDirect3D9::CreateDevice and the device's methods.
namespace graphics {
void Install();
void Rehook();   // per frame: puts the device vtable hooks back (the runtime rewrites its per-device vtable)

// dev background mode (devbg.cpp): present parameters before CreateDevice / Reset, and each Present
extern void (*g_adjustPresentParams)(D3DPRESENT_PARAMETERS* pp);
extern void (*g_onPresent)(IDirect3DDevice9* dev);
// after CreateDevice succeeded (front-end: the game sets its first viewport / projection right after it)
extern void (*g_onDeviceCreated)(int backBufferW, int backBufferH);
extern bool g_retryLostCreate;   // dev background mode: CreateDevice retried while it says D3DERR_DEVICELOST

// Virtual screen (front end / loading screens, frontend.cpp): the game lays out and draws a virtW x virtH picture (it is
// told the screen has that size), mapped onto the real back buffer (realW x realH) scaled by min(realW/virtW,
// realH/virtH) and centred - pre-transformed (XYZRHW) vertices of the user-pointer draws, viewports, scissor rectangles
// and clears. Reset keeps the real back-buffer size.
void SetVirtualScreen(bool on, int realW = 0, int realH = 0, int virtW = 800, int virtH = 600);
bool VirtualScreenOn();
}  // namespace graphics
