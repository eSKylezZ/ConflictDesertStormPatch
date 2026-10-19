# GlobalStormFix

Enhancement patch for **Conflict: Global Storm / Global Terror** (PC, 2005), delivered as a `dinput8.dll` plugin
that patches the game in memory. The exe on disk is never modified.

Sister project of [DesertStormFix](../../README.md) and DesertStorm2Fix. Status: widescreen modes, font
fix and menu aspect done; more features are ported one by one, see [PORTING.md](PORTING.md).

## Build

Visual Studio 2022 (MSVC x86) + CMake 3.21+. This folder is its own CMake project (part of the cross-game repo,
`games/cgs`), from here:

```
cmake --preset x86 && cmake --build --preset release
```

or from the repo root (presets `cgs`, `cgs-dist`; binary dirs `build-cgs`, `build-cgs-dist`):

```
cmake --preset cgs && cmake --build --preset cgs-release
```

Copy `CMakeUserPresets.example.json` to `CMakeUserPresets.json` and set `DS_GAME_DIR` to deploy into your install
(`x86-local` / `release-local`).

## Layout

- `src/dllmain.cpp` – proxy exports + entry-point hook, `Init()`.
- `src/core/` – byte patching, registry settings (`HKLM\SOFTWARE\Pivotal\Conflict Global\Enhancements`), log, pad reading.
- `src/features/` – Global Storm features (exe identification, widescreen modes, fonts / menu aspect, D3D9 hooks, frame hook, dev background test mode).
- `../ds1-reference/ds1-features/` – DesertStormFix's features (DS1 addresses, snapshot shared by all games), the source to port from. Not built.
- `scripts/`, `src/generated/` – input prompt icons and their generators (sources: the repo root's `assets/`).

## License

MIT (see LICENSE). Kenney Input Prompts: CC0. Controller art: CC BY 3.0 (Zacksly).
