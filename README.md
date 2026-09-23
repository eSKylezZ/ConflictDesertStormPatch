# ConflictDesertStormPatch
Patch for the PC version of Conflict: Desert Storm, adding 16x9 support, controller support, split-screen and more

## Features
- Widescreen resolutions in the launcher (the 3D view is already Hor+)
- HUD, text and menus scaled to the resolution (no stretching on 16:9)
- DualShock 4 / DualSense support (button layout + D-pad)
- Fixed game speed at high frame rates, plus a frame cap (monitor refresh rate, max 240 fps)
- Settings in the launcher, and the launcher can be used with a controller

## Install
Copy `dinput8.dll` into the game folder (next to `DesertStorm.exe`). The exe itself is not modified;
delete the DLL to uninstall. Settings live in the game's registry key and will be editable from the
launcher / Options menu.

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

Byte patches are defined per feature in `scripts/patch_defs.py`; after changing them run
`python scripts/gen_patch_tables.py` to regenerate `src/generated/patch_tables.h`. Button icons are
baked by `python scripts/gen_prompt_icons.py` (needs Pillow).

### What not to commit
Nothing from the game itself: no executables, DLLs, level data, textures, sounds or disassembly
exports/databases. `.gitignore` blocks the usual file types - keep it that way.

## Credits
Controller button icons: [Input Prompts](https://kenney-assets.itch.io/input-prompts) by Kenney (CC0), in `assets/input-prompts`.
