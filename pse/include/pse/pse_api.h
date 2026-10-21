/*
 * Pivotal Script Extender (PSE) - public plugin API.
 *
 * Design: docs/internal/DESIGN.md. Reference: docs/sdk/reference/. Changes: docs/sdk/API_CHANGELOG.md.
 *
 * Stability: API version 0 = UNSTABLE (phase 1, inside DesertStormFix). Everything here may change until SDK 1.0
 * freezes the v1 interfaces. After that: structs only grow (every struct starts with `size`), interfaces get new
 * versions instead of changing old ones.
 *
 * ABI rules: plain C, 32-bit x86, MSVC. Every function pointer is __cdecl unless stated. No C++ types, exceptions or
 * CRT objects cross the boundary (each plugin has its own CRT: memory is freed by the side that allocated it).
 * Strings are UTF-8, NUL-terminated, owned by the caller unless stated. Unless a function says otherwise it must be
 * called on the game's main thread (see pse.tasks for other threads).
 */
#ifndef PSE_API_H
#define PSE_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSE_CALL __cdecl
#define PSE_MAKE_VERSION(major, minor, patch) \
    ((((uint32_t)(major) & 0xFF) << 24) | (((uint32_t)(minor) & 0xFFF) << 12) | ((uint32_t)(patch) & 0xFFF))
#define PSE_VERSION_MAJOR(v) (((v) >> 24) & 0xFF)
#define PSE_VERSION_MINOR(v) (((v) >> 12) & 0xFFF)
#define PSE_VERSION_PATCH(v) ((v) & 0xFFF)

/* Loader API version. 0 = unstable (pre-SDK 1.0). */
#define PSE_API_VERSION 0u

/* Identifies a plugin in every call. 0 is never a valid handle. */
typedef uint32_t PSEPluginHandle;

typedef enum PSEResult {
    PSE_OK = 0,
    PSE_E_INVALID_ARG = 1,   /* NULL / out of range / bad size field */
    PSE_E_MISMATCH = 2,      /* the bytes at a site are not what the caller expected (different exe version?) */
    PSE_E_CONFLICT = 3,      /* another plugin owns (part of) the range */
    PSE_E_NOT_FOUND = 4,
    PSE_E_VERSION = 5,       /* interface / data version not supported */
    PSE_E_NO_MEMORY = 6,
    PSE_E_WRONG_PHASE = 7,   /* not allowed at this point of start-up / on this thread */
    PSE_E_ACCESS = 8,        /* VirtualProtect / registry / file access failed */
    PSE_E_EXISTS = 9,        /* already registered */
    PSE_E_UNSUPPORTED = 10
} PSEResult;

/* ------------------------------------------------------------------------------------------------------------------
 * Plugin info + entry points (exported by every plugin DLL)
 * ------------------------------------------------------------------------------------------------------------------ */

enum {
    PSE_PLUGIN_SIMULATION = 0x1,            /* changes gameplay: online peers must have the same plugin + version + hash */
    PSE_PLUGIN_CLIENT_ONLY = 0x2,           /* cosmetic / UI only: may differ between online peers */
    PSE_PLUGIN_USES_RAW_ADDRESSES = 0x4,    /* built against PSELib / raw addresses rather than pse.game wrappers */
    PSE_PLUGIN_INTERNAL = 0x80000000u       /* compiled into the loader (DesertStormFix features); set by the loader */
};

typedef struct PSEPluginInfo {
    uint32_t size;               /* sizeof(PSEPluginInfo) */
    uint32_t infoVersion;        /* 1 */
    const char* name;            /* unique id: [a-z0-9_.-], e.g. "dsfix.discord", "kyle.alienfaction" */
    const char* displayName;
    const char* author;
    uint32_t version;            /* PSE_MAKE_VERSION */
    uint32_t minApiVersion;      /* lowest loader API version it works with */
    const char* const* games;    /* NULL-terminated game ids ("ds1", "ds2", "cv", "cgs", "tge"); NULL = any game */
    uint32_t flags;              /* PSE_PLUGIN_*; neither SIMULATION nor CLIENT_ONLY = treated as SIMULATION */
} PSEPluginInfo;

