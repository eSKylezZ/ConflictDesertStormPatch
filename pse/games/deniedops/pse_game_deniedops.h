#pragma once
// Denied Ops game layer side of pse.game.deniedops (public header pse/include/pse/pse_game_deniedops.h,
// docs/sdk/reference/game-deniedops.md). Reads game memory and the loader only.
#include <string>

#include "pse/pse_game_deniedops.h"

namespace pse_game_deniedops {
const PSEGameDeniedOpsV1* Table();
// The layer's plugin ("pse.game.deniedops" 1.0.0, CLIENT_ONLY) - PSEGame_Info.plugin, registered by the loader.
extern const PSEPluginInfo kPluginInfo;
// SHA-256 (upper-case hex, LAA cleared) of the ConflictDeniedOps.exe this layer was mapped on (Steam no-CD, 2008).
const char* KnownSha256();
// Registers the built-in plugin "pse.game.deniedops" (if the loader hasn't) and publishes the interface.
PSEPluginHandle Register();
// SHA-256 (upper-case hex) of a file with the PE Large Address Aware flag cleared; "" if it can't be read.
std::string ExeFileSha256(const wchar_t* path);
}  // namespace pse_game_deniedops
