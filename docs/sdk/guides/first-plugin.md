# Your first plugin

This builds `example.hello` (`pse/examples/hello/`) from nothing but the SDK: the header `pse/include/pse/pse_api.h`
and the CMake helper `pse/cmake/PSEPlugin.cmake`. No loader source, no DesertStormFix code. At the end you have a DLL
and a manifest to drop into `<game>\PSE\Plugins\`.

What it does: logs a line when it loads, declares a setting, logs every N frames (N = the setting), and offers other
plugins a function `Add(a, b)`. A second plugin, `example.hello_client`, calls it.

Reference for everything used here: [plugins.md](../reference/plugins.md) (files, manifest, lifecycle),
[log.md](../reference/log.md), [settings.md](../reference/settings.md), [messaging.md](../reference/messaging.md).

## 1. You need

- Visual Studio 2022 (or its Build Tools) with the C++ workload. All Pivotal games are 32-bit: always **Win32**.
- CMake 3.21+ (VS ships one).
- The SDK folder: `pse/include` and `pse/cmake` from this repo (later: the SDK zip).

## 2. Project layout

```text
hello/
  CMakeLists.txt
  example.hello.pse      the manifest
  example_hello.h        the interface other plugins include (optional)
  hello.cpp
```

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.21)
project(hello LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

include(path/to/pse/cmake/PSEPlugin.cmake)   # defines pse_add_plugin()

pse_add_plugin(hello SOURCES hello.cpp example_hello.h MANIFEST example.hello.pse)
```

`pse_add_plugin` makes a DLL named after the manifest (`example.hello.dll`), with the static CRT (nothing to
install, no CRT shared with other plugins), the SDK header on the include path, and copies the manifest next to the
DLL in `build/pse_plugins/<config>/` (`OUTPUT_DIR` to change that).

## 3. The manifest

```text
# example.hello.pse
name = example.hello
```

The name is the plugin's identity: lower case, `[a-z0-9_.-]`, use a prefix of your own (`kyle.alienfaction`). A
plugin that needs another one says so here, and the loader loads that one first or refuses yours with a reason
(see `example.hello_client.pse`: `requires = example.hello >= 1.0`).

## 4. The info export

```cpp
#include <pse/pse_api.h>

namespace { const char* const kGames[] = {"ds1", nullptr}; }

extern "C" {

__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info = {
    sizeof(PSEPluginInfo),
    1,                              // infoVersion
    "example.hello",                // = the manifest's name
    "Hello (example plugin)",       // shown in the PLUGINS list
    "PSE",                          // author
    PSE_MAKE_VERSION(1, 0, 0),
    0,                              // minApiVersion
    kGames,                         // NULL = every game
    PSE_PLUGIN_CLIENT_ONLY,         // changes nothing in the simulation
};
```

Two traps:

- **Keep `extern`.** Inside `extern "C" { }` a `const` object without `extern` is not exported, and the loader
  refuses the plugin ("the DLL has no PSEPlugin_Info export").
- **Only constant data.** The loader reads this struct from the DLL file *before* loading it (no code of yours has
  run), so it can't depend on anything computed at run time.

Pick the flag honestly: `PSE_PLUGIN_SIMULATION` for anything that changes gameplay (online peers must then have the
same plugin), `PSE_PLUGIN_CLIENT_ONLY` for looks / UI. Neither = simulation.

## 5. Load

```cpp
namespace {
PSEPluginHandle g_self;
const PSELogInterface* g_log;
const PSEMessagingInterface* g_messaging;
const PSESettingsInterface* g_settings;
}

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    g_self = pse->plugin;   // pass this to every call
    g_log = (const PSELogInterface*)pse->Query(g_self, PSE_LOG_INTERFACE, 1);
    g_messaging = (const PSEMessagingInterface*)pse->Query(g_self, PSE_MESSAGING_INTERFACE, 1);
    g_settings = (const PSESettingsInterface*)pse->Query(g_self, PSE_SETTINGS_INTERFACE, 1);
    if (!g_log || !g_messaging || !g_settings) return 0;   // too old a loader: refuse
    ...
    g_log->Write(g_self, PSE_LOG_INFO, "hello: loaded (game %s)", pse->gameId);
    return 1;
}
```

