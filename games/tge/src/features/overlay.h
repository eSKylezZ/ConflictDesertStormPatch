#pragma once
#include <cstddef>
#include <cstdint>

#include "generated/game_icons.h"

// Shapes, pictures and text drawn over the game (port of DesertStormFix's overlay, games/ds1/src/features/overlay.h):
// queued while the game renders a frame, drawn right before Present (the layer's PSE_EVENT_PRESENT) with every touched
// device state put back. Button icons = Kenney input prompts (generated/game_icons.*, RCDATA 300); controller pictures
// RCDATA 301 / 302. Text: GDI-rendered textures (cached per string), so it doesn't depend on the game's fonts.
// Coordinates: back-buffer pixels.
namespace overlay {
using Icon = gen::GameIcon;

void QueueIcon(Icon icon, float x, float y, float size, float alpha = 1.0f);
void QueueImage(void* texture, float x, float y, float w, float h, float u1 = 1.0f, float v1 = 1.0f, float alpha = 1.0f);
void QueueRect(float x, float y, float w, float h, uint32_t argb);
void QueueLine(float x0, float y0, float x1, float y1, float width, uint32_t argb);
// Text in a GDI font `height` pixels tall at x, y (top-left; align 0 left, 1 centre, 2 right of x); returns its width.
float QueueText(const char* text, float x, float y, float height, uint32_t argb, int align = 0, bool bold = true);
float TextWidth(const char* text, float height, bool bold = true);
// A PNG from this DLL's RCDATA `resource` as a texW x texH texture (image top-left), cached per device.
void* LoadTexture(int resource, int texW, int texH);
// The back buffer's size.
void ScreenSize(float& w, float& h);
// A function drawn every frame after the queue (it queues and the queue is drawn right after it); up to 16.
using LateDraw = void (*)();
void AddLateDraw(LateDraw fn);
// Installs the EndScene subscriber (tgefix.cpp, once).
void Install();
}  // namespace overlay
