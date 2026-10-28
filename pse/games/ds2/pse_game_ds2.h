#pragma once
// DS2 game layer side of pse.game.ds2 (public header pse/include/pse/pse_game_ds2.h, docs/sdk/reference/game-ds2.md).
// Reads game memory and the loader only, so pse_tests compiles it alone (pse/tests/test_game_ds2.cpp).
#include <string>

#include "pse/pse_game_ds2.h"

namespace pse_game_ds2 {
const PSEGameDs2V1* Table();
// The layer's plugin ("pse.game.ds2" 1.0.0, CLIENT_ONLY) - PSEGame_Info.plugin, registered by the loader.
extern const PSEPluginInfo kPluginInfo;
// SHA-256 (upper-case hex, LAA cleared) of the one CDS II.exe the layer supports (the disc-free 1.0).
const char* KnownSha256();
// SHA-256 of the retail SecuROM exe (same PE header, other .text - recognised only to log why PSE stays off).
const char* SecuromSha256();
// Registers the built-in plugin "pse.game.ds2" (if the loader hasn't) and publishes the interface. Needs pse::Init.
PSEPluginHandle Register();

// "" / "FrontEnd" / anything else -> PSEDs2LevelKind (pure).
uint32_t KindOf(const char* level);
// SHA-256 (upper-case hex) of a file with the PE Large Address Aware flag cleared; "" if it can't be read.
std::string ExeFileSha256(const wchar_t* path);
}  // namespace pse_game_ds2
