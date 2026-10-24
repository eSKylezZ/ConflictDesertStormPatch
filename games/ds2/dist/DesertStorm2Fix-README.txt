DesertStorm2Fix - Conflict: Desert Storm II enhancement patch
==============================================================

DesertStorm2Fix runs on the Pivotal Script Extender (PSE), a plugin loader for Pivotal's Conflict games.

Install: copy everything in this zip into the game folder (next to "CDS II.exe"), keeping the folders:

    dinput8.dll                     starts PSE when the game starts
    PSE\pse_loader.dll              the plugin loader
    PSE\Games\pse_ds2.dll           Conflict: Desert Storm II support
    PSE\Plugins\ds2fix.dll + .pse   DesertStorm2Fix itself
    PSE\Plugins\ds2fix.widescreen.dll + .pse   widescreen menus + HUD scaling (optional: delete it for the
                                    game's original HUD size)
    PSE\Plugins\ds2fix.memory.dll + .pse   room for big mod textures, memory figures in the log (optional)
    PSE\Plugins\ds2fix.limits.dll + .pse   bigger engine limits for big battles and big mods (physics, particles,
                                    projectiles, AI); saves stay compatible with the game without it (optional)
    DesertStorm2Fix-Patcher.exe     the 4 GB patch (see below; nothing happens until you run it)

If an older DesertStorm2Fix (a single dinput8.dll) is installed, let this one replace it. Its settings are taken over
at the first start.

Uninstall: delete dinput8.dll and the PSE folder (and turn 4 GB off first if you turned it on, see below).

The game's files are not changed (only the 4 GB patch below changes one flag in CDS II.exe, if you run it). Needs
the disc-free CDS II.exe (1.0); with any other exe the game runs unpatched.
Settings are kept in the registry (HKEY_CURRENT_USER\Software\PSE\ds2), never in files. If something goes wrong,
PSE\Logs\loader.log says what was loaded.

HUD size: OPTIONS (main menu or pause menu) has HUD SCALING (ON / OFF) and HUD SIZE (25-400 %).

Mods: files in Mods\<mod name>\ replace the game's files of the same name (a folder starting with "_" is switched
off). Main menu > MODS lists your mods: select one to switch it ON / OFF, then APPLY. Mission files change from the
next level; files the game read when it started (menus, texts, tables) need a restart - the menu says RESTART THE GAME
TO APPLY then. Mods\DesertStorm2Fix-report.txt lists what was used and any problems.
A mod's .skin file dresses particular soldiers (by unit name, e.g. Bradley) in its own textures, body model and
portrait without changing anyone else - see the mod's own notes; problems are listed in the same report.
Table mods: a mod's Weaps.txt / Units.txt / ... holds only new rows (added to the game's), a .replace.txt the rows it
changes; .weapon / .vehicle / .unit files describe weapons, vehicles and soldier types by name, .strings files
replace texts. Rows the game couldn't load are left out and listed in the report. Tables are read when the game
starts (the merged copies are built in %TEMP%\DesertStorm2Fix\merged), so the MODS menu asks for a restart.
Sound mods: a mod's .sch sound pack adds sounds to every level, a .sch named like a level's (mission01.sch) changes
that level's sounds, .psf files replace the music and .bik files the films. They take effect at the next level (or
when the menus load again); the changed sound files are built in your temp folder (%TEMP%\DesertStorm2Fix\sound).

Lua missions and campaigns: a mod's .lua file is a mission scripted in Lua (on one of the game's maps or its own); a
mod's campaign.txt lists such missions - SINGLE PLAYER > CUSTOM CAMPAIGNS shows them, then DIFFICULTY starts the
first, and each success continues with the next. Save / load work as in the game's missions. Command line:
"CDS II.exe" +level <name>.lua starts straight in that mission (used by the ModKit editor's Play).

4 GB memory: the game is a 32-bit program and gets 2 GB of memory unless its exe says it can use more. Run
DesertStorm2Fix-Patcher.exe, pick the game and press "Turn 4 GB on": it sets that one flag in CDS II.exe (Large
Address Aware) after checking it is the disc-free 1.0 exe, and keeps the original as "CDS II.exe.original". "Turn 4 GB
off" puts the original back. Close the game first. The main menu shows "2 GB MEMORY MODE" while the flag is off. Only
needed for big mods (large textures); the retail SecuROM exe is never changed. Command line: DesertStorm2Fix-Patcher.exe
/status | /enable | /disable ["game folder"].