typedef struct PSELoadInterface {
    uint32_t size;
    uint32_t apiVersion;         /* PSE_API_VERSION of the running loader */
    const char* gameId;          /* "ds1" */
    PSEPluginHandle plugin;      /* the receiving plugin's handle */
    /* Returns the interface table for `id` ("pse.log", "pse.hooks", ...) at version >= minVersion, or NULL. Also
     * finds interfaces other plugins registered (pse.messaging RegisterInterface). Pointers stay valid until exit. */
    const void* (PSE_CALL* Query)(PSEPluginHandle plugin, const char* id, uint32_t minVersion);
} PSELoadInterface;

/* Exports of an external plugin DLL (phase 2):
 *   __declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info;
 *   __declspec(dllexport) int  PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse);   // non-zero = loaded
 *   __declspec(dllexport) void PSE_CALL PSEPlugin_Unload(void);                         // optional            */
typedef int(PSE_CALL* PSEPluginLoadFn)(const PSELoadInterface* pse);
typedef void(PSE_CALL* PSEPluginUnloadFn)(void);

/* ------------------------------------------------------------------------------------------------------------------
 * pse.log (version 1)
 * ------------------------------------------------------------------------------------------------------------------ */

#define PSE_LOG_INTERFACE "pse.log"
typedef enum PSELogLevel { PSE_LOG_DEBUG = 0, PSE_LOG_INFO = 1, PSE_LOG_WARN = 2, PSE_LOG_ERROR = 3 } PSELogLevel;

typedef struct PSELogInterface {
    uint32_t size;
    uint32_t version;  /* 1 */
    /* printf-style. Prefixed with time + plugin name. Any thread. */
    void (PSE_CALL* Write)(PSEPluginHandle plugin, PSELogLevel level, const char* fmt, ...);
    /* A message for players/modders: log + the mod report (Mods\DesertStormFix-report.txt). error != 0 counts as a
     * problem (main menu "N problems"). Any thread. */
    void (PSE_CALL* Report)(PSEPluginHandle plugin, int error, const char* fmt, ...);
} PSELogInterface;

/* ------------------------------------------------------------------------------------------------------------------
 * pse.hooks (version 1) - the broker. All code / data patching goes through here.
 *
 * Every patched byte has an owner. A write overlapping bytes another plugin owns fails with PSE_E_CONFLICT (both
 * names logged). Hooks on the same site are CHAINED: each subscriber passes a `next` variable; the broker keeps it
 * pointing at the next subscriber in priority order (or the original code), and rewrites it when hooks are added or
 * removed. Call it like the original function. Higher priority runs first; equal priority = registration order.
 * ------------------------------------------------------------------------------------------------------------------ */

#define PSE_HOOKS_INTERFACE "pse.hooks"
typedef uint32_t PSEHookId;    /* 0 = none */

typedef struct PSEBytePatch {
    uint32_t va;
    const uint8_t* original;   /* expected bytes (ignored when anyOriginal) */
    const uint8_t* patched;
    uint32_t len;
    uint32_t anyOriginal;      /* site holds a tunable value: original not checked (revert restores what was there) */
    const char* name;          /* for the log, may be NULL */
} PSEBytePatch;

typedef enum PSEHookKind {
    PSE_HOOK_BYTES = 0,        /* PatchGroup / PatchBytes */
    PSE_HOOK_CALL_SITE = 1,    /* `call rel32` redirected */
    PSE_HOOK_FUNCTION = 2,     /* entry detour + trampoline */
    PSE_HOOK_INDIRECT_CALL = 3,/* `call [iat]` (6 bytes) */
    PSE_HOOK_POINTER = 4       /* a code pointer in memory (vtable slot, callback table, IAT slot) */
} PSEHookKind;

typedef struct PSEHookInfo {
    uint32_t size;
    PSEHookKind kind;
    PSEHookId id;              /* 0 for byte groups */
    PSEPluginHandle owner;
    const char* ownerName;
    const char* name;          /* group / hook name, may be NULL */
    uint32_t va;               /* site */
    uint32_t len;              /* bytes owned at va */
    int32_t priority;
    uint32_t chainPosition;    /* 0 = runs first */
} PSEHookInfo;

