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
// Like QueueImage but in plain back-buffer pixels: no menu transition / fade, no split-screen view offset.
void QueueScreenImage(void* texture, float x, float y, float w, float h, float u1 = 1.0f, float v1 = 1.0f);
// A solid rectangle in plain back-buffer pixels (no menu fade, no split-screen offset).
void QueueScreenRect(float x, float y, float w, float h, uint32_t argb);
// Solid shapes (colour 0xAARRGGBB; the alpha is multiplied by the menus' fade).
void QueueRect(float x, float y, float w, float h, uint32_t argb);
void QueueLine(float x0, float y0, float x1, float y1, float width, uint32_t argb);
// A PNG from this DLL's RCDATA `resource` as a texture of texW x texH (image top-left), cached per device.
void* LoadTexture(int resource, int texW, int texH);
// An image file (PNG, DDS ...) as a managed A8R8G8B8 texture, sized up to powers of two (image at the top-left).
void* LoadTextureFile(const char* path, int& w, int& h, int& texW, int& texH);
// The image FUN_0053c9e0 is drawing right now (sheet, image number) - for its page texture bind (fontsharp.cpp).
const void* DrawingSheet();
int DrawingImage();
// A managed A8R8G8B8 texture on the game's current device, and a copy of w x h BGRA pixels (rows of w) into it.
void* CreateTexture(int texW, int texH);
bool UploadTexture(void* texture, const uint32_t* pixels, int w, int h);
void ReleaseTexture(void* texture);
void* CurrentDevice();
// Alpha multiplier the game's menu art is drawn with right now (image sheet [0x60EE18] +0x38; menu fades change it).
float MenuAlpha();
// A function drawn at the very end of the frame, after the queue (game-font text over our shapes); null = none.
using LateDraw = void (*)();
void SetLateDraw(LateDraw fn);
// Another late draw that stays registered (up to 4; the function itself decides each frame whether to draw).
void AddLateDraw(LateDraw fn);
// Draws the queued icons (called before the game's EndScene) and empties the queue.
void Render(void* device);
// Draws solid rectangles {x, y, w, h} (back-buffer pixels) right now, over whatever is drawn so far this frame.
void FillRects(const float (*rects)[4], int count, uint32_t argb);

// Installs the glyph hooks (once, before the game starts) and keeps the fonts' icon glyphs in place (every frame).
bool Install();
void SetImageProbe(void* probe);  // dev HUD probe: stdcall(caller, sheet, image, x, y) on every image draw
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
