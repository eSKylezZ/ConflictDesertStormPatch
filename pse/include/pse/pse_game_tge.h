/*
 * Pivotal Script Extender (PSE) - game interface for The Great Escape (game id "tge"): pse.game.tge.
 *
 * Reference: docs/sdk/reference/game-tge.md. Changes: docs/sdk/API_CHANGELOG.md.
 *
 * The stable way to read The Great Escape's state (DESIGN.md §5.10). Version 1 is READ-ONLY and small: what the TGE
 * game layer itself reads (dispatch mode, level, level-table entry, window, exe). Same ABI rules as pse_api.h: plain C,
 * 32-bit x86, MSVC, PSE_CALL (__cdecl), every struct starts with `size`, tables only grow. Strings are UTF-8, copied
 * into the caller's buffer. Unless a function says otherwise: game thread only.
 *
 *     const PSEGameTgeV1* game =
 *         (const PSEGameTgeV1*)pse->Query(pse->plugin, PSE_GAME_TGE_INTERFACE, 1);
 *     if (!game || game->size < sizeof(PSEGameTgeV1)) return 0;     // not The Great Escape, or too old a host
 *
 * Served by the built-in plugin "pse.game.tge" (version 1.0.0): a manifest can say `requires = pse.game.tge >= 1.0`.
 * The loader interfaces and the core game events (frame, render, level load / unload, front end ready, D3D8 device,
 * exit) are the same as on Desert Storm 1 / 2, so a plugin written against those runs here unchanged.
 */
#ifndef PSE_GAME_TGE_H
#define PSE_GAME_TGE_H

#include "pse_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSE_GAME_TGE_INTERFACE "pse.game.tge"   /* Query id */
#define PSE_GAME_TGE_PLUGIN "pse.game.tge"      /* the built-in plugin serving it (for manifests) */

/* Hook priorities of the TGE game layer's own hooks (owner "pse.game.tge"), with the same meaning as DS1's
 * (pse_game_ds1.h): INNER = the layer's hook is the closest to the game, OUTER = it runs first and brackets the rest. */
#define PSE_TGE_LAYER_PRIORITY_INNER (-1000)
#define PSE_TGE_LAYER_PRIORITY_OUTER 1000

/* What kind of level is loaded (LevelKind). The game loads everything - menus, films, logos, missions - as entries of
 * one level table; LevelName / LevelKind follow the entry the layer saw load last. */
typedef enum PSETgeLevelKind {
    PSE_TGE_LEVEL_NONE = 0,       /* nothing loaded yet (start-up, launcher) */
    PSE_TGE_LEVEL_FRONTEND = 1,   /* the menus (level-table entry "FE", type 0) */
    PSE_TGE_LEVEL_MISSION = 2,    /* a mission: a "*.dll" level (Level_Init) */
    PSE_TGE_LEVEL_SCREEN = 3      /* anything else: films, briefings, logos, the attract loop, language select */
} PSETgeLevelKind;

typedef struct PSEGameTgeV1 {
    uint32_t size;                /* sizeof(PSEGameTgeV1) of the host; check >= the size you were built with */
    uint32_t version;             /* 1 */

    /* ---- state ---- */

    /* The main loop's dispatch mode, u8 [0x66A69C]: 0 = a level runs (Game_Update), 1 = a level load is pending /
     * running this frame (the whole load is one frame), 2..7 = films and other screens. */
    uint32_t (PSE_CALL* DispatchMode)(void);
    /* The level-table entry being / last loaded, i32 [0x7DDAEC] (-1 = a name from Level_NameBuffer). */
    int32_t (PSE_CALL* LevelTableIndex)(void);

    /* ---- level ---- */

    /* The name of the level loaded last: a mission's "Level01.dll", the front end's "FE", a film's "FMV02" ... ("" before
     * the first load and after an unload). Copies into buf (NUL-terminated when bufLen > 0); returns the full length. */
    uint32_t (PSE_CALL* LevelName)(char* buf, uint32_t bufLen);
    /* PSETgeLevelKind of LevelName. */
    uint32_t (PSE_CALL* LevelKind)(void);

    /* ---- process ---- */

    /* The game's window, HWND [0x7DC238] (class "The Great Escape"); NULL before it exists. Any thread. */
    void* (PSE_CALL* GameWindow)(void);
    /* SHA-256 of the running GreatEscape.exe (upper-case hex, Large Address Aware flag cleared), "" if unreadable.
     * Any thread; computed on the first call. */
    const char* (PSE_CALL* ExeSha256)(void);
    /* 1 = the exe is the build every address here was taken from: GreatEscape.exe 1.0 without the disc check,
     * 50F4A87EA7F370BDB0F41E029351B07B752E4042B26553D1CECE0EB577702CDF. Any thread. */
    int (PSE_CALL* ExeKnown)(void);
} PSEGameTgeV1;

#ifdef __cplusplus
}
#endif

#endif /* PSE_GAME_TGE_H */
