/*
 * Pivotal Script Extender (PSE) - game interface for Conflict: Global Storm / Global Terror (game id "cgs"):
 * pse.game.cgs.
 *
 * Reference: docs/sdk/reference/game-cgs.md. Changes: docs/sdk/API_CHANGELOG.md.
 *
 * Version 1 is deliberately small: ConflictGlobal.exe runs most of its logic inside a SecuROM virtual machine with no
 * static call sites (docs/sdk/reference/game-cgs.md "SecuROM VM"), so the layer only exposes what it can read without
 * hooking VM'd code - process identity and the one frame-dispatch byte GlobalStormFix's standalone build already
 * hooked at plain (non-VM) call sites. Same ABI rules as pse_api.h: plain C, 32-bit x86, MSVC, PSE_CALL (__cdecl),
 * every struct starts with `size`, tables only grow.
 *
 *     const PSEGameCgsV1* game =
 *         (const PSEGameCgsV1*)pse->Query(pse->plugin, PSE_GAME_CGS_INTERFACE, 1);
 *     if (!game || game->size < sizeof(PSEGameCgsV1)) return 0;     // not Global Storm, or too old a host
 *
 * Served by the built-in plugin "pse.game.cgs" (version 1.0.0).
 */
#ifndef PSE_GAME_CGS_H
#define PSE_GAME_CGS_H

#include "pse_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSE_GAME_CGS_INTERFACE "pse.game.cgs"   /* Query id */
#define PSE_GAME_CGS_PLUGIN "pse.game.cgs"      /* the built-in plugin serving it (for manifests) */

typedef struct PSEGameCgsV1 {
    uint32_t size;      /* sizeof(PSEGameCgsV1) of the host; check >= the size you were built with */
    uint32_t version;   /* 1 */

    /* ---- process ---- */

    /* The game's window, HWND; NULL before it exists. Any thread. */
    void* (PSE_CALL* GameWindow)(void);
    /* SHA-256 of the running ConflictGlobal.exe (upper-case hex), "" if unreadable. Any thread; computed once. */
    const char* (PSE_CALL* ExeSha256)(void);
    /* 1 = the exe is the RELOADED NoCD build every address here was taken from (D1EE014F...39A1). Any thread. */
    int (PSE_CALL* ExeKnown)(void);
} PSEGameCgsV1;

#ifdef __cplusplus
}
#endif

#endif /* PSE_GAME_CGS_H */
