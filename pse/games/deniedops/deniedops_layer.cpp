// PSE\Games\pse_deniedops.dll: Conflict: Denied Ops game layer (docs/sdk/reference/game-deniedops.md, layout.md).
//   PSEGame_Detect - the exe is ConflictDeniedOps.exe, Steam no-CD, at 0x400000: the one this layer was mapped on
//                    (SHA-256 104AA5D5..., LAA bit cleared) or another file of that build (PE time stamp
//                    0x478652D0, Thu Jan 10 17:16:00 2008);
//   PSEGame_Init   - registers pse.game.deniedops v1. No core-event hooks yet (step 1 of the port: confirm the shim's
//                    entry-point redirect survives SteamStub before building anything on top - see below).
//   PSEGame_Info   - game id "deniedops", the layer's plugin, the host plugins it allows ("deniedopsfix").
// Loader services through pse/hostlink (the layer gets pse.host like a host plugin).
//
// SteamStub (docs/internal note, games/deniedops/CLAUDE.md "Environment"): the exe's on-disk entry point (RVA
// 0xB4D3DB, in an added .bind section) is the stub's unpacking code, not the game's. This does NOT break the PSE
// shim: DINPUT8.dll is a STATIC import of the exe, so Windows calls the shim's DllMain (DLL_PROCESS_ATTACH) before
// the exe's AddressOfEntryPoint ever runs - stub or not. The shim hooks whatever is at that entry point (the stub's
// first instruction), runs PSE, restores the original bytes, then lets the stub continue exactly as it would have.
// PSE is fully started (loader + this layer + plugins) *before* the stub decrypts the game's code: .text is
// ENCRYPTED in the file (2026-10-25), so at PSEGame_Init / PSEPlugin_Load every code address still holds encrypted
// bytes. Code patches must wait for the hand-over - DeniedOpsFix hooks the import slot MSVCR80!__set_app_type
// (0x90E2F4), the first call of the game's CRT start-up (games/deniedops/src/features/unpack.cpp). Import slots and
// .rdata / .data are plain at load time. Confirmed live: the shim's entry redirect does not crash or hang the stub.
#include <windows.h>

#include <cstring>

#include "hostlink.h"
#include "pse_game_deniedops.h"

namespace {
const char* const kHostPlugins[] = {"deniedopsfix", nullptr};
constexpr uint32_t kBuildTimestamp = 0x478652D0;   // ConflictDeniedOps.exe, Steam no-CD, Jan 2008
}  // namespace

extern "C" {
__declspec(dllexport) extern const PSEGameLayerInfo PSEGame_Info;
const PSEGameLayerInfo PSEGame_Info = {sizeof(PSEGameLayerInfo), 1, "deniedops", &pse_game_deniedops::kPluginInfo,
                                        kHostPlugins};

__declspec(dllexport) int PSE_CALL PSEGame_Detect(const PSEGameDetectInfo* d) {
    if (!d || d->size < sizeof(PSEGameDetectInfo) || !d->exeSha256 || d->imageBase != 0x400000) return 0;
    return _stricmp(d->exeSha256, pse_game_deniedops::KnownSha256()) == 0 || d->peTimestamp == kBuildTimestamp ? 1 : 0;
}

__declspec(dllexport) int PSE_CALL PSEGame_Init(const PSELoadInterface* pse) {
    if (!pse::hostlink::Attach(pse)) return 0;
    if (!pse_game_deniedops::Register()) return 0;
    return 1;
}
}
