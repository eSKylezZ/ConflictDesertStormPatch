/* ds2fix.graphics (version 1): DesertStorm2Fix's Direct3D 9 state for other DS2 plugins (games/ds2/plugins/<name>).
 *
 * Published by the host plugin ds2fix (src/pse/ds2fix.cpp) through pse.messaging RegisterInterface while it loads, so
 * any plugin loaded after it finds it in its own PSEPlugin_Load (manifest: `requires = ds2fix >= 0.1`):
 *
 *   const PSEMessagingInterface* m = (const PSEMessagingInterface*)pse->Query(pse->plugin, PSE_MESSAGING_INTERFACE, 1);
 *   const Ds2FixGraphicsV1* gfx = (const Ds2FixGraphicsV1*)m->QueryInterface(pse->plugin, DS2FIX_GRAPHICS_INTERFACE, 1);
 *   if (!gfx || gfx->size < sizeof(Ds2FixGraphicsV1)) return 0;
 *
 * The functions are graphics.h's (games/ds2/src/features/graphics.h): the front end's "virtual screen" (the game lays
 * out a 1280 x 1024 picture, ds2fix maps it onto the real back buffer, scaled + centred) and the game's device. Game
 * thread only. For "the device was created / reset" use the layer's PSE_MSG_DEVICE_CREATED / _RESET messages
 * (docs/sdk/reference/game-ds2.md). Docs: game-ds2.md "DS2 plugins". */
#pragma once
#include <stdint.h>

#include "pse/pse_api.h"

#define DS2FIX_GRAPHICS_INTERFACE "ds2fix.graphics"
#define DS2FIX_VIRTUAL_W 1280
#define DS2FIX_VIRTUAL_H 1024

typedef struct Ds2FixGraphicsV1 {
    uint32_t size;
    uint32_t version; /* 1 */
    /* on != 0: map the virtual 1280 x 1024 screen onto a realW x realH back buffer; 0 = off. */
    void (PSE_CALL* SetVirtualScreen)(int on, int realW, int realH);
    int (PSE_CALL* VirtualScreenOn)(void);
    /* a back-buffer pixel position -> the virtual screen's (in place; unchanged while the virtual screen is off) */
    void (PSE_CALL* RealToVirtual)(long* x, long* y);
    int (PSE_CALL* RealWidth)(void);   /* the real back-buffer size while the virtual screen is on */
    int (PSE_CALL* RealHeight)(void);
    float (PSE_CALL* VirtualScale)(void); /* the virtual screen's scale onto the back buffer (1 while it is off) */
    void* (PSE_CALL* Device)(void);       /* the game's IDirect3DDevice9*, NULL before CreateDevice */
} Ds2FixGraphicsV1;
