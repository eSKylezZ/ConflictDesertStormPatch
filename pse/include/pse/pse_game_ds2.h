/*
 * Pivotal Script Extender (PSE) - game interface for Conflict: Desert Storm II (game id "ds2"): pse.game.ds2.
 *
 * Reference: docs/sdk/reference/game-ds2.md. Changes: docs/sdk/API_CHANGELOG.md.
 *
 * The stable way to read DS2's state (DESIGN.md §5.10). Version 1 is READ-ONLY and small: only what the DS2 game layer
 * itself already reads (front-end state, dispatch mode, level, cutscene bit, split screen, window, exe). Same ABI rules
 * as pse_api.h: plain C, 32-bit x86, MSVC, PSE_CALL (__cdecl), every struct starts with `size`, tables only grow.
 * Strings are UTF-8, copied into the caller's buffer. Unless a function says otherwise: game thread only.
 *
 *     const PSEGameDs2V1* game =
 *         (const PSEGameDs2V1*)pse->Query(pse->plugin, PSE_GAME_DS2_INTERFACE, 1);
 *     if (!game || game->size < sizeof(PSEGameDs2V1)) return 0;     // not Desert Storm II, or too old a host
 *
 * Served by the built-in plugin "pse.game.ds2" (version 1.0.0): a manifest can say `requires = pse.game.ds2 >= 1.0`.
 * A plugin for BOTH games queries pse.game.ds1 and pse.game.ds2 and uses whichever it gets (or neither: the loader
 * interfaces and the core game events are the same on both games).
 */
#ifndef PSE_GAME_DS2_H
#define PSE_GAME_DS2_H

#include "pse_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSE_GAME_DS2_INTERFACE "pse.game.ds2"   /* Query id */
#define PSE_GAME_DS2_PLUGIN "pse.game.ds2"      /* the built-in plugin serving it (for manifests) */

/* Hook priorities of the DS2 game layer's own hooks (owner "pse.game.ds2"), with the same meaning as DS1's
 * (pse_game_ds1.h): INNER = the layer's hook is the closest to the game, OUTER = it runs first and brackets the rest. */
#define PSE_DS2_LAYER_PRIORITY_INNER (-1000)
#define PSE_DS2_LAYER_PRIORITY_OUTER 1000

/* What kind of level is loaded (LevelKind). */
typedef enum PSEDs2LevelKind {
    PSE_DS2_LEVEL_NONE = 0,       /* nothing loaded yet (start-up, launcher, logo films) */
    PSE_DS2_LEVEL_FRONTEND = 1,   /* the menus (scene "FrontEnd") */
    PSE_DS2_LEVEL_MISSION = 2     /* anything else: missions, training - also paused, in a cutscene, while loading */
} PSEDs2LevelKind;

typedef struct PSEGameDs2V1 {
    uint32_t size;                /* sizeof(PSEGameDs2V1) of the host; check >= the size you were built with */
    uint32_t version;             /* 1 */

    /* ---- state ---- */

    /* The front-end state machine's current state, u32 [0x63433C] (0x13 = a mission is running; other values = menu
     * screens, briefing, pause ...). 0 before the front end exists. */
    uint32_t (PSE_CALL* FrontEndState)(void);
    /* The main loop's dispatch mode, u8 [0x627158]: 0 = front end, 1 = mission running, 2 = a level load is pending /
     * running this frame (the whole load is one frame), 3 = a film / briefing entry of the campaign flow, 4 = other. */
    uint32_t (PSE_CALL* DispatchMode)(void);

    /* ---- level ---- */

    /* The loaded scene's name: the char* at [0x68008C] + 0x1BBC ("FrontEnd" in the menus; a mission's scene name,
     * e.g. "Mission01"; "" before the first load). Copies into buf (NUL-terminated when bufLen > 0); returns the full
     * length. */
    uint32_t (PSE_CALL* LevelName)(char* buf, uint32_t bufLen);
    /* PSEDs2LevelKind of LevelName: "" none, "FrontEnd" (any case) front end, anything else mission. */
    uint32_t (PSE_CALL* LevelKind)(void);
    /* 1 while a cutscene plays: bit 0 of the byte [[0x68008C] + 0x1B8C] (the world object's cinematic flag; the render
     * loop then draws one full-screen view). 0 when no world exists. */
    int (PSE_CALL* InCutscene)(void);

    /* ---- players ---- */

    /* Views on this screen: the game's split-screen object 0x626C18 (+0 local multiplayer flag, +4 player count) -
     * its player count (2..4) while the flag is set, else 1. */
    uint32_t (PSE_CALL* SplitScreenPlayers)(void);

    /* ---- process ---- */

    /* The game's window, HWND [0x62F41C] (class "CDS II"); NULL before it exists. Any thread. */
    void* (PSE_CALL* GameWindow)(void);
    /* SHA-256 of the running CDS II.exe (upper-case hex, Large Address Aware flag cleared), "" if unreadable.
     * Any thread; computed on the first call. */
    const char* (PSE_CALL* ExeSha256)(void);
    /* 1 = the exe is the build every address here was taken from: CDS II.exe 1.0 disc-free,
     * E9B46420DC00B13C3C48A9E9B7B3FDA29458D5EA83AC0D203F5B68FDD111542A. Any thread. */
    int (PSE_CALL* ExeKnown)(void);
} PSEGameDs2V1;

#ifdef __cplusplus
}
#endif

#endif /* PSE_GAME_DS2_H */
