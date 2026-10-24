// PSE\Games\pse_ds2.dll: the Conflict: Desert Storm II game layer (docs/sdk/reference/game-ds2.md, layout.md).
//   PSEGame_Detect - the exe is CDS II.exe 1.0 at 0x400000: the disc-free one (SHA-256 E9B46420..., LAA bit cleared) or
//                    another file of that build (PE time stamp 0x3F4B1A8A: no-CD exes). The retail SecuROM exe
//                    (44EF1E16..., or SecuROM's entry point) is refused: its imports go through SecuROM's own thunks,
//                    so the import / call-site hooks of this layer don't fit it;
//   PSEGame_Init   - registers the ID database (pse.ids, signatures checked against the ORIGINAL bytes - before any
//                    plugin patches), pse.game.ds2 v1, then installs the hooks that drive the core game events and the
//                    co-save (ds2_events.cpp: frame, render, level, save / load, D3D9, exit - owner pse.game.ds2).
//                    Without a host plugin the loader's own PSE\Logs\loader.log takes every line;
//   PSEGame_Info   - game id "ds2", the layer's plugin, the host plugins it allows ("ds2fix", the planned DesertStorm2Fix
//                    host plugin - docs/sdk/reference/game-ds2.md "DesertStorm2Fix as a host plugin").
// Loader services through pse/hostlink (the layer gets pse.host like a host plugin).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "ds2_events.h"
#include "generated/pse_ids_ds2.h"   // pse/ids/generated (scripts/gen_ids.py)
#include "hostlink.h"
#include "ids.h"
#include "pse_game_ds2.h"

namespace {

const char* const kHostPlugins[] = {"ds2fix", nullptr};

constexpr uint32_t kBuildTimestamp = 0x3F4B1A8A;   // CDS II.exe 1.0 (retail and disc-free share it)
constexpr uint32_t kSecuromEntry = 0x42CC17;       // the retail exe's entry point: SecuROM's loader

// The mapped exe's entry point RVA, 0 when its headers can't be read (Detect runs in the game: the exe at imageBase).
uint32_t EntryPointRva(uint32_t imageBase) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(imageBase), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x800) return 0;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(imageBase + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt->OptionalHeader.AddressOfEntryPoint : 0;
}

}  // namespace

extern "C" {
__declspec(dllexport) extern const PSEGameLayerInfo PSEGame_Info;
const PSEGameLayerInfo PSEGame_Info = {sizeof(PSEGameLayerInfo), 1, "ds2", &pse_game_ds2::kPluginInfo, kHostPlugins};

__declspec(dllexport) int PSE_CALL PSEGame_Detect(const PSEGameDetectInfo* d) {
    if (!d || d->size < sizeof(PSEGameDetectInfo) || !d->exeSha256 || d->imageBase != 0x400000) return 0;
    // The retail SecuROM exe - its file, or a changed copy still starting in SecuROM's loader - calls its imports through
    // SecuROM's own thunks and won't start without the disc: refused.
    if (_stricmp(d->exeSha256, pse_game_ds2::SecuromSha256()) == 0 || EntryPointRva(d->imageBase) == kSecuromEntry) {
        // Detect has no log yet: the loader logs "doesn't recognise this exe"; a debugger sees why.
        OutputDebugStringA("[pse_ds2] CDS II - Original.exe (SecuROM) is not supported - use the disc-free CDS II.exe\n");
        return 0;
    }
    // The disc-free exe, or another file of the same 1.0 build (same PE time stamp at the same base: no-CD exes, a
    // changed header - 2026-10-22: a player's CDS II.exe was refused that the single-DLL DesertStorm2Fix had run).
    // Every hook checks the bytes it replaces and pse.ids its signatures, so a part that differs is skipped.
    return _stricmp(d->exeSha256, pse_game_ds2::KnownSha256()) == 0 || d->peTimestamp == kBuildTimestamp ? 1 : 0;
}

__declspec(dllexport) int PSE_CALL PSEGame_Init(const PSELoadInterface* pse) {
    if (!pse::hostlink::Attach(pse)) return 0;
    // ID database first: the loader calls this before any plugin, so the game's bytes are still the original ones.
    pse::ids::RegisterGameForProcess(pse_ids_ds2::kGame, pse->plugin);
    if (!pse_game_ds2::Register()) return 0;
    ds2_events::Install(pse->plugin);
    return 1;
}
}
