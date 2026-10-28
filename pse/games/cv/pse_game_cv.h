#pragma once
// CV game layer side of pse.game.cv (public header pse/include/pse/pse_game_cv.h, docs/sdk/reference/game-cv.md).
// Reads game memory and the loader only.
#include <string>

#include "pse/pse_game_cv.h"

namespace pse_game_cv {
const PSEGameCvV1* Table();
// The layer's plugin ("pse.game.cv" 1.0.0, CLIENT_ONLY) - PSEGame_Info.plugin, registered by the loader.
extern const PSEPluginInfo kPluginInfo;
// SHA-256 (upper-case hex, LAA cleared) of the one Vietnam.exe the layer was mapped on (Sep 2004 build, no SecuROM).
const char* KnownSha256();
// Registers the built-in plugin "pse.game.cv" (if the loader hasn't) and publishes the interface. Needs pse::Init.
PSEPluginHandle Register();

// SHA-256 (upper-case hex) of a file with the PE Large Address Aware flag cleared; "" if it can't be read.
std::string ExeFileSha256(const wchar_t* path);
}  // namespace pse_game_cv
