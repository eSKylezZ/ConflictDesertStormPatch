#pragma once
// DS1 game layer side of pse.game.ds1 (public header pse/include/pse/pse_game_ds1.h, docs/sdk/reference/game-ds1.md).
// The implementation (pse_game_ds1.cpp) reads game memory and the loader only; what it needs from DesertStormFix's
// features (split screen, the online session) comes in through Providers, so pse_tests can compile it alone.
#include <string>

#include "pse/pse_game_ds1.h"

namespace pse_game_ds1 {
struct Providers {
    int (*splitScreenPlayers)() = nullptr;     // features::SplitScreenPlayers
    void (*online)(PSEDs1Online* out) = nullptr;   // fills active / role / localPlayer / players / peers / inMission
};
void SetProviders(const Providers& p);

const PSEGameDs1V1* Table();
// The layer's plugin ("pse.game.ds1" 1.0.0, CLIENT_ONLY) - PSEGame_Info.plugin, registered by the loader.
const PSEPluginInfo* PluginInfo();
extern const PSEPluginInfo kPluginInfo;
// SHA-256 (upper-case hex, LAA cleared) of the one DesertStorm.exe known (Steam = GOG).
const char* KnownSha256();
// Registers the built-in plugin "pse.game.ds1" (version 1.0.0) and its interface. Needs pse::Init. Returns its handle.
PSEPluginHandle Register();

// Shared with features (customise / photo mode): the level's title as the game shows it ("RESCUE", ANSI, "" if
// unknown) and its display name ("Mission4.env" -> "Mission 4"). Game thread (the first one).
std::string LevelTitleText(const char* level);
std::string LevelDisplayName(const char* level);
// "Mission 4: Desert Watch" (UTF-8) for a level name, "" for front end / outro / nothing (= LevelTitle).
std::string LevelTitleUtf8(const char* level);

// SHA-256 (upper-case hex) of a file with the PE Large Address Aware flag cleared; "" if it can't be read.
std::string ExeFileSha256(const wchar_t* path);
uint32_t TextHash(const char* key);
}  // namespace pse_game_ds1
