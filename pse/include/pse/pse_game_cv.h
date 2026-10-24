/*
 * Pivotal Script Extender (PSE) - game interface for Conflict: Vietnam (game id "cv"): pse.game.cv.
 *
 * Reference: docs/sdk/reference/game-cv.md. Changes: docs/sdk/API_CHANGELOG.md.
 *
 * Version 1 is deliberately small: just exe identification. Unlike the DS1/DS2/TGE layers, the CV layer does not
 * (yet) hook the frame loop, render call, level load or window shutdown - those addresses are not confidently mapped
 * (games/cv/CLAUDE.md "Widescreen, HUD, front end" gives call sites, not verified function entries / prologues), so
 * no PSE_EVENT_FRAME / LEVEL_LOAD_* / PSE_MSG_EXIT are raised by this layer today. cvfix's widescreen and mods
 * features don't need them (both patch once at PSEPlugin_Load). Same ABI rules as pse_api.h: plain C, 32-bit x86,
 * MSVC, PSE_CALL (__cdecl), every struct starts with `size`, tables only grow.
 *
 *     const PSEGameCvV1* game = (const PSEGameCvV1*)pse->Query(pse->plugin, PSE_GAME_CV_INTERFACE, 1);
 *     if (!game || game->size < sizeof(PSEGameCvV1)) return 0;     // not Conflict: Vietnam, or too old a host
 *
 * Served by the built-in plugin "pse.game.cv" (version 1.0.0): a manifest can say `requires = pse.game.cv >= 1.0`.
 */
#ifndef PSE_GAME_CV_H
#define PSE_GAME_CV_H

#include "pse_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSE_GAME_CV_INTERFACE "pse.game.cv"   /* Query id */
#define PSE_GAME_CV_PLUGIN "pse.game.cv"      /* the built-in plugin serving it (for manifests) */

typedef struct PSEGameCvV1 {
    uint32_t size;      /* sizeof(PSEGameCvV1) of the host; check >= the size you were built with */
    uint32_t version;   /* 1 */

    /* SHA-256 of the running Vietnam.exe (upper-case hex, Large Address Aware flag cleared), "" if unreadable.
     * Any thread; computed on the first call. */
    const char* (PSE_CALL* ExeSha256)(void);
    /* 1 = the exe is the build every address here was taken from: Vietnam.exe, Sep 2004 (no SecuROM),
     * 636576CA842066A56660C1DAE615FBA4DB9726AEA1EF851EE5FD4C6D8CE3710C. Any thread. */
    int (PSE_CALL* ExeKnown)(void);
} PSEGameCvV1;

#ifdef __cplusplus
}
#endif

#endif /* PSE_GAME_CV_H */
