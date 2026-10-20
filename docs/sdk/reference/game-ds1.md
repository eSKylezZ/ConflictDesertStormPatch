# pse.game.ds1 (version 1) – Conflict: Desert Storm

The game interface for Conflict: Desert Storm (game id `ds1`): the stable way to read the game's
state without raw addresses. Version 1 is **read-only** – nothing in it writes game memory or calls a game function
that changes anything. Header: [`pse/include/pse/pse_game_ds1.h`](../../../pse/include/pse/pse_game_ds1.h). API 0 =
unstable (phase 2); the interface itself is versioned like the loader interfaces.

Served by the DS1 game layer `PSE\Games\pse_ds1.dll` ([layout.md](../layout.md)): `pse/games/ds1/pse_game_ds1.cpp`
(memory reads, text, hashes), registered in `PSEGame_Init` (`pse/games/ds1/ds1_layer.cpp`) before any plugin loads.
DesertStormFix (`src/core/pse_host.cpp`) supplies the split-screen count and the online session through the layer's
private interface `pse.game.ds1.host` v0 (`pse_game_ds1_host.h`: `SetProviders`, plus the level-title texts it reads
back, and its reader of the old `.sav.dsfix` sidecars). The same layer drives PSE's core game **events and
messages** and the co-save ([below](#events-and-messages)). Tests: `pse/tests/test_game_ds1.cpp` (tiers 1 + 2),
`tests/live/test_game_interface.py` (tier 3); events: `pse/tests/test_ds1_layer_events.cpp`,
`tests/live/test_layer_events.py`, `tests/live/test_layer_without_dsfix.py`.

## Getting it

```c
#include <pse/pse_game_ds1.h>

const PSEGameDs1V1* game = (const PSEGameDs1V1*)pse->Query(pse->plugin, PSE_GAME_DS1_INTERFACE, 1);
if (!game || game->size < sizeof(PSEGameDs1V1)) return 0;   /* not Desert Storm, or a host too old */
```

- Served by the **built-in plugin `pse.game.ds1`** (version 1.0.0 = interface version 1, `PSE_PLUGIN_INTERNAL |
  PSE_PLUGIN_CLIENT_ONLY`, registered right after the core plugin `dsfix`, before every feature and external plugin).
  The interface is registered with `pse.messaging` `RegisterInterface(…, "pse.game.ds1", 1, table)`, so `Query` from
  `PSELoadInterface` and `QueryInterface` both find it, already in `PSEPlugin_Load`.
- A plugin that can't work without it says so in its manifest – the loader then refuses it with a reason on any host
  that doesn't have it:

  ```text
  requires = pse.game.ds1 >= 1.0
  ```

- Check `size`: later versions only append functions. A plugin built against v1 reads only the v1 part.

Threads: every function is for the **game thread** unless its entry says "any thread". Strings come back as UTF-8 in
the caller's buffer: always NUL-terminated when `bufLen` > 0, cut to fit, and the return value is the full length in
bytes without the NUL (call with `bufLen` 0 to size a buffer). Game memory reads are fault-guarded: a stale pointer
gives 0 / "" / "no player", never a crash.

## Types

| Type | What |
| --- | --- |
| `PSEDs1LevelKind` | `PSE_DS1_LEVEL_NONE` 0 (nothing loaded yet), `FRONTEND` 1 (menus: `FrontEnd*`, `intro*` – the front end's film scene), `MISSION` 2 (everything else: missions, training, MP maps, Lua / mod levels; also paused, in a cutscene, on the loading screen), `OUTRO` 3 (`Outro*`, the credits) |
| `PSEDs1Soldier` | opaque `uint32_t` handle = the soldier's address; 0 = none. Valid only until the soldier is removed or the level ends: re-read it every frame, compare it, pass it back – never dereference it |
| `PSEDs1Vec3` | `{float x, y, z}` – centimetres, y up, the game's world space |
| `PSEDs1Player` | `{size, block, soldier, position, health}` – one player of this PC (`GetLocalPlayer`) |
| `PSEDs1OnlineRole` | `PSE_DS1_ONLINE_NONE` 0, `HOST` 1, `JOINER` 2 |
| `PSEDs1Online` | `{size, active, role, localPlayer, players, connectedPeers, inMission}` (`GetOnline`) |

`PSEDs1Player` / `PSEDs1Online`: the caller sets `size` (≥ the v1 `sizeof`); a smaller size is refused (returns 0).

## State

### FrontEndState

```c
uint32_t FrontEndState(void);
```

The front-end state machine's current state, `u32 [0x617C18]`: 1 main menu, 0xE playing a mission, 0xF pause menu,
0x14 mission briefing; other values are the other menu screens (CLAUDE.md "Front-end menus"; DesertStormFix's own
screens use 0x3E and up). 0 before the front end exists.

### DispatchMode

```c
uint32_t DispatchMode(void);
```

The main loop's dispatch mode, `u32 [0x60EC8C]`: 0 running (front end or a mission), 1 a level load is pending /
running this frame (the whole load is one frame), 3 other. A plugin that must not act during a load checks for 0.

## Level

### LevelName

```c
uint32_t LevelName(char* buf, uint32_t bufLen);
```

The loaded level's scene file, `char[64]` at `0x606880`: `"Mission4.env"`, `"Mission5a.env"`, `"Training1.env"`,
`"FrontEnd.env"`, `"intro.env"` (the front end's film scene), `""` before the first load. A Lua / mod level reports the
scene it runs on.

```c
char level[64];
game->LevelName(level, sizeof level);
```

### LevelKind

```c
uint32_t LevelKind(void);   /* PSEDs1LevelKind */
```

`LevelName` classified by prefix (case-insensitive): `""` → NONE, `frontend` / `intro` → FRONTEND, `outro` → OUTRO,
anything else → MISSION.

### InCutscene

```c
int InCutscene(void);
```

1 while a cutscene / film plays: bit 0 of the byte at `[0x63C948] + 0x1B8C` (the world object's cinematic flag, set
by the script API's MovieStart `0x486A00`, cleared by MovieEnd `0x486F10`; the mission overview fly-over counts). 0
when no world exists.

## Text

### TextHash

```c
uint32_t TextHash(const char* key);   /* any thread */
```

The hash the game's text catalog uses for a key: the archive LFSR (`FUN_004b9a10`) over the **lower-cased** key, same
as the game's `FUN_004b9ab0` (checked against it in the image test). `TextHash("MISS1_TITLE")` = `0xA90BE3A7`;
`NULL` / `""` → 1. Pure.

### LookupText

```c
uint32_t LookupText(uint32_t hash, char* buf, uint32_t bufLen);
```

One text in the game's current language, whatever the game's own menus would show: the catalog lookup `FUN_004f6730`
(thiscall on the text table `[0x60EDCC]`) – in DesertStormFix that entry is its `Lookup` hook, so mod `.strings`
texts, the level's `MnnStrs.loc` texts while loaded and DesertStormFix's own menu texts are included. Converted from
the game's ANSI code page to UTF-8, otherwise as stored (no case change; icon control characters kept). Returns 0 and
`""` for an unknown hash, an empty text or before the text table exists (start-up, launcher). Also `""` while a level
load has the table's search arrays freed; the lookup runs fault-guarded, so asking mid-load is safe (it used to crash
the game at `0x569FD7`).

```c
char title[128];
if (game->LookupText(game->TextHash("MISS4_TITLE"), title, sizeof title)) ...   /* "DESERT WATCH" (English) */
```

### LevelTitle

```c
uint32_t LevelTitle(char* buf, uint32_t bufLen);
```

The current level's title the way a person writes it (what the Discord status shows), UTF-8:

| Level | Result |
| --- | --- |
| `Mission4.env` | `Mission 4: Desert Watch` |
| `Mission5a.env` | `Mission 5A: <MISS5_TITLE>` |
| `Training1.env` | `Basic Training` (training: the title alone) |
| `MP_MISSION2.env` | `Multiplayer mission 2` (no catalog title) |
| `ambush.env` (no number pattern) | `ambush` |
| front end / outro / nothing | `""`, returns 0 |

Built from `LevelName`: the number part (`Mission`, `Training`, `Multiplayer mission` + the rest upper-cased), then
`": "` + the catalog title `MISSn_TITLE` / `TRAINn_TITLE` (Mission 5A and 5B use `MISS5_TITLE` / `MISS5B_TITLE`,
11A / 12A the 11 / 12 ones) from `LookupText`. An all-capitals title becomes Title Case (`BASIC TRAINING` → `Basic
Training`; titles with any lower-case letter are kept); control characters are dropped. No title found → the number
part only.

## Players

### SplitScreenPlayers

```c
uint32_t SplitScreenPlayers(void);
```

The number of views the current level was built with: 2–4 in split screen, 0 or 1 = one view
(`features::SplitScreenPlayers`, DesertStormFix's split-screen set-up). In the front end it keeps the last level's
value – the menus are always one view, so check `LevelKind` first.

### LocalPlayerCount

```c
uint32_t LocalPlayerCount(void);
```

Players controlled on this PC in the current level: 0 outside missions (`LevelKind` ≠ MISSION), 1 in an online session
(each PC has its own player), otherwise `max(1, SplitScreenPlayers())`.

### GetLocalPlayer

```c
int GetLocalPlayer(uint32_t index, PSEDs1Player* out);
```

The `index`-th player of this PC, 0 … `LocalPlayerCount()` − 1. Reads input block `0x60F5B8 + block * 0x478`: soldier
= block `+0x310`, position = soldier `+0x30` (`PSEDs1Vec3`), health = soldier `+0x54` (float; Bradley starts Mission 1
with 150, 0 = dead). `block` = `index` (split screen: player n = block n − 1); online = this PC's player number − 1.
Returns 1 and fills `*out`; 0 when the index is out of range, the player has no soldier (cutscenes, the loading screen,
a level being set up) or `out->size` is too small.

```c
PSEDs1Player p = {sizeof p};
for (uint32_t i = 0; i < game->LocalPlayerCount(); ++i)
    if (game->GetLocalPlayer(i, &p)) log->Write(me, PSE_LOG_INFO, "P%u at %.0f %.0f %.0f, health %.1f",
                                                i + 1, p.position.x, p.position.y, p.position.z, p.health);
```

## Online

### GetOnline

```c
int GetOnline(PSEDs1Online* out);
```

DesertStormFix's online co-op session (`core/net`, read-only – nothing here can start, join or send). Always fills
`*out` (zeros = no session); returns `out->active`.

| Field | Meaning |
| --- | --- |
| `active` | 1 = hosting or joined (the session exists; a mission may or may not run) |
| `role` | `PSEDs1OnlineRole` |
| `localPlayer` | this PC's player number: 1 host, 2.. joiners; 0 = none |
| `players` | players in the session including this PC: the co-op session's count once the host pressed START, else the connected peers + 1 (host) / 2 (a connected joiner) |
| `connectedPeers` | other PCs connected right now (host: its joiners; joiner: 1 = the host) |
| `inMission` | 1 = an online mission is being kept in step right now |

## Process

### GameWindow

```c
void* GameWindow(void);   /* HWND; any thread */
```

`[0x606A60]`, the game's window (class `ConflictDesertStorm`); `NULL` before it exists (start-up, launcher).

### ExeSha256 / ExeKnown

```c
const char* ExeSha256(void);   /* any thread */
int ExeKnown(void);            /* any thread */
```

`ExeSha256`: SHA-256 of the running `DesertStorm.exe` file as 64 upper-case hex digits, hashed with the Large Address
Aware flag (`IMAGE_FILE_HEADER.Characteristics` 0x20) cleared, so DesertStormFix-Patcher's 4 GB switch doesn't change
it; `""` if the file can't be read. Computed on the first call (reads the 2.3 MB file once), the string stays valid
until exit. `ExeKnown`: 1 when it is `D712F5EC82C14DA66B098332B3CC9C99689091F46317112EA35DFD393DC3E164`, the build
every address on this page was taken from (Steam and GOG ship the same exe).

## Events and messages

The DS1 layer installs these hooks in `PSEGame_Init` (before any plugin; `pse/games/ds1/ds1_events.cpp`), through the
hooks broker, owner `pse.game.ds1` - so **every plugin gets them with or without DesertStormFix installed**:

| Site | Hook | Priority | Sends |
| --- | --- | --- | --- |
| `0x4116EC` call `Main_FrameDispatch` (message loop, only while the window is active) | call site | INNER | before the game's frame: `PSE_MSG_FRONT_END_READY` (once per front-end load, first frame with dispatch mode 0), then [pse.tasks](tasks.md) and `PSE_EVENT_FRAME` |
| `0x40FD9E` call `Render_Frame(1)` (game update, every view) | call site | INNER | `PSE_EVENT_PRE_RENDER`, the game's render loop, `PSE_EVENT_POST_RENDER` |
| `0x40E05D` call `Level_Init(name)` | call site | OUTER | `PSE_MSG_NEW_GAME` (+ revert callbacks; [serialization](serialization.md#ds1-when-the-co-save-is-written-and-read)), `PSE_EVENT_LEVEL_LOAD_BEGIN` (name), the level init (the whole load, incl. the loading screen), `PSE_EVENT_LEVEL_LOAD_END` (name) |
| `0x40DF18` (level change) / `0x411404` (window destroyed) call `Level_Unload` | call site | OUTER | `PSE_EVENT_LEVEL_UNLOAD` (the loaded level's name) before the unload; nothing when no level is loaded |
| `0x415650` `Save_Game` entry | function | OUTER | `PSE_MSG_SAVE_BEGIN` / co-save / `PSE_MSG_SAVE_END` |
| `0x415B60` `Save_Load` entry | function | OUTER | `PSE_MSG_LOAD_BEGIN` / co-save / `PSE_MSG_LOAD_END` |
| `0x5D8308` import slot `Direct3DCreate8` → `IDirect3D8::CreateDevice` (vtable 15, per COM class) | pointer | OUTER | `PSE_MSG_DEVICE_CREATED` after a successful CreateDevice; hooks the device's slots below |
| device `TestCooperativeLevel` (3) | pointer | INNER | `PSE_MSG_DEVICE_LOST` once when it starts answering DEVICELOST / DEVICENOTRESET |
| device `Reset` (14) | pointer | OUTER | `PSE_MSG_DEVICE_RESET` after a successful reset (also after OPTIONS > VIDEO's live mode change) |
| device `EndScene` (35) / `Present` (15) | pointer | INNER | `PSE_EVENT_END_SCENE` / `PSE_EVENT_PRESENT` (data = the device) before the call |

INNER = `PSE_DS1_LAYER_PRIORITY_INNER` (-1000): the layer's hook is the closest to the game; a plugin hooking the same
site above -1000 runs first and reaches the layer through its `next`. OUTER = `PSE_DS1_LAYER_PRIORITY_OUTER` (1000):
the layer runs first, so its messages bracket everything other hooks do there (`pse/pse_game_ds1.h`). Sender of the
messages: "pse". A hook the broker refuses costs only its event (a `[fail]` line), never the layer.

**With DesertStormFix** (its hooks at priority 0 on the same sites; `tests/live/test_layer_events.py`):

- frame: `dsfix.framecap` runs first - frame-cap wait, its lockstep / determinism step (`features::OnFrameEarly`) -
  then the layer: tasks, `PSE_EVENT_FRAME` (other plugins' subscribers at their priorities, then DesertStormFix's
  per-frame work `features::OnFrame` as a subscriber at -1000000), then the game's frame. Online catch-up frames repeat
  that (lockstep step, then the layer). The same order as before 2026-10-19, when framecap called `pse::Frame` from
  inside `features::OnFrame`. Without a layer hook on the site (an old `pse_ds1.dll`) framecap calls `pse::Frame`
  itself.
- render: `dsfix.diagnostics`' frame recorder times the layer's PRE / POST_RENDER events as render time. On a lost
  device online, DesertStormFix skips the render (no PRE / POST_RENDER, no PRESENT that frame).
- level init: the layer first (NEW_GAME, LEVEL_LOAD_BEGIN), then `dsfix.scripthost` (Lua level set-up, unit / vehicle
  file borrowing), the game, then LEVEL_LOAD_END.
- D3D: the layer's `Direct3DCreate8` hook is outer, so with Renderer 1-3 it sees DesertStormFix's D3D9-wrapper object
  and chains on that class; `dsfix.graphics` (priority 0) runs before the layer's EndScene / Present (its overlay is
  drawn before `PSE_EVENT_END_SCENE`, its V-Sync wait and photo capture before `PSE_EVENT_PRESENT`) and inside the
  layer's Reset / CreateDevice (MSAA fallback included before `DEVICE_RESET` / `DEVICE_CREATED`).
- saves: DesertStormFix's chunks and the old sidecar reader - [serialization.md](serialization.md#ds1-when-the-co-save-is-written-and-read).

**Without DesertStormFix**: everything above still runs (`tests/live/test_layer_without_dsfix.py`: an example plugin
logs frames, render / EndScene / Present, device created, NEW_GAME, level load / unload, save / load with its own
co-save chunk round trip). Not sent without it: `PSE_MSG_EXIT` (DesertStormFix's window procedure sends it). No frame
cap either - the game then runs as fast as it can.

## What is not here (yet)

Writing anything (spawning, damage, levels, settings of the game), game events (killed, damaged …),
cameras, renderer, script API calls, IDs instead of addresses. Those come with later versions;
until then plugins that need them use PSELib-style raw addresses and set `PSE_PLUGIN_USES_RAW_ADDRESSES`.

## First user

`dsfix.discord` (`plugins/discord/discord.cpp`, [ds1-plugins.md](../ds1-plugins.md#external-plugins)) uses
`LevelName`, `LevelKind`, `LevelTitle`, `SplitScreenPlayers` and `ExeKnown` – nothing else of the game.
