# pse.vfs (version 1) – game files from providers

Header: [`pse/include/pse/pse_vfs.h`](../../../pse/include/pse/pse_vfs.h) (includes `pse_api.h`). Loader:
`pse/loader/vfs.cpp`.

A **provider** answers "do you have this file, and where are its bytes?" for a file name (or the game's name hash) in a
**context** – for Conflict: Desert Storm the archive the game is looking in (`"mission1.dat"`). The game layer hooks the
game's own file lookups into `pse.vfs`, so a provider can serve a file the game would otherwise read from its archives:
the asset store, generated files, files a plugin makes up at run time.

```c
const PSEVfsInterface* vfs = (const PSEVfsInterface*)pse->Query(pse->plugin, PSE_VFS_INTERFACE, 1);
```

## Rules

- **Order**: providers are asked by priority, higher first; equal priority = registration order. The first that
  answers wins; the others are not asked.
- **Mods first**: files in `<game>\Mods\…` are resolved before `pse.vfs` is asked (DS1: the mod resolver hooks the
  game's name resolver `FUN_004be810`, which runs before any archive lookup). A provider can't hide a mod's file.
- **Name hash**: the game layer sets the game's file-name hash once (`SetNameHash`; DS1: LFSR over the upper-cased
  bare name, `FUN_004b9a10`). Queries that give only a name get the hash filled in before providers see them, so a
  provider can work from hashes alone (DS1 archives store no names).
- **Threads**: every function may be called from any thread. Providers must be thread-safe.
- **Crash guard**: an EXTERNAL plugin's `Locate` / `Read` run under SEH; a fault disables the plugin (its providers go
  with it) – [plugins.md "Crash guard"](plugins.md). Internal (sub-)plugins are called directly, like every service.
- **Disabled / unloaded plugins**: their providers are removed (and the name hash, if they set it).

## Types

| Type | Fields |
| --- | --- |
| `PSEVfsQuery` | `size`; `name` (file name, folders ignored by DS1; may be NULL when `nameHash` is set); `nameHash` (0 = computed from `name`); `context` (DS1: archive file name; NULL = the game layer's default = the archive the game is using now); `flags` (`PSE_VFS_EXACT` = only the context's own copy, no fallback to other archives) |
| `PSEVfsLocation` | `size` (caller sets it); `path` (UTF-8 disk file holding the bytes, `""` = only through `Read`); `offset`, `length` (the file's bytes inside `path`); `provider` (filled by pse.vfs); `source` (provider's note: e.g. the archive the bytes really came from) |
| `PSEVfsProvider` | `size`; `name` (copied, for logs); `Locate(q, out, ctx)` → 1 = have it (fill `out`); `Read(q, offset, buf, len, ctx)` optional → bytes copied or -1 (NULL = pse.vfs reads `Locate`'s path) |
| `PSEVfsProviderInfo` | `size`, `id`, `owner`, `priority`, `name` (ListProviders) |

## Functions

| Function | Returns | Notes |
| --- | --- | --- |
| `RegisterProvider(plugin, provider, priority, ctx, &id)` | `PSE_OK`; `PSE_E_INVALID_ARG` (no `Locate`, bad size, unknown plugin) | Any phase, any thread. The struct is copied; `ctx` is passed back to its callbacks. |
| `UnregisterProvider(plugin, id)` | `PSE_OK`; `PSE_E_NOT_FOUND` (not this plugin's) | |
| `Exists(plugin, q)` | 1 / 0 | 0 also for a bad query. |
| `ResolvePath(plugin, q, &loc)` | `PSE_OK`; `PSE_E_NOT_FOUND`; `PSE_E_INVALID_ARG` | Where the bytes are: `path` + `offset` + `length`. The DS1 store answers with `store.pack` and the blob's offset. |
| `Read(plugin, q, buf, bufLen, &len)` | `PSE_OK`; `PSE_E_NO_MEMORY` (buf NULL / too small: `len` = the size, nothing copied); `PSE_E_NOT_FOUND`; `PSE_E_ACCESS` (the provider's file couldn't be read) | Whole file. |
| `ReadAlloc(plugin, q, &data, &len)` | as `Read` | Buffer from the process heap; give it back with `Free(data)`. |
| `Free(data)` | – | NULL is fine. |
| `SetNameHash(plugin, fn)` | `PSE_OK`; `PSE_E_EXISTS` (already set) | Game layer / host, once. |
| `NameHash(name)` | the hash, 0 while none is set | |
| `ListProviders(out, max)` | total count | Asking order. `out[i].size` must be set. |

## Example: a provider for generated files

```c
static int PSE_CALL MyLocate(const PSEVfsQuery* q, PSEVfsLocation* out, void* ctx) {
    if (!q->name || _stricmp(q->name, "MYTABLE.TXT") != 0) return 0;
    out->path[0] = 0;            /* no disk file: served by MyRead */
    out->length = (uint32_t)strlen(g_table);
    return 1;
}
static int32_t PSE_CALL MyRead(const PSEVfsQuery* q, uint32_t off, void* buf, uint32_t len, void* ctx) {
    uint32_t n = (uint32_t)strlen(g_table);
    if (off > n) return 0;
    if (len > n - off) len = n - off;
    memcpy(buf, g_table + off, len);
    return (int32_t)len;
}
...
PSEVfsProvider p = {sizeof p, "example.tables", &MyLocate, &MyRead};
vfs->RegisterProvider(pse->plugin, &p, 10, NULL, NULL);
```

**DS1 limitation (stage 1)**: the game's archive opener can only read a file that sits in a disk file (it opens the
file through its own disk branch and seeks), so in the game a provider must give a `path`; `Read`-only providers are
reached through `pse.vfs` calls (plugins, tools) but not yet by the game's own loads.

## In Conflict: Desert Storm

Nothing in the shipped game asks `pse.vfs` yet: the hooks are part of the asset store, a player setting
(SHARED ASSET STORE, default off; dev `Dev\PseStore`). With it on, the store is the provider `ds1.store` (priority 0,
owner `dsfix.assetstore`) and the game's current-archive lookup `FUN_004be360` + opener `FUN_004bd8a0` ask `pse.vfs`
first, with `context` = the archive the game is using.

Not in v1: `MergedDir`, table-merge helpers (DesertStormFix's `mods.cpp` still merges tables itself).
