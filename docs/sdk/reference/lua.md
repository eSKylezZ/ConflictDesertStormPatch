# pse.lua (version 1)

Add Lua functions to mission scripts, and call the script back. The Papyrus-native registration of PSE. Header:
`pse/pse_api.h` (block "pse.lua"). API 0 = unstable (phase 1).

```c
const PSELuaInterface* lua = pse->Query(pse->plugin, PSE_LUA_INTERFACE, 1);
typedef int (*PSELuaNative)(PSELuaCall* call, void* ctx);   /* PSE_LUA_RETURN or PSE_LUA_ERROR */
```

```c
static int PSE_CALL Greet(PSELuaCall* call, void* ctx) {
    const char* name;
    if (!lua->CheckString(call, 1, &name, NULL)) return PSE_LUA_ERROR;   /* "bad argument #1 ..." */
    char buf[128];
    snprintf(buf, sizeof buf, "Hello, %s!", name);
    lua->PushString(call, buf, PSE_LUA_NUL);
    return PSE_LUA_RETURN;
}
/* in PSEPlugin_Load */
lua->RegisterNamespace(me, "alienfaction");
lua->RegisterFunction(me, "alienfaction", "greet", Greet, NULL);
```

```lua
-- Mods\MyMission\mymission.lua
function init()
    game.log(alienfaction.greet("Bradley"))   -- Hello, Bradley!
end
```

Full example: `pse/examples/lua_native/` (`example.lua_native`, namespace `example`).

## Why not lua_State?

The game layer owns **one Lua 5.4 state per level** (DS1: `.lua` levels, `src/features/scripthost.cpp`; Lua is linked
into the host statically). A plugin that linked its own Lua and touched that state would run a second copy of the
interpreter's code on the first copy's data - different allocator, string table, GC and `longjmp` buffers: memory
corruption. So the ABI never exposes `lua_State` or Lua's headers. A native gets an opaque `PSELuaCall*` and reads /
makes values through the interface's accessor functions; the game layer translates each of them into the Lua C API.
Plugins link nothing but the SDK header and can be built with any compiler that does `__cdecl`.

A second reason: Lua reports errors with `longjmp`. A `longjmp` through a plugin's C++ frames skips its destructors
and through another module's SEH frames is undefined. With the accessors no Lua error is ever raised while plugin code
is on the stack: `Check*` and `Error` only **record** the error; the game layer raises it after the native returned.
Table access is raw (no metamethods run, so no script code - and no script error - can run inside an accessor).

## Namespaces

One namespace per plugin, claimed by `RegisterNamespace` (or the first `RegisterFunction`). Scripts see it as a global
table: `alienfaction.spawn(...)`. Rules:

- `name` or dotted `a.b.c` (nested tables: `kyle.aliens.spawn(...)`); each part `[a-z][a-z0-9_]*` (lower case, no
  leading `_`), not a Lua keyword; at most 64 characters in all.
