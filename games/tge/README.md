# GreatEscapeFix

Enhancement patch for **The Great Escape** (PC, Pivotal Games 2003), built on the Pivotal Script Extender (PSE) from
this repo: the game-neutral shim `dinput8.dll`, `PSE\pse_loader.dll`, the game layer `PSE\Games\pse_tge.dll`
(`pse/games/tge/`) and GreatEscapeFix as the host plugin `PSE\Plugins\tgefix.dll` (`src/`).

Features:

- **Widescreen**: every display mode in the launcher (was 640x480 / 800x600 / 1024x768 / 1280x1024); missions render
  16:9 without stretching.
- **Menu / HUD scaling**: the GUI laid out on a virtual screen and drawn k times bigger (800 x 600 look at any size).
- **Windowed / borderless**: the launcher's hidden Windowed box shown and working.
- **Frame cap**: monitor refresh, max 240 (the game ran at 1000+ fps).
- **Sharper text**: font pages enlarged 2-4x with crisp edges while text is scaled up.
- **Films**: Bink films from mods, `game.film` from Lua; the menu background / story films fill a widescreen.
- **Mods folder**: `<game>\Mods\<mod>\...\<FILE>` replaces or adds game files by name; report in
  `Mods\GreatEscapeFix-report.txt`. Merged tables, `.strings`, `.weapon` / `.vehicle` / `.unit` files, levels from
  mods (`.uses` / `.envpatch` / `.dllpatch` / `.level`), sound packs and weapon sounds, weapon pictures. MODS menu on
  the main menu.
- **Lua missions and campaigns**: `.lua` levels on top of a mission DLL, `campaign.txt`, CUSTOM MISSIONS on the main
  menu, `+level` / `+campaign`.
- **Controllers**: PlayStation / Xbox pads remapped to the game's layout (fire on R2 / RT by default), direct pad I/O
  (XInput, DualSense / DualShock 4 HID), rumble, trigger motors, adaptive triggers, light bar, hot-plug with a
  disconnect pause, the launcher driven with a pad, a button-layout picture.
- **Follows the device in use**: input mode, cursor, menu prompts, key names and tutorial texts switch between pad and
  keyboard / mouse.
- **Action prompts**: "[button] Pick up fire extinguisher", "Hold [button] Take down", "Talk" ... in missions.
- **In-game menus**: OPTIONS > GREATESCAPEFIX, OPTIONS > CONTROLS > CONTROLLER (+ BUTTON LAYOUT).

Status and to-do across all the games: the repo root's FEATURES.md and TODO.md.

Build (Visual Studio 2022 Build Tools, x86):

```
cmake --preset tge && cmake --build --preset tge                # repo root: build-tge/tree/RelWithDebInfo
cmake --preset tge-dist && cmake --build --preset tge-dist      # release: build-tge-dist/package + zip
```

or from this folder with `--preset x86` / `dist`. `-DTGE_GAME_DIR=<install>` copies the tree into the game.
Reference: [docs/sdk/reference/game-tge.md](../../docs/sdk/reference/game-tge.md).
