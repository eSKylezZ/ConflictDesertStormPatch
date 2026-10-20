# pse.messaging (version 1)

Messages between plugins, interfaces other plugins can call, and prioritised events. Header: `pse/pse_api.h`.
API 0 = unstable (phase 1).

```c
const PSEMessagingInterface* msg = pse->Query(pse->plugin, PSE_MESSAGING_INTERFACE, 1);
```

All functions: game thread (use [pse.tasks](tasks.md) from other threads). Every callback runs synchronously, on the
thread of the call that triggered it.

**Reentrancy.** A callback may register listeners, dispatch, subscribe, unsubscribe and raise events (also the same
event) while it runs. Each delivery works on a snapshot of the subscribers taken when it started:

- a listener or subscriber added during a delivery first runs on the next one;
- a subscriber removed during a Raise does not run in that Raise if it hasn't run yet.

**Crash guard.** Listeners and subscribers of external plugins are called inside SEH: one that faults is disabled
with its plugin ([plugins.md - Crash guard](plugins.md#crash-guard-callbacks-after-load)), the delivery carries on with
the others, and that plugin's remaining callbacks in the same delivery are skipped. A crashed subscriber never stops
an event. Built-in plugins' callbacks are called directly. Interface tables (`QueryInterface`) are called directly by
the caller: no guard - a fault inside one is blamed on the plugin whose callback made the call.

## Messages

```c
typedef struct PSEMessage {
    uint32_t size;
    const char* sender;   /* plugin name; "pse" = the loader, "pse.game" = the game layer */
    uint32_t type;
    const void* data;     /* valid during the callback only */
    uint32_t dataLen;
} PSEMessage;
typedef void (*PSEMessageCallback)(const PSEMessage* msg, void* ctx);
```

### RegisterListener

```c
PSEResult RegisterListener(PSEPluginHandle plugin, const char* sender, PSEMessageCallback cb, void* ctx);
```

Receive messages from `sender` (a plugin name; it doesn't need to be loaded yet) or, with `sender` NULL, from
everyone. Usually in `Load`. Errors: `PSE_E_INVALID_ARG` (bad handle, NULL `cb`). There is no unregister.

### Dispatch

```c
PSEResult Dispatch(PSEPluginHandle plugin, uint32_t type, const void* data, uint32_t dataLen, const char* receiver);
```

Sends a message from `plugin`. `receiver` NULL = to every listener that listens to this plugin (or to everyone);
`receiver` = a plugin name = only that plugin's matching listeners. Synchronous: all callbacks have run when it
returns. Errors: `PSE_E_INVALID_ARG` (bad handle), `PSE_E_NOT_FOUND` (no plugin named `receiver`).

```c
static void OnLoader(const PSEMessage* m, void* ctx) {
    if (m->type == PSE_MSG_POST_LOAD) g_online = msg->QueryInterface(me, "dsfix.online.session", 1);
}
msg->RegisterListener(me, "pse", OnLoader, NULL);
```

### Loader messages (sender "pse")

| Type | When | data |
| --- | --- | --- |
| `PSE_MSG_POST_LOAD` (1) | every plugin's `Load` returned – look up other plugins' interfaces now | NULL |
| `PSE_MSG_POST_POST_LOAD` (2) | after that | NULL |
| `PSE_MSG_GAME_ENTRY` (3) | just before the game's own entry point | NULL |
| `PSE_MSG_EXIT` (4) | the game is closing | NULL |
| `PSE_MSG_NEW_GAME` (5) | a new game starts (before its level loads) | `const char*` level name |
| `PSE_MSG_SAVE_BEGIN` (6) / `SAVE_END` (7) | around the game's save | `const char*` save path |
| `PSE_MSG_LOAD_BEGIN` (8) / `LOAD_END` (9) | around a save being loaded | `const char*` save path |
| `PSE_MSG_FRONT_END_READY` (10) | the first running frame of a front-end level (the menus are up) | `const char*` level name |
| `PSE_MSG_DEVICE_CREATED` (11) | the game's D3D device was created | the device itself, `dataLen` 0 |
| `PSE_MSG_DEVICE_RESET` (12) | the device was reset (alt-tab back, display mode change) | the device, `dataLen` 0 |
| `PSE_MSG_DEVICE_LOST` (13) | the device was lost (a reset follows) | the device, `dataLen` 0 |

`NEW_GAME`, `SAVE_*` / `LOAD_*`, `FRONT_END_READY` and `DEVICE_*` come from the game layer (sender "pse" like the
loader's own); which of them a game sends, and exactly when, is listed on its game layer page
([DS1](game-ds1.md#events-and-messages): all of them). String data is NUL-terminated and `dataLen` counts the NUL.
Game-specific messages start at `PSE_MSG_GAME_FIRST` (0x1000).

## Interfaces between plugins

### RegisterInterface

```c
PSEResult RegisterInterface(PSEPluginHandle plugin, const char* id, uint32_t version, const void* table);
```

Publishes a table of function pointers (start it with `uint32_t size, version` like the loader's tables). One table
per (id, version); a higher version should be a superset of the lower ones. The table must stay valid until exit.
Errors: `PSE_E_INVALID_ARG` (bad handle, empty id, NULL table), `PSE_E_EXISTS` (that id + version is already
registered, or the id is one of the loader's `pse.*` interfaces).

### QueryInterface

```c
const void* QueryInterface(PSEPluginHandle plugin, const char* id, uint32_t minVersion);
```

The highest registered version >= `minVersion`, or NULL (not installed: carry on without it). Also returns the
loader's own tables (`pse.log`, …), like `PSELoadInterface.Query`. Call it in `PSE_MSG_POST_LOAD` – in `Load` the
other plugin may not be loaded yet.

```c
typedef struct { uint32_t size, version; int (*IsOnline)(void); } OnlineSessionV1;
static const OnlineSessionV1 kSession = {sizeof kSession, 1, IsOnline};
msg->RegisterInterface(me, "dsfix.online.session", 1, &kSession);
```

## Events

```c
typedef int (*PSEEventCallback)(PSEEventId event, void* data, void* ctx);   /* non-zero = stop */
```

### EventId

```c
PSEEventId EventId(const char* name);
```

The id for an event name: the same name always gives the same id. New names get ids from
`PSE_EVENT_FIRST_DYNAMIC` (0x10000) up. NULL or "" = 0. Ids are not stable between game starts – look them up by
name. Built-in events:

| Id | Name | When | data |
| --- | --- | --- | --- |
| `PSE_EVENT_FRAME` (1) | `pse.frame` | once per game frame, after the frame's [tasks](tasks.md), before update + render | NULL |
| `PSE_EVENT_PRE_RENDER` (2) | `pse.pre_render` | before the frame is drawn | NULL |
| `PSE_EVENT_POST_RENDER` (3) | `pse.post_render` | after the frame is drawn | NULL |
| `PSE_EVENT_END_SCENE` (4) | `pse.end_scene` | at the D3D EndScene | the D3D device |
| `PSE_EVENT_PRESENT` (5) | `pse.present` | at the D3D Present | the D3D device |
| `PSE_EVENT_LEVEL_LOAD_BEGIN` (6) | `pse.level_load_begin` | a level starts loading | `const char*` level name |
| `PSE_EVENT_LEVEL_LOAD_END` (7) | `pse.level_load_end` | the level is loaded | `const char*` level name |
| `PSE_EVENT_LEVEL_UNLOAD` (8) | `pse.level_unload` | the level is unloaded | `const char*` level name |

All of them are raised by the **game layer** from its own hooks (the frame event through the loader's `Frame`: the
frame's tasks first, then `pse.frame`), so a plugin gets them whether or not a host plugin such as DesertStormFix is
installed. When, exactly, per game: [DS1](game-ds1.md#events-and-messages). None of the built-in events is
cancellable: return 0. Level names are valid during the callback only.

### Subscribe

```c
PSESubscriptionId Subscribe(PSEPluginHandle plugin, PSEEventId event, PSEEventCallback cb, void* ctx, int32_t priority);
```

Higher priority runs first; equal priority = subscription order. Returns 0 on failure (bad handle, NULL `cb`, an
event id that is neither built in nor handed out by `EventId`).

### Unsubscribe

```c
PSEResult Unsubscribe(PSEPluginHandle plugin, PSESubscriptionId id);
```

Only the plugin that subscribed can remove it. Safe inside any callback, including the subscription's own.
Errors: `PSE_E_INVALID_ARG` (bad handle, id 0), `PSE_E_NOT_FOUND` (unknown id, already removed, or another plugin's).

### Raise

```c
int Raise(PSEPluginHandle plugin, PSEEventId event, void* data);
```

Runs the subscribers in order. A callback returning non-zero stops the event (only for events documented as
cancellable); `Raise` then returns 1, else 0. An event without subscribers returns 0.

```c
static int OnFrame(PSEEventId e, void* data, void* ctx) { UpdateOverlay(); return 0; }
msg->Subscribe(me, PSE_EVENT_FRAME, OnFrame, NULL, 0);

PSEEventId spawned = msg->EventId("kyle.aliens.spawned");   /* own event */
msg->Raise(me, spawned, alien);
```