- **Reserved** as the first part: `game`, `dev`, `pse`; Lua's libraries (`string table math utf8 coroutine io os
  package debug bit32`) and base functions (`print pairs ipairs type error pcall select next load require …`); the
  mission callbacks `init`, `update`, `on_message`; any name starting with `_` (the prelude's `_frame`, `_save_state`
  …) → `PSE_E_INVALID_ARG`.
- **Collisions are refused** (`PSE_E_CONFLICT`, logged): another plugin's namespace, a dotted prefix of it or an
  extension of it (`kyle` vs `kyle.aliens`), or a second namespace of the same plugin. Claiming your own again is
  `PSE_OK`.
- Function names: `[A-Za-z_][A-Za-z0-9_]*`, not a keyword, ≤ 64 characters. Registering one twice → `PSE_E_EXISTS`.
- A crashed / failed / unloaded plugin's namespace is freed.

**When scripts see them:** every level's state gets all registered functions right after Lua's libraries are opened -
before the prelude and the script, so `init()` and top-level script code can use them. Registering while a level runs
works too: the running script gets the new functions at its next update (the game layer re-installs whenever the
registry changed). A script that made its own global of the same name keeps it (the plugin's functions are then not
installed there; logged). The global is a plain table, so a script can also replace single functions in it - the next
re-install puts them back.

Calls go through the function's id, not its address: once the plugin is disabled (crash) or unloaded, its functions
still exist in a running script but raise `ns.fn: plugin x is not loaded (it crashed or was unloaded)`.

## Slots

Values inside a call are **slots** (`PSELuaSlot`, an int):

- `1 .. ArgCount(call)`: the arguments of a native (or, in a `CallGlobal` results callback, the function's results);
- every `Get*`, `Next` and `New*` call makes a new slot (it stays until the callback returns);
- `0` = no value (returned on failure; passing 0 or an unknown slot to any function is safe and fails).

Slots, and string pointers read from them, are valid only until the callback returns. Copy what you keep. Each new
slot uses one Lua stack entry (Lua's limit is about a million per call); a callback that walks a huge table with
`Next` uses two per entry.

A native's **results** are the slots passed to `Return` (or made by `Push*`), in that order; an argument slot can be
returned as it is. No `Return` = no results.

## Marshalling rules

| Lua value | `Type` | read with | made with |
| --- | --- | --- | --- |
| (no argument / slot) | `PSE_LUA_NONE` | – | – |
| `nil` | `PSE_LUA_NIL` | – | `NewNil` / `PushNil` |
| `true` / `false` | `PSE_LUA_BOOLEAN` | `ToBoolean` (Lua truth: only nil / false / no value are 0) | `NewBoolean` / `PushBoolean` |
| integer (64-bit) | `PSE_LUA_INTEGER` | `CheckInteger` (`int64_t`) | `NewInteger` / `PushInteger` |
| float | `PSE_LUA_NUMBER` | `CheckNumber` (`double`) | `NewNumber` / `PushNumber` |
| string (bytes, may hold NULs) | `PSE_LUA_STRING` | `CheckString` (pointer + length; NUL-terminated) | `NewString` / `PushString` (length or `PSE_LUA_NUL`) |
| table | `PSE_LUA_TABLE` | `CheckTable`, `Length`, `GetField`, `GetIndex`, `Next` | `NewTable` / `PushTable` + `SetField` / `SetIndex` |
| function | `PSE_LUA_FUNCTION` | – (pass through with `Return`) | – |
| userdata, thread | `PSE_LUA_OTHER` | – | – |

Conversions follow Lua's `luaL_check*`: `CheckInteger` takes integers, floats with an integral value (`2.0`) and
numeric strings (`"3"`); a float like `1.5` fails with "number has no integer representation". `CheckNumber` takes
numbers and numeric strings. `CheckString` takes strings and numbers (a number argument is converted to its string in
place, as in Lua). Strings are bytes; game text is UTF-8 by convention.

Lua integers are 64-bit; the game's ids, addresses and hashes are 32-bit - range-check what you cast.

## Errors

| What happens | The script sees (a Lua error, catchable with `pcall`) |
| --- | --- |
| `Check*` with the wrong type, then `return PSE_LUA_ERROR` | `file:line: ns.fn: bad argument #2 (integer expected, got string) (plugin x)` |
| `return lua->Error(call, "no unit %d", id);` | `file:line: ns.fn: no unit 7 (plugin x)` |
| `return PSE_LUA_ERROR` with no message | `file:line: ns.fn: failed (plugin x)` |
| a recorded error but `PSE_LUA_RETURN` | the error anyway (results dropped) |
| the native crashes (external plugin) | `file:line: ns.fn: plugin x crashed (exception 0xC0000005 at x.dll+0x1A2B) and was disabled` |
| the plugin is disabled / unloaded | `file:line: ns.fn: plugin x is not loaded (it crashed or was unloaded)` |