typedef struct PSEHooksInterface {
    uint32_t size;
    uint32_t version;  /* 1 */

    /* All-or-nothing group of byte patches (named; the name is the revert key per plugin). Every site must hold its
     * original bytes (or already the patched ones, or anything when anyOriginal). */
    PSEResult (PSE_CALL* PatchGroup)(PSEPluginHandle plugin, const char* group, const PSEBytePatch* patches,
                                     uint32_t count);
    /* Restores the bytes of the plugin's group as they were before PatchGroup. */
    PSEResult (PSE_CALL* RevertGroup)(PSEPluginHandle plugin, const char* group);

    /* `call rel32` at site (must currently reach expectedTarget, possibly through earlier subscribers). */
    PSEResult (PSE_CALL* HookCallSite)(PSEPluginHandle plugin, uint32_t site, uint32_t expectedTarget, void* fn,
                                       int32_t priority, void** next, PSEHookId* outId);
    /* Entry detour: prologueLen (>= 5) bytes at entry are moved into a trampoline (relative call/jmp/jcc in them
     * are relocated; anything that can't be moved fails with PSE_E_UNSUPPORTED). `next` reaches the trampoline or
     * the next subscriber. If expectedPrologue is not NULL the bytes must match it (before anyone hooked). */
    PSEResult (PSE_CALL* HookFunction)(PSEPluginHandle plugin, uint32_t entry, uint32_t prologueLen,
                                       const uint8_t* expectedPrologue, void* fn, int32_t priority, void** next,
                                       PSEHookId* outId);
    /* `call dword ptr [iatSlot]` at site (6 bytes) -> call fn; nop. */
    PSEResult (PSE_CALL* HookIndirectCall)(PSEPluginHandle plugin, uint32_t site, uint32_t iatSlot, void* fn,
                                           int32_t priority, void** next, PSEHookId* outId);
    /* A function pointer stored in memory (vtable slot, IAT slot, callback table). `slot` = its address. */
    PSEResult (PSE_CALL* HookPointer)(PSEPluginHandle plugin, uint32_t slot, void* fn, int32_t priority, void** next,
                                      PSEHookId* outId);
    /* Removes one hook; the chain closes around it. The site is restored when its last hook goes. */
    PSEResult (PSE_CALL* Unhook)(PSEPluginHandle plugin, PSEHookId id);

    /* Executable memory inside rel32 reach of the game image (stubs, caves). Freed at exit. NULL on failure. */
    void* (PSE_CALL* AllocCode)(PSEPluginHandle plugin, uint32_t size);

    /* Introspection: fills up to max entries, returns the total count. */
    uint32_t (PSE_CALL* ListHooks)(PSEHookInfo* out, uint32_t max);
    /* Owner of a byte, or 0. */
    PSEPluginHandle (PSE_CALL* OwnerOf)(uint32_t va);
} PSEHooksInterface;

/* ------------------------------------------------------------------------------------------------------------------
 * pse.messaging (version 1) - messages, events, cross-plugin interfaces
 * ------------------------------------------------------------------------------------------------------------------ */

#define PSE_MESSAGING_INTERFACE "pse.messaging"

/* Messages the loader ("pse") sends to every plugin. Game layers add their own from 0x1000. */
enum {
    PSE_MSG_POST_LOAD = 1,         /* every plugin is loaded: look up other plugins' interfaces now */
    PSE_MSG_POST_POST_LOAD = 2,
    PSE_MSG_GAME_ENTRY = 3,        /* just before the game's own entry point runs */
    PSE_MSG_EXIT = 4,
    PSE_MSG_NEW_GAME = 5,
    PSE_MSG_SAVE_BEGIN = 6,        /* data: const char* save path */
    PSE_MSG_SAVE_END = 7,
    PSE_MSG_LOAD_BEGIN = 8,
    PSE_MSG_LOAD_END = 9,
    /* Sent by the game layer (sender "pse"), see the game's reference page for when exactly: */
    PSE_MSG_FRONT_END_READY = 10,  /* first frame of a front-end level (menus up); data: const char* level name */
    PSE_MSG_DEVICE_CREATED = 11,   /* the D3D device was created; data: the device (not a copy), dataLen 0 */
    PSE_MSG_DEVICE_RESET = 12,     /* the D3D device was reset (alt-tab, mode change); data: the device, dataLen 0 */
    PSE_MSG_DEVICE_LOST = 13,      /* the D3D device was lost (reset follows); data: the device, dataLen 0 */
    PSE_MSG_GAME_FIRST = 0x1000    /* game layer messages, see the game's reference page */
};

