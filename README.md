# ConflictDesertStormPatch
Patch for the PC version of Conflict: Desert Storm, adding 16x9 support, controller support, split-screen and more

## Download

- **DesertStormFix (the compiled `dinput8.dll`)**: [Nexus Mods](https://www.nexusmods.com/games/conflictdesertstorm/mods/1)
- **Blender add-on** (import / export the game's models, characters and animations for mods):
  [Nexus Mods](https://www.nexusmods.com/games/conflictdesertstorm/mods/2)

## Features
- Widescreen resolutions in the launcher (the 3D view is already Hor+)
- HUD, text and menus scaled to the resolution (no stretching on 16:9)
- DualShock 4 / DualSense support (button layout + D-pad)
- Fixed game speed at high frame rates, plus a frame cap (monitor refresh rate, max 240 fps)
- Settings in the launcher, and the launcher can be used with a controller

## Games
One repository for the Pivotal Games "Conflict" series patches. They share the Pivotal Script Extender in `pse/`
(loader + one game layer per game in `pse/games/<id>`), the input-prompt art in `assets/` and Lua in `third_party/`.
What each game's patch does and what is still to do: **[FEATURES.md](FEATURES.md)** (feature matrix for all six games).

| Game | Patch | Where | State |
| --- | --- | --- | --- |
| Conflict: Desert Storm (`ds1`) | DesertStormFix | [games/ds1](games/ds1), PSE layer `pse/games/ds1` | full feature set (this README) |
| The Great Escape (`tge`) | GreatEscapeFix | [games/tge](games/tge/README.md), PSE layer `pse/games/tge` | widescreen, menu / HUD scaling, windowed, frame cap, pads (unverified), Mods folder + MODS menu, mod tables / sounds / levels, Lua missions |
| Conflict: Desert Storm II (`ds2`) | DesertStorm2Fix | [games/ds2](games/ds2/README.md), PSE layer `pse/games/ds2` | widescreen, HUD / menu scaling, split screen 2-4 + co-op join, Mods folder + MODS menu, `.skin` / `.weapon`, mod sounds / levels, Lua missions, limits, loadout screen |
| Conflict: Vietnam (`cv`) | VietnamFix | [games/cv](games/cv/README.md), PSE layer `pse/games/cv` | widescreen, HUD / menu scaling, Mods folder + MODS menu |
| Conflict: Global Storm (`cgs`) | GlobalStormFix | [games/cgs](games/cgs/README.md), PSE layer `pse/games/cgs` | widescreen, high-res fonts, menus kept 4:3, Mods folder (packed assets) + MODS menu |
| Conflict: Denied Ops (`do`) | DeniedOpsFix | [games/deniedops](games/deniedops), PSE layer `pse/games/deniedops` | widescreen, Mods folder + MODS menu |

Each `games/<id>` is its own CMake project (`cmake -S games/<id> --preset x86`), or build it from the repo root with
the presets `ds2`, `cv`, `cgs` (+ `-dist`; `ds2-pse` = `ds2fix.dll`). The ports start from DesertStormFix's own
sources in `games/ds1`.

## Install
Copy `dinput8.dll` into the game folder (next to `DesertStorm.exe`). The exe itself is not modified;
delete the DLL to uninstall. Settings live in the registry (`HKCU\Software\PSE\ds1\Plugins\dsfix`, see
[docs/sdk/ds1-settings.md](docs/sdk/ds1-settings.md)) and are changed from the launcher / Options menu.

## Building
Requirements: Visual Studio 2022 Build Tools (C++ workload, includes CMake) and VS Code with the
C/C++ and CMake Tools extensions (recommended automatically when you open the folder). You need your
own copy of the game (Steam or GOG) to test.

```
cmake --preset x86
cmake --build --preset release      # build/RelWithDebInfo/dinput8.dll
cmake --preset dist
cmake --build --preset dist         # release package: build-dist/DesertStormFix-<version>.zip
```

To build straight into your game folder, copy `CMakeUserPresets.example.json` to
`CMakeUserPresets.json` (ignored by git) and set `DS_GAME_DIR` to your install. The `*-local` presets
then deploy on every build; in VS Code you can add a `launch.json` that starts `DesertStorm.exe` under
the debugger.

DesertStormFix's sources live in `games/ds1/` (`src/`, `plugins/`, `scripts/`, `dist/`); the root
`CMakeLists.txt` builds them together with PSE (`pse/`). Byte patches are defined per feature in
`games/ds1/scripts/patch_defs.py`; after changing them run `python games/ds1/scripts/gen_patch_tables.py`
to regenerate `games/ds1/src/generated/patch_tables.h`. Button icons are baked by
`python games/ds1/scripts/gen_prompt_icons.py` (needs Pillow).

### What not to commit
Nothing from the game itself: no executables, DLLs, level data, textures, sounds or disassembly
exports/databases. `.gitignore` blocks the usual file types - keep it that way.

## Credits
Controller button icons: [Input Prompts](https://kenney-assets.itch.io/input-prompts) by Kenney (CC0), in `assets/input-prompts`.
