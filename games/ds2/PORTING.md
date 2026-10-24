# Porting DesertStormFix features to Conflict: Desert Storm II

DesertStorm2Fix starts from DesertStormFix's plugin frame. The DS1 feature sources to port from are DesertStormFix's own:
[games/ds1/src/features/](../ds1/src/features/) and [games/ds1/plugins/](../ds1/plugins/) (DS1 byte patches: [games/ds1/scripts/patch_defs.py](../ds1/scripts/patch_defs.py)).
Each feature moves into `src/features/` and `CMakeLists.txt` once its DS2 addresses are mapped and checked.

## Ported so far (2026-10-10)

| Feature | DS2 addresses | State |
| --- | --- | --- |
| Widescreen / 4K modes in the launcher (`widescreen.cpp`) | resolution whitelist `FUN_00420590` (640x480, 800x600, 1024x768, 1280x1024 only) -> `mov eax,1; ret`; list builder `FUN_00420900` | every adapter mode listed (verified up to 2560x1440 on a 1440p monitor; 4K appears with DSR) |
| HUD scaling (`hud.cpp`) | sheet scale setters `FUN_0052b1a0` (uniform, ret 4) / `FUN_0052b1e0` (x, y, flag, ret 0xC) jump to C++ copies x k = max(1, min(W/800, H/600)) * HudScalePercent; renderer `[0x680038]` W `+0x41A38` / H `+0x41A3C` | missions verified by background runs at 4K, 3440x1440, 2560x1440, 1920x1200, 1600x1200, 1280x1024 |
| Layout clamps (`generated/layout_clamps.h`, `tools/ds2/gen_clamps.py --cpp`) | 41 sites `c * max(800, W) / 800 * sheet scale` take the 800 path (incl. helper `FUN_0040c280` = max(800,W), used by the soldier panel's target icon); 15 raw-offset sites: forced path + their `mov X, 800` imm32 rewritten to 800*k at run time | portraits / target icons / panels aligned at every aspect |
| Text queue restore | loop at `0x50CBE0`: items set their own percent (100 -> k), `0x50CD6F` restores the saved (already scaled) value -> raw setter | |
| Small font `[0x62F438]` (ammo count, stats tables) | never handed to a setter (sheet stayed 1.0): set-up call `0x40C74E` also scales it, and text routine `FUN_005294e0` is entered through a stub that brings that font's sheet to k | ammo count scaled at 4K / 1440p |
| Front-end menus | designed for 1280x1024: pixel-sized 2D menus framed by a 3D monitor placed from the screen width (`FUN_00418af0`: distance W x 0.234375, globe scale 640/W, globe position table for W = 640/800/1024/1280) - lined up only at 1280x1024 | virtual 1280x1024 screen (`frontend.cpp` + `graphics.cpp`): renderer W/H fields + its viewport copy (+0x41A08.., `FUN_00537380` at 0x541F67) say 1280x1024 from device creation (re-set after engine init `0x4137E3`, front-end init `0x42CF9B`; real again at mission init `0x42CB64`); XYZRHW UP draws, viewports, scissors, clears mapped x min(W/1280, H/1024), centred; HUD k = 1 there; menu mouse `ScreenToClient` (0x4CB1B5, 0x4CB17C) mapped back. 4K = the 1280x1024 picture x2.11 |
| In-mission menus (`frontend.cpp`, 2026-10-22) | the pause menu (state 0x14) and the screens it opens (MISSION STATS, LOAD / SAVE GAME, OPTIONS state 6 ...) are the front end's monitor screens, made for 1280x1024 with unscaled art: in a mission (real screen, sheets x k) their text and boxes overflowed at every size above 800x600 (1280x1024 too). Entering a state, `FUN_0043df10` switches the menu backdrop with `FUN_0040b2b0(on)` (only caller `0x43DF2E`; flags `[0x609684]` / `[0x609688]`) - replaced: in a mission it also switches the virtual screen (renderer viewport list +0x30..+0x93 and full viewport +0x41A00..+0x41A23 saved, HUD sheets saved and put at k = 1; all back when the backdrop goes off) | verified by background runs (`Dev\HudStates`, `MenuKeys`): pause menu, OPTIONS, back to the mission with the HUD as before, at 1280x1024 and 2560x1440 |
| HUD fixes from DS1 (`hudfix.cpp`, 2026-10-22) | hint bar `FUN_00448fb0` (top, "To fire a weapon, press MOUSE 1"): box = measured text + fixed paddings `0x44908D` (+6 bottom) / `0x44909C` (-4 left) / `0x44909F` (+4 right) - imm8s now x k (text sat on the bottom edge); blinking item icon (around `0x423D90`): raw height getter `FUN_0052b140` at `0x423EC6` / `0x423FA2` and raw x offset getter `FUN_0052b160` at `0x423E48` / `0x423F76` -> scaled values (DS1 `0x44BEDC` / `0x44BFB8`); loading screen title (font `[0x62F440]`, `0x40F64A`) drew at the front end's 1.0 while the date / story had k -> its sheet gets 100 % (= k) first; `tools/ds2/gen_clamps.py` read its sheet-scale check out of step and took 3 scaled clamps (`0x423DF4`, `0x42C2A7`, `0x43F9B6`) for raw offsets (their result was scaled twice) - forward check added, table regenerated (44 scaled, 12 raw) | hint bar + loading title verified by background shots; blinking icon not seen on screen (no scripted way to trigger it) |
| In-game HUD option (`menus.cpp` OPTIONS rows, `hud.cpp` `SetHudScale`, 2026-10-22) | OPTIONS screen (state 6, vtable `0x5E7038`, set-up `FUN_0045d1f0`, update `FUN_0045d4f0`): items `FUN_0051d4f0` thiscall(item; id +4, description hash +0x24 (the box under the list), text +8 (label of a plain row), -, flags +0x10, label of a slider row +0x14, -, draw callback +0xC ...); item type +0xB0 (0 plain `FUN_0051e6e0`, 1 `0051e710`, 2 slider `0051e760`, 3 `0051e7b0`) - only type 2 is moved by Left / Right in the list read (`FUN_0051fa20` -> `FUN_0051d720`, list callbacks +0xB0 / +0xB8 = `FUN_0043b100` / `0043b1a0`: block +0x154 / +0x158 bit 4); sliders: draw callback `0x463EC0`, +0xB4 steps, +0xB8 float value; rows HUD SCALING (ON / OFF) + HUD SIZE (25-400 %, 15 steps) added before the empty-box fill (call `0x45D4D2` -> `FUN_0042d590`), handled at the update's list read `0x45D4FA` | live: the setting is saved, clamp sites follow on / off, every sheet the setters scaled gets the new k (`RescaleHudSheets`; inside the pause menu when it closes). Verified by background runs (MenuKeys L / R added) |
| Menu structure (`menus.cpp`, `menus.h`) | state setter `FUN_0043dfd0` (current `[0x63433C]`), state -> screen `FUN_0043bba0` (states 1..0x35), `createGameMenus` `FUN_0043bf40`; main menu state 2 (vtable `0x5E6F58`: set-up `FUN_0045cd90`, update `FUN_0045ca50`), the unused single-player menu state 3 (vtable `0x5E7968`); items `FUN_0051d4f0`, list read `FUN_0051fa20`, Back query `FUN_0043af40`, text lookup `FUN_004fbe50` (UTF-16) | main menu SINGLE PLAYER / CO-OP / [MODS] / OPTIONS / QUIT (MODS = state 0x37, modsmenu.cpp); state 3 START GAME / LOAD GAME / TRAINING via the main menu's own handler; Back from their screens returns to it; CO-OP = state 0x36 "CO-OPERATIVE GAME" (hooks for the co-op work in `menus.h`). Verified by background runs with `Dev\MenuKeys` |

Dev test mode (`devbg.cpp`, dev builds only): `tools/ds2/bg_run.py <out> <WxH> [s] [level]` runs the game off-screen, windowed,
never focused, muted, with launcher + Settings OK'd unseen (`Dev\Resolution`; `DS_FORCE=WxH` rewrites the picked mode -
4K / 21:9 without such a monitor), "press any key" waits skipped (`0x40FB50`, `0x4440ED`), message boxes logged and
answered, cursor clip / moves dropped, back-buffer screenshots every 2 s (`Dev\ShotDir`). Restores the user's Device
Settings (VirtualStore copy) and dev values afterwards. Muting is remembered by Windows per app -> `Dev\MutedByTest`,
the next normal start unmutes. `DS_DEV="A=1;B=2"` adds dev values (`HudProbe`, `TextProbe`, `ClampSkip`). Menu navigation in tests: `DS_DEV="MenuKeys=S,D,S,W,B;MenuKeyMs=2500"`
(U / D / S select / B back / W wait; menus.cpp feeds them to the game's list reads and Back query, nothing goes to a window).

## What carried over as-is

| Part | State |
| --- | --- |
| `dinput8.dll` proxy (`dllmain.cpp`, `dinput8.def`) | works: CDS II.exe imports DINPUT8.DLL; loaded, entry hook ran, game window opened (2026-10-10) |
| Entry-point hook | works on both builds: the retail exe enters SecuROM's loader (RVA 0x42CC17), the disc-free one a stub (RVA 0x3AC35D); `.text` is plain code in both, so patching before either is safe |
| `core/patch` (verified byte patches, call hooks) | builds; nothing patched yet |
| `core/settings` | key `HKLM\SOFTWARE\SCi Games\CDS II\Enhancements` (the game's own keys: `SOFTWARE\SCi Games\CDS II\` + `Device Settings`, `Window Placement`; value `Language`) |
| `core/log` | `DesertStorm2Fix.log` next to the exe |
| `core/gamepad` (XInput + Sony DirectInput, used by the launcher pad UI) | builds, game-independent |
| Prompt / controller icon assets + generators | copied (`scripts/gen_*`, `src/generated/`; the Kenney / controller sources = the repo root's `assets/`, shared by every game) |
| Lua 5.4.7 | the repo root's `third_party/lua` (shared; same 5.4.7 sources) for the script host; not built yet |

## DS1 -> DS2 address matching (first pass)

A masked byte-signature search (DS1 code around each patch site, rel32 / image addresses wildcarded) over DS2's
`.text` found almost nothing: DS2 is the same engine rebuilt (other code generation, struct offsets, new code), not a
patched copy. Only these matched:

| DS1 | DS2 | What |
| --- | --- | --- |
| `0x53C890` | `0x52B1E0` (unique) | image sheet scale setter (x, y, flag) - HUD scaling hook point |
| `0x4F7980` | 3 candidates `0x565100` / `0x565490` / `0x565910` | EVO object reader (check in Ghidra) |
| `0x55F540` | `0x4B5DE0` (the 3 first candidates were wrong; found by "CSoundManager::SetSoundCache") | level sound cache (.sch) loader |

Everything else needs anchors instead: strings (e.g. `levelInitialise`, error texts, file names, registry key
names), imports (`Direct3DCreate9`, `QueryPerformanceCounter`, `DirectInput8Create`, `CreateWindowExA`), and call
structure from those. The Ghidra project has DS2 at `/ds2/CDS II.exe` (analysed: 7530 functions).

## Feature-by-feature

Order = rough value / effort. "Engine-generic" = the DS1 code needs only addresses; "D3D9" = the DS1 code talks to
Direct3D 8 and needs rework for DS2's Direct3D 9 renderer.

| Feature (DS1 file) | Port notes |
| --- | --- |
| Widescreen launcher modes (`widescreen.cpp`, patch_defs) | find DS2's launcher resolution filter (4:3 epsilon compare); check whether DS2 already allows widescreen |
| Frame timing clock + frame cap (`timing.cpp`, `framecap.cpp`) | find the QPC-based ms clock (x87 precision bug likely still there) and the per-frame dispatcher in the message loop |
| High-FPS physics / landing fixes (`timing.cpp`) | same engine (Karma, character mover) - re-find the mover's landing threshold and the world step call |
| HUD / text scaling (`hud.cpp`, cave in patch_defs) | hook point found (`0x52B1E0`); sheet layout (+0x20/+0x24/+0x28) to confirm; layout clamps must be re-generated (`gen_clamps.py` logic) |
| Controller remap + pad I/O + rumble (`controller.cpp`, `padio.cpp`, `rumble.cpp`, `hotplug.cpp`) | padio's XInput / HID / WGI code is game-independent; needs DS2's joystick slot array, poll function, FF table |
| Display mode windowed / borderless (`display.cpp`) | D3D9 present parameters; window creation / style loop |
| MSAA / anisotropy / V-Sync (`graphics.cpp`) | D3D9 rework (vtable indices differ from D3D8; `Direct3DCreate9` import) |
| Overlay icons / text (`overlay.cpp`, `fontsharp.cpp`) | D3D9 rework |
| Discord presence (`discord.cpp`) | IPC client is generic; needs level name / text lookup addresses; DS2's Discord app id to look up |
| Skip intro / cutscenes (`cinematics.cpp`) | DS2 uses Bink (`binkw32.dll`) films - different from DS1 |
| Mods folder / resolver (`mods.cpp`) | basic resolver + in-game MODS menu (live Apply) done 2026-10-22 (see "Mods folder" below); `.skin` files done 2026-10-22 ("Skins" below); sound packs / level sounds / music / films done 2026-10-22 ("Sounds from the Mods folder" below); still to port: additive tables, `.weapon` / `.strings` |
| Launcher settings UI + pad navigation (`launcher.cpp`, `launcher_pad.cpp`) | map DS2's launcher dialog ids |
| Split screen / co-op (`splitscreen.cpp`, `coop.cpp`) | done 2026-10-10: 2-4 players + join screen on the menu feature's CO-OP screen - see "Split screen" below |
| Script host, editor support (`scripthost.cpp`, `campaigns.cpp`, ...) | done 2026-10-23 - see "Lua missions" below |

## Verify each port

- Patch sites verified byte-for-byte by `patch::ApplyGroup` (a different exe skips the feature).
- Launch `<install>\CDS II.exe`, read `DesertStorm2Fix.log`, kill the process afterwards.

## Split screen (2026-10-10, `splitscreen.cpp`, `splitscreen_layout.h`, `coop.cpp`)

DS2's PC build kept the same console remnants as DS1 (DS1 addresses in brackets). Verified by background runs: 2 / 3 / 4
views in Mission 1 at 1280x1024 and 1920x1080, each with its own camera, soldier, HUD (panels, weapon, compass) and grey
dividers; and the join flow main menu -> CO-OP -> 2 / 3 fake players -> START GAME path (profile, difficulty) ->
briefing film -> Mission 1 in 2 / 3 views (that run needed the menu feature's `menus.cpp`, copied in temporarily).

| What | DS2 | Notes |
| --- | --- | --- |
| split object | `0x626C18` (ctor `FUN_0040a2d0`, called at `0x5D75D5`) [`0x606410`] | +0 local MP flag, +4 player count, +8 mode (ctor 2; HUD code tests 3 / 4), +0xC orientation (`FUN_0040a290`), +0x10 HUD layout table, +0x14 (`[0x626C2C]`, another flag). Start-up option `-P` keeps the flag (`0x40B205`), otherwise cleared |
| layout tables (Xbox format, 0x80 B: offsets[4][2], views {used,x,y,w,h}[4] at +0x20, variant[4] at +0x70) | `0x6086D0` 1p, `0x608750` 2p top/bottom, `0x6087D0` 2p side by side (ctor default) [`0x5E6A48..`] | the table selectors are stripped; ours go into +0x10 with variant 0 for every view (the console variants 1-3 put the soldier panels at the view's top left, stacking upward out of it) |
| HUD anchors | `FUN_00459d10(out, view, anchor)` -> `FUN_00459c30` = view origin + view size x anchor + screen size x offset; anchor table `0x613FE0` float[variant][9][2] [`FUN_004a05d0`, `0x619A30`] | 11 callers; anchor 0 soldier panels (bottom left, stacking up), 1 weapon / ammo; compass top right |
| level dispatcher / level load | `FUN_0042cef0` (called through a vtable; mission -> `call FUN_0042cad0` at `0x42CFC1`; front-end `FUN_0042c930` at `0x42CF9B`) [`FUN_0040de70`] | `FUN_0042cad0`: input manager `0x62FE40` reset (`FUN_00428d40`), one block (`FUN_00428a90`), one more per player with the flag, then level init `FUN_00412690` |
| input blocks | block i = `0x62FF7C + i*0x58C` (manager +0x13C; count u16 `[0x6315B0]`) [`0x60F5B8 + i*0x478`] | +4 player object, +0xC devices (bit 0 keyboard/mouse, bit 1 pad; block init `FUN_00423710`; `0x42846C` clears the pad bit while no joystick exists), +0x2EC soldier, +0x39C binding set, +0x3A0, +0x3A4 joystick (-1 = block off), +0x448 view index, +0x41C / +0x42C last-input flags |
| players | `0x62F36C[4]` [`0x606A68`] | |
| player set-up | `FUN_0040c8b0` (level init `0x413247`; also `0x4144EC`, `0x449377`) [`FUN_0040bae0`] | per renderer viewport: camera `[[0x680120]+0x10+i*4]`, player i <- block i's soldier; `FUN_00422ed0` = thiscall(block, soldier) set the block's soldier; player 1's pick `FUN_00423620` at `0x40C94A` (squad member with owner == block +0x39C, lowest squad slot unit +0x1CF) - the loop for players 2..N is stripped |
| squad slot -> player | `FUN_00475610` (soldier +0xEC4 owner by player count, jump table `0x4756C4`), owner getter `FUN_00475970` [`FUN_00436f70`, +0x2B00] | 2p: slots 0+2 -> P1, 1+3 -> P2 (as DS1); 3p: P1 = Foley + Jones |
| renderer viewports | renderer `[0x680038]` +0x30 count, +0x34 D3DVIEWPORT9[] (0x18 each), full screen `+0x41A00`, current `+0x41A18`, back buffer +0x94 / +0x98, device +0x4187C; reset `FUN_00537380` (only at device creation, `0x541F67`), GetViewport `FUN_00536bb0`, SetViewport `FUN_00537440` [`+0x20/+0x24`, `FUN_00542e50`, `FUN_00547c30`, `FUN_00542f40`] | the builder (DS1 `FUN_00547b30`) is gone - the plugin writes the list. frontend.cpp's virtual screen writes entry 0's size (+0x3C/+0x40) at mission init, after the split set-up -> rewritten in the player set-up |
| render loop | `FUN_0040fba0` (from `0x410990`) [`FUN_0040ef70`] | per viewport: current view `[0x680134]`, `FUN_0040dd00(i)` world, view controllers `0x6800D0[4]`; full screen when `[[0x68008C]+0x1B8C]` bit 0, state `[0x63433C]` 0x15/0x19/0x2A/0x30, or split flag `[0x62F3A8]` = 0 (set by level init). HUD pass `FUN_0040dbc0` (per view: SetViewport, `FUN_0043aeb0(i)`); full viewport again at `0x40FF4A` |
| soldier panels | draw loop `FUN_00466920` over all blocks (anchor 0 of view i) -> panel list; per soldier `FUN_00467910` (callback `0x468570`, viewer block = this+0x128; skips soldiers whose owner != viewer +0x39C); set-up `0x468590` | vehicle passengers have no panel (4 players in the Mission 1 jeep: only the driver's) |
| bindings | table `[0x627078]` {sets, slots 3, actions 0x51, data}, allocated by `FUN_0040a3e0` called at `0x40A556` (`push esi` = 1 set) [`0x60687C`, `0x409F60`]; lookup `FUN_0040a470(set, slot, action)`; button eval `FUN_00422fa0` (checks +0xC) | 4 sets (call hooked), sets 1-3 = copies of set 0 at each split level load |
| analog | `FUN_004230f0` (thiscall block, action, opposite; 0x51 = none; axis floats `0x67E948 + (code + joystick*0x70)*4`, key held `FUN_004cc030`, keyboard-used `[0x62FE1C]`) [`FUN_00450920`] | no device check -> replaced by `AxisValue` (entry jump) |
| mouse look | smoother `FUN_004cacf0` on `0x680018`, read at `0x425BD8` / `0x428817` with esi = block [`FUN_00538080`, `0x44B396` / `0x44F8D7`] | stub: no movement for blocks without bit 0 |
| downed slow motion | tail jump `0x427816` -> "Slow down time" `FUN_0041e4b0` [`0x44EFAB` -> `FUN_00443d00`] | none in split screen |
| joysticks | DirectInput devices `0x67E7DC[8]`, count `[0x67F6C0]`, enumeration `FUN_004cc690` / callback `FUN_004cc370` (c_dfDIJoystick2 at `0x6009AC`), states from `0x67E828` (stride 0x1C0) | read raw by the join screen |
| front-end | state setter `FUN_0043dfd0`, state `[0x63433C]` (0x13 = mission running), screen per state `FUN_0043bba0`; main menu (state 2) update `FUN_0045ca50`, START GAME case `0x45CAA9` (item 0x3FD) | co-op Begin runs that case |
| per frame | message loop `FUN_00414ac0`: `0x414BFC` `FUN_00412040`, `0x414C01` `FUN_0040b2d0`, `0x414C06` `FUN_004cafe0` | coop.cpp hooks `0x414C06` (a frame cap port may want the same loop - merge carefully) |
| campaign flow | flow object `0x631608` (+0x10 entry index into `0x60C450`, 32-byte entries: Logo / AMODE / FIN / B01 briefing / mission01.dll / B02 ...; select by name `FUN_00430ec0`, current name `FUN_00430900`); `FUN_0042ccd0` plays the entry's Bink film | not needed (the START GAME path does it) |

Plugin (`splitscreen.cpp`): the level-load call `0x42CFC1` is wrapped - with 2-4 players wanted (co-op session or dev
`Dev\SplitPlayers`) it sets the split object (flag, count, orientation from `SplitScreenLayout`, our layout table),
writes the viewports and copies the binding sets before the game adds the input blocks; `0x40C94A` (`AssignPlayers`)
re-adds the players 2..N loop (block +4 = player, +0x448 = view, pad only; co-op session: each player's device and
joystick) and rewrites the viewports; the front-end init (`FUN_0042c930` entry, trampoline) and a 1-player load put
back one view. HUD art scale k comes from the smallest view (hud.cpp `features::SplitViewSize`). Dividers: Clear rects
after `0x40FF4A`.

Co-op (`coop.cpp`): fills the menu feature's CO-OPERATIVE GAME screen (`menus::SetCoopScreenHooks`; compiled only when
`features/menus.h` exists): rows PLAYER 1-4 (device), joining per device (keyboard Space / Backspace / Enter, pads
Cross-A / Circle-B / Options-Start), selecting a row or Start begins with 2+ players -> session + START GAME path. The
session ends on the main menu. Dev: `CoopFakeJoin` = n, `CoopAutoBegin` = ms, `CoopAutoOpen` = 1 (with the menu
feature's `MenuKeys` the whole flow runs in a background test).

Open: real pads (joining, P2-P4 movement / aiming) and keyboard joining need a human; in 3-player mode the half view
uses the quarter views' HUD scale; pause / objectives screens and cutscenes in split screen not checked in play;
vehicles with several players (only the driver has a panel; the Mission 1 jeep only drives on while an AI drives);
save / load of a co-op game (no co-op save naming yet).

## PSE: DesertStorm2Fix as the host plugin ds2fix (2026-10-22)

The DS2 release is now the PSE install tree (shim `dinput8.dll`, `PSE\pse_loader.dll`, `PSE\Games\pse_ds2.dll`,
`PSE\Plugins\ds2fix.dll` + `.pse`, `PSE\Plugins\<plugin>` from `plugins/<name>/`); build / package / deploy and the
rules for DS2 plugins: docs/sdk/reference/game-ds2.md (repo root) "DesertStorm2Fix as a host plugin" + "DS2 plugins".
What differs from the standalone `dinput8.dll` (same features, `#ifdef DS2_PSE_HOST` or own files in `src/pse/`):

| Part | Standalone `dinput8.dll` | ds2fix |
| --- | --- | --- |
| Start | own entry-point hook (`dllmain.cpp`) | the PSE shim + loader; `PSEPlugin_Load` (`src/pse/ds2fix.cpp`) |
| Patches | `src/core/patch.cpp` (raw writes) | DS1's broker-backed `src/core/patch.cpp`, every record owned by `ds2fix.<feature>` |
| D3D9 hooks | vtable slots written by hand (`HookVtable`) | broker pointer hooks per COM class (`core/comhooks.h`), chained with the layer's |
| Settings | `HKLM\SOFTWARE\SCi Games\CDS II\Enhancements` (VirtualStore) | `HKCU\Software\PSE\ds2\Plugins\ds2fix` (`src/pse/settings_pse.cpp`), copied once from the old key |
| Log | `DesertStorm2Fix.log` (dev) | same in dev (+ loader / plugin lines, broker records); release: `PSE\Logs\loader.log` |
| Shutdown | nothing | `PSE_MSG_EXIT` + plugin unload from the layer at WM_DESTROY |
| Interfaces | - | `ds2fix.graphics` v1 for DS2 plugins (`src/pse/ds2fix_graphics.h`) |

## Mods folder (2026-10-22, `mods.cpp`; modder page: repo root `docs/sdk/ds2-mods.md`)

`<game>\Mods\<mod>\**` files replace / add any file the game opens by name (index by upper-case file name, name order,
later wins, `_` / `.` folders off; report `Mods\DesertStorm2Fix-report.txt`; setting `FeatMods`, default on). DS1
addresses in brackets.

| What | DS2 | Notes |
| --- | --- | --- |
| resolver | `FUN_004c3c40`(name, paths, flags) cdecl -> buffer `0x67CFA8` (0x200 B) [`FUN_004be810`, `0x63BF28`] | flags `[0x665E4C]`: bit 0 archive mode (asks `FUN_004c2810`, returns the bare name), 0x40 pass-through, bit 1 archive build (start-up option); else game dir, `Data\`, search paths. Entry hook (`A0 4C 5E 66 00`) |
| open / readers | `FUN_004c4000` (resolve + `FUN_004c3140(path, "rb")`), whole-file reader `FUN_004c49e0`(name, &buf, &size, heap, pool) [`FUN_004bfb90`], `FUN_004c49c0` open, `FUN_004c49a0` exists check | errors: "Couldn't open %s", "Empty file [%s]", "Failed to allocate memory for file %s" |
| opener | `FUN_004c2ea0` thiscall(file, path) ret 4 [`FUN_004bd8a0`] | flags -> file +0x140; archive branch: lower-case hash `FUN_004c0b30`, lookup `FUN_004c2320` on `[0x665E54]`; disk branch fopen +0x134. One file object `[0x665E48]`. Entry hook: Mods paths with bits 0 + 1 cleared |
| in archive? | `FUN_004c2810`(name) cdecl -> 1 / 0 | callers: resolver, character model preload `0x457411`. Entry hook: 1 for a mod's file |
| textures | extension tries `FUN_005427b0` (.ddl / .dds / .png / .bmp / .tga via `FUN_004c49a0`); per-texture load `FUN_00542f30` thiscall(texture) (+8 name, +0x30 / +0x34 memory buffer) [`FUN_00549a10`] | entry hook drops the buffer when a mod has the name |
| Global Heap | `[0x664380]`, ctor `FUN_004c0650` thiscall(size, name) at `0x4136FC` (1 024 000), +0x48 size, +0x4C free, +0x50 used, +0x58 buffer | call hooked: + largest mod file (1 MB steps, cap 64 MB); passes through when the plugin ds2fix.memory is installed (it sizes the heap, see "Memory") |

**MODS menu** (`modsmenu.cpp`, 2026-10-22; DS1's `modsmenu.cpp` minus the table re-parsing - DS2 has no table merging):
main menu SINGLE PLAYER / CO-OP / MODS / OPTIONS / QUIT (menus.cpp item 0x405, shown while `SetModsScreenHooks` is set =
FeatMods on) -> state 0x37 (`menus::kModsState`, single-player-menu class like the co-op screen): up to 7 mod rows
`NAME: ON / OFF` + the Apply row (`APPLY n CHANGES` / `RESTART THE GAME TO APPLY` / `CHANGES APPLIED` / `NO CHANGES TO
APPLY`); empty Mods = `NO MODS FOUND`. Toggle = folder rename (`_` prefix). Scrolling: Down on the last mod row / Up on
the first moves the window (title `MODS (a-b OF n)`) - only for keyboard / pad navigation (`menus.cpp` NavInput reads
the list callbacks' inputs: block +0x14C / +0x88 up, +0x150 / +0x8C down, bit 4), the mouse never scrolls. Apply =
`features::ReloadMods`: new index + report; a changed file name the resolver was already asked for **outside a
mission** (world scene name `[0x68008C]` +0x1BBC != "FrontEnd": set by mission init at `0x41279C` before its files are
read; `[0x63433C]` is 0x13 at start-up, useless) = restart (start-up tables WEAPS.TXT ... CONNOBJS.TXT, fonts, front-end
art, FrontEnd.sch are all read before the main menu), mission-only names = next level. The resolver hooks are installed
with no mods too, and the heap is sized from every folder (on or off), so switching a mod on live needs no restart
unless it was added while the game ran with a bigger file. Main menu line under the menu panel: main menu vtable +0x14
(0x462000, shared draw `FUN_0042e6c0`) replaced in the main menu's vtable; text = `FUN_00529490`(font [0x62F43C] small
/ [0x62F440] large, text, x, y, 1) after `FUN_00526e40` (alpha) / `FUN_00526e20` (colour); coordinates in the 1280x1024
virtual screen (menu panel x 372..910, y 200..713; y = the line's middle), drawn only while the 2D screen is 1280x1024.
Item descriptions are typed into the box once per highlight (the text is copied) - keep them fixed and <= 2 lines.
Dev `Dev\ModsOpenLog` = 1 logs each name the resolver is asked for (" (mission)" when in a mission).
Verified 2026-10-22 by background runs (scratchpad runner ds2modsmenu `run.py`: DS2 lock, backs up dinput8 + PSE\ +
HKCU\Software\PSE\ds2, test mods, restores): PSE tree at 1920x1080 / 2560x1440 and the standalone dinput8 - toggles
rename the folders, Apply rebuilds the index / report, 10 mods scroll down / up, empty state, restart case (a mod with
`WEAPS.TXT` switched on -> RESTART THE GAME TO APPLY + report line), a 65 MB file -> problems line. Not tried: mouse,
real keyboard / pad presses, a mod switched on and then a mission played.

Verified by background runs (both builds, Mission01): mission textures replaced (2048x1024 Land Rover texture through the
grown heap), a mod `.evo` naming a mod-only texture, `_` folder = vanilla. Not covered: Bink films, level DLLs, the
game's own fopens.

Verified 2026-10-22 by background runs (scratchpad runner `ds2run.py`: same dev values, standalone `dinput8.dll` vs
the ds2fix tree, both from one source state): front end at 1920x1080 and 3440x1440 (`DS_FORCE`), Mission 1 at both,
2-player split screen at both, the co-op flow (`MenuKeys` + `CoopAutoOpen` / `CoopFakeJoin` / `CoopAutoBegin`: join
screen, 2 fake players, session, START GAME path) - same shot counts, same feature log lines (only heap addresses and
the module name differ), front-end shots within 0.1-1.0 mean pixel difference, mission / split shots the same layout
(HUD, panels, compass, dividers; the rest is AI / timing); 79 broker records, all owned by `ds2fix.<feature>` or
`pse.game.ds2`, no "outside the broker" line, no exception. Settings migration: a VirtualStore Enhancements key with
2 values → both copied (log line), old key unchanged. The release package (`ds2-pse-dist`) boots to the main menu
(TEST ONLY layer harness: FRONT_END_READY, EXIT, exit code 0; release ds2fix logs into `PSE\Logs\loader.log`).
Not yet: a human play-test of the package (fullscreen, pads, real menus), saves through the real LOAD GAME menu.

## Mods: tables, texts, .weapon / .vehicle / .unit (2026-10-22, `mods_tables.cpp`, part of `ds2fix.mods`)

DS1's additive tables, `.replace.txt`, `.strings`, `.weapon`, `.vehicle`, `.unit` and the unit / vehicle table checks
(repo root `src/features/mods.cpp`), ported. Modder page: repo root `docs/sdk/ds2-mods.md`. DS1 addresses in brackets.

| What | DS2 | Notes |
| --- | --- | --- |
| start-up table loader | `FUN_0040b360` [`FUN_00411740`] | each table read whole (`FUN_004c49e0` → the resolver) into the Global Heap, parser called, a parser returning 0 ends the start-up (`je 0x40bcd6`) |
| parsers (in load order) | Weaps `0x452C20` (array `[0x635F68]`), Inven `0x4553B0`, Props `0x4561D0`, HStat `0x44FA80`, VStat `0x4543E0`, fNames `0x44FC60`, sNames `0x44FD20`, CSkins ×8 `0x4527F0`, ConnObjs `0x4529D0`, Units `0x454810` (loadHumanStats; `[0x635F7C]` × 0x1D4, count `[0x635518]`), Vehicle `0x454D60`, featStat `0x450D10`, Structs `0x4511B0`, Feats `0x450E70`, Doors `0x456400`, Targets `0x456770`, Switches `0x4FF0D0`, UnitWpns `0x455850(text, size, 1)`, HSkills `0x455DE0`, VehWpns `0x455850(…, 0)`, AlternativeVehicles `0x455AE0`, ExMods `0x456020`; rule tables `0x414145..0x4141BF` | all in `catalog.dat` (lower-case archive hash) |
| row count | `FUN_004c0d60` = number of `'\n'` | a blank line in a row table = an empty row = "Invalid base stat" → start-up ends; merged row tables drop blank / `#` / `//` lines (the *StateRules tables keep theirs: own tokenizer) |
| Units row (15 cols, sscanf into 36-byte buffers) | 0 name, 1 type (`FUN_0044f9c0`: SOLDIER CIVILIAN GOAT HERDER EMIR MISSILE EXPERT POW), 2 body SAS → unit +8 (35 B), 3 body Delta → +0x80, 4 → +0x2B, 5 HStat (`FUN_0044fde0`, unknown = quit), 9 int → +0xB8, 10 experience (`FUN_00450b80`, 15 names, unknown = quit), 11 speech bank → +0x1A4, 12 squad role, 13 skin key, 14 item slots → +0xE2 (items +0xE4[], amounts +0x148[]) | DS1's 14 columns + the Delta body at 3 |
| UnitWpns / VehWpns (`loadUnitItems`) | unit = a Units row / vehicle = a Vehicle row, type `FUN_00450540` (0x1C = unknown), item = Weaps (`FUN_0044ff50`, type 5) or Inven (`FUN_004502e0`), vehicle items must be weapons, rows > slots ("Trying to allocate too many Items") – each = quit | slots: Units col 14 / Vehicle.txt token 50 (space-separated, 52 tokens; one game row has 46) |
| Weaps (79 columns) | DS1's 75 + tracer red / green / blue / alpha at 44–47 ("Invalid … tracer component (should be between 0 and 1)") | singletons "More than one grenade stat / fist / grenadeLauncher/rifle / grenade launcher has been loaded!" as DS1 |
| text lookup | `FUN_004fbe50` thiscall(catalog, hash) → UTF-16 (menus.cpp `TextLookupStub` → `OwnText` → `features::ModText`) | string bundles (catalog.dat `GenStrs/InvStrs/TitleStrs/PCStrs.loc`, mission01.dat + `M01Strs.loc`): header, u32 count, u32 8 languages, offsets, then count × {u32 offset, u32 hash} sorted by hash, then UTF-16 texts – **no key names**; hash = archive LFSR over the lower-cased key (weapon names = their Weaps id; 54 of 85 found) |
| EVO texture copy | EOBJ +8 model name, TEXL u32 n × {u32 hash, char[64]} (same as DS1 in version 0x35) | `CopyModel` = DS1's (hash swapped everywhere in the file) |

Mod files → `%TEMP%\DesertStorm2Fix\merged\<FNV of the content>\<NAME>` (a changed content = a new folder, so the MODS
menu's Apply sees the change and says "restart" for the start-up tables), served through mods.cpp's index
(`features::SetModFile`, which replaces a mod's own `Weaps.txt` part by the merged table; `AddModRoot` = the opener's disk
branch for that folder). Wiring: mods.cpp calls `BuildModTables()` after every index build (start-up, MODS menu Apply).
Weapon sounds go through mods_sound.cpp (`MakeWeaponSoundBank` / `BorrowSoundBank`).

Found while testing (background runs, Mission 1; scratchpad runner `ds2modtables/run.py` + `probe.py` reading the unit /
weapon tables from memory):
- weapon definitions `[0x635F68]` × 0x3BC (count `[0x635508]`), +4 id char*, **+0x92 u16 magazine size** (as DS1);
  unit +0xB0 → HStat record (health at +0x24), +0xE2 slots, +0xE4 item definitions, +0x148 amounts.
- the grunt ctor `FUN_004835d0` gives the unit kit (`0x483BD4..`: item `FUN_004830e0`(def) builds a scene record on the
  stack and creates the object through the object manager `0x4CDEA0`), then the scene record's kit (+0x20E6C, 25
  entries); soldier inventory +0x6D4 (25 slots).
- **a unit kit that grows broke shipped levels - fixed 2026-10-23 (octree rebuild)**: Bradley + a pistol / an AK (7 rows)
  and Iraqi_Conscript + a knife each made Mission 1's opening never hand over (conscripts: blank screen); a MediKit (stacks
  with his own: FUN_004830e0 returns the existing category-8 item, no object) or a swap (same row count) were fine. Cause:
  every kit item is a scene object made inside the grunt ctor (FUN_004830e0 -> 0x4CDEA0, its own EnvInstance) while the
  scene loads, so every later object / instance comes one place later than the shipped `<scene>.OCT` (built from the
  vanilla scene) expects - the octree then culls / collides with the wrong objects. Evidence (Frida, scratchpad
  ds2kitfix/rover.js, object by script id = thiscall 0x4CF4F0([0x6800E0], id), position instance +0x108): the Land Rover
  (script id 0xC3, carries the squad) sat 6 cm lower and never moved off waypoint 109 (Bradley + AK), fell to y -1236
  (conscripts + knife); vanilla drives 109 -> 114 in ~8 s. Mission01.dll's waypoint handler (FUN_10001d50, message
  "arrived": unit 0xC3 at waypoint 114 -> 0x10002265 = state 2 + jmp 0x100017F0 = end of the intro: letterbox off, player
  camera) waits for exactly that. Ruled out on the way: random draws (the seed 0x623FE0 and the random-table indices
  0x65F748 / 0x65F74C don't change in FUN_004830e0), the .FAS object pools (FUN_00509660: EnvInstances 1356 / 1356 used in
  vanilla - one more overflows to a heap instance, [pool+8] = 1, harmless: with the octree rebuilt the overflowed run
  plays; a bigger-pool experiment alone didn't help), held weapon (M16 both). Fix: `features::ModKitsGrown()`
  (mods_tables.cpp CheckTables: some unit has more UnitWpns rows than the game's) makes mods_levels.cpp's octree hooks
  (start-up flag 'c' at 0x412E64) rebuild the level's octree from the loaded scene - 47 ms for Mission 1 - also for a
  shipped scene. Verified: Bradley + AK / Desert Eagle, conscripts + knife -> opening hands over, item in his
  inventory; vanilla unchanged (no rebuild without such a mod).
- **scene-placed vehicles take the model named in their HLOU record**, not Vehicle.txt col 1: an in-place `.vehicle`
  with a new model name loaded the copy but the Land Rover kept its look → in place the model file itself is replaced by
  the retextured copy (same name; verified: checker Land Rover in Mission 1, mission runs).

## Weapon pictures (`.weapon` `hud icon = <file>`, 2026-10-23; `weaponicons.cpp`, sub-plugin `ds2fix.weaponicons`)

DS1's `weaponicons.cpp` (repo root `games/ds1/src/features/`), rebuilt for DS2's sprite sheets. DS1 addresses in brackets.

| What | DS2 | Notes |
| --- | --- | --- |
| picture lookup | `FUN_00452170` [`FUN_004966c0`] | name in **eax**, number in ax, plain `ret`; strcmp chain `mov edi, <string>` ... `mov ax, <n>`; unknown = `FUN_004cd600` "Warning - unable to find interface ID %s" + 0xFFFF. Callers: Weaps parser `0x453170` (stack record +0x84 → weapon def +0x84 via `FUN_00451420`), Units `0x454AED`, Inven `0x455546`. Entry `56 57 BF 64 55 5E 00` (7-byte prologue) → broker HookFunction, naked stub keeps eax |
| interface sheet | `[0x62F434]` ds_int.img [GWInt `[0x60EE18]`] | CSpriteRender 0x50 B (ctor `FUN_0052b290`, read `FUN_0052ad20` at `0x41448F` - start-up, once; clean-up `FUN_0052ac30` at exit `0x40D56C`). +0 header 0x11C B (+4 version, +6 images = 178, +0xA pages = 6, +0xC page names 16 B each), +4 records 0x1C B (+0 page u16, +4 owned pointer freed at exit, +8 ?, +0xA x, +0xC y, +0xE w, +0x10 h, +0x12 / +0x14 offset s16, +0x18 name hash), +8 UVs 16 B (u0 v0 u1 v1, computed at read time), +0xC page objects (`FUN_005432b0` texture objects: +0xC palette index, -1 none → `FUN_00536af0` = SetCurrentTexturePalette; +0x10 IDirect3DTexture9*), +0x20 scale flag, +0x24 / +0x28 scale. File: 284-byte header + 178 × 272-byte entries {u16 page, char[256] name, u16 × 7} (IMAGE_WEAPON_M16M203 = 166, its record 101,203 93×33 on page 2) |
| image draws | `FUN_0052b7b0` (image = first stack argument) + 13 variants `0x52BE70..` | every one binds `pages[record.page] + 0x10` through the SetTexture wrapper `0x5364E0` (sites `0x52BA5E 0x52BE23 0x52C191 0x52C3A6 0x52C3D9 0x52C70F 0x52CA51 0x52CA86 0x52CE9A 0x52D0B2 0x52D376 0x52D69A 0x52D9A7 0x52DA4C 0x52E284`) |
| game allocator | `FUN_004be320`(size, tag) cdecl / free `FUN_004be3b0` | what the sheet reader uses - our bigger arrays come from it so the exit clean-up frees them |

DS1 swapped the texture at the bind (shared hook point with the sharp text). DS2 needs no bind hook: our images (1000 +
n) get page objects of their own (pages 6 + n: palette -1, texture = the picture), so all 15 draw variants bind the
picture by themselves and `ds2fix.widescreen`'s font hook at `0x52BA5E` is untouched (works with and without that
plugin). Header counts stay the game's (the clean-up walks only those). The extension runs on PSE_EVENT_FRAME once the
sheet and the device exist (start-up reads the sheet before the first menu frame); pictures are WIC-decoded into managed
A8R8G8B8 textures (survive Reset). Standalone dinput8.dll: not available (`AddWeaponIcon` = -1 → base picture + report
line).

## Skins (character textures + `.skin` files, 2026-10-22; `skins.cpp`, sub-plugin `ds2fix.skins`)

How CDS II.exe textures a soldier (DS1 addresses in brackets; DS1's `bodies.cpp` notes apply unless said otherwise):

| What | DS2 | Notes |
| --- | --- | --- |
| unit table (UNITS.TXT, `loadHumanStats` `FUN_00454810`) | array `[0x635F7C]`, count `[0x635518]`, 0x1D4 bytes | +4 char* name (col 0), +8 body model for the SAS uniform (col 2, e.g. `Rifleman_SAS_Standard`), +0x2B body for Delta (col 3), +0xB0 HSTAT row, +0x1AC speech bank, +0x1CF squad role (-1 = not squad), +0x1D0 skin key hash (col 13, `BradleySkin`) |
| skin tables (`loadCharSkinStats` `FUN_004527f0`) [`FUN_00496d20`] | `[0x635524 + (uniform*4 + role)*4]`, counts `[0x6354C8 + ...]`; 2 uniforms (0 SAS / 1 Delta) x 4 roles; **only the first 2 rows of each table are read** (count forced to 2) | entry 0x5C B as DS1: +0 key hash, +4 texture name (char[0x23]), +0x27 portrait, +0x4C portrait texture, +0x50 / +0x54 first / last name. Lookup `FUN_00451fc0(key, uniform, role)` [`FUN_00496500`]; uniform `[0x627154]` |
| soldier (CHLOGrunt, ctor `FUN_004835d0` thiscall(scene record, unit)) | +0x24 unit, **+0x30 render instance** [+0x2C], +0xE24 skin entry [+0x2A68], +0xE20 portrait index | every grunt's ctor ends with the skin apply - squad members with their entry, everyone else with null |
| skin apply `FUN_00475020` thiscall(soldier, entry) ret 4 [`FUN_00435be0`] | callers `0x484361` (ctor), `0x42F6D3` (profile / uniform change) | registers the portrait (+0x4C); the entry's texture goes into the instance override (`FUN_0055dd60`) **only when the instance already has one** (check `0x475087`: +0x20C non-null) - then the old override is released (`FUN_00543ca0`) and `FUN_0055dd70` nulls the default texture of every skinned object of the shared model. Soldiers start without one (scene loader sets it only for root flag 0x400 models, `0x4CF279` / `0x50AFF5`; soldier bodies have 0x4030), so the shipped squad draws its body's own TEXL texture (the hero bodies name their skin, e.g. `Rifleman_DF_Standard`) |
| render instance (CAnimInstance, vtable `0x5DAE78`, init `FUN_0055e020`) | +0xC8 current model (first object), +0x1DC full-detail model, +0x1E0 / +0x1E4 LOD models (`534_Poly_Soldier`, `LowPoly_Soldier`; not for the player's team), +0x1C0 / +0x1C4 from model +0xEC (skeleton), **+0x20C skin texture override** [+0x2B0], +0x240 hidden-object bits (object +0x1A8 = bit index), +0x18 flags (0x100000 LOD on) | LOD switch `0x55FB40..`: vtable +0x1C = **SetModel `0x55F6D0`** thiscall(model) (same key count: the base setter `0x4D1D70`; else `0x55FA07..` re-sizes the pose arrays and keeps the animation layers), then `0x55FBCE` re-puts the override |
| model objects | first = instance +0xC8; next **+0xE0** [+0xE8]; +0xC kind (2 = skinned), +0xB4 skin data, **+0xDC name hash** (the EOBJ header's, reader `FUN_005087b0` - the node name itself isn't kept), +0x1A8 key index, +0xC4 flags (0x400 = character), +0xEC skeleton (+0 key count; every shipped soldier body: 40) | skin data +4 -> part record, +4 = the part's own texture (from the model's TEXL) |
| **skin draw** `FUN_0055f1f0` thiscall(instance, a, b, texture) ret 0xC (one caller `0x560AB0`) | per object of instance +0xC8 (not hidden, kind 2): `FUN_00556290(skin data, texture)` at call **`0x55F334`** with texture = the draw's argument, else **instance +0x20C**, else (null) the object's own `skin data +4 -> +4` | **the override replaces the texture of EVERY skin object of the character**; with no override each skin object draws its own TEXL texture |
| texture manager | `[0x680064]` (+0xC list, +0x10 "something pending"; texture object +8 name, +0x10 the D3D texture once loaded, +0x24 refs, +0x2C flags): add / find `FUN_005432b0` thiscall(name, flags) (stays pending: +0x10 = 1), load pending `FUN_00543430` thiscall(progress callback) ret 4 (level init `0x4131CC`), release `FUN_00543ca0` | [`FUN_0054a0b0`, `FUN_0054a3e0`] |
| model load | `FUN_004e5d20` thiscall(world `[0x68008C]`, type 4, name, 1, 0) between `FUN_004c12c0` (save FPU control) + `FUN_004c12a0` (level precision) and `FUN_004c12d0(saved)` | as the grunt ctor loads the LOD models (`0x483935..0x48399D`) [`FUN_004dd5c0`] |
| scene texture field | scene object loader `0x50AFC4`: an ENV record's texture (not `NOTEXTURE`) on a character model (flag 0x400) -> instance override | script API `0x4498D0` (id, texture) does the same |

Consequences for multi-part characters (several skin objects, one texture each): normally no instance has an override
(+0x20C = 0; corrected 2026-10-22 - an earlier note here said the squad always has one; the ModKit exporter's Mission01
test and the `0x475087` check show otherwise), so each part draws its own TEXL texture and a multi-part body replacing
a game model works as it is. An override, where one is set (scene texture field / script API `0x4498D0` on flag-0x400
models), covers every part; the LOD switch `0x55FBCE` pins the first skin object's texture as the override, and the
blood / colourize passes `0x55D890` / `0x55D8C0` use only the first kind-2 object. The textures are per model, i.e.
shared by every soldier with that body: DesertStorm2Fix's `.skin` therefore picks the texture per soldier and per part
at the draw call `0x55F334`, leaving the game's override and the shared model alone.

`.skin` (modder page: repo root `docs/sdk/ds2-mods.md`): `skins.cpp` reads every `*.skin` in the mods index at start-up
(and after the MODS menu's Apply), then wraps the skin apply (entry detour `0x475020`, 5-byte prologue): a soldier a
`.skin` names (unit +4 name, uniform `[0x627154]`) gets - for names / portrait - a private copy of his skin entry
(0x5C B, name records 0x28 B), then after the game's apply: body = `FUN_004e5d20` load + SetModel via the instance's
vtable (+0x1DC = it too, so the LOD switch comes back to it), a record {model, whole texture, object -> texture}; the
draw call `0x55F334` (`call 0x556290`) goes to a naked stub that takes esi (instance) / edi (object) and passes the
record's texture when the draw has no texture of its own. A body's own part textures are taken when the model is first
seen. Textures registered after the level's batch (a model's TEXL textures don't always set the manager's pending flag)
are loaded on their first draw (+0x10 pending, `FUN_00543430(mgr, 0)`). Mission 1 runs with the Delta uniform
(`[0x627154]` = 1); its squad = Bradley (front passenger seat of the Land Rover) + Foley.
Texture flags: a texture registered while the manager's +4 is set gets +0x2C bit 0x10, and the batch's first pass sets
it for a texture the level archive lacks (the second pass then tries the global archives); a body's TEXL texture drawn
mid-level came out with 0x10 and unloaded (Iraqi_Soldier_01), so the first-draw load clears the bit first.
The soldier panel's name plate doesn't come from the skin entry (its +0x50 / +0x54 name records feed the name getters
`0x4756F0` / `0x475710`: messages, stats, `0x461A25`, `0x4267C2`, `0x4363E2`, `0x46A1BB`); the portrait (+0x27 name,
+0x4C texture) does show on the panel.

Verified 2026-10-22 by background runs (scratchpad runner `ds2skin/run.py`: DS2 lock, dinput8 / patcher / PSE\ /
`HKCU\Software\PSE\ds2` backed up and restored, test mod `Mods\ZZSkinTest` removed, Mods folder removed again;
assets from the ModKit: recoloured `Rifleman_DF_Standard`, `IRAQI_SOLDIER_01.EVO` renamed `TESTBODY` by
`evo_write.rebuild(model_name=)`, a 3-part `SNIPER_DF_STANDARD` split by the exporter's writer and renamed `MULTIBODY`):
Bradley red (Delta skin applied, the SAS one not), Foley on MULTIBODY (42 keys, instance built on it, parts drawn
MS_GREEN / TEST_RED_DF by object name / MS_BLUE), Bradley on TESTBODY with the Iraqi texture + a mod portrait on the
panel, entry names "JACK" "FOX", 13 Iraqi_Conscripts recorded with MS_BLUE (not on screen in 60 s), `[fail]` lines for
a missing texture / body / part number; the standalone `dinput8.dll` the same. Not yet: a human play-test, enemies
seen on screen, a save loaded, the MODS menu's live reload, LOD distance.

## Sounds from the Mods folder (2026-10-22, `mods_sound.cpp` + `mods_sound_adpcm.h`, sub-plugin `ds2fix.modsound`)

Modder page: repo root `docs/sdk/ds2-mods.md` "Sounds". DS2's sound library is DS1's (GAMELIB SoundDX, same chunk
format, same ADPCM): the ModKit's `sound.py` / `cli/ds_sound.py` read DS2's caches as they are (`decode_stream` now also
takes DS2's `PSF\x81` header). DS1 addresses in brackets.

| What | DS2 | Notes |
| --- | --- | --- |
| sound manager | `[0x6800EC]`: +0x3C language kept (6 = American on the dev PC), +0x1594 loaded banks (list: +0 key, +0xC next), +0x159C cache file | [`[0x63C9AC]`, +0x24] |
| CSoundManager::SetSoundCache thiscall(name) ret 4 | `0x4B5DE0` [`0x55F540`]; callers mission init `0x4129DA` ("<level>.sch"), front-end init `0x42C9A7` ("FrontEnd.sch") | chunks BANK / PFSM / PFST / IMUS / EOF (`0x464F45`); PFSM / PFST kept when language = +0x3C or -1; **an unknown / bad chunk = "Cache file ... seems to be corrupt" + `__exit(0)`** |
| cache open `FUN_004b5d60` (only caller SetSoundCache) | call **`0x4B5D6D`** `call 0x4C49C0` (open wrapper (name, flags) → resolver → `FUN_004c3140(path, "rb")`) | hooked: a level the mods change → merged copy in `%TEMP%\DesertStorm2Fix\sound\` opened by `FUN_004c3140` with `[0x665E4C]` bits 0 / 1 cleared (disk branch); else the game's open |
| LoadSoundBank thiscall(name, flag) | `0x4B33C0` (17 callers): ELF hash `FUN_004c0ca0` → `FUN_004c0be0` of "<name>.sbk" in the bank list, else `FUN_004b7250` reads the file (open wrapper → resolver; first byte `V` + version: the original tool's bank file) | [`FUN_0055dd20`]; a mod `.sbk` is served by mods.cpp's resolver |
| ADPCM filter table | `0x6196F0` (5 float pairs, = DS1 `0x5FE64C`); decoder object `0x619718` (`FUN_004b82c0`) | encoder = DS1's (`mods_sound_adpcm.h`) |
| stream reader CWaveDataSourcePsfStream | vtable `0x5EB560`; ReadFromSoundCache **`0x4B7790`** (entry `83 EC 0C 8D 44 24 00`, = DS1 `0x5610E0`): +0x18 path (`FUN_004be320(size, 7)`, freed by `FUN_004be3b0` in the destructor `0x4B79B0`), +0x14 bytes per channel, +0x10 rate, +0x1C channels; open slot 4 `0x4B7900` = fopen + fseek 8 | entry detour: a mod `.psf` of that file name takes the path, +0x14 = (size − 8) / 32 × 16. DS2's one PFST = `data\sound\PC_FrontEndMusic.psf` ("PSF\x81", 44.1 kHz stereo) |
| interactive music CWaveDataSourceIMus | vtable `0x5EB4B4`; ReadFromSoundCache **`0x4B7370`** (entry `83 EC 08 8D 44 24 00`): +0x14 path, +0x18 segment table (12 B per segment, offsets into the file), +0x1C segment count; open `0x4B7640` (fopen) | entry detour: the mod file only when its size equals the game file's (segment offsets), else `[fail]`. The 8 mission music files (`PC_Mobeus` … `PC_Lose`, "PSF1" + segments of "PSF\x81" streams) |
| Bink films | `BinkOpen` from one site **`0x526401`** (`call [0x5DA2A0]`, name `data\FMV\<film>[UK / US]PAL.bik` relative, flags 0x4000); film name built in `FUN_0042cd10..` from the campaign flow `0x631608` | `HookIndirectCall`: a mod `.bik` of that file name opens instead |
| WEAPS.TXT | shot bank = **column 68** (0-based 67) [DS1 col 64]; gun banks = click, distant tail, shot 1, shot 2 (all hand-held guns; ZSU-23 6, launchers / grenades other layouts) | |

Speech: no streams in DS2 (DS1's `CS_*` / `OBJ_*` don't exist): every squad line is a PFSM per language (0-4, 6) in each
level cache; a level `.SCH` sample of language -1 replaces that id in every language (`BuildCache` relabels it).

For the tables agent's `.weapon` (`features.h`): `MakeWeaponSoundBank(donor bank, new bank, mod folder, {shot wavs},
tail wav, file name for the report)` = DS1's MakeSoundBank (new sample ids `ElfHash(new + "#n") | 0x10000000`, bank
key `ElfHash(new + ".sbk")`, pack added to every level); `BorrowSoundBank(name)` = DS1's BorrowBank (a bank + its
samples from the first game cache with it, added to every level); `ClearWeaponSoundBanks()` before re-making them in a
MODS-menu reload. Call before the level loads; the weapon row's col 68 = the new bank name. `.uses` `bank X` lines
(mods_levels.cpp `BuildUses`) call `BorrowGameBank(X, file)`; `BuildModReferences` drops them first with
`ClearGameBankReferences` (separate from the `.weapon` packs, which `ClearWeaponSoundBanks` drops).

Verified 2026-10-22 by background runs (scratchpad ds2modsound `run.py`: DS2 lock, dinput8 + PSE\ + HKCU\Software\PSE\ds2
backed up / restored, test mod `Mods\ZZSoundTest` removed after, PSE tree RelWithDebInfo): front end - `FrontEnd.sch`
opened from the merged copy (+1 pack bank), `PC_FrontEndMusic.psf` stream from the mod (a ds_sound.py `psf` file),
`Logo1PAL.bik` from the mod; Mission01 - level `.SCH` "7 sound(s) replaced" (an M16A2 shot + one speech line in all 6
languages), the pack bank + a `Dev\SoundTestWeapon` bank (AK47 + a WAV) in the manager's bank list (58 = 56 + 2), both
replaced samples' ADPCM bytes found in the game's memory (the speech one under language 6), `PC_Mobeus.psf` (same size)
taken, `PC_Lose.psf` (32 bytes short) `[fail]`, a corrupt `.sch` `[fail]` and not used; vanilla Mission01 (no mods) opens
its own cache. Offline: encode → decode SNR 58 dB (sine), 17 dB (noise burst), a decoded game sample re-encodes
exactly. The standalone `dinput8.dll` builds with it (not run). Not yet: anything heard by a human, the MODS-menu
reload with sound mods, a `.weapon` using the API (the tables agent's part).

## Memory (2026-10-22, plugin `ds2fix.memory` in `plugins/memory`, + the 4 GB patcher)

Port of DS1's `memory.cpp` as an external PSE plugin (`CLIENT_ONLY`, no settings; reference with every address:
repo root `docs/sdk/reference/game-ds2.md` "ds2fix.memory"). DS1 addresses in brackets.

| What | DS2 | Notes |
| --- | --- | --- |
| Global Heap set-up | call `0x4136FC` → ctor `0x4C0650` thiscall(size, name), object `[0x664380]` [`0x40FFD7`] | +0x48 size, +0x4C free, +0x50 used, +0x58 buffer, +0x5C not owned, +0x64 marks; no buffer-taking ctor [`0x4C0340`]: the plugin runs the ctor with 16 bytes and swaps its VirtualAlloc buffer in |
| alloc / alloc16 / pop / mark / release | `0x4C0750` / `0x4C07A0` / `0x4C07E0` / `0x4C0810` / `0x4C0830` | entry detours (peak, failures; release / pop free the outside buffer) |
| reader | `0x4C49E0` (entry `A1 AC D3 67 00`) [`0x4BFB90`] | texture loaders' read calls `0x54222A` / `0x5422DA` / `0x542811` / `0x542E5A` |
| game allocator / free | `0x4BE320` (retries for ever) / `0x4BE3B0` | |
| D3D9 | device slot 23 CreateTexture, texture slot 2 Release | broker pointer hooks per class |
| main menu draw | vtable `0x5E6F58` slot 5 | notice at y 305 of the 1280 x 1024 front end (the monitor shows only y ~287..752, x ~330..953 of it) |

Heap ownership: the plugin's call hook (priority 100) runs in front of `ds2fix.mods`' (0); `mods.cpp` sees the
`ds2fix.memory` interface and passes the call through (and grows on top of what arrives when the plugin isn't there).
Other plugins get room with `AddHeapReserve` (interface `ds2fix.memory` v1, `src/pse/ds2fix_memory.h`) at
`PSE_MSG_POST_LOAD`.

4 GB patcher: repo root `src/patcher/patcher.cpp` built with `PATCHER_GAME_DS2` (target `ds2_patcher`,
`DesertStorm2Fix-Patcher.exe`, in the tree + package): CDS II.exe 1.0 (disc-free hash or PE time stamp 0x3F4B1A8A),
never the SecuROM exe (hash / entry 0x42CC17, exit code 6). Build option `-DDS2_SKIP_PLUGINS=limits` leaves a plugin
folder out (used while `plugins/limits` was being written).

Verified 2026-10-22 (scratchpad runner ds2memory `run_mem.py`: DS2 lock, dinput8 + PSE\ + logs + `HKCU\Software\PSE\ds2`
backed up and restored exactly, test mod `Mods\ZZMemTest\SAS_LAND_ROVER_1.DDS` = 2048² DXT5 checker, 5.3 MB):
- vanilla front end / Mission 1: heap stays 1000 KB, peak 167 / 512 KB, log lines per level + every 30 s;
- mod, 2 GB: heap 7.0 MB, the texture read into it (peak 5461 KB), the Land Rover shows the checker;
- forced fallback (`Dev\HeapMB=1`): `loaded outside it` + `freed` twice, 0 failures, texture on screen;
- without the plugin: `ds2fix.mods` grows the heap itself (1024000 -> 7315456);
- a LAA copy of the exe (`CDS II LAA.exe`, removed after): address space 4096 MB, largest free 2046 MB, cap 256 MB,
  the PSE loader recognised it (all plugins loaded), mod texture on screen;
- patcher on exe copies (`laa_test.py`, 27/27): 1-byte change at the flag, backup = original, file times, loader hash
  unchanged, in use (suspended process) → 3, another 1.0 file known, other time stamp → 5 / `/force`, SecuROM exe
  (also a changed one, by entry point) → 6 even with `/force`, a 64 MB reserve at 0x90000000 in a suspended process
  only with the flag; the user's real CDS II.exe untouched.
Not yet: the real exe patched by a human, a human play-test with big texture mods, the notice seen by a human.

## Engine limits (`plugins/limits` = `ds2fix.limits`, 2026-10-22)

Port of DS1's `limits.cpp` (repo root `src/features/limits.cpp`, `research/large_battles.md`) as an external PSE plugin
(SIMULATION, `requires = pse.game.ds2 >= 1.0, ds2fix >= 0.1`). Research notes: `research/limits.md` (local). DS1
addresses / values in brackets. **What DS2 already does itself**: the physics pools are per level, from the level's
`.FAS` (`"DynamicObjects"` / `"MeBodies"` / `"MeModels"`, world +0x1BC8 / +0x1BCC / +0x1BD0, defaults 100 / 250 / 200
without a .FAS; the shipped levels hold measured values: 9-20 / 72-97 / 96-164); waypoints are counted in the ENV (no
fixed table); constraints 1000 [500] and collision pairs / objects 800 / 800 [400 / 400] were doubled.

| Limit | DS2 site | Vanilla → plugin | How |
| --- | --- | --- | --- |
| Physics-object list (dynamics mgr [0x680090] +4 cap) | ctor `0x4F8600(objects, bodies, models)` called at `0x50992C`; add `0x4F6D80` | FAS DynamicObjects (Mission 1: 20) → ≥ 4096; add reuses nulled slots, refuses when full | call hook + entry replaced |
| Karma bodies (MdtWorldCreate `0x5B8930`) | same ctor call | FAS MeBodies (Mission 1: 90) → ≥ 2048 | call hook |
| Collision models (McdInit `0x59BB70`) | same ctor call | FAS MeModels (Mission 1: 111) → ≥ 4096 | call hook |
| Karma constraints (joints + contacts) | imm `0x4F8663` | 1000 → 8192 [500 → 8192] | byte |
| Collision pairs / space objects (`0x59E1F0`) | imm `0x4F8769` / `0x4F876E` | 800 / 800 → 16384 / 4096 | byte |
| Projectiles [0x6800E4] (`0x4F3A80(count, gravity)`) | call `0x413C6E` (push 0x32 at `0x413C6A`) | 50 → 1024 | call hook |
| Particles [0x6800B4] (count = budget / 0xC0) | imm `0x413CEC` | 48 000 B = 250 → 384 000 B = 2000 [50 000 / 0xC8] | byte |
| Event queue [0x6800B8] (`0x4E1890(count)`, 0x4C B) | imm `0x4CFEF0` (made again at level unload) / `0x4D13E1` (engine init) | 200 → 2048 | byte |
| Render command arena [0x680070] (`0x54EE10(size)`) | imm `0x4D0F56` | 64 KB → 1 MB | byte |
| Decal store ring (decal mgr +0x40, `0x550B40`) | imm `0x550C41` | 64 KB → 512 KB [128 KB] | byte |
| A* searches per frame (router [0x6800E8] +0x30) | imm `0x4B93B3` | 3 → 12 | byte |
| Software sound voices (sound mgr [0x6800EC] +0x50; +0x54 = +10) | imm `0x4B2F58` | 6 (16) → 32 (42) | byte |
| Script-id table (object mgr +0x2C) built while the highest id ≤ | imm `0x4CE755` | 1000 → 65534 | byte |
| Save buffer (saver [0x62F458] +0x88) | imm `0x4197FD` | 0x384AF + extras (232 182 B in Mission 1) → + 4 MB | byte |
| AI think budget (scheduler [0x62F424] +0x58 soldiers / +0x5C vehicles, fixed 1 each) [DS1: adaptive, clamp 2 / 2] | call `0x4107BD` (→ `0x401DF0`) | 1 / 1 → max(1, ⌈soldiers / 40⌉) ≤ 32 / max(1, ⌈vehicles / 20⌉) ≤ 16 (type-1 / type-0 object lists, all teams) | call hook, set before each call |
| Global Heap (1 024 000 B) | `ds2fix.memory` AddHeapReserve, else heap set-up call `0x4136FC` (priority 200) | + 2.5 MB for the pools above | interface / call hook |

**Saves** (all through call hooks; a save made with the plugin is the unmodified game's file byte for byte in layout and
size, unless a scene really needs more than the vanilla buffer):

| What | Site | Rule |
| --- | --- | --- |
| particle writer `0x4F1300` (every slot, record 0x88 via `0x4F0F50`) | call `0x41BF0A` | 250 slots (the loader's `push 0xFA` at `0x41C439`), live ones first (slot +0xB0 top bits ≠ 01) |
| projectile writer `0x4F4930` (every slot via `0x4F44E0`) | call `0x41BF1A` | 50 slots (loader `push 0x32` `0x41C44E`), live first (+0x9C ≠ 0x64) |
| event counter `0x4E1B60` → header byte +0x20 (`0x419D9E`) | call `0x419D99` | min(n, 255) |
| event writer `0x4E1F20` (0x48 B per saveable event via `0x4E1E40`) | call `0x41BF2C` | the first 255 saveable (flag +4 & 0x20 clear, type 0x63, filter ≥ 0) |
| fwrite of the whole zeroed buffer (`0x4C2800` → fwrite) | call `0x41C6AC` | up to the last non-zero byte, never below buffer size − 4 MB |

Loading (`0x41C7C0`) zeroes the buffer and freads up to its size, so the bigger buffer reads vanilla files unchanged.

**Verified 2026-10-22** (background runs, Mission 1, PSE dev tree with ds2fix + ds2fix.memory; dev `Dev\NoLimits` = 1 =
the game's own limits; scratchpad runner `ds2limits/run.py`):

- values in memory at level load (dev log): physics list cap 4096 (FAS 20), projectiles 1024, particles 2000, events 2048,
  render arena 1 MB, A* 12, voices 32 + 10, save buffer 4 426 486, Global Heap 3.5 MB (via ds2fix.memory);
- saves: plugin save → loaded by the plugin and by the vanilla limits, vanilla save → loaded by the plugin; both files
  232 182 bytes (= the vanilla buffer); the game ran on after each load. In vanilla Mission 1 the 250 particles are in
  use most of the time (the plugin's 2000 fill up in big explosions too);
- stress (`Mods\ZZLimitsTest\mission01.env`: +50 T-62s, +100 Iraqi conscripts, +50 Delta, ModKit env.py): the vanilla
  limits crash while the level loads (AV `0x5B9B78` in Karma body creation - FAS MeBodies 90), the plugin loads it
  (physics list 70, 159 soldiers, 62 vehicles at load, AI 4 soldiers / frame) and runs it for 3 min; save 328 403 bytes
  (> the vanilla buffer: only the plugin can save that scene) and loads back. **Scene-edit finding** (not the plugin):
  any HLOI appended to Mission 1's scene - even one copied soldier, with or without groups, vanilla limits too - leaves
  the mission in a letterboxed cinematic that never ends (the copied tanks also drop out of the vehicle lists within
  2 s), so the stress scene couldn't be played; a round-tripped unchanged scene plays normally. **Cause found
  2026-10-22** ("Levels from mods" below): the shipped `MISSION01.OCT` indexes the scene's objects by their place in it -
  with an appended HLOI nothing of the world is drawn and the intro never ends; `ds2fix.modlevels` rebuilds the octree
  for a scene a mod changed, and the same one-soldier scene then plays;
- frame time (background runs, 1280 x 1024 windowed, uncapped, normal Mission 1, 10-s windows): plugin 0.65 / 0.81 /
  0.63 / 0.99 / 1.25 / 1.55 / 1.86 ms, vanilla limits 0.60 / 0.66 / 0.61 / 0.95 / 1.07 / 1.25 / 1.18 ms - the later
  windows cost more with the plugin because up to 2000 particles are alive instead of 250. Not seen by a human: the
  bigger particle / decal budgets on screen, sound with 42 voices, a mod that really needs the pools.

## Levels from mods (2026-10-22, `mods_levels.cpp`, sub-plugin `ds2fix.modlevels`; modder page: repo root `docs/sdk/ds2-mods.md` "Levels and missions")

Port of DS1's mission-DLL / game-reference / edited-scene support (repo root `src/features/mods.cpp` BuildReferences,
`scripthost.cpp`). Every address: repo root `docs/sdk/reference/game-ds2.md` "Levels from mods". Level init
`FUN_00412690` is DS1's `FUN_0040c090` almost line for line: archive "<name−4>.dat" (`0x41276F`, `0x412879`,
`0x4130D0`), `.sch` (`0x412963` strcpy → `0x4129DA`), "<stem>Music.sbk" (`0x412A49`), FAS (`sprintf` `0x5096BB` from
world +0x1BBC), extension check "dll" → `FUN_00422020` (LoadLibraryA `0x42202D`), "env" → `FUN_00411970`
LoadEnvironment directly (a bare `.env` level - not used yet), else the text manifest `FUN_00421f40`; octree flags
'c' / 'd' / 'r' at `0x412E64..`, Routing_Load / Build at `0x412FE7` / `0x413003`.

- mods.cpp wiring: `BuildModReferences()` after each `BuildIndex()`; `AddBuiltModFile` (index entry; the opener hook
  opens that folder from disk too: `g_builtRoot`).
- `.dllpatch` / `.envpatch` / `.rmdpatch` / `.uses` (`file`, `piece`; `bank` = `[fail]`, sounds by reference are the
  sound feature's) built into `%TEMP%\DesertStorm2Fix\merged\game`; archive hash lower-cased; route files v3 (29-byte
  markers); kit pieces: EOBJ 0x35 parent name at +0xAC (DS1 +0xB0) - C++ cut byte-identical with ModKit `kits.extract`
  (DS_M01_Z05B_LAND.EVO object 1).
- `<stem>.level` `--@archive`: decided at the level's first archive open (the stem comes from "<stem>.dat").
- New-name DLL levels: `<scene>.si` loaded at `0x411CE1` when the campaign has no entry for the level (Mission01.dll
  run as MyOp.dll crashed at `0x43629E` after 34 s without it).
- Octree: mod scene (its .ENV in the index, no real `.OCT` in the mod) → flag 'c' answered yes, fopen to `%TEMP%`, free
  + load-back skipped (47 ms for Mission 1).
- Navigation: mod scene without own .QTD / .RMD / .rmdpatch → Routing_Load answers 0, the game builds from the scene,
  `MergeHandPlaced` puts the base route file's hand-placed markers (Mission 1: 426, flags bit 0 clear) in front (DS1
  BuildNav ported: in-memory marker 32 B, links +0x10, flags +0x1A, island byte +0x1C; quadtree node +2 shifted).
  Mission 1 unedited counts: 918 generated (shipped 920) + 426 hand-placed, 1 link lost, 8 shipped links kept.

Verified 2026-10-22 by background runs (scratchpad `ds2modlevels/run.py` + `gen.py`, PSE dev tree, game data only in
the scratchpad): `Mission01.dllpatch` → `MISSION01.DLL` built and loaded from `%TEMP%`; `MyOp.dllpatch` (= Mission01.dll)
+ `MyOp.level` (`--@archive mission01`) boots, plays 90 s (before the `.si` fix: crash at 34 s); `.envpatch` props
(8-16 dragon's teeth, 6x scaled ones visible in the intro shot), octree + navigation rebuilt, Mission 1 plays
(soldier path equal to vanilla); one appended Iraqi soldier (new script id 312): plays with the rebuilt octree, stuck
letterboxed with nothing drawn with the shipped `.OCT` copied into the mod; a ModKit editor Save as mod (`.envpatch`
only) plays; bad files (wrong DLL size / bytes, unknown base, bad hunk, bad count, `.level` without `--@archive`, bad
`.uses` lines, piece index out of range) → `[fail]` lines, no crash; no mods = no rebuild, vanilla path; no new file in the
game folder. Not verified: a prop actually blocking a soldier / vehicle (the road-block row didn't stop the jeep -
probably placed off its road), a full mission played through, MODS-menu Apply with level mods, the standalone dinput8
in game (it compiles).

## Lua missions (2026-10-23, `scripthost.cpp` + `campaigns.cpp`, sub-plugin `ds2fix.scripthost`)

Port of DS1's scripthost.cpp / campaigns.cpp. Modder page: repo root `docs/sdk/ds2-mods.md` "Lua missions and custom
campaigns"; every address: `docs/sdk/reference/game-ds2.md` "Lua missions". DS1 addresses in brackets.

- Level init's text-manifest call `0x412C51` [`0x40C405`] runs `.lua` levels; script object `[0x62FE00]` [`0x60F578`],
  same 6 callbacks; API table `0x60D638` (0x9F0) [`0x5ED238` (0x8D8)]. The script object is taken back at the unload
  call `0x40D7A8` (the game clears it only with a module) and the Lua state closed.
- `--@archive`: mods_levels.cpp's `StartLevel` asks `features::ScriptArchiveFor(flow name)`; `--@base`: DLL loaded with
  `FUN_00422020` while API +0x70 points at the mission's scene, then the base scene's `.si` is read (campaign levels get
  it from LoadEnvironment, a custom level doesn't).
- Objectives: DS2 keeps DS1's campaign objective list (`CMission::AddObjective` `0x42F9E0`), but its API table has no
  add / show / complete entries - the `.si` reader calls cdecl helpers `0x440310` / `0x440330` / `0x440390` /
  `0x440520` ... directly; Lua does the same. The API's objective slots (+0x5AC ...) work on `.si` objectives by
  number (activate / complete / deactivate - the ModKit's api_slots.json names are off by one meaning there).
- No "set next level" slot: DS2's campaign flow (`0x631608`) picks the next level. The flow's advance `0x430FE0` (12
  calls hooked) goes to set_next_level / the custom campaign's next mission after a `.lua` level, else the main menu;
  the dispatcher's custom-name source call `0x42CF3B` (Pivotal's level dialog `0x40C830`) answers the campaign's saved
  level name (+0x18) - restart and LOAD GAME of a custom level; the front-end frame's update call `0x40E73C` returns 2
  once to start a campaign from our menu.
- CUSTOM CAMPAIGNS: menus.cpp state 0x38 (`SetCampaignScreenHooks`, SINGLE PLAYER's 4th row), then the game's
  DIFFICULTY screen (state 8); its pick (state 0xA request) runs the hook's start instead (`menus::OpenDifficulty`).
- Messages differ from DS1 (Mission01.dll's handler: 0x11 complete, 0x12 failed, 0x16 timer, 0x4F `.si` objective done;
  queries 0x0F, 0x1B, 0x24-0x29, 0x2A / 0x2B (return value), 0x3B, 0x3E, 0x5A).
- No standard animation table: DS2's exe registers the standard states (`0x60E088`, 0x38-byte entries); the DLLs add
  only their scripted ones.
- Texts: a level's compiled texts are "<STEM>.BLF" (level init `0x4127E1` -> `FUN_0040f820`); a level on another
  level's archive (`.lua` `--@archive`, `<Level>.level`) had none - the end screen read "*no string*" everywhere.
  mods_levels.cpp loads the archive level's BLF instead (also at `0x40F9D4`, `0x42CE97`, `0x41F36A`).

Verified 2026-10-23 by background runs (scratchpad runner `ds2lua/run.py`): see game-ds2.md. Test pitfall: `CreateDevice`
fails with D3DERR_DEVICELOST (0x88760868) while the PC's display is off (the user away) - every background run then
quits at 0.4 s; the runner waits with a D3D9 probe (`probe64.py`) and retries.