`Query` returns NULL when the interface (at that version) isn't there – check it. Returning 0 means "don't load
me": the loader takes back everything you registered so far and frees the DLL. So does a crash in `Load`.

## 6. A setting

```cpp
PSESettingDesc every = {};
every.size = sizeof every;
every.key = "LogEveryFrames";        // registry value name
every.type = PSE_SETTING_INT;
every.flags = PSE_SETTING_LIVE;      // takes effect at once
every.defaultInt = 600;
every.minInt = 1;
every.maxInt = 100000;
every.label = "HELLO: LOG EVERY N FRAMES";
if (g_settings->Declare(g_self, &every) != PSE_OK) return 0;
```

Stored in `HKCU\Software\PSE\ds1\Plugins\example.hello`. Read it with `GetInt(g_self, "LogEveryFrames")` – no ini
files.

**You write no UI for it.** In the game, OPTIONS > DESERTSTORMFIX > PLUGINS lists your plugin and opens a page with a
row per declared setting (bool ON / OFF, int with your `step`, choice labels, colour with a colour wheel, string
read-only; `group` = headings, `help` = the line under the list, `PSE_SETTING_HIDDEN` = no row). Changing a row stores
the value and runs your `OnChange` callbacks. Add `PSE_SETTING_LAUNCHER` and a bool / int / choice also appears in the
launcher's Detail Settings. Details: [reference/ui.md](../reference/ui.md#settings-need-no-ui-code).

## 7. An event

```cpp
int PSE_CALL OnFrame(PSEEventId, void*, void*) {
    ++g_frames;
    int every = g_settings->GetInt(g_self, "LogEveryFrames");
    if (g_frames % every == 0) g_log->Write(g_self, PSE_LOG_INFO, "hello: frame %u", g_frames);
    return 0;   // 0 = let later subscribers run
}
...
if (!g_messaging->Subscribe(g_self, PSE_EVENT_FRAME, &OnFrame, nullptr, 0)) return 0;
```

## 8. An interface for other plugins

Put the table in a header other plugins can include:

```c
/* example_hello.h */
#define EXAMPLE_HELLO_INTERFACE "example.hello"
typedef struct ExampleHelloV1 {
    uint32_t size;                         /* tables start with their size and only grow */
    int (PSE_CALL* Add)(int a, int b);
} ExampleHelloV1;
```

```cpp
int PSE_CALL Add(int a, int b) { return a + b; }
const ExampleHelloV1 kInterface = {sizeof(ExampleHelloV1), &Add};
...
g_messaging->RegisterInterface(g_self, EXAMPLE_HELLO_INTERFACE, 1, &kInterface);
```

The table must stay valid while the plugin is loaded (a `const` global is). A breaking change = a new version
number (register both if you can).

## 9. Unload (optional)

```cpp
__declspec(dllexport) void PSE_CALL PSEPlugin_Unload(void) {
    g_log->Write(g_self, PSE_LOG_INFO, "hello: unloading after %u frames", g_frames);
}

}  // extern "C"
```

At exit, in reverse load order. Your hooks and subscriptions are removed by the loader afterwards; free your own
resources here.

## 10. Calling another plugin

`example.hello_client` (`pse/examples/hello_client/`): its manifest has `requires = example.hello >= 1.0`, so it is
loaded after hello, or not at all (with "requires example.hello >= 1.0.0, which is not installed" in the log and
the PLUGINS list). It asks for hello's interface in `PSE_MSG_POST_LOAD` – the first moment every plugin's `Load` has
run:

```cpp
void PSE_CALL OnLoaderMessage(const PSEMessage* msg, void*) {
    if (msg->type != PSE_MSG_POST_LOAD) return;
    auto hello = (const ExampleHelloV1*)g_messaging->QueryInterface(g_self, EXAMPLE_HELLO_INTERFACE, 1);
    if (hello && hello->size >= sizeof(ExampleHelloV1)) g_answer = hello->Add(2, 3);
}
...
g_messaging->RegisterListener(g_self, "pse", &OnLoaderMessage, nullptr);   // in Load
```