typedef struct PSEMessage {
    uint32_t size;
    const char* sender;            /* plugin name, "pse" = loader, "pse.game" = game layer */
    uint32_t type;
    const void* data;
    uint32_t dataLen;
} PSEMessage;
typedef void(PSE_CALL* PSEMessageCallback)(const PSEMessage* msg, void* ctx);

/* Events: per-frame / game notifications with priorities. Built-in ids below; game layers and plugins can define
 * more by name (EventId). The callback's data layout is documented per event. */
typedef uint32_t PSEEventId;
enum {
    PSE_EVENT_FRAME = 1,           /* once per game frame, before update + render; data NULL */
    PSE_EVENT_PRE_RENDER = 2,
    PSE_EVENT_POST_RENDER = 3,
    PSE_EVENT_END_SCENE = 4,       /* data: the D3D device */
    PSE_EVENT_PRESENT = 5,         /* data: the D3D device */
    PSE_EVENT_LEVEL_LOAD_BEGIN = 6,/* data: const char* level name */
    PSE_EVENT_LEVEL_LOAD_END = 7,
    PSE_EVENT_LEVEL_UNLOAD = 8,
    PSE_EVENT_FIRST_DYNAMIC = 0x10000
};
typedef uint32_t PSESubscriptionId;
/* Return 0 to let later subscribers run; non-zero stops the event (only for events documented as cancellable). */
typedef int(PSE_CALL* PSEEventCallback)(PSEEventId event, void* data, void* ctx);

typedef struct PSEMessagingInterface {
    uint32_t size;
    uint32_t version;  /* 1 */

    /* sender NULL = from anyone. */
    PSEResult (PSE_CALL* RegisterListener)(PSEPluginHandle plugin, const char* sender, PSEMessageCallback cb, void* ctx);
    /* receiver NULL = broadcast to every listener of this plugin's messages. Synchronous. */
    PSEResult (PSE_CALL* Dispatch)(PSEPluginHandle plugin, uint32_t type, const void* data, uint32_t dataLen,
                                   const char* receiver);

    /* Cross-plugin calls: publish a table of function pointers under an id ("dsfix.online.session"). One table per
     * (id, version); higher versions should be supersets. Query finds the highest version >= minVersion. */
    PSEResult (PSE_CALL* RegisterInterface)(PSEPluginHandle plugin, const char* id, uint32_t version, const void* table);
    const void* (PSE_CALL* QueryInterface)(PSEPluginHandle plugin, const char* id, uint32_t minVersion);

    /* Events. */
    PSEEventId (PSE_CALL* EventId)(const char* name);   /* same name = same id; built-ins: "pse.frame", ... */
    PSESubscriptionId (PSE_CALL* Subscribe)(PSEPluginHandle plugin, PSEEventId event, PSEEventCallback cb, void* ctx,
                                            int32_t priority);
    PSEResult (PSE_CALL* Unsubscribe)(PSEPluginHandle plugin, PSESubscriptionId id);
    /* Runs the subscribers (higher priority first). Returns non-zero if one stopped it. */
    int (PSE_CALL* Raise)(PSEPluginHandle plugin, PSEEventId event, void* data);
} PSEMessagingInterface;

/* ------------------------------------------------------------------------------------------------------------------
 * pse.tasks (version 1)
 * ------------------------------------------------------------------------------------------------------------------ */

#define PSE_TASKS_INTERFACE "pse.tasks"
typedef void(PSE_CALL* PSETaskFn)(void* ctx);

