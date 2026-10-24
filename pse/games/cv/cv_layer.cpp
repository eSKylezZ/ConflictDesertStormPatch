// PSE\Games\pse_cv.dll: Conflict: Vietnam game layer (docs/sdk/reference/game-cv.md, layout.md).
//   PSEGame_Detect - the exe is Vietnam.exe, Sep 2004 build at 0x400000 (SHA-256 636576CA..., or PE time stamp
//                    0x414ECA49 of another file of that build). The earlier retail / "1.0 Fixed" builds (time stamp
//                    0x411BBEAF) are a different compile - every address here is mapped against the Sep 2004 one, so
//                    they are refused rather than silently mismatched.
//   PSEGame_Init   - registers the ID database (pse.ids, signatures checked against the ORIGINAL bytes) and
//                    pse.game.cv v1 (exe identification only - see pse_game_cv.h for why there are no core game
//                    events yet). Without a host plugin the loader's own PSE\Logs\loader.log takes every line.
//   PSEGame_Info   - game id "cv", the layer's plugin, the host plugins it allows ("cvfix" = VietnamFix, games/cv).
// Loader services through pse/hostlink (the layer gets pse.host like a host plugin).
#include <windows.h>

#include <cstring>

#include "generated/pse_ids_cv.h"   // pse/ids/generated (scripts/gen_ids.py)
#include "hostlink.h"
#include "ids.h"
#include "pse_game_cv.h"

namespace {
const char* const kHostPlugins[] = {"cvfix", nullptr};

constexpr uint32_t kBuildTimestamp = 0x414ECA49;   // Vietnam.exe, Sep 2004 (no SecuROM)
}  // namespace

extern "C" {
__declspec(dllexport) extern const PSEGameLayerInfo PSEGame_Info;
const PSEGameLayerInfo PSEGame_Info = {sizeof(PSEGameLayerInfo), 1, "cv", &pse_game_cv::kPluginInfo, kHostPlugins};

__declspec(dllexport) int PSE_CALL PSEGame_Detect(const PSEGameDetectInfo* d) {
    if (!d || d->size < sizeof(PSEGameDetectInfo) || !d->exeSha256 || d->imageBase != 0x400000) return 0;
    return _stricmp(d->exeSha256, pse_game_cv::KnownSha256()) == 0 || d->peTimestamp == kBuildTimestamp ? 1 : 0;
}

__declspec(dllexport) int PSE_CALL PSEGame_Init(const PSELoadInterface* pse) {
    if (!pse::hostlink::Attach(pse)) return 0;
    // ID database first: the loader calls this before any plugin, so the game's bytes are still the original ones.
    pse::ids::RegisterGameForProcess(pse_ids_cv::kGame, pse->plugin);
    if (!pse_game_cv::Register()) return 0;
    return 1;
}
}
