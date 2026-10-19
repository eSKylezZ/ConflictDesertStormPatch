// PSE\Games\pse_ds1.dll: the Conflict: Desert Storm game layer (docs/sdk/layout.md, DESIGN.md 3).
//   PSEGame_Detect - the exe is DesertStorm.exe SHA-256 D712F5EC... (LAA bit cleared, Steam = GOG);
//   PSEGame_Init   - registers the ID database (pse.ids, docs/sdk/reference/ids.md: signatures checked against the
//                    ORIGINAL bytes - before any plugin patches), pse.game.ds1 v1 (public) and pse.game.ds1.host v0
//                    (private, for DesertStormFix), then installs the hooks that drive the core game events and the
//                    co-save (ds1_events.cpp: frame, render, level, save / load, D3D - owner pse.game.ds1);
//   PSEGame_Info   - game id "ds1", the layer's plugin, the host plugins it allows ("dsfix").
// Loader services through pse/hostlink (the layer gets pse.host like a host plugin). Its only patches are those event
// hooks (broker records, docs/sdk/reference/game-ds1.md "Events and messages").
#include <windows.h>

#include <cstring>

#include "ds1_events.h"
#include "ds1_saves.h"
#include "generated/pse_ids_ds1.h"   // src/generated (scripts/gen_ids.py)
#include "hostlink.h"
#include "ids.h"
#include "pse_game_ds1.h"
#include "pse_game_ds1_host.h"

namespace {

uint32_t CopyOut(const std::string& s, char* buf, uint32_t bufLen) {
    if (buf && bufLen) {
        const size_t n = s.size() < bufLen - 1 ? s.size() : bufLen - 1;
        std::memcpy(buf, s.data(), n);
        buf[n] = 0;
    }
    return static_cast<uint32_t>(s.size());
}

void PSE_CALL SetProviders(int (*splitScreenPlayers)(), void (*online)(PSEDs1Online* out)) {
    pse_game_ds1::Providers p;
    p.splitScreenPlayers = splitScreenPlayers;
    p.online = online;
    pse_game_ds1::SetProviders(p);
}
uint32_t PSE_CALL LevelTitleText(const char* level, char* buf, uint32_t bufLen) {
    return CopyOut(pse_game_ds1::LevelTitleText(level), buf, bufLen);
}
uint32_t PSE_CALL LevelDisplayName(const char* level, char* buf, uint32_t bufLen) {
    return CopyOut(pse_game_ds1::LevelDisplayName(level), buf, bufLen);
}
void PSE_CALL SetLegacyCosave(PSEDs1LegacyCosaveFn fn) { ds1_saves::SetLegacy(fn); }
const PSEGameDs1HostV0 kHostTable = {sizeof(PSEGameDs1HostV0), 0, &SetProviders, &LevelTitleText, &LevelDisplayName,
                                     &SetLegacyCosave};

const char* const kHostPlugins[] = {"dsfix", nullptr};

}  // namespace

extern "C" {
__declspec(dllexport) extern const PSEGameLayerInfo PSEGame_Info;
const PSEGameLayerInfo PSEGame_Info = {sizeof(PSEGameLayerInfo), 1, "ds1", &pse_game_ds1::kPluginInfo,
                                       kHostPlugins};

__declspec(dllexport) int PSE_CALL PSEGame_Detect(const PSEGameDetectInfo* d) {
    if (!d || d->size < sizeof(PSEGameDetectInfo) || !d->exeSha256) return 0;
    // Only the hash decides: DesertStormFix's patches are made for exactly this exe. (Image base 0x400000 and the
    // PE time stamp come with it; other DS1 builds - retail CD, no-CD, other regions - need the ID database first.)
    return _stricmp(d->exeSha256, pse_game_ds1::KnownSha256()) == 0 && d->imageBase == 0x400000 ? 1 : 0;
}

__declspec(dllexport) int PSE_CALL PSEGame_Init(const PSELoadInterface* pse) {
    if (!pse::hostlink::Attach(pse)) return 0;
    // ID database first: the loader calls this before any plugin, so the game's bytes are still the original ones.
    pse::ids::RegisterGameForProcess(pse_ids_ds1::kGame, pse->plugin);
    if (!pse_game_ds1::Register()) return 0;   // finds the plugin the loader registered, publishes the interface
    const PSEMessagingInterface* m = pse::MessagingInterface();
    if (m->RegisterInterface(pse->plugin, PSE_GAME_DS1_HOST_INTERFACE, 0, &kHostTable) != PSE_OK) return 0;
    // The core game events (frames, render, level, D3D) and the co-save. A hook the broker refuses only costs its
    // event (logged): the layer stays up.
    ds1_events::Install(pse->plugin);
    return 1;
}
}
