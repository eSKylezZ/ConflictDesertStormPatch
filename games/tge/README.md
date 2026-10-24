# GreatEscapeFix

Enhancement patch for **The Great Escape** (PC, Pivotal Games 2003), built on the Pivotal Script Extender (PSE) from
this repo: the game-neutral shim `dinput8.dll`, `PSE\pse_loader.dll`, the game layer `PSE\Games\pse_tge.dll`
(`pse/games/tge/`) and GreatEscapeFix as the host plugin `PSE\Plugins\tgefix.dll` (`src/`).

Features:

- **Widescreen**: every display mode in the launcher (was 640x480 / 800x600 / 1024x768 / 1280x1024); missions render
  16:9 without stretching.
- **Menu / HUD scaling**: the GUI laid out on a virtual screen and drawn k times bigger (800 x 600 look at any size).
- **Windowed / borderless**: the launcher's hidden Windowed box shown and working.
- **Mods folder**: `<game>\Mods\<mod>\...\<FILE>` replaces or adds game files by name; report in
  `Mods\GreatEscapeFix-report.txt`.

Not yet: saves / co-save, an options menu, sharper scaled text.

Build (Visual Studio 2022 Build Tools, x86):

```
cmake --preset tge && cmake --build --preset tge                # repo root: build-tge/tree/RelWithDebInfo
cmake --preset tge-dist && cmake --build --preset tge-dist      # release: build-tge-dist/package + zip
```

or from this folder with `--preset x86` / `dist`. `-DTGE_GAME_DIR=<install>` copies the tree into the game.
Reference: [docs/sdk/reference/game-tge.md](../../docs/sdk/reference/game-tge.md).