typedef struct PSETasksInterface {
    uint32_t size;
    uint32_t version;  /* 1 */
    /* Any thread. Runs at the start of the next game frame (or later), on the game thread, in queue order. */
    PSEResult (PSE_CALL* RunOnGameThread)(PSEPluginHandle plugin, PSETaskFn fn, void* ctx);
    /* Any thread. Runs after `frames` more game frames (0 = next frame). */
    PSEResult (PSE_CALL* RunAfterFrames)(PSEPluginHandle plugin, uint32_t frames, PSETaskFn fn, void* ctx);
    int (PSE_CALL* IsGameThread)(void);
    uint32_t (PSE_CALL* FrameNumber)(void);   /* game frames since start */
} PSETasksInterface;

/* ------------------------------------------------------------------------------------------------------------------
 * pse.settings (version 1) - registry-backed, shown in the game's menus by the game layer. No ini files.
 * Stored under HKCU\Software\PSE\<gameId>\Plugins\<plugin name>.
 * ------------------------------------------------------------------------------------------------------------------ */

#define PSE_SETTINGS_INTERFACE "pse.settings"
typedef enum PSESettingType {
    PSE_SETTING_BOOL = 0, PSE_SETTING_INT = 1, PSE_SETTING_CHOICE = 2, PSE_SETTING_COLOUR = 3, PSE_SETTING_STRING = 4
} PSESettingType;
enum {
    PSE_SETTING_LIVE = 0x1,        /* takes effect at once (change callback); otherwise "restart the game" */
    PSE_SETTING_LAUNCHER = 0x2,    /* also shown in the launcher (games that have one) */
    PSE_SETTING_HIDDEN = 0x4,      /* not shown in menus (internal / dev) */
    PSE_SETTING_NEXT_LEVEL = 0x8   /* takes effect at the next level start */
};

typedef struct PSESettingDesc {
    uint32_t size;
    const char* key;               /* registry value name: [A-Za-z0-9_] */
    PSESettingType type;
    uint32_t flags;
    int32_t defaultInt;            /* bool / int / choice index / colour 0xRRGGBB */
    int32_t minInt, maxInt;        /* int */
    int32_t step;                  /* int, menu step (0 = 1) */
    const char* defaultString;     /* string */
    const char* label;             /* menu text (UTF-8, upper case by convention) */
    const char* help;              /* one line, may be NULL */
    const char* const* choices;    /* choice: NULL-terminated labels */
    const char* group;             /* menu sub-heading, may be NULL */
} PSESettingDesc;
typedef void(PSE_CALL* PSESettingChanged)(const char* key, void* ctx);

typedef struct PSESettingsInterface {
    uint32_t size;
    uint32_t version;  /* 1 */
    /* Declare once, in Load. Reads the stored value (or the default; out-of-range values are clamped). */
    PSEResult (PSE_CALL* Declare)(PSEPluginHandle plugin, const PSESettingDesc* desc);
    int32_t (PSE_CALL* GetInt)(PSEPluginHandle plugin, const char* key);          /* bool / int / choice / colour */
    PSEResult (PSE_CALL* SetInt)(PSEPluginHandle plugin, const char* key, int32_t value);
    /* Copies into buf (always NUL-terminated); returns the full length. */
    uint32_t (PSE_CALL* GetString)(PSEPluginHandle plugin, const char* key, char* buf, uint32_t bufLen);
    PSEResult (PSE_CALL* SetString)(PSEPluginHandle plugin, const char* key, const char* value);
    PSEResult (PSE_CALL* OnChange)(PSEPluginHandle plugin, PSESettingChanged cb, void* ctx);
    /* Enumeration for menus: fills *out with the i-th declared setting of `owner` (0 = any plugin, in order). */
    PSEResult (PSE_CALL* Describe)(PSEPluginHandle owner, uint32_t index, const PSESettingDesc** out,
                                   PSEPluginHandle* outOwner);
} PSESettingsInterface;

/* ------------------------------------------------------------------------------------------------------------------
 * pse.serialization (version 1) - co-save "<save>.pse" next to each game save
 * File: "PSE\x01" u32 chunkCount, chunks {u8 nameLen, name, u32 tag, u32 version, u32 length, data}. Chunks of
 * plugins that aren't loaded are kept when the save is rewritten.
 * ------------------------------------------------------------------------------------------------------------------ */

