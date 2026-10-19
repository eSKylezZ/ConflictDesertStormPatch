/*
 * Pivotal Script Extender (PSE) - game interface for Conflict: Desert Storm (game id "ds1"): pse.game.ds1.
 *
 * Reference: docs/sdk/reference/game-ds1.md. Changes: docs/sdk/API_CHANGELOG.md.
 *
 * The stable way to read the game's state (DESIGN.md §5.10): each function says what it reads in game memory, so a
 * plugin needs no raw addresses for these jobs. Version 1 is READ-ONLY - nothing here writes game memory or calls a
 * game function that changes anything.
 *
 * Same ABI rules as pse_api.h: plain C, 32-bit x86, MSVC, PSE_CALL (__cdecl) everywhere, every struct starts with
 * `size`, tables only grow, a breaking change = a new interface version. Strings are UTF-8 and copied into the
 * caller's buffer (nothing to free). Unless a function says otherwise it must be called on the game's main thread.
 *
 * Getting it (in PSEPlugin_Load or later):
 *
 *     const PSEGameDs1V1* game =
 *         (const PSEGameDs1V1*)pse->Query(pse->plugin, PSE_GAME_DS1_INTERFACE, 1);
 *     if (!game || game->size < sizeof(PSEGameDs1V1)) return 0;     // not Desert Storm, or too old a host
 *
 * The interface is served by the built-in plugin "pse.game.ds1" (version = interface version, 1.0.0), so a manifest
 * can say `requires = pse.game.ds1 >= 1.0` and the loader refuses the plugin on any other game / host.
 */
#ifndef PSE_GAME_DS1_H
#define PSE_GAME_DS1_H

#include "pse_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSE_GAME_DS1_INTERFACE "pse.game.ds1"   /* Query id */
#define PSE_GAME_DS1_PLUGIN "pse.game.ds1"      /* the built-in plugin serving it (for manifests) */

/* Hook priorities of the game layer's own hooks (owner "pse.game.ds1"; reference game-ds1.md "Events and messages"):
 * INNER sites - the frame dispatch call, the render call, D3D EndScene / Present / TestCooperativeLevel - run the
 * layer's hook closest to the game (another plugin's hook above -1000 runs first and calls it through `next`); OUTER
 * sites - level init / unload, save, load, Direct3DCreate8, IDirect3D8::CreateDevice, Reset - run the layer's hook
 * first, so its messages bracket everything the other hooks do there. */
#define PSE_DS1_LAYER_PRIORITY_INNER (-1000)
#define PSE_DS1_LAYER_PRIORITY_OUTER 1000

/* What kind of level is loaded (LevelKind). */
typedef enum PSEDs1LevelKind {
    PSE_DS1_LEVEL_NONE = 0,       /* nothing loaded yet (start-up, launcher) */
    PSE_DS1_LEVEL_FRONTEND = 1,   /* the menus: FrontEnd / intro (the front end's own film scene) */
    PSE_DS1_LEVEL_MISSION = 2,    /* anything else: missions, training, multiplayer maps, Lua / mod levels - also
                                     while paused, in its cutscenes or on its loading screen */
    PSE_DS1_LEVEL_OUTRO = 3       /* the end credits (Outro) */
} PSEDs1LevelKind;

/* Opaque soldier handle: the soldier's address. 0 = none. Valid only until the soldier is removed or the level ends -
 * re-read it every frame, never keep it across frames. Only for comparing / passing back to this interface. */
typedef uint32_t PSEDs1Soldier;

typedef struct PSEDs1Vec3 {
    float x, y, z;                /* centimetres, y up (the game's world space) */
} PSEDs1Vec3;

/* One player of this PC (GetLocalPlayer). */
typedef struct PSEDs1Player {
    uint32_t size;                /* caller sets sizeof(PSEDs1Player) */
    uint32_t block;               /* the game's input block / player slot 0..3 (split screen: player n = block n-1;
                                     online: this PC's own player number - 1) */
    PSEDs1Soldier soldier;        /* the soldier this player controls now */
    PSEDs1Vec3 position;          /* soldier +0x30 */
    float health;                 /* soldier +0x54 (Bradley starts Mission 1 with 150; 0 = dead) */
} PSEDs1Player;

typedef enum PSEDs1OnlineRole {
    PSE_DS1_ONLINE_NONE = 0,
    PSE_DS1_ONLINE_HOST = 1,
    PSE_DS1_ONLINE_JOINER = 2
} PSEDs1OnlineRole;

/* The online co-op session (GetOnline). */
typedef struct PSEDs1Online {
    uint32_t size;                /* caller sets sizeof(PSEDs1Online) */
    uint32_t active;              /* 1 = hosting or joined (a session exists, a mission may or may not run) */
    uint32_t role;                /* PSEDs1OnlineRole */
    uint32_t localPlayer;         /* this PC's player number: 1 = the host, 2.. = joiners; 0 = no session */
    uint32_t players;             /* players in the session including this PC (0 = no session) */
    uint32_t connectedPeers;      /* other PCs connected right now */
    uint32_t inMission;           /* 1 = an online mission is being kept in step right now */
} PSEDs1Online;

