/*
 * Pivotal Script Extender (PSE) - pse.ids: stable ids for game addresses (like SKSE's Address Library).
 *
 * Reference: docs/sdk/reference/ids.md. Per-game lists: docs/sdk/reference/ids-<game>.md. Id constants:
 * pse/include/pse/pse_ids_<game>.h (generated). Changes: docs/sdk/API_CHANGELOG.md.
 *
 * Every game function, global, code site, vtable or data table a plugin may need has a numeric id and a name
 * ("ds1" id 167 = "Soldier_TakeDamage"). The game layer knows the address of every id for each exe build it
 * supports (Steam, GOG, retail CD, other regions ...) and checks each one at start-up against a short byte
 * signature taken from that build. A plugin asks for the id and gets the address for the exe that is running, so
 * one plugin binary works on every build the database covers - and gets 0 (never a wrong address) where it doesn't.
 *
 * Stability: an id is never reused, renumbered, renamed or given another kind once published (the list of
 * published ids is checked by the build). New ids are only added. The database version grows when ids are added.
 *
 * ABI: as pse_api.h (plain C, 32-bit, __cdecl). Every function may be called from any thread once the game layer
 * has registered its database (before any plugin loads); the tables never change afterwards.
 *
 *   const PSEIdsInterface* ids = (const PSEIdsInterface*)pse->Query(pse->plugin, PSE_IDS_INTERFACE, 1);
 *   uint32_t takeDamage = ids ? ids->Resolve("ds1", PSE_ID(ds1, Soldier_TakeDamage)) : 0;
 *   if (!takeDamage) { ... this exe build isn't covered: switch the feature off, don't guess ... }
 */
#ifndef PSE_IDS_H
#define PSE_IDS_H

#include <stdint.h>

#include "pse_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSE_IDS_INTERFACE "pse.ids"

/* A game's id. 0 is never a valid id. Ids are per game: "ds1" 12 and "ds2" 12 are unrelated. */
typedef uint32_t PSEId;

typedef enum PSEIdKind {
    PSE_ID_KIND_NONE = 0,
    PSE_ID_KIND_FUNCTION = 1,  /* entry point of a function (HookFunction, calls) */
    PSE_ID_KIND_GLOBAL = 2,    /* a variable in .data / .bss (the address of the variable, not its value) */
    PSE_ID_KIND_SITE = 3,      /* an instruction inside a function: call / jump site, an immediate operand */
    PSE_ID_KIND_VTABLE = 4,    /* start of a vtable (first slot) */
    PSE_ID_KIND_DATA = 5       /* tables, constants, single vtable slots, IAT slots */
} PSEIdKind;

typedef enum PSEIdStatus {
    PSE_ID_UNKNOWN = 0,        /* the game has no such id (or no database is registered for that game) */
    PSE_ID_OK = 1,             /* resolved and its signature matched */
    PSE_ID_NOT_IN_BUILD = 2,   /* the id exists but the database has no address for the running build */
    PSE_ID_MISMATCH = 3,       /* the running build's bytes didn't match the signature: resolves to 0 (logged) */
    PSE_ID_UNKNOWN_BUILD = 4   /* the running exe is no build the database knows: every id resolves to 0 */
} PSEIdStatus;

typedef struct PSEIdInfo {
    uint32_t size;             /* in: sizeof(PSEIdInfo) */
    PSEId id;
    const char* name;          /* "Soldier_TakeDamage"; NULL when status is PSE_ID_UNKNOWN */
    PSEIdKind kind;
    PSEIdStatus status;
    uint32_t va;               /* resolved address, 0 unless status is PSE_ID_OK */
} PSEIdInfo;

typedef struct PSEIdBuildInfo {
    uint32_t size;             /* in: sizeof(PSEIdBuildInfo) */
    const char* gameId;        /* "ds1" */
    const char* gameName;      /* "Conflict: Desert Storm" */
    const char* build;         /* the running exe's build id ("steam-gog-1.0"), "" when the exe is unknown */
    const char* buildNotes;    /* one line from the database, "" when unknown */
    const char* sha256;        /* the build's file SHA-256 (upper-case hex) as the database has it, "" when unknown */
    uint32_t timestamp;        /* PE header TimeDateStamp of the running exe */
    uint32_t sizeOfImage;      /* PE header SizeOfImage of the running exe */
    uint32_t databaseVersion;  /* grows when ids are added */
    uint32_t idCount;          /* ids in the database */
    uint32_t resolvedCount;    /* ids with status PSE_ID_OK */
    uint32_t mismatchCount;    /* ids with status PSE_ID_MISMATCH */
    uint32_t buildCount;       /* builds the database knows */
} PSEIdBuildInfo;

/* gameId: "ds1", ... ; NULL or "" = the running game. Unknown game = 0 / NULL / PSE_E_NOT_FOUND. */
typedef struct PSEIdsInterface {
    uint32_t size;
    uint32_t version;  /* 1 */

    /* Address of the id for the running exe; 0 = unknown id, not in this build, or signature mismatch. */
    uint32_t (PSE_CALL* Resolve)(const char* gameId, PSEId id);
    /* The same by name (case-sensitive, "Soldier_TakeDamage"). Prefer ids in shipped plugins: names are for
     * consoles, scripts and logs. */
    uint32_t (PSE_CALL* ResolveByName)(const char* gameId, const char* name);
    /* Name of an id (also for ids that don't resolve on this build), NULL for an unknown id. Static string. */
    const char* (PSE_CALL* Name)(const char* gameId, PSEId id);
    /* Id of a name, 0 = none. */
    PSEId (PSE_CALL* IdOf)(const char* gameId, const char* name);
    /* Everything about one id. out->size must be set. PSE_E_NOT_FOUND = unknown id (out still filled: status
     * PSE_ID_UNKNOWN). */
    PSEResult (PSE_CALL* Info)(const char* gameId, PSEId id, PSEIdInfo* out);
    /* The i-th id of the game in id order (0 .. idCount-1), for listing. PSE_E_NOT_FOUND past the end. */
    PSEResult (PSE_CALL* InfoAt)(const char* gameId, uint32_t index, PSEIdInfo* out);
    /* Which build is running, how many ids resolved. out->size must be set. */
    PSEResult (PSE_CALL* Build)(const char* gameId, PSEIdBuildInfo* out);
} PSEIdsInterface;

/* PSE_ID(ds1, Soldier_TakeDamage) -> PSE_ID_ds1_Soldier_TakeDamage (from pse_ids_ds1.h). A misspelt name is a
 * compile error, never a wrong id. */
#define PSE_ID(game, name) PSE_ID_##game##_##name
/* PSE_RESOLVE(ids, ds1, Soldier_TakeDamage) -> uint32_t address (0 = not available on this exe). */
#define PSE_RESOLVE(ids, game, name) ((ids)->Resolve(#game, PSE_ID(game, name)))

#ifdef __cplusplus
}  /* extern "C" */

/* C++: the address as a typed pointer, nullptr when it doesn't resolve.
 *   using TakeDamageFn = void(__thiscall*)(void* soldier, float amount, void* attacker);
 *   auto takeDamage = PSEResolveAs<TakeDamageFn>(ids, "ds1", PSE_ID(ds1, Soldier_TakeDamage));          */
template <class T>
inline T PSEResolveAs(const PSEIdsInterface* ids, const char* gameId, PSEId id) {
    const uint32_t va = ids ? ids->Resolve(gameId, id) : 0;
    return reinterpret_cast<T>(static_cast<uintptr_t>(va));
}
#endif

#endif /* PSE_IDS_H */