#define PSE_SERIALIZATION_INTERFACE "pse.serialization"
typedef struct PSESaveWriter PSESaveWriter;   /* opaque */
typedef struct PSESaveReader PSESaveReader;

typedef void(PSE_CALL* PSESaveCallback)(PSESaveWriter* w, void* ctx);
typedef void(PSE_CALL* PSELoadCallback)(PSESaveReader* r, void* ctx);
typedef void(PSE_CALL* PSERevertCallback)(void* ctx);   /* new game, or a save without this plugin's chunks */

typedef struct PSESerializationInterface {
    uint32_t size;
    uint32_t version;  /* 1 */
    PSEResult (PSE_CALL* SetCallbacks)(PSEPluginHandle plugin, PSESaveCallback save, PSELoadCallback load,
                                       PSERevertCallback revert, void* ctx);
    /* Writer: one call per chunk (several per plugin allowed, any tags). */
    PSEResult (PSE_CALL* WriteChunk)(PSESaveWriter* w, uint32_t tag, uint32_t version, const void* data, uint32_t len);
    /* Reader: walks this plugin's chunks in file order. Returns 0 when there are no more. */
    int (PSE_CALL* NextChunk)(PSESaveReader* r, uint32_t* tag, uint32_t* version, uint32_t* len);
    /* Reads up to len bytes of the current chunk; returns bytes read. */
    uint32_t (PSE_CALL* Read)(PSESaveReader* r, void* buf, uint32_t len);
} PSESerializationInterface;

/* Tag helper: PSE_TAG('V','E','H','1'). */
#define PSE_TAG(a, b, c, d) \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | ((uint32_t)(uint8_t)(c) << 8) | (uint32_t)(uint8_t)(d))

/* ==================================================================================================================
 * BEGIN pse.lua (version 1) - Lua functions for mission scripts. Reference: docs/sdk/reference/lua.md
 *
 * Plugins never link Lua and never see a lua_State: the game layer owns the one Lua 5.4 state of the running level
 * and marshals every call through the accessor functions below. A native registered as `RegisterFunction(plugin,
 * "alienfaction", "spawn", fn, ctx)` is called by scripts as `alienfaction.spawn(...)`.
 *
 * Values inside a call are addressed by SLOT numbers: 1..ArgCount() are the arguments (or, in a CallGlobal results
 * callback, the function's results); Get* / Next / New* return new slots; 0 = no value. Slots, and strings read from
 * them, are valid only until the callback returns. A native's results are the slots passed to Return() (or made by
 * Push*), in that order.
 *
 * Errors never unwind through plugin code: Check* functions and Error() only RECORD the error and return 0 /
 * PSE_LUA_ERROR; the game layer raises the Lua error after the native has returned. A native that crashes (external
 * plugins only) is disabled like any crashing plugin (reference/plugins.md "Crash guard") and the script gets a Lua
 * error instead. Game thread only.
 * ================================================================================================================== */

#define PSE_LUA_INTERFACE "pse.lua"

/* PSEResult of CallGlobal when the Lua function raised an error (LastError has the message). */
#define PSE_E_SCRIPT ((PSEResult)11)

typedef struct PSELuaCall PSELuaCall;   /* opaque; valid only during the callback it is passed to */
typedef int32_t PSELuaSlot;             /* 0 = none */

typedef enum PSELuaType {
    PSE_LUA_NONE = 0,       /* no such slot */
    PSE_LUA_NIL = 1,
    PSE_LUA_BOOLEAN = 2,
    PSE_LUA_INTEGER = 3,    /* a Lua integer (64-bit) */
    PSE_LUA_NUMBER = 4,     /* a Lua float */
    PSE_LUA_STRING = 5,
    PSE_LUA_TABLE = 6,
    PSE_LUA_FUNCTION = 7,
    PSE_LUA_OTHER = 8       /* userdata, thread */
} PSELuaType;

