# DesertStorm2Fix

Enhancement patch for **Conflict: Desert Storm II – Back to Baghdad** (PC, 2003), delivered as a `dinput8.dll` plugin
that patches the game in memory. The exe on disk is never modified.

Sister project of [DesertStormFix](../../README.md) (the first game). Status: **plugin frame only**: it
loads, identifies the exe and reads its settings. Features are ported one by one, see [PORTING.md](PORTING.md).

## Build

Visual Studio 2022 (MSVC x86) + CMake 3.21+. This folder is its own CMake project (part of the cross-game repo,
`games/ds2`), from here:

```
cmake --preset x86 && cmake --build --preset release
```

or from the repo root (presets `ds2`, `ds2-dist`; binary dirs `build-ds2`, `build-ds2-dist`):

```
cmake --preset ds2 && cmake --build --preset ds2-release
```

Copy `CMakeUserPresets.example.json` to `CMakeUserPresets.json` and set `DS_GAME_DIR` to deploy into your install
(`x86-local` / `release-local`).

## Layout

- `src/dllmain.cpp` – proxy exports + entry-point hook, `Init()`.
- `src/core/` – byte patching, registry settings (`HKLM\SOFTWARE\SCi Games\CDS II\Enhancements`), log, pad reading.
- `src/features/` – DS2 features (so far: exe identification).
- `../ds1/src/features/`, `../ds1/plugins/` – DesertStormFix's features (DS1 addresses), the source to port from.
- `scripts/`, `src/generated/` – input prompt icons and their generators (sources: the repo root's `assets/`).

## License

MIT (see LICENSE). Kenney Input Prompts: CC0. Controller art: CC BY 3.0 (Zacksly).
