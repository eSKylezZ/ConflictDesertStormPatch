// PSE\Games\pse_cgs.dll: Conflict: Global Storm / Global Terror game layer (docs/sdk/reference/game-cgs.md).
//   PSEGame_Detect - the exe is the RELOADED NoCD dump of ConflictGlobal.exe (PE time stamp 0x43032523, image base
//                    0x400000; the retail SecuROM 7 original never runs on current Windows, so it is never matched).
//   PSEGame_Init   - registers pse.ids (database currently empty/unpublished, see pse/ids/cgs.ids.txt), pse.game.cgs
//                    v1. No core-event hooks yet: this exe runs almost everything (window creation, the launcher
//                    dialogs, the resolution filter, the keyboard poll, command line parsing - see games/cgs/CLAUDE.md
//                    "Environment") inside a SecuROM VM with no static call sites, so there is no single frame /
//                    render / level call site the layer can safely hook game-neutrally the way the DS1/DS2/TGE layers
//                    do. cgsfix (games/cgs/src/pse/cgsfix.cpp) hooks the same import slots the standalone
//                    GlobalStormFix build already uses (devbg.cpp's list) directly, as its own patches, instead of
//                    going through layer-owned events - that's why this layer is detection + identity only for now.
#include <windows.h>

#include <cstring>

#include "generated/pse_ids_cgs.h"   // pse/ids/generated (scripts/gen_ids.py)
#include "hostlink.h"
#include "ids.h"
#include "pse_game_cgs.h"

namespace {
const char* const kHostPlugins[] = {"cgsfix", nullptr};
constexpr uint32_t kBuildTimestamp = 0x43032523;   // ConflictGlobal.exe RELOADED NoCD
}  // namespace

extern "C" {
__declspec(dllexport) extern const PSEGameLayerInfo PSEGame_Info;
const PSEGameLayerInfo PSEGame_Info = {sizeof(PSEGameLayerInfo), 1, "cgs", &pse_game_cgs::kPluginInfo, kHostPlugins};

__declspec(dllexport) int PSE_CALL PSEGame_Detect(const PSEGameDetectInfo* d) {
    if (!d || d->size < sizeof(PSEGameDetectInfo) || d->imageBase != 0x400000) return 0;
    if (d->exeSha256 && _stricmp(d->exeSha256, pse_game_cgs::KnownSha256()) == 0) return 1;
    return d->peTimestamp == kBuildTimestamp ? 1 : 0;
}

__declspec(dllexport) int PSE_CALL PSEGame_Init(const PSELoadInterface* pse) {
    if (!pse::hostlink::Attach(pse)) return 0;
    pse::ids::RegisterGameForProcess(pse_ids_cgs::kGame, pse->plugin);
    if (!pse_game_cgs::Register()) return 0;
    return 1;
}
}