`file:line` is the script line that called the native (none when a C function such as `pcall` calls it directly: `pcall(ns.fn, x)` - as with Lua's own functions). The first recorded error of a call wins. An error ends the
script callback that called the native the way any Lua error does - in DS1 the script host reports it (`[fail] script
…` in the mod report) and stops calling the script, unless the script caught it with `pcall`. The game keeps running.

**Crash guard.** A native of an **external** plugin (and its `CallGlobal` callbacks) runs under SEH like every other
call into external plugins ([plugins.md – Crash guard](plugins.md#crash-guard-callbacks-after-load), reported as
"crashed in the Lua function ns.fn"). A fault disables the plugin exactly like a crash in its frame callback (hooks,
listeners, tasks, settings, co-save callbacks and its Lua namespace taken back, DLL kept mapped) and the script gets
the Lua error above instead of a crashed game. Built-in plugins' natives are called directly.

## Calling the script (`CallGlobal`)

```c
PSEResult CallGlobal(PSEPluginHandle plugin, const char* fn, PSELuaNative args, PSELuaNative results, void* ctx);
```

Calls the level script's global function `fn` - from a frame / event callback, or from inside a native (the call
nests). `args(call, ctx)` makes the arguments with `New*` + `Return` / `Push*` (NULL = none); `results(call, ctx)`
reads the function's results as slots `1..ArgCount` (NULL = ignored). The Lua function runs under `pcall` with a
traceback.

```c
static int PSE_CALL Args(PSELuaCall* c, void* ctx) { lua->PushInteger(c, *(int*)ctx); return PSE_LUA_RETURN; }
static int PSE_CALL Results(PSELuaCall* c, void* ctx) {
    int64_t v;
    if (!lua->CheckInteger(c, 1, &v)) return PSE_LUA_ERROR;
    *(int*)ctx = (int)v;
    return PSE_LUA_RETURN;
}
int x = 21;
if (lua->CallGlobal(me, "double", Args, Results, &x) == PSE_OK) { /* x == 42 */ }
```

| Result | When |
| --- | --- |
| `PSE_OK` | called; `results` (if any) succeeded |
| `PSE_E_WRONG_PHASE` | no Lua level is running (DLL level, front end) or not on the game thread |
| `PSE_E_NOT_FOUND` | no global `fn`, or it isn't a function (raw global lookup: no `__index` on `_G`) |
| `PSE_E_INVALID_ARG` | bad handle / NULL `fn`; `args` recorded an error or returned `PSE_LUA_ERROR` (the function is **not** called); `results` failed (the function **was** called) |
| `PSE_E_SCRIPT` (11) | the Lua function raised an error - the script keeps running (unlike its own callbacks' errors); logged as a warning |
| `PSE_E_ACCESS` | `args` / `results` crashed: the plugin is disabled |

`LastError(plugin, buf, len)` returns the message of the plugin's last failed call (with the Lua traceback for
`PSE_E_SCRIPT`), "" after a success; it copies into `buf` (NUL-terminated) and returns the full length.
`ScriptRunning()` = 1 while a level's script state exists.

`CallGlobal` changes script state like any script code (`game.state` is saved with the game).

## Events

Raised through [pse.messaging](messaging.md) (`EventId(name)`), on the game thread:

| Event | data | When |
| --- | --- | --- |
| `pse.lua.level_start` (`PSE_LUA_EVENT_LEVEL_START`) | `const char*` level name ("ambush.lua") | the script ran and its `init()` returned; `CallGlobal` works (the functions are already usable during `init()`) |
| `pse.lua.level_end` (`PSE_LUA_EVENT_LEVEL_END`) | `const char*` level name | the next level starts (any kind) - the state stops being "the script" |

Game messages (`on_message` ids, timers …) are the script's; a plugin that wants them asks the script to forward them,
e.g. the script calls `myplugin.on_message(id, a, b)` from its own `on_message`.

## Functions

All game thread only (`PSE_E_WRONG_PHASE` / 0 elsewhere); accessors with a NULL / finished `call` return 0 / NONE.

| Function | Notes |
| --- | --- |
| `PSEResult RegisterNamespace(plugin, ns)` | see Namespaces |
| `PSEResult RegisterFunction(plugin, ns, name, fn, ctx)` | `ctx` is passed to every call; claims `ns` if needed |
| `uint32_t ArgCount(call)` | arguments (results in a results callback); 0 in an args callback |
| `PSELuaType Type(call, slot)` | |
| `int CheckInteger(call, slot, int64_t* out)` | 1 ok; 0 = error recorded |
| `int CheckNumber(call, slot, double* out)` | |
| `int CheckString(call, slot, const char** out, uint32_t* len)` | `len` may be NULL |
| `int CheckTable(call, slot)` | |
| `int ToBoolean(call, slot)` | never an error |
| `uint32_t Length(call, table)` | raw `#t` (a border); 0 for non-tables |
| `PSELuaSlot GetField(call, table, key)` / `GetIndex(call, table, i)` | raw; nil slot when missing, 0 when not a table |
| `int Next(call, table, PSELuaSlot* key, PSELuaSlot* value)` | `*key = 0` to start; pass the key slot back unchanged; don't add keys while iterating (Lua's `next` rules) |
| `PSELuaSlot NewNil / NewBoolean / NewInteger / NewNumber / NewString / NewTable` | `NewString(call, s, len)`: `PSE_LUA_NUL` = `strlen` |
| `int SetField(call, table, key, value)` / `SetIndex(call, table, i, value)` | raw set; 0 = not a table / bad slot |
| `int Return(call, slot)` | appends a result (native) or an argument (args callback) |
| `int PushNil / PushBoolean / PushInteger / PushNumber / PushString` | New + Return |
| `PSELuaSlot PushTable(call)` | a new, returned table - fill it afterwards |
| `int Error(call, fmt, ...)` | records, returns `PSE_LUA_ERROR` |
| `const char* FunctionName(call)` | "ns.fn"; "" in CallGlobal callbacks |
| `PSEResult CallGlobal(plugin, fn, args, results, ctx)` | see above |
| `uint32_t LastError(plugin, buf, len)` | |
| `int ScriptRunning(void)` | |