typedef struct PSEGameDs1V1 {
    uint32_t size;                /* sizeof(PSEGameDs1V1) of the host; check >= the size you were built with */
    uint32_t version;             /* 1 */

    /* ---- state ---- */

    /* The front-end state machine's current state, [0x617C18]: 1 main menu, 0xE playing a mission, 0xF pause menu,
     * 0x14 briefing, other values = other menu screens (CLAUDE.md "Front-end menus"). */
    uint32_t (PSE_CALL* FrontEndState)(void);
    /* The main loop's dispatch mode, [0x60EC8C]: 0 = running (front end or mission), 1 = a level load is pending /
     * running this frame, 3 = other (shut-down, ...). */
    uint32_t (PSE_CALL* DispatchMode)(void);

    /* ---- level ---- */

    /* The loaded level's scene file name, char[] at 0x606880 ("Mission4.env", "FrontEnd.env", "intro.env"; "" before
     * the first load). Copies into buf (always NUL-terminated when bufLen > 0); returns the full length. */
    uint32_t (PSE_CALL* LevelName)(char* buf, uint32_t bufLen);
    /* PSEDs1LevelKind of LevelName: by its prefix (case-insensitive): "" none, "frontend" / "intro" front end, "outro"
     * outro, anything else mission. */
    uint32_t (PSE_CALL* LevelKind)(void);
    /* 1 while a cutscene / film plays: the cinematic bit, bit 0 of byte [[0x63C948] + 0x1B8C] (set by the script API's
     * MovieStart, cleared by MovieEnd). 0 when no world exists. */
    int (PSE_CALL* InCutscene)(void);

    /* ---- text ---- */

    /* The game's text hash of a catalog key: the archive LFSR (game FUN_004b9ab0) over the lower-cased key -
     * TextHash("MISS1_TITLE") = 0xA90BE3A7. Pure, any thread. */
    uint32_t (PSE_CALL* TextHash)(const char* key);
    /* A text of the game's catalog in the current language (catalog.dat + the level's MnnStrs.loc, mod .strings
     * texts first - whatever the game's own menus would show): the lookup FUN_004f6730 (thiscall on the text table
     * [0x60EDCC]) for `hash`, converted from the game's ANSI code page to UTF-8. Copies into buf (always NUL-terminated
     * when bufLen > 0); returns the full length in bytes, 0 = unknown hash / empty text / no text table yet. */
    uint32_t (PSE_CALL* LookupText)(uint32_t hash, char* buf, uint32_t bufLen);
    /* The current mission's title as a person would write it, UTF-8: "Mission 4: Desert Watch", "Mission 5A: <title>",
     * "Basic Training" (training levels: the title only). From LevelName: the number part ("Mission 5A",
     * "Training 2", "Multiplayer mission 2"), plus ": " and the catalog title (MISSn_TITLE / TRAINn_TITLE via
     * LookupText; an all-capitals title is turned into Title Case; control characters dropped). No title (unknown,
     * multiplayer maps) -> the number part only; a name without such a pattern -> the scene name without extension.
     * Front end / outro / nothing loaded -> "" and 0. Returns the
     * full length; buf as above. */
    uint32_t (PSE_CALL* LevelTitle)(char* buf, uint32_t bufLen);

    /* ---- players ---- */

    /* Views on this screen: the split-screen player count the current level was built with (2..4), 0 or 1 = one view.
     * Front end: whatever the last level had (the menus are always one view). */
    uint32_t (PSE_CALL* SplitScreenPlayers)(void);
    /* Players on this PC in the current level: 0 outside missions (LevelKind != MISSION); online 1; else
     * max(1, SplitScreenPlayers()). */
    uint32_t (PSE_CALL* LocalPlayerCount)(void);
    /* The index-th player of this PC (0 .. LocalPlayerCount()-1): input block 0x60F5B8 + block * 0x478, soldier =
     * block +0x310, position soldier +0x30, health soldier +0x54 (reads are fault-guarded). Returns 1 and fills *out
     * (out->size must be >= sizeof(PSEDs1Player)); 0 = no such player or no soldier (cutscenes, loading). */
    int (PSE_CALL* GetLocalPlayer)(uint32_t index, PSEDs1Player* out);

    /* ---- online ---- */

    /* The online co-op session (DesertStormFix's own connection; read-only). Always fills *out (out->size must be >=
     * sizeof(PSEDs1Online)); returns out->active. */
    int (PSE_CALL* GetOnline)(PSEDs1Online* out);

    /* ---- process ---- */

    /* The game's window (HWND), [0x606A60]; NULL before the window exists. Any thread. */
    void* (PSE_CALL* GameWindow)(void);
    /* SHA-256 of the running DesertStorm.exe as 64 upper-case hex digits, hashed with the Large Address Aware flag
     * cleared (so the 4 GB patcher doesn't change it); "" if the file can't be read. Computed once (first call reads
     * the ~2.3 MB file), the string stays valid until exit. Any thread. */
    const char* (PSE_CALL* ExeSha256)(void);
    /* 1 = ExeSha256 is the known build every address here was taken from (D712F5EC...; Steam = GOG). Any thread. */
    int (PSE_CALL* ExeKnown)(void);
} PSEGameDs1V1;

#ifdef __cplusplus
}
#endif

#endif /* PSE_GAME_DS1_H */