An `optional =` dependency works the same way, with a NULL check instead of a guarantee.

## 10b. Using the game interface

To read the game itself, use its game interface instead of raw addresses – for Desert Storm `pse.game.ds1`
([reference](../reference/game-ds1.md), header `pse/pse_game_ds1.h`, read-only in version 1: front-end state, level
name / kind / title, catalog text, local players with position and health, the online session, the window, the exe
hash). It is served by the built-in plugin `pse.game.ds1`, so ask for it in the manifest and the loader refuses your
plugin anywhere it doesn't exist:

```text
requires = pse.game.ds1 >= 1.0
```

```cpp
#include <pse/pse_game_ds1.h>

const PSEGameDs1V1* g_game;
...
g_game = (const PSEGameDs1V1*)pse->Query(g_self, PSE_GAME_DS1_INTERFACE, 1);   // in Load
if (!g_game || g_game->size < sizeof(PSEGameDs1V1)) return 0;
...
int PSE_CALL OnFrame(PSEEventId, void*, void*) {            // game thread
    if (g_game->LevelKind() != PSE_DS1_LEVEL_MISSION) return 0;
    char title[128];
    g_game->LevelTitle(title, sizeof title);                  // "Mission 1: Rescue"
    PSEDs1Player p = {sizeof p};
    if (g_game->GetLocalPlayer(0, &p) && p.health <= 0) { /* player 1 is down */ }
    return 0;
}
```