## DS1

- The state: `.lua` levels only (`scripthost.cpp`, CLAUDE.md "Lua mission scripts": `game.*` natives + prelude,
  `--@archive`, `--@base` missions). Mission DLL levels and the front end have no state: `CallGlobal` →
  `PSE_E_WRONG_PHASE`, natives are simply not there. A DLL level after a Lua one ends the Lua level for plugins
  (`pse.lua.level_end`).
- Installed into each new state after `OpenLibs` (`pse::lua_bridge::Install`), re-synced at the start of every script
  update. Log: `Script: n plugin Lua function(s) installed`.
- The dev server's own Lua states (`dev.*`, dev builds only) are separate and don't get plugin
  functions.
- Online: a script that uses a plugin's functions needs that plugin on every PC; the plugin-set check compares
  SIMULATION plugins only ([online-compat.md](../online-compat.md)) - flag a plugin SIMULATION if its natives change
  what happens in a mission.

## DS2

- The state: `.lua` levels of Desert Storm II (`games/ds2/src/features/scripthost.cpp`, sub-plugin `ds2fix.scripthost`,
  [ds2-mods.md](../ds2-mods.md#lua-missions-and-custom-campaigns)). Same binding as DS1: `ds2fix` attaches
  `lua_bridge.h` with its load interface (host plugin), installs the namespaces after `OpenLibs`, syncs them every
  script update, sends `pse.lua.level_start` after `init()` and `pse.lua.level_end` when the level unloads (the state is
  closed then - DS1 keeps it until the next Lua level). The standalone `dinput8.dll` runs `.lua` levels without plugin
  functions.
- A plugin's namespaces are the same in DS1 and DS2 scripts; the game-specific `game.*` functions differ (DS2's API
  slots, [game-ds2.md](game-ds2.md#lua-missions-ds2fixscripthost-gamesds2srcfeaturesscripthostcpp--campaignscpp-2026-10-23)).

## Internals (not SDK)

- `pse/loader/lua_iface.cpp` (in `pse_loader.dll`): registry (ids never reused, a generation counter), the public
  table, the crash-guarded calls (`detail::GuardedLuaNative`, guard.cpp), `detail::ForgetLua` (from the plugin
  cleanup: failed / crashed / unloaded plugins), the events. Lua-agnostic: `pse_core` links no Lua.
- `pse/loader/lua_iface.h`: the contract between loader and the Lua binding - `PSELuaCall` (POD: backend, state,
  base, argCount, result list, `what`, fixed 512-byte error buffer), `lua::Backend` (one function per accessor) and the
  **private table `pse.lua.host` v1** (`SetHost`, `Generation`, `ListFunctions`, `Invoke`, `InvokeCallback`,
  `LevelStarted`, `LevelEnded`). Plain C types only, because the binding lives in another DLL with its own CRT
  (DS1: `dsfix.dll`). `Query` hands `pse.lua.host` only to host plugins, game layers and built-in plugins, never to
  ordinary external plugins (like `pse.host`).
- `pse/loader/lua_bridge.h` (header-only, included by the owner of the state and `pse_tests`, which link
  `lua_static`): the `Backend` (slot = stack index − call base), `Install` / `Sync`, the closures (upvalue = function
  id; the error is raised after every C++ object of the call is gone), `CallGlobal`, `Attach(PSELoadInterface*)` /
  `SetState` / `LevelStarted` / `LevelEnded`. DS1: `scripthost.cpp` attaches with `dsfix`'s load interface in
  `ApplyScriptHost`.
- Tests: `pse/tests/test_lua_iface.cpp` (tier 1: real Lua state, namespace rules, marshalling, errors, live
  registration, CallGlobal, the private table's gate, the external example incl. its crash),
  `tests/live/test_lua_plugins.py` (tier 3: `tests/live/data/pselua_test.lua` on Mission 1's map).
