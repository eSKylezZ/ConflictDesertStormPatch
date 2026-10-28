/* dsfix.overlay v1 - DesertStormFix's overlay drawing (features/overlay.h) for its own plugin DLLs.
 *
 * Published by dsfix.dll (features/overlay_iface.cpp) when it loads, before the other plugins. Plain C: every DLL has
 * its own static C runtime, so nothing here passes C++ objects or memory one side frees for the other. A plugin on the
 * dsfix kit doesn't use this table directly: the kit's overlay client (plugins/kit/overlay_client.cpp) implements the
 * functions of features/overlay.h on it, so a feature's sources keep calling overlay::QueueRect(...) as before.
 *
 * Coordinates are back-buffer pixels, colours 0xAARRGGBB, textures IDirect3DTexture8* on the game's device. Late
 * draws are called on the game thread at the end of the frame (see overlay.h). Texture loaders return managed
 * A8R8G8B8 textures the caller releases with ReleaseTexture (cached ones excepted - see overlay.h). */
#ifndef DSFIX_OVERLAY_H
#define DSFIX_OVERLAY_H

#include <stddef.h>
#include <stdint.h>

#include "pse/pse_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSFIX_OVERLAY_INTERFACE "dsfix.overlay"

typedef void (*DsfixLateDraw)(void);

typedef struct DsfixOverlayV1 {
    uint32_t size;      /* sizeof(DsfixOverlayV1) of the publishing dsfix.dll */
    uint32_t version;   /* 1 */
    /* overlay::QueueIcon .. QueueLine (icon = gen::GameIcon; alpha < 0 = the menus' fade) */
    void (PSE_CALL* QueueIcon)(int icon, float x, float y, float size, float alpha);
    void (PSE_CALL* QueueImage)(void* texture, float x, float y, float w, float h, float u1, float v1, float alpha);
    void (PSE_CALL* QueueScreenImage)(void* texture, float x, float y, float w, float h, float u1, float v1);
    /* corners: top-left, top-right, bottom-left, bottom-right as x, y pairs (8 floats) */
    void (PSE_CALL* QueueScreenQuad)(void* texture, const float* corners, float u0, float v0, float u1, float v1,
                                     uint32_t argb);
    void (PSE_CALL* QueueScreenRect)(float x, float y, float w, float h, uint32_t argb);
    void (PSE_CALL* QueueRect)(float x, float y, float w, float h, uint32_t argb);
    void (PSE_CALL* QueueLine)(float x0, float y0, float x1, float y1, float width, uint32_t argb);
    /* An encoded image (PNG ...) as a texW x texH texture (image top-left); NULL on failure. `what` names it in the log. */
    void* (PSE_CALL* LoadTextureImage)(const void* bytes, size_t size, int texW, int texH, const char* what);
    /* overlay::LoadTextureFile / LoadTextureBytes: sizes go out through the pointers (none may be NULL) */
    void* (PSE_CALL* LoadTextureFile)(const char* path, int* w, int* h, int* texW, int* texH);
    void* (PSE_CALL* LoadTextureBytes)(const void* bytes, size_t size, int* w, int* h, int* texW, int* texH,
                                       const char* what);
    const void* (PSE_CALL* DrawingSheet)(void);
    int (PSE_CALL* DrawingImage)(void);
    void* (PSE_CALL* CreateTexture)(int texW, int texH);
    int (PSE_CALL* UploadTexture)(void* texture, const uint32_t* pixels, int w, int h);
    void (PSE_CALL* ReleaseTexture)(void* texture);
    void* (PSE_CALL* CurrentDevice)(void);
    float (PSE_CALL* MenuAlpha)(void);
    /* the one replaceable late draw / the permanent ones (up to 32) / the top draw - as overlay.h */
    void (PSE_CALL* SetLateDraw)(DsfixLateDraw fn);
    void (PSE_CALL* AddLateDraw)(DsfixLateDraw fn);
    void (PSE_CALL* SetTopDraw)(DsfixLateDraw fn);
    void (PSE_CALL* Flush)(void);
    /* rects: count x {x, y, w, h} */
    void (PSE_CALL* FillRects)(const float* rects, int count, uint32_t argb);
    const char* (PSE_CALL* IconChar)(int icon);
    int (PSE_CALL* GlyphsReady)(void);
    void* (PSE_CALL* MenuFont)(void);
    int (PSE_CALL* TextWidth)(void* font, const char* text);
    void (PSE_CALL* DrawLabel)(void* font, const char* text, int x, int y, uint32_t argb);
} DsfixOverlayV1;

#ifdef __cplusplus
}
#endif
#endif
