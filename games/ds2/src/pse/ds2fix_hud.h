/* ds2fix.hudhost (version 1): what DesertStorm2Fix (the host plugin ds2fix) offers the HUD / widescreen plugin
 * ds2fix.widescreen (games/ds2/plugins/widescreen) - the parts of ds2fix the HUD scale depends on, and the OPTIONS menu
 * rows that show its settings.
 *
 * Published by ds2fix (src/pse/ds2fix.cpp) through pse.messaging RegisterInterface while it loads; a plugin loaded after
 * it finds it in its own PSEPlugin_Load (manifest: `requires = ds2fix >= 0.1`):
 *
 *   const Ds2FixHudHostV1* host = (const Ds2FixHudHostV1*)m->QueryInterface(pse->plugin, DS2FIX_HUDHOST_INTERFACE, 1);
 *
 * ds2fix runs without that plugin (no HUD scaling, no OPTIONS rows); the plugin needs ds2fix (the front end's virtual
 * screen is ds2fix.graphics). Game thread only. Docs: docs/sdk/reference/game-ds2.md "DS2 plugins". */
#pragma once
#include <stdint.h>

#include "pse/pse_api.h"

#define DS2FIX_HUDHOST_INTERFACE "ds2fix.hudhost"

/* The OPTIONS rows HUD SCALING (on / off) and HUD SIZE (25 - 400 %, steps of 25): the plugin's values, read when the
 * screen is built and changed by the player (set = apply + save at once). */
typedef struct Ds2FixHudOptionV1 {
    int (PSE_CALL* Scaling)(void);
    uint32_t (PSE_CALL* Percent)(void);
    void (PSE_CALL* Set)(int on, uint32_t percent);
} Ds2FixHudOptionV1;

typedef struct Ds2FixHudHostV1 {
    uint32_t size;
    uint32_t version; /* 1 */
    /* split screen: *w / *h become the smallest view's size (the HUD art is sized per view); unchanged otherwise */
    void (PSE_CALL* SplitViewSize)(int* w, int* h);
    /* shows the OPTIONS rows for these values (the table must stay valid; NULL = rows hidden again) */
    void (PSE_CALL* SetHudOption)(const Ds2FixHudOptionV1* option);
} Ds2FixHudHostV1;
