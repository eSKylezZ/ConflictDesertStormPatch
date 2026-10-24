// Interface dsfix.overlay v1 (core/dsfix_overlay.h): overlay.h for DesertStormFix's own plugin DLLs, which use it
// through the kit's overlay client (plugins/kit/overlay_client.cpp). Registered next to dsfix.services.
#include <windows.h>

#include "core/dsfix_overlay.h"
#include "core/log.h"
#include "core/pse_host.h"
#include "features/features.h"
#include "features/overlay.h"
#include "loader.h"

namespace {
using overlay::Icon;

void PSE_CALL QueueIcon(int icon, float x, float y, float size, float alpha) {
    overlay::QueueIcon(static_cast<Icon>(icon), x, y, size, alpha);
}
void PSE_CALL QueueImage(void* t, float x, float y, float w, float h, float u1, float v1, float alpha) {
    overlay::QueueImage(t, x, y, w, h, u1, v1, alpha);
}
void PSE_CALL QueueScreenImage(void* t, float x, float y, float w, float h, float u1, float v1) {
    overlay::QueueScreenImage(t, x, y, w, h, u1, v1);
}
void PSE_CALL QueueScreenQuad(void* t, const float* c, float u0, float v0, float u1, float v1, uint32_t argb) {
    if (c) overlay::QueueScreenQuad(t, *reinterpret_cast<const float(*)[4][2]>(c), u0, v0, u1, v1, argb);
}
void PSE_CALL QueueScreenRect(float x, float y, float w, float h, uint32_t argb) { overlay::QueueScreenRect(x, y, w, h, argb); }
void PSE_CALL QueueRect(float x, float y, float w, float h, uint32_t argb) { overlay::QueueRect(x, y, w, h, argb); }
void PSE_CALL QueueLine(float x0, float y0, float x1, float y1, float width, uint32_t argb) {
    overlay::QueueLine(x0, y0, x1, y1, width, argb);
}
void* PSE_CALL LoadTextureImage(const void* b, size_t size, int texW, int texH, const char* what) {
    return b ? overlay::LoadTextureImage(b, size, texW, texH, what) : nullptr;
}
void* PSE_CALL LoadTextureFile(const char* path, int* w, int* h, int* texW, int* texH) {
    if (!path || !w || !h || !texW || !texH) return nullptr;
    return overlay::LoadTextureFile(path, *w, *h, *texW, *texH);
}
void* PSE_CALL LoadTextureBytes(const void* b, size_t size, int* w, int* h, int* texW, int* texH, const char* what) {
    if (!b || !w || !h || !texW || !texH) return nullptr;
    return overlay::LoadTextureBytes(b, size, *w, *h, *texW, *texH, what);
}
const void* PSE_CALL DrawingSheet() { return overlay::DrawingSheet(); }
int PSE_CALL DrawingImage() { return overlay::DrawingImage(); }
void* PSE_CALL CreateTexture(int texW, int texH) { return overlay::CreateTexture(texW, texH); }
int PSE_CALL UploadTexture(void* t, const uint32_t* px, int w, int h) { return overlay::UploadTexture(t, px, w, h) ? 1 : 0; }
void PSE_CALL ReleaseTexture(void* t) { overlay::ReleaseTexture(t); }
void* PSE_CALL CurrentDevice() { return overlay::CurrentDevice(); }
float PSE_CALL MenuAlpha() { return overlay::MenuAlpha(); }
void PSE_CALL SetLateDraw(DsfixLateDraw fn) { overlay::SetLateDraw(fn); }
void PSE_CALL AddLateDraw(DsfixLateDraw fn) { if (fn) overlay::AddLateDraw(fn); }
void PSE_CALL SetTopDraw(DsfixLateDraw fn) { overlay::SetTopDraw(fn); }
void PSE_CALL Flush() { overlay::Flush(); }
void PSE_CALL FillRects(const float* r, int count, uint32_t argb) {
    if (r && count > 0) overlay::FillRects(reinterpret_cast<const float(*)[4]>(r), count, argb);
}
const char* PSE_CALL IconChar(int icon) { return overlay::IconChar(static_cast<Icon>(icon)); }
int PSE_CALL GlyphsReady() { return overlay::GlyphsReady() ? 1 : 0; }
void* PSE_CALL MenuFont() { return overlay::MenuFont(); }
int PSE_CALL TextWidth(void* font, const char* text) { return text ? overlay::TextWidth(font, text) : 0; }
void PSE_CALL DrawLabel(void* font, const char* text, int x, int y, uint32_t argb) {
    if (text) overlay::DrawLabel(font, text, x, y, argb);
}

const DsfixOverlayV1 kOverlay = {sizeof(DsfixOverlayV1), 1, &QueueIcon, &QueueImage, &QueueScreenImage,
                                 &QueueScreenQuad, &QueueScreenRect, &QueueRect, &QueueLine, &LoadTextureImage,
                                 &LoadTextureFile, &LoadTextureBytes, &DrawingSheet, &DrawingImage, &CreateTexture,
                                 &UploadTexture, &ReleaseTexture, &CurrentDevice, &MenuAlpha, &SetLateDraw,
                                 &AddLateDraw, &SetTopDraw, &Flush, &FillRects, &IconChar, &GlyphsReady, &MenuFont,
                                 &TextWidth, &DrawLabel};
}  // namespace

void features::RegisterOverlayInterface() {
    static bool done = false;
    if (done) return;
    done = true;
    const PSEMessagingInterface* m = pse::MessagingInterface();
    const bool ok = m && m->RegisterInterface(pse_host::Core(), DSFIX_OVERLAY_INTERFACE, 1, &kOverlay) == PSE_OK;
    dslog::Write(ok ? "[ok]   " DSFIX_OVERLAY_INTERFACE " 1: registered" : "[fail] " DSFIX_OVERLAY_INTERFACE ": not registered");
}