/* Native return values. */
#define PSE_LUA_RETURN 0    /* success: the Return()ed slots are the results */
#define PSE_LUA_ERROR 1     /* failure: Error()'s message (or "failed") becomes a Lua error */

/* A Lua function implemented by a plugin; also the shape of CallGlobal's argument / result callbacks. */
typedef int(PSE_CALL* PSELuaNative)(PSELuaCall* call, void* ctx);

/* Length argument of NewString / PushString meaning "NUL-terminated". */
#define PSE_LUA_NUL 0xFFFFFFFFu

/* Events (pse.messaging, raised by the game layer; get the id with EventId(name)):
 *   "pse.lua.level_start"  a level's script state is ready and its init() has run; data = const char* level name.
 *                          CallGlobal works from here on.
 *   "pse.lua.level_end"    the state is about to be closed (level change / exit); data = const char* level name. */
#define PSE_LUA_EVENT_LEVEL_START "pse.lua.level_start"
#define PSE_LUA_EVENT_LEVEL_END "pse.lua.level_end"

typedef struct PSELuaInterface {
    uint32_t size;
    uint32_t version;  /* 1 */

    /* ---- registration (game thread; PSEPlugin_Load is the usual place) ----
     * Claims `ns` for the plugin: one namespace per plugin, "name" or dotted "a.b" (each part [a-z_][a-z0-9_]*, no
     * leading '_', <= 64 characters in all). Refused: reserved names (game, dev, pse, Lua's libraries and base
     * functions, keywords, the mission callbacks init / update / on_message) -> PSE_E_INVALID_ARG; another plugin's
     * namespace, or a prefix / extension of it -> PSE_E_CONFLICT; a second namespace of the same plugin ->
     * PSE_E_CONFLICT. Claiming the plugin's own namespace again -> PSE_OK. */
    PSEResult (PSE_CALL* RegisterNamespace)(PSEPluginHandle plugin, const char* ns);
    /* Adds ns.name (the namespace is claimed as by RegisterNamespace if needed). name: [A-Za-z_][A-Za-z0-9_]*.
     * Already registered -> PSE_E_EXISTS. Scripts see it from the next level start; a level already running gets it
     * at its next update. */
    PSEResult (PSE_CALL* RegisterFunction)(PSEPluginHandle plugin, const char* ns, const char* name, PSELuaNative fn,
                                           void* ctx);

    /* ---- reading values ---- */
    uint32_t (PSE_CALL* ArgCount)(PSELuaCall* call);
    PSELuaType (PSE_CALL* Type)(PSELuaCall* call, PSELuaSlot slot);
    /* Check*: 1 + *out on success. 0 when the value has the wrong type: the error "bad argument #n to 'ns.fn'
     * (integer expected, got string)" is recorded - return PSE_LUA_ERROR. Conversions as Lua's luaL_check*: numbers
     * with an integral value and numeric strings pass as integers, numeric strings as numbers, numbers as strings. */
    int (PSE_CALL* CheckInteger)(PSELuaCall* call, PSELuaSlot slot, int64_t* out);
    int (PSE_CALL* CheckNumber)(PSELuaCall* call, PSELuaSlot slot, double* out);
    /* *out: UTF-8 bytes, NUL-terminated (may contain more NULs: *len, may be NULL, is the full length). */
    int (PSE_CALL* CheckString)(PSELuaCall* call, PSELuaSlot slot, const char** out, uint32_t* len);
    int (PSE_CALL* CheckTable)(PSELuaCall* call, PSELuaSlot slot);
    /* Lua truth: nil / false / no value = 0, anything else 1. Never an error. */
    int (PSE_CALL* ToBoolean)(PSELuaCall* call, PSELuaSlot slot);
    /* Tables (raw access: no metamethods). Length = border (#t); 0 for non-tables. Get* = the value as a new slot
     * (nil when missing; 0 when `table` is not a table). Next: start with *key = 0; returns 1 with new key / value
     * slots, 0 at the end (order as Lua's next). */
    uint32_t (PSE_CALL* Length)(PSELuaCall* call, PSELuaSlot table);
    PSELuaSlot (PSE_CALL* GetField)(PSELuaCall* call, PSELuaSlot table, const char* key);
    PSELuaSlot (PSE_CALL* GetIndex)(PSELuaCall* call, PSELuaSlot table, int64_t index);
    int (PSE_CALL* Next)(PSELuaCall* call, PSELuaSlot table, PSELuaSlot* key, PSELuaSlot* value);

    /* ---- making values (new slots; 0 on failure) ---- */
    PSELuaSlot (PSE_CALL* NewNil)(PSELuaCall* call);
    PSELuaSlot (PSE_CALL* NewBoolean)(PSELuaCall* call, int value);
    PSELuaSlot (PSE_CALL* NewInteger)(PSELuaCall* call, int64_t value);
    PSELuaSlot (PSE_CALL* NewNumber)(PSELuaCall* call, double value);
    PSELuaSlot (PSE_CALL* NewString)(PSELuaCall* call, const char* s, uint32_t len);   /* len PSE_LUA_NUL = strlen */
    PSELuaSlot (PSE_CALL* NewTable)(PSELuaCall* call);
    /* table[key] = value (raw). 1 = done, 0 = `table` not a table / bad slot. */
    int (PSE_CALL* SetField)(PSELuaCall* call, PSELuaSlot table, const char* key, PSELuaSlot value);
    int (PSE_CALL* SetIndex)(PSELuaCall* call, PSELuaSlot table, int64_t index, PSELuaSlot value);

    /* ---- results (in a native) / arguments (in a CallGlobal argument callback) ----
     * Return appends a slot (an argument slot is fine). Push* = New* + Return. 1 = done. */
    int (PSE_CALL* Return)(PSELuaCall* call, PSELuaSlot slot);
    int (PSE_CALL* PushNil)(PSELuaCall* call);
    int (PSE_CALL* PushBoolean)(PSELuaCall* call, int value);
    int (PSE_CALL* PushInteger)(PSELuaCall* call, int64_t value);
    int (PSE_CALL* PushNumber)(PSELuaCall* call, double value);
    int (PSE_CALL* PushString)(PSELuaCall* call, const char* s, uint32_t len);
    PSELuaSlot (PSE_CALL* PushTable)(PSELuaCall* call);   /* new table, returned; fill it with SetField / SetIndex */

    /* ---- errors ----
     * Records the message (printf-style; the first recorded error wins) and returns PSE_LUA_ERROR, so a native can
     * `return lua->Error(call, "no unit %d", id);`. The script sees "ns.fn: message (plugin <name>)". */
    int (PSE_CALL* Error)(PSELuaCall* call, const char* fmt, ...);
    /* "ns.fn" of the native being called ("" in CallGlobal callbacks). */
    const char* (PSE_CALL* FunctionName)(PSELuaCall* call);

    /* ---- calling the mission script ----
     * Calls the level script's global function `fn` (game thread; also from inside a native). `args` (may be NULL)
     * supplies the arguments with New* + Return / Push*; `results` (may be NULL) reads the results as slots
     * 1..ArgCount. Both are called with ctx. Returns PSE_OK; PSE_E_WRONG_PHASE (no Lua level running, not the game
     * thread); PSE_E_NOT_FOUND (no such global function); PSE_E_INVALID_ARG (`args` failed / returned
     * PSE_LUA_ERROR - the function is not called); PSE_E_SCRIPT (the function raised an error; the script keeps
     * running); PSE_E_ACCESS (a callback crashed: the plugin is disabled). */
    PSEResult (PSE_CALL* CallGlobal)(PSEPluginHandle plugin, const char* fn, PSELuaNative args, PSELuaNative results,
                                     void* ctx);
    /* The plugin's last CallGlobal error message (with the Lua traceback). Copies into buf (NUL-terminated),
     * returns the full length; "" after a successful call. */
    uint32_t (PSE_CALL* LastError)(PSEPluginHandle plugin, char* buf, uint32_t bufLen);
    /* 1 while a level's script state exists (a .lua level), else 0. */
    int (PSE_CALL* ScriptRunning)(void);
} PSELuaInterface;

/* ==================================================================================================================
 * END pse.lua
 * ================================================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* PSE_API_H */
