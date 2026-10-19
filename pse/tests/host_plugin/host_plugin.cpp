// A HOST plugin for pse_tests (test_layout.cpp): links pse_hostlink like DesertStormFix's dsfix.dll and checks the
// forwarders from inside a real plugin DLL. Built twice: "test.host" (pse_plugins/) and a stand-in named "dsfix"
// (pse_test_plugins/, for the start-up test - the DS1 game layer allows only that name as a host).
//   Load: attaches pse.host, registers the sub-plugin "<name>.sub", sets a log sink (keeps the lines), publishes
//   "test.host.probe" v1. Environment TEST_HOST_FAIL=1: returns 0 after registering the sub-plugin.
#include <windows.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "hostlink.h"
#include "internal.h"   // pse::detail::PluginsInOrder

#ifndef TEST_HOST_NAME
#define TEST_HOST_NAME "test.host"
#endif

namespace {
std::mutex g_mx;
std::vector<std::string> g_lines;
PSEPluginHandle g_sub = 0;
std::string g_hash;
std::string g_key;

void PSE_CALL Sink(const char* line) {
    std::lock_guard lock(g_mx);
    g_lines.emplace_back(line);
}

const PSEPluginInfo kSubInfo = {sizeof(PSEPluginInfo), 1, TEST_HOST_NAME ".sub", "Test host sub-plugin", "PSE tests",
                                PSE_MAKE_VERSION(1, 0, 0), PSE_API_VERSION, nullptr, PSE_PLUGIN_SIMULATION};

// ---- probe interface (read by test_layout.cpp) ----
uint32_t PSE_CALL SubHandle() { return g_sub; }
int PSE_CALL SelfIsHost() { return pse::PluginHost(pse::hostlink::Self()) ? 1 : 0; }
int PSE_CALL SinkHas(const char* text) {
    std::lock_guard lock(g_mx);
    for (const std::string& l : g_lines)
        if (l.find(text) != std::string::npos) return 1;
    return 0;
}
// compat::LocalPlugins (compiled into pse_hostlink, fed by the forwarders): the hash entry of `name`.
const char* PSE_CALL LocalHash(const char* name) {
    g_hash = "(missing)";
    for (const pse::compat::PluginEntry& e : pse::compat::LocalPlugins())
        if (e.name == name) g_hash = e.hash;
    return g_hash.c_str();
}
uint32_t PSE_CALL ResultCount() { return static_cast<uint32_t>(pse::PluginResults().size()); }
const char* PSE_CALL SettingsKey() {
    g_key = pse::PluginSettingsKey(pse::hostlink::Self());
    return g_key.c_str();
}
uint32_t PSE_CALL InOrderCount() { return static_cast<uint32_t>(pse::detail::PluginsInOrder().size()); }

struct Probe {
    uint32_t size, version;
    uint32_t(PSE_CALL* SubHandle)();
    int(PSE_CALL* SelfIsHost)();
    int(PSE_CALL* SinkHas)(const char*);
    const char*(PSE_CALL* LocalHash)(const char*);
    uint32_t(PSE_CALL* ResultCount)();
    const char*(PSE_CALL* SettingsKey)();
    uint32_t(PSE_CALL* InOrderCount)();
};
const Probe kProbe = {sizeof(Probe), 1, &SubHandle, &SelfIsHost, &SinkHas, &LocalHash, &ResultCount, &SettingsKey,
                      &InOrderCount};
}  // namespace

extern "C" {
__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info;
const PSEPluginInfo PSEPlugin_Info = {sizeof(PSEPluginInfo), 1, TEST_HOST_NAME, "Test host plugin", "PSE tests",
                                      PSE_MAKE_VERSION(1, 0, 0), PSE_API_VERSION, nullptr, PSE_PLUGIN_SIMULATION};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    if (!pse::hostlink::Attach(pse)) {
        auto log = static_cast<const PSELogInterface*>(pse->Query(pse->plugin, PSE_LOG_INTERFACE, 1));
        if (log) log->Write(pse->plugin, PSE_LOG_ERROR, "no pse.host for " TEST_HOST_NAME);
        return 0;
    }
    pse::hostlink::Host()->SetLogSink(&Sink);
    g_sub = pse::RegisterInternalPlugin(&kSubInfo);
    if (!g_sub) return 0;
    if (GetEnvironmentVariableA("TEST_HOST_FAIL", nullptr, 0)) return 0;
    pse::MessagingInterface()->RegisterInterface(pse->plugin, "test.host.probe", 1, &kProbe);
    return 1;
}

__declspec(dllexport) void PSE_CALL PSEPlugin_Unload(void) {
    if (pse::hostlink::Attached()) pse::hostlink::Host()->SetLogSink(nullptr);   // its sink is about to go away
}
}
