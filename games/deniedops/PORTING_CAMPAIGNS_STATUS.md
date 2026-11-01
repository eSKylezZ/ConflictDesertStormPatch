# Custom campaigns + mission DLLs for Conflict: Denied Ops (running status)

Port of the `campaign.txt` custom campaigns (DS1 / DS2 / TGE / Vietnam) to DeniedOpsFix. Updated after every stage.
Addresses = the Steamless copy of `ConflictDeniedOps.exe` (code decrypted after SteamStub's hand-over, unpack.cpp).

## Stages

1. RE of the operation flow - DONE (2026-10-30, below).
2. Mission by name + mod mission DLLs - DONE: `features::StartMission(dll)` starts an Operations.xml operation from
   code (dev KeyScript `op:M_CMB.dll`: briefing film, level, playable - background runs); the mission DLL load
   (`call [LoadLibraryA]` at `0x444452`) is hooked: a mod's DLL with the operation's name replaces it, a mod DLL with a
   new name + `<Name>.level` (`--@archive m_wtr`) plays on that operation's data. Mission success from code for tests:
   KeyScript `win` (script API +0xCB4 with 0x10).
3. CUSTOM CAMPAIGNS - DONE: sub-plugin `deniedopsfix.campaigns` (`FeatCampaigns` default 1), main menu item after MODS,
   one screen `SCR_DSFIX_CAMPAIGNS` for campaigns / chapters / missions (modsmenu.cpp `ListScreen`), 8-row window
   that scrolls, LOCKED rows, Back steps up a list, chaining after a won mission, progress DWORD.
4. Verified in game by background runs - see "Verified".

## How an operation runs (game flow object `0xA29290`)

- Operations.xml records: 0xD28 bytes each from `flow+8`, count `+0x109278` (a second table `+0x10728`, count
  `+0x10927C`); record `+0` id, `+0x1C` (1 = single-player op), `+0x7C` briefing film name, `+0x12C` dllname,
  `+0x1F8` completed, `+0x1FC` (>= 100 = start of the campaign), `+0x20C`, `+0x210` / `+0x214` oncompletion action /
  next operation id. Find by dllname `0x470880` thiscall(flow, name) ret 4; by id `0x470910`; create a record for an
  unknown DLL `0x473020` (dev level picker path).
- `+0x10720C` current operation, `+0x1097B4` state, `+0x1097B8` / `+0x1097BC` / `+0x1097C0` front-end mode / movie
  mode / movie name, `+0x1097D0` after-state, `+0x1097D4` campaign running, `+0x109870` operation picked on the map.
- State machine `0x4756A0` thiscall(flow) (called from the movie end `0x4582C4`, screens, the starters): 5 = start
  (briefing film when `+0x7C` is set -> 6), 0xF -> `0x475060` = start the level (state 7; level init `0x441820`
  derives `<dll stem>.pc.dat`, `Assets_<name>.xml`, `.sch`, ... and loads the DLL at `0x444452`), 7 = playing; after
  the level 0x10: `0x474B40` marks the record completed (+ profile save `0x4709F0`), `0x474A30` (teaser films ->
  after-state 8 -> 0x11), 0x11: `0x474D10` = the oncompletion step (one caller `0x4758B6`): 0 next op by id, 1
  operations map (`SCR 0x8E9E69B0`), 2 main menu, 3 / 4 others.
- Start an operation from code the way the operations map does (`0x4758F0`): current = record, state = 5,
  `+0x1097D4` = 1, call `0x4756A0`, `[0xC49D18]` = 1. Works from the main menu (no profile needed in background runs).
- Mission end: script API slot +0xCB4 = `0x48EB70` cdecl(result) (`0x48BC20(0)`, then `0x459A20`
  thiscall([0xA08B5C], result, 0)); 0x10 = success. The debug key "Successful completion of mission" (`0x4389C0`)
  and the debug screen CCompleteMission (vtable `0x928F04`) only send message 0x31 to the mission script, which the
  shipped DLLs act on only with their own debug flag (M_CMB `[0x1000F398]`).
- After a won mission the game shows EQUIPMENT UPGRADES (`SCR 0x96736240`, NEXT UPGRADE) before state 0x11.

## Verified (2026-10-30, background runs, dev build)

- Start from code: `op:M_CMB.dll` from the main menu -> briefing film -> Comms Base loads and plays.
- Mission success from code: `win` -> after-state 8, EQUIPMENT UPGRADES screen.
- Menus: MAIN MENU > CUSTOM CAMPAIGNS (after MODS) > campaigns list > THE LONG WAR (10 missions: starts on the last
  open row, window scrolled; Up scrolls back to the top) > Back > OPERATION TESTBED chapters (ACT II: REMIX - LOCKED,
  Enter on it does nothing) > ACT I missions (SAWMILL - LOCKED) > Back > Back > main menu. A pick in a mission list
  starts the mission (briefing film).
- Chain: CUSTOM CAMPAIGNS > OPERATION TESTBED > ACT I > COMMS BASE -> briefing film -> Santa Cecilia Monastery ->
  `win` -> EQUIPMENT UPGRADES (2 x Enter) -> "Campaign: mission won - next M_SAW.dll" -> Sawmill briefing film ->
  Salem Union Sawmill loading screen; progress DWORD 1 -> 2.
- End: one-mission campaign with `mission select = no`: picking it starts Comms Base at once; after the win "campaign
  finished, back to the main menu" (SCR_GAME_MENU). The game's own CONTINUE CAMPAIGN item is enabled afterwards (the
  operation counts as completed in memory, as from the operations map; no profile was loaded, nothing written to
  `Documents\Eidos\ConflictDeniedOps`).
- Mod mission DLL: `op:TestCampaign:2` = `MyOp.dll` (a copy of M_WTR.dll) + `MyOp.level` `--@archive m_wtr`: the M_WTR
  operation started, "Mods: mission M_WTR.dll loaded from ...\Mods\TestCampaign\MyOp.dll", script object created,
  level plays.
- Screenshots: session scratchpad `do_screens\` (main menu, lists, LOCKED chapter, scrolling, mission, upgrades,
  chained mission, mod DLL mission, main menu after the campaign).
- Dist build (`deniedops-pse-dist`, build-deniedops-dist) compiles clean; not installed anywhere.
- Original game crash seen in 3 of ~10 level loads (EIP `0x6B1A24` / `0x6BC435` / `0x6AEECD`, stack through
  `0x6B1A4E` <- `0x4657E4`, during or right after the level load) - the intermittent crash the crate harness saw
  before PSE existed; retry.

## Not done / open

- The game's difficulty pick (NEW CAMPAIGN > difficulty) isn't on the path: a campaign mission runs at the difficulty
  the game has at that moment.
- A failed / quit mission: not tried (the oncompletion hook only acts on the operation the campaign started; a failed
  mission most likely never reaches state 0x11).
- A human with pad / mouse; with a loaded profile (a won campaign mission then saves the operation as completed in the
  profile, like the operations map would).
- Mission titles default to the DLL name (`M_TWN`) - the operations' own names (Operations.xml `opnameid`) aren't
  looked up.
- Lua missions for Denied Ops (925-slot renumbered script API) - a later step.