Strings come back UTF-8 in your buffer (always NUL-terminated, the full length returned), structs you pass start with
their `size`. The real example is DesertStormFix's Discord status, `plugins/discord/discord.cpp`
([ds1-plugins.md](../ds1-plugins.md#external-plugins)): an external plugin that uses nothing but `pse_api.h` and
`pse_game_ds1.h`.

## 10c. Adding Lua functions

Mission scripts (`.lua` levels) can call your plugin through `pse.lua` ([reference](../reference/lua.md)). You never
link Lua and never see a `lua_State` – the game owns the one Lua state per level and hands your native an opaque
`PSELuaCall*`; you read the arguments and make the results through the interface. The full example is
`pse/examples/lua_native/` (`example.lua_native`, namespace `example`).

```cpp
const PSELuaInterface* g_lua;

// alienfaction.spawn(unitName, count) -> number spawned
int PSE_CALL Spawn(PSELuaCall* call, void*) {
    const char* unit = nullptr;
    int64_t count = 1;
    if (!g_lua->CheckString(call, 1, &unit, nullptr)) return PSE_LUA_ERROR;        // "bad argument #1 ..."
    if (g_lua->Type(call, 2) > PSE_LUA_NIL && !g_lua->CheckInteger(call, 2, &count)) return PSE_LUA_ERROR;
    if (count < 1 || count > 50) return g_lua->Error(call, "count %lld out of range", count);
    int spawned = SpawnUnits(unit, static_cast<int>(count));                       // your code
    g_lua->PushInteger(call, spawned);
    return PSE_LUA_RETURN;
}
...
g_lua = (const PSELuaInterface*)pse->Query(g_self, PSE_LUA_INTERFACE, 1);         // in Load
if (!g_lua || g_lua->RegisterNamespace(g_self, "alienfaction") != PSE_OK) return 0;
g_lua->RegisterFunction(g_self, "alienfaction", "spawn", &Spawn, nullptr);
```

```lua
function init()
    local n = alienfaction.spawn("Alien_Grunt", 6)
    local ok, err = pcall(alienfaction.spawn, "Alien_Grunt", 99)   -- err: "...: alienfaction.spawn: count 99 out of range (plugin kyle.alienfaction)"
end
```

Rules worth knowing:

- **Never longjmp, never throw**: `Check*` and `Error` only record the error – return `PSE_LUA_ERROR` and the game
  raises the Lua error after you returned. Results are the slots you `Return` / `Push*`.
- Slots and strings you read are valid only during the call; copy what you keep.
- One namespace per plugin, lower case, not a reserved name (`game`, `string`, `print`, `init` …); collisions with
  other plugins are refused.
- To call back into the script (e.g. on the `pse.lua.level_start` event), use `CallGlobal(g_self, "fn", args,
  results, ctx)` with an argument callback that pushes values and a results callback that reads them.
- A crash inside your native disables your plugin (crash guard) and the script gets a Lua error; the game goes on.

## 10d. Resolving game addresses

When the game interface doesn't cover what you need (hooking a game function, reading a global), don't hard-code
`0x424200`: ask `pse.ids` ([reference](../reference/ids.md), [DS1 list](../reference/ids-ds1.md)) for the id. The
game layer knows the address for each exe build it supports and checks it against the original bytes at start-up,
so your binary works on every covered build and gets 0 (not a wrong address) on anything else.

```cpp
#include <pse/pse_ids.h>
#include <pse/pse_ids_ds1.h>   // PSE_ID_ds1_<Name> constants

using TakeDamageFn = void(__thiscall*)(void* soldier, float amount, void* attacker);
TakeDamageFn g_takeDamage;

// in Load:
const auto* ids = (const PSEIdsInterface*)pse->Query(g_self, PSE_IDS_INTERFACE, 1);
g_takeDamage = PSEResolveAs<TakeDamageFn>(ids, "ds1", PSE_ID(ds1, Soldier_TakeDamage));
if (!g_takeDamage) { /* this exe isn't covered: log it and leave the feature off */ }
```

To hook it, pass the resolved address to `pse.hooks` (`HookFunction`, `HookCallSite` for a `Site_*` id,
`HookPointer` for a `*_Slot` / `Iat_*` id). Set `PSE_PLUGIN_USES_RAW_ADDRESSES` in your info flags. A missing id is
added to the database (reference: "Adding an id") – ids never change once published.

## 10e. Drawing, texts, pop-ups (pse.ui)

For what a settings page doesn't cover, the host serves `pse.ui` ([reference](../reference/ui.md), header
`<pse/pse_ui.h>`): draw rects / text / images over the game each frame, register texts the game can show, show a
pop-up message, take the keyboard while your own UI is up. It comes from DesertStormFix, so it may be missing – check
for NULL:

```cpp
#include <pse/pse_ui.h>
const PSEUiInterface* g_ui;   // in Load: (const PSEUiInterface*)pse->Query(g_self, PSE_UI_INTERFACE, 1), may be NULL

int PSE_CALL OnFrame(PSEEventId, void*, void*) {      // PSE_EVENT_FRAME: queue again every frame
    if (g_ui) g_ui->DrawText(g_self, 0.5f, 0.05f, "Hello from my plugin", 0xFFE0D878, PSE_UI_CENTER);
    return 0;
}
// anywhere on the game thread:
if (g_ui) g_ui->ShowPopup(g_self, "Something happened", 3000);
```

Coordinates are fractions of the screen unless you pass `PSE_UI_PIXELS`; text uses the game's own fonts (UTF-8 in,
characters the font lacks are replaced). The complete example is `pse/examples/ui_demo` (`example.ui_demo`): a setting
of every type, a banner drawn from them, a registered text and pop-ups.

## 11. Build and install

```text
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config RelWithDebInfo
```

Copy `build/pse_plugins/RelWithDebInfo/example.hello.dll` and `example.hello.pse` into `<game>\PSE\Plugins\`.
Start the game; the loader log (DesertStormFix.log during phase 2) shows `plugins: loaded example.hello 1.0.0`, and
the developer console's `pse plugins` lists it. If it was refused or failed, both say why.

In this repo the examples are built with everything else (non-release builds) into `build*/pse_plugins/<config>/`,
and `pse_tests` loads them (`pse/tests/test_plugins.cpp`) – including `example.bad_crash`, which crashes in `Load` on
purpose to prove the loader survives it.
