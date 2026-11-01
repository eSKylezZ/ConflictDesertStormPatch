# Conflict: Denied Ops – missions and campaigns from the Mods folder

DeniedOpsFix (PSE host plugin `deniedopsfix.dll`, sub-plugins `deniedopsfix.mods` and `deniedopsfix.campaigns`) plays
mission DLLs from mods and `campaign.txt` campaigns from the main menu, in the same format as the other Conflict games.
Mods live in `<game>\Mods\<mod>\` (any sub folders; folders starting with `_` or `.` are switched off - the main
menu's MODS screen does that). Every address: [reference/game-deniedops.md](reference/game-deniedops.md) "Custom
campaigns and mission DLLs".

A Denied Ops mission is an **operation** of the game's `Operations.xml`; each one names its mission script DLL:

| DLL | Operation |
| --- | --- |
| `M_CMB.dll` | the first campaign operation (Comms Base) |
| `M_SAW.dll`, `M_TWN.dll`, `M_WTR.dll`, `M_BAS.dll`, `M_CTL.dll`, `M_DMN.dll`, `M_OIL.dll`, `M_SUB.dll`, `M_WHL.dll` | the other campaign operations |

## Mission DLLs from a mod

- **Same name** (`M_CMB.dll` in a mod): that mission's script is loaded from the mod instead of the game folder,
  wherever the game starts it.
- **New name** (`MyOp.dll`): a mission of its own that plays on a shipped operation's level data. A text file next to
  it, named like the DLL, says which operation:

```text
# Mods\MyMission\MyOp.level
--@archive m_wtr          # plays on M_WTR's level: its archives, packages, scene, sounds, briefing film
```

(`archive = m_wtr` works too.) A mission with a new name is started from a campaign (below); the operation named in
`--@archive` is started and its DLL load gets `MyOp.dll`. Log line: `Mods: mission M_WTR.dll loaded from
...\Mods\MyMission\MyOp.dll`.

Lua missions don't exist for Denied Ops yet (its script API has 925 slots in a different order than Desert Storm's);
a mission is a DLL.

## Custom campaigns

```text
# Mods\MyCampaign\campaign.txt
name = Operation Testbed
unlock = progress
chapter = Act I: Shipped Ops
mission = M_CMB.dll | Comms Base
mission = M_SAW | Sawmill
chapter = Act II: Remix
mission = MyOp.dll | My Own Op
mission = M_TWN.dll
```

The same format as Desert Storm's, Vietnam's and The Great Escape's:

| Line | What it does |
| --- | --- |
| `name = …` | the campaign's name in the list (default: the mod folder's name) |
| `mission = file` / `mission = file \| Title` | the next mission: an operation's DLL (`M_SAW.dll`, `.dll` may be left out) or a mod's mission DLL with its `.level`; the title shows in the mission list (default: the file name without its extension) |
| `chapter = …` | optional: starts a chapter - the missions below it, up to the next `chapter` line, belong to it. Missions above the first `chapter` line go into a chapter called PROLOGUE; a chapter with no mission is left out |
| `unlock = progress` / `unlock = all` | `progress` (the default): a mission opens once the player reaches it - the first is always open; `all`: every mission can be picked straight away |
| `mission select = no` | picking the campaign starts its first mission not yet reached, with no chapter or mission list (default `yes`) |

The main menu shows **CUSTOM CAMPAIGNS** (after MODS) while a switched-on mod has a `campaign.txt`. Picking a campaign
shows its chapters (with two or more) or straight away its missions; Back (the game's Cancel: Esc) steps up a list, and out of the
campaigns list to the main menu. Locked chapters / missions read "- LOCKED" and can't be picked; the lists start on the
newest chapter / mission reached and scroll past 8 rows. Picking a mission starts it the way the game's operations map
does (briefing film, loading screen). When it is won (after the EQUIPMENT UPGRADES screen) the campaign's next mission
starts and opens in the lists - chapters don't break the chain; after the last one the game returns to the main menu.
Progress is kept per campaign: DWORD `Campaign <mod folder>` (how many missions are open) in
`HKCU\Software\PSE\deniedops\Plugins\deniedopsfix`, written when a mission starts and when the chain moves on.

Not on the path: the game's own difficulty pick (SINGLE PLAYER CAMPAIGN > NEW CAMPAIGN asks for one) - a campaign
mission is played at the difficulty the game currently has. A won campaign mission also counts as completed for the
game's own campaign, as it would from the operations map.

Setting: `FeatCampaigns` (default on) in DeniedOpsFix's key; it needs `FeatMods`.

Log / report lines (`Mods\DeniedOpsFix-report.txt`): `Campaign: OPERATION TESTBED (MyCampaign, 4 missions, 2
chapters)`, `[fail] Campaign X: campaign.txt lists no mission`, `Campaign X: chapter Y lists no mission - left out`,
`[fail] Campaign: MyOp.dll is no Operations.xml mission and no mod has it`, `[fail] Campaign: mod mission MyOp.dll
needs a MyOp.level with "--@archive <operation>"`.

## An operation's loading screen

The loading screen of an operation shows the picture CampaignGame.xml names (`m_BAS_LoadingMap.bmp` = the texture
`M_BAS_LOADINGMAP`, 2048 x 1024 DXT1, of the package `M_BAS_HARDCODED`), the header LOADING, the operation's caps title
bottom right (`M_BAS_TITLE_CAPS`) and gameplay tips. A mod changes them:

- the picture: ship the operation's whole `<OP>_HARDCODED.APAK` with `<OP>_LOADINGMAP` replaced (served by name like
  any package); the screen shows the texture's rows 0.09-0.92, stretched to a wide band, at about 0.6 brightness;
- the texts: `<OP>.loading` (OP = the operation's dllname without `.dll`, e.g. `M_BAS.loading`), UTF-8 lines

```
# comment
LOADING = BASIC TRAINING              a catalog key (or 0xHASH) answered while the loading screen is up
M_BAS_TITLE_CAPS = BASIC TRAINING
tip = July 28, 1990 - Special Forces Basic Training Area     replaces every tip (several tip lines: taken in turn;
                                                             "tip =" alone hides the tips)
```

The file applies while that operation is the current one. The ModKit's `ds1todo_loading.py` writes both for a converted
DS1 level (DS1's own loading picture, title, date and place). Log line: `Loading screen: <path> for M_BAS.dll - n
text(s), n tip line(s)`.
