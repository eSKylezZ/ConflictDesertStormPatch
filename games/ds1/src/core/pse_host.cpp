#include "core/pse_host.h"

#include <windows.h>

#include <deque>
#include <string>

#include "core/log.h"
#include "core/net.h"
#include "core/patch.h"
#include "core/pse_saves.h"
#include "features/features.h"
#include "hostlink.h"
#include "pse_game_ds1_host.h"

namespace {
PSEPluginHandle g_core = 0;
const PSEGameDs1HostV0* g_ds1 = nullptr;   // the game layer's private side (PSE\Games\pse_ds1.dll)

void PSE_CALL LogSink(const char* line) { dslog::Write("%s", line); }
void PSE_CALL ReportSink(const char* line, int) { features::ModLog("%s", line); }

// pse.game.ds1's view of DesertStormFix (pse/games/ds1/pse_game_ds1.h Providers): read-only.
void OnlineInfo(PSEDs1Online* out) {
    const int local = net::LocalPlayer();
    if (!net::Active() || local <= 0) return;
    const uint32_t peers = static_cast<uint32_t>(features::OnlineConnectedPeers());   // dsfix.online
    out->active = 1;
    out->role = local == 1 ? PSE_DS1_ONLINE_HOST : PSE_DS1_ONLINE_JOINER;
    out->localPlayer = static_cast<uint32_t>(local);
    out->connectedPeers = peers;
    const int session = features::CoopPlayers();   // the running co-op session (set when the host pressed START)
    const uint32_t counted = local == 1 ? 1 + peers : (peers ? 2 : 1);
    out->players = session > 0 && static_cast<uint32_t>(session) > counted ? static_cast<uint32_t>(session) : counted;
    out->inMission = features::LockstepActive() ? 1 : 0;
}

std::string FromLayer(uint32_t (PSE_CALL* fn)(const char*, char*, uint32_t), const char* level) {
    if (!fn) return {};
    char small[256];
    const uint32_t n = fn(level, small, sizeof small);
    if (n < sizeof small) return std::string(small, n);
    std::string big(n + 1, '\0');
    fn(level, big.data(), n + 1);
    big.resize(n);
    return big;
}
}  // namespace

bool pse_host::Init(const PSELoadInterface* pse) {
    // dsfix.dll is a HOST plugin (docs/sdk/reference/plugins.md "Host plugins"): the loader in pse_loader.dll hands it
    // the private pse.host table, and pse/hostlink forwards every pse:: call of the features through it.
    if (!pse::hostlink::Attach(pse)) return false;
    g_core = pse->plugin;
    const PSEHostInterface* host = pse::hostlink::Host();
    host->SetLogSink(&LogSink);          // loader + plugin lines (also those from before this) -> DesertStormFix.log
    host->SetReportSink(&ReportSink);    // -> the mod report (Mods\DesertStormFix-report.txt)
    // pse.game.ds1 (served by the game layer, docs/sdk/reference/game-ds1.md) reads split screen / the online
    // session from DesertStormFix through these providers.
    g_ds1 = static_cast<const PSEGameDs1HostV0*>(
        pse::MessagingInterface()->QueryInterface(g_core, PSE_GAME_DS1_HOST_INTERFACE, 0));
    if (g_ds1 && g_ds1->size >= sizeof(PSEGameDs1HostV0)) {
        g_ds1->SetProviders(&features::SplitScreenPlayers, &OnlineInfo);
        // The layer owns the co-save (pse/games/ds1/ds1_saves.cpp); a save made before it has only DesertStormFix's
        // old .sav.dsfix sidecar, which only DesertStormFix can read.
        g_ds1->SetLegacyCosave(&pse_saves::LegacyCosave);
    } else {
        g_ds1 = nullptr;
        dslog::Write("[fail] PSE: the game layer's pse.game.ds1.host interface is missing (old pse_ds1.dll?)");
    }
    return true;
}

// Level titles for the briefing / photo mode screens: the same code as pse.game.ds1's LevelTitle (game layer).
std::string features::LevelTitleText(const char* level) {
    return FromLayer(g_ds1 ? g_ds1->LevelTitleText : nullptr, level);
}
std::string features::LevelDisplayName(const char* level) {
    return FromLayer(g_ds1 ? g_ds1->LevelDisplayName : nullptr, level);
}

PSEPluginHandle pse_host::Core() { return g_core; }

PSEPluginHandle pse_host::Register(const PSEPluginInfo* info) {
    return g_core ? pse::RegisterInternalPlugin(info) : 0;   // a sub-plugin of dsfix (pse.host, parent = dsfix)
}

PSEPluginHandle pse_host::Find(const char* name) { return g_core && name ? pse::FindPlugin(name) : 0; }

void pse_host::RegisterFeatures(const Feature* table, size_t count) {
    static std::deque<PSEPluginInfo> infos;  // must outlive the process
    for (size_t i = 0; i < count; ++i) {
        const Feature& f = table[i];
        if (Find(f.name)) continue;
        infos.push_back({sizeof(PSEPluginInfo), 1, f.name, f.display, "DesertStormFix", kVersion, PSE_API_VERSION,
                         nullptr, f.flags});
        if (!Register(&infos.back())) dslog::Write("[fail] PSE: internal plugin %s not registered", f.name);
    }
}

void pse_host::RunAs(const char* plugin, void (*fn)()) {
    if (!fn) return;
    patch::OwnerScope scope(Find(plugin));
    fn();
}

void pse_host::Shutdown() {
    if (g_core) pse::hostlink::Shutdown();   // the loader: PSE_MSG_EXIT, then the other plugins unloaded (once)
}
