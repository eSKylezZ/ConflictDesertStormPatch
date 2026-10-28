# Mission patches – change a mission without shipping it

A **mission patch** changes one of the game's missions (`Mission*.dll`, `Training*.dll`) and ships **only your own
code**. DesertStormFix turns the player's own mission DLL into readable Lua, applies your patch to it, and runs the
result in place of the DLL. Nothing of the game is in your mod, and every mod that patches the same mission applies.

Needs DesertStormFix 3.3 (plugin `dsfix.missions`). It works in every build, with MISSIONS FROM LUA on or off.

## The file

`<game>\Mods\<Mod>\<Stem>.patch.lua` (any sub folder except the mod's `records\`), for example
`Mods\MyMod\Mission1.patch.lua`:

```lua
--@patches Mission1.dll
--@readable 1
-- MyMod: Foley takes the Medi-Kit too.

--[[@edit
        api.give_item(obj.HERO_FOLEY, obj.FOLEY_AMMO)
--@to
        api.give_item(obj.HERO_FOLEY, obj.FOLEY_AMMO)
        my_foley_takes_medikit()
]]

function my_foley_takes_medikit()
  api.give_item(SCENE.objects.HERO_FOLEY, SCENE.objects.FOLEY_MEDIKIT)
end
```

- **Header** (the first lines, as long as they start with `--`):
  - `--@patches <Stem>.dll` names the mission. It is required, and case doesn't matter.
  - `--@readable <n>` is the readable-mission version the patch was written against (currently **1**). A patch for
    another version is left out.
- **Edit blocks**: `--[[@edit`, then the lines of the readable mission to change, then a line `--@to`, then the lines
  that replace them, then `]]`. Any long-bracket level works (`--[==[@edit` … `]==]`) when the lines contain `]]`.
  - The lines before `--@to` must appear in the readable mission **exactly once**, as whole consecutive lines,
    indentation included. Copy them from the ModKit's output (see below).
  - An empty `--@to` part deletes the lines.
  - Edit blocks are Lua comments, so the file stays valid Lua.
- **Everything else is Lua.** It runs once, after the mission's own code is defined and before the level starts. It
  runs in the mission's environment:
  - the mission's functions (`mission_update`, `on_killed`, `f_47B0` …) and globals (`mission_phase`, `g_14258` …);
  - `api.*`: every named script API slot, not only the ones the mission calls;
  - `obj.*` / `wp.*` / `rgn.*`, `mem.*`, `scratch()`, `f32()`, `call()`.

  Defining a function replaces the mission's own one everywhere it is called, including the callbacks the game makes
  (update, messages, queued calls). Keep the old one in a local to call through:

  ```lua
  local dll_update = mission_update
  function mission_update(...)
    -- yours
    return dll_update(...)
  end
  ```

### What DesertStormFix adds for patches

| Name | What it is |
| --- | --- |
| `SCENE.objects.NAME` / `SCENE.waypoints.NAME` / `SCENE.regions.NAME` | **Every** editor ID of the level's scene → its number. The mission's `obj.*` / `wp.*` / `rgn.*` hold only the ones its own logic names. |
| `SCENE.cameras.NAME` | The scene's camera records → their number for `api.set_view_camera(n)` (the n-th camera record, 0 = the first). |
| `TEXT.KEY` | The catalog hash of a text key: the level's texts (`M01Strs.loc` …), the game's texts, and a mod's `.strings` keys, as `api.show_message(TEXT.MY_KEY, 3000)` wants them. |
| `MOVIE.KEY` | A film subtitle key's hash, as `api.show_movie_text` wants it. |
| `text_hash(key)` / `movie_hash(key)` | The same as functions. |
| `X` | DesertStormFix's mission runtime (`X86RT`): `X.r32` / `X.w32` / `X.call` …, as the readable mission's own glue uses it. |
| `game.log(...)` / `print(...)` | Writes to `DesertStormFix.log` ("Mission source: [lua] …"). |

## Writing one

1. Make the readable mission from your own game files, with the ModKit:

   ```
   python cli/ds_mission2lua_readable.py "<game>\Mission1.dll" -o work
   ```

   or with DesertStormFix's `pse_build`: `pse_build missions --readable --dll "<game>\Mission1.dll" --out work`. Both
   give the same text byte for byte (the gate: `scripts/m2r_gate.py`), and DesertStormFix makes the same text in the
   game. If `Mission1.dll` isn't in the game folder, it is in `<game>\PSE\Originals`. Read
   `work\Mission1\scripts\events.lua`.
2. Write `Mission1.patch.lua`: copy the lines you want to change into edit blocks, and put your own code after them.
   Or edit `work\Mission1\scripts\events.lua` itself and let the ModKit write the edit blocks for you:
   `python cli/ds_mission_patch.py --make "<game>\Mission1.dll" work\Mission1\scripts\events.lua -o Mission1.patch.lua`.
   It makes one block per changed place, each with just enough context to match once. Add your own Lua below the
   blocks.
3. Check it without the game:

   ```
   pse_build missions --readable --dll "<game>\Mission1.dll" --out work --patch Mission1.patch.lua
   ```

   It says whether every edit applied and whether the result compiles, and writes `work\Mission1.patched.lua`. In
   that file the patches are inline, an approximation of what the game runs.
4. Put it in `Mods\<Mod>\` and start the mission. The log (`DesertStormFix.log`) and the mod report
   (`Mods\DesertStormFix-report.txt`) say "Mission1.dll runs as readable Lua with 1 mission patch(es): …". The F11
   overlay shows "mission script: Lua (readable + mission patches)".

**Share the patch, never the generated files**: the generated files are game code made from your game files.

## Which script runs

When the game loads `<Stem>.dll`:

1. a mod has `<Stem>.dll` itself → that DLL;
2. a mod has `<Stem>.lua` with `--@replaces <Stem>.dll` ([mission replacements](mission-replacement.md)) → that Lua;
   any patches for the mission are not used;
3. **any enabled mod has `<Stem>.patch.lua`** → the readable mission + every such patch;
4. MISSIONS FROM LUA on → the cached stage-1 translation of the DLL;
5. else the game's DLL.

## Several patches, and failures

- Patches apply in the mods' **load order** (the MODS menu order; the one loaded last applies last). Each sees the
  mission as the earlier ones left it, so two patches that edit the same lines conflict: the later one's edit no
  longer matches. That patch is left out and the report says so.
- A patch is all or nothing. If an edit matches 0 or more than 1 places, the header is wrong, or the file doesn't
  compile, the whole patch is left out and the others still apply:
  `[fail] MyMod\Mission1.patch.lua: the edit block at line 5 ("api.give_item(...)" ...) matches 0 places in the mission, not one - ... - this patch is left out`.
- If a patch's code fails when it runs, or the mission fails to start with the patches, the mission runs **without
  patches** (stage-1 translation or the DLL). The reason is in `DesertStormFix.log`.
- If no patch applies, nothing changes: the mission runs as it would without them.

## Where the readable mission comes from

DesertStormFix makes it the first time a patched mission loads (0.3–3.5 s, once). It uses the C++ port of the ModKit's
`mission2lua_readable` (`pse/build/mission2lua/m2r_*.cpp`) and caches the result in
`<game>\PSE\Cache\missions\readable\<Stem>.lua` + `.stamp` (else `%LOCALAPPDATA%\PSE\ds1-<hash>\missions\readable`). It
is made again when the DLL or the translator changes. Editor IDs come from the level's scene in the game's own archives,
not from a mod's edited scene, so names are the same for everyone.

The readable mission behaves like the DLL. Mission 1 was checked in game: API trace and world hashes identical, save
block identical. All 18 missions behave the same as the stage-1 translation in the ModKit's offline differential test
(`tests/mission2lua_diff.py`, 2 seeds × 3000 frames each). Saves are the DLL's own save block, so saves made with
patches load without them and back. A patch that changes what the mission saves is its own responsibility.

## Example

ModKit `examples/restored_missions/RestoredMissions/`: cut content put back into Training 1–3 and Mission 1. The mod is
four patch files plus a stream-entry `.sch` for Training 1's intro film. It covers edit blocks, overriding the
mission's functions, `SCENE` / `TEXT` / `MOVIE`, and `api.show_message_raw` to show a string the DLL already holds
without copying it.
