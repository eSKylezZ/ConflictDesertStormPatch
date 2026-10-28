#pragma once
// "pse.game.ds1.host" (version 0, PRIVATE - not in the SDK): what the DS1 game layer (PSE\Games\pse_ds1.dll) needs from
// and gives to its host plugin, DesertStormFix (dsfix.dll). Registered by the layer through pse.messaging at
// PSEGame_Init; DesertStormFix queries it in its PSEPlugin_Load (pse_host.cpp). Other plugins must not use it.
#include <stdint.h>

#include "pse/pse_game_ds1.h"

#define PSE_GAME_DS1_HOST_INTERFACE "pse.game.ds1.host"

extern "C" {
// A save's co-save image (the .pse file format) from older data next to it: DesertStormFix's ".sav.dsfix" sidecar
// (until 3.3.0-Alpha). savPath = the game's save ("...\Name.sav"). Copies up to bufLen bytes into buf (buf may be NULL
// with bufLen 0) and returns the image's full size; 0 = nothing older there. Game thread.
typedef uint32_t(PSE_CALL* PSEDs1LegacyCosaveFn)(const char* savPath, void* buf, uint32_t bufLen);

typedef struct PSEGameDs1HostV0 {
    uint32_t size;
    uint32_t version;   // 0
    // pse.game.ds1's view of DesertStormFix: split-screen players and the online session (NULL = none). Main thread.
    void (PSE_CALL* SetProviders)(int (*splitScreenPlayers)(), void (*online)(PSEDs1Online* out));
    // The level's title as the game shows it ("RESCUE", ANSI) / its display name ("Mission4.env" -> "Mission 4") into
    // buf (NUL-terminated); return the full length. Game thread.
    uint32_t (PSE_CALL* LevelTitleText)(const char* level, char* buf, uint32_t bufLen);
    uint32_t (PSE_CALL* LevelDisplayName)(const char* level, char* buf, uint32_t bufLen);
    // (2026-10-19, appended) The layer's co-save load (game layer: ds1_saves.cpp) asks this for a save without a .pse
    // before it reverts every plugin. NULL = none. Main thread, in the host's PSEPlugin_Load.
    void (PSE_CALL* SetLegacyCosave)(PSEDs1LegacyCosaveFn fn);
} PSEGameDs1HostV0;
}
