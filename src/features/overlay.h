#pragma once
#include <cstdint>

#include "generated/game_icons.h"

// Button icons drawn over the game (Kenney input prompts, src/generated/game_icons.*). Screens queue icons while the
// game renders; they are drawn at the end of the frame (EndScene, graphics.cpp) so they sit on top of the menus.
// Icons inside game text: IconChar() gives a one-character string for an icon; put it in any text the game draws
// with its fonts and the game measures, centres, wraps and draws it as a blank glyph of the icon's size, where the
// icon then appears (the fonts get extra glyphs, see overlay.cpp).
// Also thin wrappers around the game's own font code, so text next to the icons matches the menus.
namespace overlay {
using Icon = gen::GameIcon;

// Queues an icon for this frame: top-left corner and height in back-buffer pixels (icons are square). Its opacity
// follows the menus' current fade (alpha tint of the HUD sheet, see MenuAlpha) unless one is given.
void QueueIcon(Icon icon, float x, float y, float size, float alpha = -1.0f);
// A texture (e.g. LoadTexture) at x, y, w x h, showing texture coordinates 0..u1 / 0..v1.
void QueueImage(void* texture, float x, float y, float w, float h, float u1 = 1.0f, float v1 = 1.0f, float alpha = -1.0f);
// Solid shapes (colour 0xAARRGGBB; the alpha is multiplied by the menus' fade).
void QueueRect(float x, float y, float w, float h, uint32_t argb);
void QueueLine(float x0, float y0, float x1, float y1, float width, uint32_t argb);
// A PNG from this DLL's RCDATA `resource` as a texture of texW x texH (image top-left), cached per device.
void* LoadTexture(int resource, int texW, int texH);
// Alpha multiplier the game's menu art is drawn with right now (image sheet [0x60EE18] +0x38; menu fades change it).
float MenuAlpha();
// Draws the queued icons (called before the game's EndScene) and empties the queue.
void Render(void* device);
// Draws solid rectangles {x, y, w, h} (back-buffer pixels) right now, over whatever is drawn so far this frame.
void FillRects(const float (*rects)[4], int count, uint32_t argb);

// Installs the glyph hooks (once, before the game starts) and keeps the fonts' icon glyphs in place (every frame).
bool Install();
void OnFrame();
// A one-character string (a control character mapped to an icon glyph) for use inside game text; valid until the
// same character is reused for another icon (27 are in use at a time - far more than one screen shows).
const char* IconChar(Icon icon);
bool GlyphsReady();  // the fonts have the icon glyphs (else keep plain text)

// The game's fonts (0x60EDB4 small / menu lists, 0x60EDB8 large). Pixel sizes include the HUD scale.
void* MenuFont();
int TextWidth(void* font, const char* text);
void DrawLabel(void* font, const char* text, int x, int y, uint32_t argb);
}  // namespace overlay
