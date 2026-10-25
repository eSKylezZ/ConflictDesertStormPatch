# Mission replacements – a mod's Lua in place of a mission DLL

A mod can run its own Lua instead of one of the game's mission DLLs (`Mission*.dll`, `Training*.dll`). The level
stays what it is – the same name, archive, sound cache, scene, briefing screen, saves and menu entry – only its
script comes from the mod. This is how an edited **readable mission** (ModKit `cli/ds_mission2lua_readable.py`, Lua
made from the player's own DLL) is played, and how the ModKit example **RestoredMissions**
(`examples/restored_missions/build.py`) puts cut content back into the training missions and Mission 1.

Needs DesertStormFix 3.3 (the `dsfix.missions` plugin, `plugins/missions/missionsource.cpp`). Works in every build,
with the MISSIONS FROM LUA setting on or off.

## The file

`<game>\Mods\<Mod>\<Stem>.lua`, where `<Stem>.dll` is the mission it replaces, with this line in its **header** (the
first lines of the file, as long as they start with `--`):

```lua
--@archive training1
--@replaces Training1.dll
-- (any other comment lines)
local MISSION = ...
```

- The name after `--@replaces` must be the DLL's name (case doesn't matter). A file of the right name without the
  line is not a replacement (it stays an ordinary `.lua` level, `level Training1.lua` in the console).
- `--@archive` and other `--@` lines are not read for a replacement: the level's files come from the DLL's name as
  always (`training1.dat`, `training1.sch`, …).
- Mods are searched in their usual order (later folders win, `_`-prefixed folders are off).

## Which script runs

When the game loads `<Stem>.dll`:

1. a mod has `<Stem>.dll` itself → that DLL (a mod's DLL always wins);
2. a mod has `<Stem>.lua` with `--@replaces <Stem>.dll` → **the mod's Lua**;
3. MISSIONS FROM LUA on → the cached translation of the game's DLL (`PSE\Cache\missions`);
4. else the game's DLL.

If the mod's Lua fails to load or start, the game's DLL runs instead (and the reason is logged). The
F11 overlay shows "mission script: Lua (mod mission)" while one runs.

## What the Lua runs in

The same host as the translated missions: its own Lua 5.4 state (base, string, table, math, utf8, coroutine), then

- `x86rt.install()` – DesertStormFix's mission runtime (in `dsfix.missions`): `X86RT.load_image`, memory access
  (`r8 … wd`, SEH-guarded), native calls, `set_object`, `anim_loaded`, `stack_top`, `fnstcw`;
- `game.log(...)` / `print(...)` → `DesertStormFix.log` ("Mission source: [lua] …");
- every plugin's Lua namespace ([lua.md](lua.md)); the events `pse.lua.level_start` / `level_end` fire for it.

Not there: the `game.*` natives of `.lua` levels (scripthost) – a replacement talks to the game through the script API
like the DLL did. The script must, in its global `init()`, give the game a script object: `X86RT.set_object(p)` with a
block holding the DLL's six callbacks (init, update, message, save, load, size) – a readable mission's generated
`scripts/start.lua` does this. No script object after `init()` → the DLL runs.

Saves are whatever the script's save callbacks write: a readable mission writes the DLL's own save block, so saves made
with the replacement load with the DLL and back.

## Messages

- `DesertStormFix.log`: "Mission source: Training1.dll replaced by the mod mission <path>"; a failure:
  "[fail] Mission source: Training1.dll: load|run|init|start: <Lua error> - the DLL runs instead".
- `Mods\DesertStormFix-report.txt` (the mod report): "Training1.dll runs as the mod mission …", or "[fail] … failed
  to start".

## Making one from a readable mission

```
python cli/ds_mission2lua_readable.py "<game>\Training1.dll" -o work      (work\Training1\ + work\Training1.lua)
… edit work\Training1\scripts\events.lua …
python cli/ds_mission2lua_readable.py --bundle work\Training1               (rebuilds work\Training1.lua)
```

then add `--@replaces Training1.dll` as the bundle's second line and copy it to `<game>\Mods\<Mod>\Training1.lua`.
Generated missions are game code made from the player's files: share a script that builds them (like
`examples/restored_missions/build.py`: anchored edits + appended Lua), not the built files.

Since ModKit 2026-10-20 a readable mission's `scripts/runtime.lua` wraps **every** named script API slot (not only the
ones its DLL calls), so an edit can call any of them by name (`api.play_stream`, `api.set_view_camera`, …).

## Pitfall: the movie sound mode

`api.set_sound_mode(1)` (+0x7FC) asks for the game's movie sound mode, in which **every ordinary sound is refused**
(only streams play). The switch is a toggle that takes effect when the next sound *starts* (`0x55E440`), and
`set_sound_mode(0)` only asks for the switch back – it happens when the next stream starts. The shipped missions always
start one (the overview narration) after a film; a training mission doesn't, so a film that calls `set_sound_mode(1)`
there leaves the level silent for good: speech never plays, `object_sound_playing` stays 0, and scripts that wait for
a line to end move on a second later (the lines' requests then come one after another). Leave the sound mode alone
for a film added to a level unless a stream plays after its `set_sound_mode(0)`.
