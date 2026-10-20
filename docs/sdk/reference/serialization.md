# pse.serialization (version 1)

Plugin data stored with each game save, in a co-save file next to it. Header: `pse/pse_api.h`. API 0 = unstable
(phase 1).

```c
const PSESerializationInterface* ser = pse->Query(pse->plugin, PSE_SERIALIZATION_INTERFACE, 1);
```

All functions: game thread. The callbacks run while the game saves / loads.

## How it works

- **Save**: the loader calls every plugin's save callback (in plugin order); each writes any number of chunks. The
  co-save is written next to the game's save (`<save>.pse`).
- **Load**: each plugin's load callback gets a reader over its own chunks. A plugin with no chunks in that save (or
  no load callback), a missing co-save, or a damaged one gets its **revert** callback instead.
- **New game**: every plugin's revert callback.
- **Plugins that aren't installed**: their chunks are kept when the save is rewritten, so removing a plugin for a
  while doesn't lose its data. Per missing plugin, the chunks of the save loaded this session win (even if that file
  was deleted since); otherwise those already in the file being overwritten are kept. A new game forgets the
  session's ones.
- A plugin that is installed but writes nothing on save drops its old chunks from that save.

Revert means "reset to a fresh state": clear what the plugin keeps per game.

## Functions

### SetCallbacks

```c
typedef void (*PSESaveCallback)(PSESaveWriter* w, void* ctx);
typedef void (*PSELoadCallback)(PSESaveReader* r, void* ctx);
typedef void (*PSERevertCallback)(void* ctx);
PSEResult SetCallbacks(PSEPluginHandle plugin, PSESaveCallback save, PSELoadCallback load, PSERevertCallback revert,
                       void* ctx);
```

In `Load`. Any callback may be NULL. Calling it again replaces the earlier set. Errors: `PSE_E_INVALID_ARG` (bad
handle).

### WriteChunk

```c
PSEResult WriteChunk(PSESaveWriter* w, uint32_t tag, uint32_t version, const void* data, uint32_t len);
```

Only inside the save callback, with its writer. Any tags, several chunks with the same tag allowed, `len` 0 allowed.
The data is copied. `version` is yours: bump it when the chunk layout changes and handle old versions on load.
Errors: `PSE_E_INVALID_ARG` (NULL writer, NULL data with `len` > 0).

```c
static void Save(PSESaveWriter* w, void* ctx) {
    ser->WriteChunk(w, PSE_TAG('V','E','H','1'), 2, g_swaps, g_swapCount * sizeof(Swap));
}
```

### NextChunk

```c
int NextChunk(PSESaveReader* r, uint32_t* tag, uint32_t* version, uint32_t* len);
```

Only inside the load callback. Moves to the plugin's next chunk in file order and fills the out-parameters (each may
be NULL). Returns 0 when there are no more.

### Read

```c
uint32_t Read(PSESaveReader* r, void* buf, uint32_t len);
```

Reads up to `len` bytes of the current chunk from where the last `Read` stopped; returns the bytes read (0 at the
end, or before the first `NextChunk`). Partial reads are fine; unread bytes are skipped by the next `NextChunk`.

```c
static void Load(PSESaveReader* r, void* ctx) {
    uint32_t tag, version, len;
    while (ser->NextChunk(r, &tag, &version, &len)) {
        if (tag == PSE_TAG('V','E','H','1') && version == 2) g_swapCount = ser->Read(r, g_swaps, sizeof g_swaps) / sizeof(Swap);
    }
}
static void Revert(void* ctx) { g_swapCount = 0; }
ser->SetCallbacks(me, Save, Load, Revert, NULL);
```

## File format

Little endian, no padding:

```text
"PSE" 0x01                      magic + format version
u32  chunk count
chunk × count:
  u8   name length (1..64)
  char name[length]             plugin name, no NUL
  u32  tag
  u32  version
  u32  length
  u8   data[length]
```

Chunks are grouped by plugin (installed plugins in plugin order, then the kept ones). A file that is too short, has
the wrong magic, a chunk running past the end or bytes after the last chunk is rejected as a whole: a warning in the
loader log, and every plugin gets revert. Saving over it writes a fresh file.

