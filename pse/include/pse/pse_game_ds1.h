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

/* ---- game events (pse.messaging; raised by the game layer on the game thread - get the id with EventId(name)) ----
 * data = PSEDs1DamageEvent* for all three:
 *   "ds1.soldier_damage"   a soldier is about to take damage - every cause that goes through the soldier's damage
 *                          function (Soldier_TakeDamage 0x424200: bullets, explosions, falls, a script's kill). A
 *                          subscriber may change `amount`; the game applies what is left after every subscriber, and
 *                          0 or less = nothing happens (no damage, no hit reaction). healthAfter = healthBefore here.
 *   "ds1.soldier_damaged"  after the game applied it: amount = what was passed to the game, healthAfter = the health
 *                          now (difficulty and the like are inside the game's function: compare the two healths).
 *   "ds1.soldier_killed"   right after "ds1.soldier_damaged" when it took the soldier from above 0 to 0 or below.
 * A soldier that is already dead gets none of them (the game ignores the call). Online, each PC raises them for the
 * damage its own game applies. Changing `amount` changes the simulation: do it only in a SIMULATION plugin (online
 * peers must run the same - docs/sdk/online-compat.md). */
#define PSE_DS1_EVENT_SOLDIER_DAMAGE "ds1.soldier_damage"
#define PSE_DS1_EVENT_SOLDIER_DAMAGED "ds1.soldier_damaged"
#define PSE_DS1_EVENT_SOLDIER_KILLED "ds1.soldier_killed"

typedef struct PSEDs1DamageEvent {
    uint32_t size;                /* sizeof(PSEDs1DamageEvent) - later versions only append */
    PSEDs1Soldier victim;         /* the soldier (same handle as GetLocalPlayer's) */
    int32_t victimId;             /* its script id (the level's object id that mission scripts use; -1 = none) */
    uint32_t attacker;            /* the object that did it: a soldier, vehicle ... (its address); 0 = none */
    int32_t attackerId;           /* its script id, -1 = none */
    float amount;                 /* damage points (see the events above) */
    float healthBefore;           /* health when the damage arrived (soldier +0x54; squad soldiers start near 150) */
    float healthAfter;
} PSEDs1DamageEvent;

/* Vehicles (tanks, trucks, helicopters, mounted guns ...) - the same three, data = PSEDs1DamageEvent* with `victim` =
 * the vehicle's address and victimId its script id; raised from the vehicles' damage path (0x43E1F0, every cause):
 *   "ds1.vehicle_damage"     before: `amount` may be changed (0 or less = nothing happens). Not raised for the game's
 *                            own "finish it off" calls (its last argument set), which only clean up a wreck.
 *   "ds1.vehicle_damaged"    after: healthAfter = the vehicle's health now (+0x54).
 *   "ds1.vehicle_destroyed"  after "damaged" when it took the vehicle from above 0 to 0 or below (it is a wreck now).
 * A wreck (health 0 or below) gets none of them. */
#define PSE_DS1_EVENT_VEHICLE_DAMAGE "ds1.vehicle_damage"
#define PSE_DS1_EVENT_VEHICLE_DAMAGED "ds1.vehicle_damaged"
#define PSE_DS1_EVENT_VEHICLE_DESTROYED "ds1.vehicle_destroyed"

/* "ds1.item_picked_up" - a soldier took an item into his inventory (weapon, ammo, medikit, key ...; FUN_00432070), raised
 * after the game did it; data = PSEDs1PickUpEvent*. Not raised when the game refused it (inventory full ...). A
 * stackable item (medikit, ammo) is merged into the stack the soldier has: `item` is the picked-up object, not the stack. */
#define PSE_DS1_EVENT_ITEM_PICKED_UP "ds1.item_picked_up"
typedef struct PSEDs1PickUpEvent {
    uint32_t size;                /* sizeof(PSEDs1PickUpEvent) - later versions only append */
    PSEDs1Soldier soldier;
    int32_t soldierId;            /* script id, -1 = none */
    uint32_t item;                /* the item object's address */
    int32_t itemId;               /* its script id (-1 for items made at run time, e.g. a dead soldier's drop) */
} PSEDs1PickUpEvent;

/* "ds1.objective_completed" - a mission objective was ticked off (the mission script's complete-objective call, script
 * API +0x4C0, for every mission: DLL, Lua, editor), raised once - not again for an objective already done; data =
 * PSEDs1ObjectiveEvent*. The title text: LookupText(titleHash, ...). */
#define PSE_DS1_EVENT_OBJECTIVE_COMPLETED "ds1.objective_completed"
typedef struct PSEDs1ObjectiveEvent {
    uint32_t size;                /* sizeof(PSEDs1ObjectiveEvent) */
    uint32_t index;               /* the objective's number in the mission (0 = the first one added) */
    uint32_t titleHash;           /* text hash of its title */
} PSEDs1ObjectiveEvent;

/* "ds1.mission_end" - the mission's result was set (script API +0x48C or the debug "complete mission"): the game shows
 * MISSION SUCCESSFUL / FAILED next. data = PSEDs1MissionEndEvent*. */
#define PSE_DS1_EVENT_MISSION_END "ds1.mission_end"
typedef struct PSEDs1MissionEndEvent {
    uint32_t size;                /* sizeof(PSEDs1MissionEndEvent) */
    uint32_t result;              /* 2 = success, 1 = failure (the values the game uses) */
} PSEDs1MissionEndEvent;

/* "ds1.script_message" - every message the game sends the level's mission script (the same the mission DLL / Lua script
 * gets in its message callback, raised just before it does): about 70 kinds - kills, pick-ups, doors, switches, enemies
 * spotted, shots, alerts, waypoints, explosions ... data = PSEDs1ScriptMessageEvent*. Read-only: the script's answer
 * to a query message is still the script's. Parameters are script ids (the level's object ids) unless noted:
 *   0x00 killed (victim, killer or -1, time)      0x03 picked up (soldier, item)        0x04 enemy on combat alert
 *   0x07 destructible destroyed (object, attacker) 0x08 waypoint reached (unit, waypoint) 0x09 path done (unit)
 *   0x0B healed     0x0C damaged       0x0D switch used (switch name hash, 1.0f, soldier)
 *   0x0E air strike called (soldier, x, y, z float)  0x11 mission complete   0x12 mission start (after the overview)
 *   0x15 door (door, 0 closing / 1 opening / 2 closed / 3 open)  0x16 script timer (timer id)
 *   0x17 explosive planted   0x18 explosion (x, y, z float)   0x1B / 0x1C mounted / dismounted (soldier, vehicle)
 *   0x34 enemy spotted (observer, target)   0x3C shot (weapon, shooter)   0x52 a save was loaded
 * (the full list: the readable mission scripts' game.EVENT names / ModKit api_slots.json). */
#define PSE_DS1_EVENT_SCRIPT_MESSAGE "ds1.script_message"
typedef struct PSEDs1ScriptMessageEvent {
    uint32_t size;                /* sizeof(PSEDs1ScriptMessageEvent) */
    uint32_t id;                  /* the message (table above) */
    const uint32_t* params;       /* its parameter dwords as the game sends them (up to 8; may be NULL) - valid only
                                     during the callback */
} PSEDs1ScriptMessageEvent;

#ifdef __cplusplus
}
#endif

#endif /* PSE_GAME_DS1_H */
