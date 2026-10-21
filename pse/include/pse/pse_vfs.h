/*
 * Pivotal Script Extender (PSE) - pse.vfs (version 1): game files by name, from any provider.
 *
 * Reference: docs/sdk/reference/vfs.md. Design: docs/internal/DESIGN.md §5.9, §12. Changes: docs/sdk/API_CHANGELOG.md.
 * Same ABI rules as pse_api.h (plain C, 32-bit x86, __cdecl, UTF-8, each side frees its own memory - except ReadAlloc,
 * whose buffer the plugin gives back with Free).
 *
 * A PROVIDER answers "do you have this file, and where are its bytes?" for a file name (or the game's name hash) in a
 * CONTEXT - for Conflict: Desert Storm the archive the game is looking in ("mission1.dat"). Providers are asked in
 * priority order (higher first; equal = registration order); the first that answers wins. The game layer hooks the
 * game's own file lookups into pse.vfs, so a provider can serve files the game would load from its archives (the asset
 * store, generated files). Mod folders keep priority over every provider (they are resolved before pse.vfs is asked).
 *
 * Threads: every function may be called from any thread; providers must be thread-safe (the game loads files on its
 * main thread, tools / plugins may ask from others). Callbacks into EXTERNAL plugins run under the crash guard.
 */
#ifndef PSE_VFS_H
#define PSE_VFS_H

#include "pse_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSE_VFS_INTERFACE "pse.vfs"
#define PSE_VFS_VERSION 1u

typedef uint32_t PSEVfsProviderId;   /* 0 = none */

/* PSEVfsQuery.flags */
enum {
    PSE_VFS_EXACT = 0x1   /* only the context's own copy: providers must not fall back to other archives / levels */
};

typedef struct PSEVfsQuery {
    uint32_t size;          /* sizeof(PSEVfsQuery) */
    const char* name;       /* file name ("T72M1.EVO"; folders are ignored by DS1). May be NULL when nameHash is set. */
    uint32_t nameHash;      /* the game's name hash; 0 = computed from `name` with the registered hash function */
    const char* context;    /* where the game is looking: DS1 = archive file name ("mission1.dat"); NULL = default */
    uint32_t flags;         /* PSE_VFS_* */
} PSEVfsQuery;

typedef struct PSEVfsLocation {
    uint32_t size;          /* sizeof(PSEVfsLocation), set by the caller */
    char path[520];         /* UTF-8 path of the disk file that holds the bytes; "" = only readable with Read */
    uint64_t offset;        /* where the file's bytes start inside `path` */
    uint32_t length;        /* the file's size in bytes */
    PSEPluginHandle provider;   /* filled by pse.vfs: the plugin whose provider answered */
    char source[64];        /* provider's note, e.g. the archive it really came from ("mission3.dat"); may be "" */
} PSEVfsLocation;

typedef struct PSEVfsProvider {
    uint32_t size;          /* sizeof(PSEVfsProvider) */
    const char* name;       /* for logs / listings ("ds1.store"); copied */
    /* 1 = I have it: fill *out (path + offset + length, or path "" + length when only Read can serve it). 0 = no.
     * The query's nameHash is always filled in (computed when the asker passed 0 and a hash function is set). */
    int (PSE_CALL* Locate)(const PSEVfsQuery* q, PSEVfsLocation* out, void* ctx);
    /* Optional (NULL = pse.vfs reads Locate's path). Copies up to bufLen bytes of the file from `offset` into buf;
     * returns the bytes copied, or -1 when it doesn't have the file. */
    int32_t (PSE_CALL* Read)(const PSEVfsQuery* q, uint32_t offset, void* buf, uint32_t bufLen, void* ctx);
} PSEVfsProvider;

/* The game's file-name hash (DS1: LFSR over the upper-cased bare name). */
typedef uint32_t(PSE_CALL* PSEVfsHashFn)(const char* name);

typedef struct PSEVfsProviderInfo {
    uint32_t size;          /* sizeof(PSEVfsProviderInfo), set by the caller */
    PSEVfsProviderId id;
    PSEPluginHandle owner;
    int32_t priority;
    const char* name;       /* valid until the provider is unregistered */
} PSEVfsProviderInfo;

typedef struct PSEVfsInterface {
    uint32_t size;
    uint32_t version;  /* 1 */

    /* Adds a provider (the struct is copied; ctx is passed back to its callbacks). Any phase, any thread.
     * PSE_E_INVALID_ARG: no Locate / bad size. */
    PSEResult (PSE_CALL* RegisterProvider)(PSEPluginHandle plugin, const PSEVfsProvider* provider, int32_t priority,
                                           void* ctx, PSEVfsProviderId* outId);
    /* PSE_E_NOT_FOUND: not this plugin's provider. A plugin's providers are also removed when it is disabled. */
    PSEResult (PSE_CALL* UnregisterProvider)(PSEPluginHandle plugin, PSEVfsProviderId id);

    /* 1 = some provider has the file. */
    int (PSE_CALL* Exists)(PSEPluginHandle plugin, const PSEVfsQuery* q);
    /* Where its bytes are (the first provider that answers). PSE_E_NOT_FOUND = no provider has it. */
    PSEResult (PSE_CALL* ResolvePath)(PSEPluginHandle plugin, const PSEVfsQuery* q, PSEVfsLocation* out);
    /* Reads the whole file into buf. *outLen = its size (always, when found). PSE_E_NO_MEMORY = buf NULL or smaller
     * than the file (nothing copied), PSE_E_NOT_FOUND, PSE_E_ACCESS = the provider's file couldn't be read. */
    PSEResult (PSE_CALL* Read)(PSEPluginHandle plugin, const PSEVfsQuery* q, void* buf, uint32_t bufLen,
                               uint32_t* outLen);
    /* Same, into a buffer pse.vfs allocates; give it back with Free. */
    PSEResult (PSE_CALL* ReadAlloc)(PSEPluginHandle plugin, const PSEVfsQuery* q, void** outData, uint32_t* outLen);
    void (PSE_CALL* Free)(void* data);

    /* The game's name hash, set once by the game layer / host (PSE_E_EXISTS after the first). NameHash returns 0 while
     * none is set. */
    PSEResult (PSE_CALL* SetNameHash)(PSEPluginHandle plugin, PSEVfsHashFn fn);
    uint32_t (PSE_CALL* NameHash)(const char* name);

    /* Providers in asking order; fills up to max, returns the total. */
    uint32_t (PSE_CALL* ListProviders)(PSEVfsProviderInfo* out, uint32_t max);
} PSEVfsInterface;

#ifdef __cplusplus
}
#endif

#endif /* PSE_VFS_H */
