/* ds2fix.options (version 1): ON / OFF rows in the game's OPTIONS screen for DS2 plugins' settings.
 *
 * Published by the host plugin ds2fix (src/pse/ds2fix.cpp, owner ds2fix.menus) through pse.messaging RegisterInterface
 * while it loads; a plugin loaded after it finds it in its own PSEPlugin_Load (manifest `requires = ds2fix >= 0.1`):
 *
 *   const Ds2FixOptionsV1* opt = (const Ds2FixOptionsV1*)m->QueryInterface(pse->plugin, DS2FIX_OPTIONS_INTERFACE, 1);
 *   if (opt && opt->size >= sizeof(Ds2FixOptionsV1)) opt->AddToggle(pse->plugin, &kMyToggle);
 *
 * The OPTIONS screen (front end and the in-mission pause menu's OPTIONS) has 8 rows: the game's EDIT PROFILE, FX VOLUME,
 * MUSIC VOLUME [, CONTROLS in the front end], ds2fix's HUD SCALING / HUD SIZE (with ds2fix.widescreen), then the toggles
 * in the order they were added while rows are free (a toggle that doesn't fit isn't shown). The row reads
 * "<label>: ON" / "<label>: OFF"; selecting it calls Set(!Get()). Game thread only. Docs: docs/sdk/reference/game-ds2.md
 * "DS2 plugins". */
#pragma once
#include <stdint.h>

#include "pse/pse_api.h"

#define DS2FIX_OPTIONS_INTERFACE "ds2fix.options"

typedef struct Ds2FixToggleV1 {
    const char* label;        /* upper case ASCII, e.g. "LOADOUT SCREEN" (<= 40 characters) */
    const char* description;  /* the box under the list, <= 2 lines; may be NULL */
    int (PSE_CALL* Get)(void);
    void (PSE_CALL* Set)(int on); /* apply + save at once */
} Ds2FixToggleV1;

typedef struct Ds2FixOptionsV1 {
    uint32_t size;
    uint32_t version; /* 1 */
    /* the table must stay valid until RemoveToggle (or exit); returns 0 when all 4 toggle slots are taken */
    int (PSE_CALL* AddToggle)(PSEPluginHandle plugin, const Ds2FixToggleV1* toggle);
    void (PSE_CALL* RemoveToggle)(PSEPluginHandle plugin, const Ds2FixToggleV1* toggle);
} Ds2FixOptionsV1;
