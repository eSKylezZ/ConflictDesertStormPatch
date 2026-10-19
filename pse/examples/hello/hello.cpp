// example.hello - the smallest real PSE plugin, built only against the SDK header (docs/sdk/guides/first-plugin.md).
//
// It shows the four things almost every plugin does:
//   1. exports PSEPlugin_Info (read by the loader before any code runs) and PSEPlugin_Load / PSEPlugin_Unload;
//   2. writes to its log (pse.log -> PSE\Logs\example.hello.log and the loader log);
//   3. declares a setting (pse.settings: registry-backed, shown in the game's menus) and reads it every frame;
//   4. subscribes to an event (PSE_EVENT_FRAME) and publishes an interface for other plugins ("example.hello" v1).
#include <pse/pse_api.h>

#include "example_hello.h"

namespace {

PSEPluginHandle g_self = 0;
const PSELogInterface* g_log = nullptr;
const PSEMessagingInterface* g_messaging = nullptr;
const PSESettingsInterface* g_settings = nullptr;
uint32_t g_frames = 0;

int PSE_CALL Add(int a, int b) { return a + b; }

const ExampleHelloV1 kInterface = {sizeof(ExampleHelloV1), &Add};

int PSE_CALL OnFrame(PSEEventId, void*, void*) {
    ++g_frames;
    int32_t every = g_settings->GetInt(g_self, "LogEveryFrames");
    if (every > 0 && g_frames % static_cast<uint32_t>(every) == 0)
        g_log->Write(g_self, PSE_LOG_INFO, "hello: frame %u", g_frames);
    return 0;   // let later subscribers run
}

const char* const kGames[] = {"ds1", nullptr};

}  // namespace

extern "C" {

__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info = {
    sizeof(PSEPluginInfo),
    1,                              // infoVersion
    "example.hello",                // must match the manifest's name
    "Hello (example plugin)",
    "PSE",
    PSE_MAKE_VERSION(1, 0, 0),
    0,                              // minApiVersion: works with API 0 and later
    kGames,
    PSE_PLUGIN_CLIENT_ONLY,         // changes nothing in the simulation: online peers may lack it
};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    g_self = pse->plugin;
    g_log = static_cast<const PSELogInterface*>(pse->Query(g_self, PSE_LOG_INTERFACE, 1));
    g_messaging = static_cast<const PSEMessagingInterface*>(pse->Query(g_self, PSE_MESSAGING_INTERFACE, 1));
    g_settings = static_cast<const PSESettingsInterface*>(pse->Query(g_self, PSE_SETTINGS_INTERFACE, 1));
    if (!g_log) return 0;   // nothing to report with: refuse quietly
    if (!g_messaging || !g_settings) {
        g_log->Write(g_self, PSE_LOG_ERROR, "hello: the loader is missing pse.messaging or pse.settings v1");
        return 0;
    }

    PSESettingDesc every = {};
    every.size = sizeof every;
    every.key = "LogEveryFrames";
    every.type = PSE_SETTING_INT;
    every.flags = PSE_SETTING_LIVE;
    every.defaultInt = 600;
    every.minInt = 1;
    every.maxInt = 100000;
    every.step = 60;
    every.label = "HELLO: LOG EVERY N FRAMES";
    every.help = "How often the example plugin writes a line to its log";
    if (g_settings->Declare(g_self, &every) != PSE_OK) return 0;

    if (!g_messaging->Subscribe(g_self, PSE_EVENT_FRAME, &OnFrame, nullptr, 0)) return 0;
    if (g_messaging->RegisterInterface(g_self, EXAMPLE_HELLO_INTERFACE, 1, &kInterface) != PSE_OK) return 0;

    g_log->Write(g_self, PSE_LOG_INFO, "hello: loaded (game %s, API %u), logging every %d frames", pse->gameId,
                 pse->apiVersion, g_settings->GetInt(g_self, "LogEveryFrames"));
    return 1;
}

__declspec(dllexport) void PSE_CALL PSEPlugin_Unload(void) {
    if (g_log) g_log->Write(g_self, PSE_LOG_INFO, "hello: unloading after %u frames", g_frames);
}

}  // extern "C"