Writes are atomic: the file is written to `<co-save>.tmp` and then moved over the old one, so a crash mid-save never
leaves half a co-save.

## DS1: when the co-save is written and read

The co-save of `Documents\SCi\DesertStorm\<name>.sav` is **`<name>.pse`** in the same folder (`.sav` replaced). The
game layer `PSE\Games\pse_ds1.dll` does this (`pse/games/ds1/ds1_saves.cpp`; entry hooks on the game's saver
`FUN_00415650` and loader `FUN_00415B60` in `ds1_events.cpp`, owner `pse.game.ds1`, priority
`PSE_DS1_LAYER_PRIORITY_OUTER`) - with or without DesertStormFix installed:

| Game action | What plugins get | Messages (`sender` "pse", `data` = the save path, NUL-terminated) |
| --- | --- | --- |
| SAVE GAME (save menu, overwrite confirmation; any call of the saver with a path and flag 0) | right after the game wrote the `.sav`: every save callback, then the `.pse` is written | `PSE_MSG_SAVE_BEGIN` before the game writes, `PSE_MSG_SAVE_END` after (also when the save failed - then no `.pse`) |
| LOAD GAME (any call of the loader with a path) | right after the game read the `.sav` (before the level loads): load / revert callbacks | `PSE_MSG_LOAD_BEGIN` before the game reads, `PSE_MSG_LOAD_END` after the plugins' callbacks |
| A game level starts from the front end without a save (new campaign, mission list, console `level`, online START) | every revert callback, at the start of `Level_Init` (before `PSE_EVENT_LEVEL_LOAD_BEGIN`) | `PSE_MSG_NEW_GAME` (`data` = the level name) |

- Level changes **inside** a game (mission → next mission, RESTART LEVEL, a level started from a mission) send nothing:
  plugin state carries on, as the game's own campaign state does. A level reached from a loaded save never gets
  NEW_GAME.
- Saves that write no file (the game's "DS_auto" call, flag 1) don't touch the co-save and send no messages.
- The game never deletes or renames save files. A `.pse` whose `.sav` is gone (deleted outside the game) is removed
  before a new save of that name is written, so its chunks can't end up in an unrelated save.
- Steam Cloud syncs the save folder (`steam_autocloud.vdf`); the `.pse` files go with the saves.
- **Older saves**: until DesertStormFix 3.3.0-Alpha the same data was a text sidecar `<name>.sav.dsfix`. A save with a
  `.dsfix` and no `.pse` is read through it: the lines become the chunks the plugins write today (`dsfix.loadout`
  `KITS`, `dsfix.gear` `GEAR`, `dsfix.vehicles` `VEHS`, [ds1-plugins.md](../ds1-plugins.md#co-save-chunks)) and go
  through the normal load callbacks; other plugins revert. The `.dsfix` is never written or deleted any more; the next
  save of that slot writes a `.pse`, which wins from then on. Only DesertStormFix knows that format: it hands the
  layer its reader (`src/core/pse_saves.cpp` `LegacyCosave`, through the private `pse.game.ds1.host`
  `SetLegacyCosave`); without DesertStormFix such a save simply reverts every plugin.
- DesertStormFix's own chunks are captured in their save callbacks (`dsfix.loadout` the squad's spawned weapons,
  `dsfix.gear` the squad's gear) - right after the game's write, as its old call-site hooks did; `dsfix.vehicles` marks
  "the next level is a loaded save" in its load / revert callback between `LOAD_BEGIN` and `LOAD_END`.

## For the game layer (not part of the SDK)

`pse::cosave::Save(path)`, `Load(path)` (returns false if the file was missing or damaged), `LoadBytes(data, len,
label)` (the same from a co-save image in memory - the compatibility reader above builds one) and `NewGame()` in
`pse/loader/loader.h`. The game layer calls them around the game's own save / load and sends the matching
`PSE_MSG_SAVE_*` / `PSE_MSG_LOAD_*` / `PSE_MSG_NEW_GAME` messages ([messaging](messaging.md)) itself (DS1:
`pse/games/ds1/ds1_saves.cpp`, tests `pse/tests/test_ds1_layer_events.cpp`, live `tests/live/test_saves_live.py`,
`test_layer_without_dsfix.py`).
