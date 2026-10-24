// PSE\Games\pse_tge.dll: The Great Escape game layer (docs/sdk/reference/game-tge.md, layout.md).
//   PSEGame_Detect - the exe is GreatEscape.exe 1.0 at 0x400000: the no-CD one this layer was mapped on (SHA-256
//                    50F4A87E..., LAA bit cleared) or another file of that build (PE time stamp 0x3F128EF0). The
//                    retail SafeDisc exe shares the time stamp but its code is encrypted on disk and decrypted by
//                    SafeDisc's driver (which current Windows doesn't load): every hook checks the bytes it replaces
//                    and pse.ids its signatures, so on such a file the parts that differ are skipped, nothing is
//                    written over them;
//   PSEGame_Init   - registers the ID database (pse.ids, signatures checked against the ORIGINAL bytes - before any
//                    plugin patches), pse.game.tge v1, then installs the hooks that drive the core game events
//                    (tge_events.cpp: frame, render, level, D3D8, exit - owner pse.game.tge). Without a host plugin
//                    the loader's own PSE\Logs\loader.log takes every line;
//   PSEGame_Info   - game id "tge", the layer's plugin, the host plugins it allows ("tgefix" = GreatEscapeFix,
//                    games/tge).
// Loader services through pse/hostlink (the layer gets pse.host like a host plugin).
//
// The exe's entry point is a no-CD stub (section .mythoep) that fills SafeDisc's import slots and jumps to the original
// entry 0x51FC72. The shim takes the stub's entry point, so PSE starts before the stub runs: nothing here may rely on
// those slots (the layer uses the normal import table only - d3d8!Direct3DCreate8 at 0x5F52A4).
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "generated/pse_ids_tge.h"   // pse/ids/generated (scripts/gen_ids.py)
#include "hostlink.h"
#include "ids.h"
#include "pse_game_tge.h"
#include "tge_events.h"

namespace {
const char* const kHostPlugins[] = {"tgefix", nullptr};

constexpr uint32_t kBuildTimestamp = 0x3F128EF0;   // GreatEscape.exe 1.0
}  // namespace

extern "C" {
__declspec(dllexport) extern const PSEGameLayerInfo PSEGame_Info;
const PSEGameLayerInfo PSEGame_Info = {sizeof(PSEGameLayerInfo), 1, "tge", &pse_game_tge::kPluginInfo, kHostPlugins};

__declspec(dllexport) int PSE_CALL PSEGame_Detect(const PSEGameDetectInfo* d) {
    if (!d || d->size < sizeof(PSEGameDetectInfo) || !d->exeSha256 || d->imageBase != 0x400000) return 0;
    return _stricmp(d->exeSha256, pse_game_tge::KnownSha256()) == 0 || d->peTimestamp == kBuildTimestamp ? 1 : 0;
}

__declspec(dllexport) int PSE_CALL PSEGame_Init(const PSELoadInterface* pse) {
    if (!pse::hostlink::Attach(pse)) return 0;
    // ID database first: the loader calls this before any plugin, so the game's bytes are still the original ones.
    pse::ids::RegisterGameForProcess(pse_ids_tge::kGame, pse->plugin);
    if (!pse_game_tge::Register()) return 0;
    tge_events::Install(pse->plugin);
    return 1;
}
}
