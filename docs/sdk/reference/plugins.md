# Plugins: files, manifest, load order, lifecycle

How the loader finds, checks, orders, loads and unloads external plugin DLLs. Header: `pse/pse_api.h` (exports,
`PSEPluginInfo`, `PSELoadInterface`). Loader side: `pse/loader/plugins.cpp`, run by `PSE\pse_loader.dll` at the
exe's entry point ([layout.md](../layout.md)). API 0 = unstable. Walk-through: [guides/first-plugin.md](../guides/first-plugin.md).

## Files

A plugin is two files in `<game>\PSE\Plugins\`, with the same name:

| File | What |
| --- | --- |
| `<Name>.pse` | the manifest (text, below) – read before any plugin code runs |
| `<Name>.dll` | the 32-bit plugin DLL |

`<Name>` is usually the plugin's name (`example.hello.pse` + `example.hello.dll`), but only the pairing matters: the
plugin's identity is the `name` inside. Files are processed in file-name order; a `.pse` without its `.dll` is
refused ("no X.dll next to the manifest"). A missing `PSE\Plugins` folder is fine: no plugins.

## Manifest (`<Name>.pse`)

```text
# comment – also after a value
name       = dsfix.online
requires   = dsfix.splitscreen >= 1.0, pse.game.ds1 >= 1.0
optional   = dsfix.discord
conflicts  = someone.otheronline
after      = dsfix.splitscreen
```

| Key | Value | Meaning |
| --- | --- | --- |
| `name` | plugin name | Required, once. `[a-z0-9_.-]`, 1–64 characters. Must equal `PSEPlugin_Info.name`. |
| `requires` | `name [>= version]`, comma-separated | Must be loaded (built in or external), at that version or newer, or this plugin isn't loaded. Loaded first. |
| `optional` | names | Loaded first **if** present; nothing happens when missing. |
| `conflicts` | names | If any of them is present (built in or external), **this** plugin is not loaded. |
| `after` | names | Load order only: after them if present. |
| `host` | `true` / `false` | **Private** - only for PSE's own host plugins ([below](#host-plugins)). Anyone else: leave it out. |

- Keys are case-insensitive; `requires` / `optional` / `conflicts` / `after` may repeat (lists add up); empty list
  items are skipped. Unknown keys are logged as a warning and ignored (newer manifests stay readable).
- Versions: `major[.minor[.patch]]` (major ≤ 255, minor / patch ≤ 4095) = `PSE_MAKE_VERSION`. Only `>=` exists.
- Syntax errors (no `name`, `name` twice, bad name, a line without `=`, an operator other than `>=`, a bad version)
  refuse the plugin with the line number. UTF-8, optional BOM, CRLF or LF.

## Exports

```c
__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info;                    /* data, required */
__declspec(dllexport) int  PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse);    /* required: non-zero = loaded */
__declspec(dllexport) void PSE_CALL PSEPlugin_Unload(void);                         /* optional */
```

In C++ put them in `extern "C" { … }` **and keep `extern` on the info**: inside a braced `extern "C"` block a
`const` variable without `extern` has internal linkage and is silently not exported (the plugin is then refused:
"the DLL has no PSEPlugin_Info export"). `PSE_CALL` = `__cdecl`; the export names are undecorated.

`PSEPluginInfo` fields the loader checks:

| Field | Rule |
| --- | --- |
| `size` | ≥ `sizeof(PSEPluginInfo)` (36) |
| `infoVersion` | 1 |
| `name` | = the manifest's `name` |
| `version` | used for `requires >= …` checks; must be the same in the file and in the loaded DLL |
| `minApiVersion` | ≤ the loader's `PSE_API_VERSION` |
| `games` | NULL (any game) or a NULL-terminated list that contains the running game id (`"ds1"` …) |
| `flags` | `PSE_PLUGIN_SIMULATION` / `PSE_PLUGIN_CLIENT_ONLY` / `PSE_PLUGIN_USES_RAW_ADDRESSES`; `PSE_PLUGIN_INTERNAL` is never set for a DLL |

The info is read **from the file** before the DLL is loaded: the DLL is mapped as an image resource
(`LOAD_LIBRARY_AS_IMAGE_RESOURCE` – no `DllMain`, no code), and the export's bytes and the strings it points at are
read from the mapping. So the info must be constant data (no values computed at run time), and every string /
array it points to must be inside the DLL image.

## Loading (`pse::LoadPlugins`)

Called once by the loader's start-up (`pse::Start`, [layout.md](../layout.md)), on the main thread, after the game
layer is initialised: `<game>\PSE\Plugins`. Two rounds: first the [host plugins](#host-plugins), then every other
plugin - each round goes through the steps below, the second one resolved against everything registered by then (a
plugin may `require` a host's sub-plugin such as `dsfix.hud`).

1. **Read.** Every `*.pse` is parsed and its DLL's `PSEPlugin_Info` read from the file. Anything wrong → refused.
2. **Resolve** (no plugin code has run yet):
   - two manifests with the same name → both refused; a name a built-in plugin already has → refused;
   - `requires` missing / not loaded / older than asked → refused (repeats until nothing changes, so a plugin that
     requires a refused plugin is refused too);
   - `conflicts`: a plugin naming a present plugin is refused (two that name each other: both);
   - **order**: topological over `requires`, `optional` (if present) and `after` (if present); among plugins that are
     free to go, by name (byte order). Plugins on a cycle are refused ("dependency cycle: a -> b -> c -> a"), and
     whoever `requires` them; plugins that are only ordered `after` / `optional` a cycle member still load.
3. **Load**, in that order, one at a time:
   - a `requires` that failed to load in this step → refused ("requires X, which failed to load");
   - `LoadLibraryExW(path, LOAD_WITH_ALTERED_SEARCH_PATH)` (the plugin's own folder is searched for its
     dependencies); the exports are read again and must match what was checked;
   - the plugin is registered (same registry as built-in plugins, `PSE_PLUGIN_INTERNAL` clear) and gets its handle;
   - `PSEPlugin_Load(pse)` runs inside SEH. **Return 0 or crash = failed** (see below).
4. **`PSE_MSG_POST_LOAD`**, then **`PSE_MSG_POST_POST_LOAD`**, to every listener (sender `"pse"`) – also when no
   plugin was found.

### What `PostLoad` means

Every plugin's `Load` has returned and every interface that will be registered at start-up is registered. This is
where a plugin looks up other plugins' interfaces (`QueryInterface`): during its own `Load`, plugins later in the
order haven't run yet. `PostPostLoad` follows for work that needs what others did in `PostLoad`. Both are delivered under
the crash guard (below): a plugin that faults in its `PostLoad` listener is disabled and never gets `PostPostLoad`.

### Failure handling

A plugin whose `PSEPlugin_Load` returns 0 or raises an exception (access violation, C++ exception, stack overflow …):

- every hook and byte group it registered is removed (`ListHooks` + `Unhook` / `RevertGroup`), and its listeners,
  event subscriptions, registered interfaces, queued tasks and co-save callbacks are dropped;
- its handle is disabled: never handed out again, still named in logs, invisible to `FindPlugin` / `Query`; its
  chunks in co-saves are kept like those of an uninstalled plugin;
- the DLL is freed – unless a hook couldn't be removed (bytes changed by someone else), then it stays in memory and a
  warning says so;
- the reason is logged and listed: "crashed in PSEPlugin_Load: exception 0xC0000005 at example.bad_crash.dll+0x1A2B"
  or "PSEPlugin_Load returned 0 (the plugin's log says why)". The game continues.

A crash in `DllMain` is caught the same way ("crashed while loading the DLL"). Plugins that `requires` a failed one
are refused.

## Crash guard (callbacks after `Load`)

Every call the loader makes into an **external** plugin's code runs inside SEH (one `__try` per call,
`pse/loader/guard.cpp`):

| Call | Reported as "crashed in ..." |
| --- | --- |
| `PSEPlugin_Load` | "PSEPlugin_Load" (see Failure handling) |
| message listeners, incl. `PSE_MSG_POST_LOAD` / `PSE_MSG_POST_POST_LOAD` | "its PSE_MSG_POST_LOAD listener", "a message listener (type 0x4242 from kyle.x)" |
| event subscribers (`pse.frame`, `pse.present`, plugin events ...) | "a pse.frame event subscriber" |
| `pse.tasks` tasks | "a pse.tasks task" |
| `pse.settings` `OnChange` callbacks | "a settings change callback (Key)" |
| `pse.serialization` save / load / revert callbacks | "its co-save save callback" ... |
| `pse.lua` natives, `CallGlobal` argument / result callbacks | "the Lua function ns.fn" – the script gets a Lua error ([lua.md](lua.md#errors)) |
| `PSEPlugin_Unload` | "PSEPlugin_Unload" (logged + reported; the unload goes on) |

A fault in any of them (access violation, stack overflow, an escaping C++ exception ...) ends that one call and the
plugin is **disabled exactly like a failed `Load`**: its hooks and byte groups are removed, its listeners,
subscriptions, registered interfaces, queued tasks, settings rows / change callbacks and co-save callbacks dropped, its
handle disabled; `PluginResults()` shows it `failed` with the reason ("crashed in a pse.frame event subscriber:
exception 0xC0000005 at kyle.x.dll+0x1A2B"), the log has an ERROR line and the mod report a `[fail]` line. The game
continues:

- the delivery in progress goes on: the other subscribers of that event / listeners of that message / due tasks still
  run; the crashed plugin's own later callbacks in the same round are skipped;
- a crashed subscriber's return value is not used (it never stops the event);
- safe from inside nested calls (a plugin's frame callback raising its own event whose subscriber faults) and from
  other threads (settings changes / messages sent off the game thread): every delivery walks a snapshot and checks the
  subscription's `alive` flag / the plugin's disabled flag before each external call;
- **the DLL stays mapped** (unlike a failed `Load`): code of the plugin can still be on a stack - an outer callback of
  the same plugin further up, or a thread it started - and unmapping it would crash the game. It is not unloaded at
  exit either (no `PSEPlugin_Unload` call into code that faulted); the process end frees it;
- chunks a crashed save callback already wrote stay in that co-save.

**Built-in plugins** (`PSE_PLUGIN_INTERNAL`, DesertStormFix's own `dsfix.*` features) are called directly - no guard,
no overhead, unchanged behaviour (a crash in our own code is a bug to fix, not a plugin to switch off).

**Cost**, measured by `pse_tests` ("crash guard - cost of PSE_EVENT_FRAME with 5 subscribers"): ~108 ns per Raise
when the 5 subscribers are built in, ~118 ns when they are external and guarded - about 2 ns per guarded call (x86
frame-based SEH: an exception registration record pushed and popped).

**Not guarded** (can't be):

- **hooks**: the game calls a plugin's detour directly (through the broker's chain); there is no loader frame around
  it. A fault there is a game crash - keep detours small and tested, use an event where one exists;
- **cross-plugin interface calls** (`QueryInterface` tables): plugin A calls plugin B's function directly. A fault
  inside B unwinds to the guard around A's callback, so **A is blamed and disabled** (the reason names the faulting
  module, e.g. "at kyle.b.dll+0x40"). Plugins that offer interfaces should check their inputs;
- threads a plugin starts itself.

SEH unwinding skips C++ destructors in the frames between the fault and the guard, so a fault can leak memory the
plugin held at that moment; the loader holds none of its locks while it calls a plugin.

## Host plugins

**Private, not part of the SDK.** A host plugin is a plugin PSE ships itself that needs the loader's internals - the
phase-1 single DLL's code moved into a plugin: DesertStormFix's `dsfix.dll`. It differs from an ordinary plugin in:

| | Host plugin | Ordinary plugin |
| --- | --- | --- |
| Gets `pse.host` (version 0, `pse/loader/host_api.h`) through its own `Query` | yes | no (`NULL`) |
| Load order | round 1, before every other plugin | round 2 |
| May register sub-plugins (`pse.host` `RegisterInternalPlugin`) | yes: `PSE_PLUGIN_INTERNAL`, called without the crash guard, strings copied, parent recorded | - |
| Online plugin-set check | compared as **built in** (name + version + the host's simulation fingerprint), not by DLL hash - a dev build and the release package of the same source connect | external SIMULATION plugins: DLL SHA-256 |
| Load fails / crashes | its sub-plugins are retired with it; **its DLL stays mapped** (raw patches may point into it) | DLL freed |
| Exit (`Shutdown`) | `PSEPlugin_Unload` runs, nothing is removed or freed | unloaded + freed |

**Who is one** - both must hold:

1. its manifest says `host = true`, and
2. its name is on the game layer's list (`PSEGame_Info.hostPlugins`; the DS1 layer: `dsfix` only; the DS2 layer: `ds2fix` only).

`host = true` with a name not on the list → refused ("host = true, but the game layer doesn't allow X as a host
plugin"). A listed name without `host = true` loads as an ordinary plugin and gets no `pse.host` (DesertStormFix's
Load then fails: "got no pse.host"). This is a guard against accidents (a third-party plugin depending on unstable
internals), **not a security boundary**: every DLL in `PSE\Plugins` runs code in the game process anyway.

**What `pse.host` holds**: the loader's service tables, log / report sinks (lines logged before the sink was set are
handed to it), sub-plugin registration, registry queries (`PluginInfo`, `PluginHost`, `PluginsInOrder`, load
results), `Frame` / `Broadcast` / `RaiseEvent` (the host drives them from its game hooks), the co-save calls and
`Shutdown`. The game layer gets the same table (it is registered by the loader with host rights). C++ code uses it
through `pse/hostlink` (static library `pse_hostlink`): the declarations of `pse/loader/loader.h`, forwarded - the
DesertStormFix feature sources call `pse::FindPlugin`, `pse::HooksInterface()`, `pse::cosave::Save` … unchanged. Never
link `pse_hostlink` and `pse_core` together.

## Unloading (`pse::UnloadPlugins`)

At exit, after the host's `PSE_MSG_EXIT`: in **reverse load order** (dependents before their dependencies) each
plugin's `PSEPlugin_Unload` runs (SEH-guarded: a crash is logged and reported and the result's reason says so), then
everything it registered is removed as above and its DLL freed (host plugins: `Unload` only). Plugins without `PSEPlugin_Unload` are just cleaned
up. A plugin that crashed earlier (crash guard) gets no `Unload` call and keeps its DLL mapped.

## Results

`pse::PluginResults()` (loader-internal, for menus and the console) = one entry per manifest: `name`, `displayName`,
`file`, `version`, `flags`, `state` (`loaded` / `refused` / `failed` / `unloaded`), `reason`, `handle` (0 = never
registered). A plugin that crashes in a callback after loading switches from `loaded` to `failed` with the reason. Loaded and failed plugins come in load order, refused ones after them by name.

Console (dev builds, or release with DEVELOPER CONSOLE on): `pse plugins` lists built-in and external plugins with
version, kind, state and reason; `pse hooks [text]` lists the broker's records (kind, owner, group name or hook id,
address, length, priority, chain position), filtered by owner, name, address or kind.

## Limits (API 0)

- A failed or unloaded plugin's settings declarations and change callbacks are dropped (its stored values stay in
  the registry for the next start).
- Hooks and cross-plugin interface calls are not SEH-guarded (see Crash guard).
- No hot reload: `LoadPlugins` runs once per process.
