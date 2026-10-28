#pragma once
// TGE game layer side of pse.game.tge (public header pse/include/pse/pse_game_tge.h, docs/sdk/reference/game-tge.md).
// Reads game memory and the loader only.
#include <string>

#include "pse/pse_game_tge.h"

namespace pse_game_tge {
const PSEGameTgeV1* Table();
// The layer's plugin ("pse.game.tge" 1.0.0, CLIENT_ONLY) - PSEGame_Info.plugin, registered by the loader.
extern const PSEPluginInfo kPluginInfo;
// SHA-256 (upper-case hex, LAA cleared) of the one GreatEscape.exe the layer was mapped on (no disc check, 1.0).
const char* KnownSha256();
// Registers the built-in plugin "pse.game.tge" (if the loader hasn't) and publishes the interface. Needs pse::Init.
PSEPluginHandle Register();

// The level the events say is loaded (tge_events.cpp keeps it; "" = none).
void SetLevel(const char* name);
// "" / "FE" / "*.dll" / anything else -> PSETgeLevelKind (pure).
uint32_t KindOf(const char* level);
// SHA-256 (upper-case hex) of a file with the PE Large Address Aware flag cleared; "" if it can't be read.
std::string ExeFileSha256(const wchar_t* path);
}  // namespace pse_game_tge
