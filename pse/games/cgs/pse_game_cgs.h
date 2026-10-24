#pragma once
// CGS (Conflict: Global Storm / Global Terror) game layer side of pse.game.cgs (public header
// pse/include/pse/pse_game_cgs.h, docs/sdk/reference/game-cgs.md). Reads game memory and the loader only.
#include <string>

#include "pse/pse_game_cgs.h"

namespace pse_game_cgs {
const PSEGameCgsV1* Table();
// The layer's plugin ("pse.game.cgs" 1.0.0, CLIENT_ONLY) - PSEGame_Info.plugin, registered by the loader.
extern const PSEPluginInfo kPluginInfo;
// SHA-256 (upper-case hex) of the one ConflictGlobal.exe (RELOADED NoCD) the layer was mapped on.
const char* KnownSha256();
// Registers the built-in plugin "pse.game.cgs" (if the loader hasn't) and publishes the interface. Needs pse::Init.
PSEPluginHandle Register();
// SHA-256 (upper-case hex) of a file; "" if it can't be read.
std::string ExeFileSha256(const wchar_t* path);
}  // namespace pse_game_cgs
