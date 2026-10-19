// loader.h for DLLs that are not the loader: forwarders through pse.host (hostlink.h). The feature sources of a host
// plugin call pse::FindPlugin, pse::HooksInterface(), pse::cosave::Save ... exactly as before the split.
#include "hostlink.h"

#include <windows.h>

#include <cstdio>

#include "internal.h"

namespace pse {
namespace {
const PSEHostInterface* g_host = nullptr;
PSEPluginHandle g_self = 0;

std::string Fetch(uint32_t (PSE_CALL* fn)(PSEPluginHandle, char*, uint32_t), PSEPluginHandle h) {
    if (!g_host || !fn) return {};
    char small[256];
    const uint32_t n = fn(h, small, sizeof small);
    if (n < sizeof small) return std::string(small, n);
    std::string big(n + 1, '\0');
    fn(h, big.data(), n + 1);
    big.resize(n);
    return big;
}
}  // namespace

namespace hostlink {
bool Attach(const PSELoadInterface* li) {
    if (!li || li->size < sizeof(PSELoadInterface) || !li->Query) return false;
    auto host = static_cast<const PSEHostInterface*>(li->Query(li->plugin, PSE_HOST_INTERFACE, PSE_HOST_INTERFACE_VERSION));
    if (!host || host->size < sizeof(PSEHostInterface) || host->version != PSE_HOST_INTERFACE_VERSION) return false;
    g_host = host;
    g_self = li->plugin;
    return true;
}
bool Attached() { return g_host != nullptr; }
PSEPluginHandle Self() { return g_self; }
const PSEHostInterface* Host() { return g_host; }
void Shutdown() {
    if (g_host) g_host->Shutdown();
}
}  // namespace hostlink

// ---- registry ----
PSEPluginHandle RegisterInternalPlugin(const PSEPluginInfo* info) {
    return g_host ? g_host->RegisterInternalPlugin(g_self, info) : 0;
}
const PSELoadInterface* LoadInterfaceFor(PSEPluginHandle plugin) {
    return g_host ? g_host->LoadInterfaceFor(plugin) : nullptr;
}
const char* PluginName(PSEPluginHandle plugin) { return g_host ? g_host->PluginName(plugin) : ""; }
PSEPluginHandle FindPlugin(const char* name) { return g_host ? g_host->FindPlugin(name) : 0; }
uint32_t PluginCount() { return g_host ? g_host->PluginCount() : 0; }
const PSEPluginInfo* PluginInfo(PSEPluginHandle plugin) { return g_host ? g_host->PluginInfo(plugin) : nullptr; }
bool PluginDisabled(PSEPluginHandle plugin) { return g_host && g_host->PluginDisabled(plugin); }
bool PluginExternal(PSEPluginHandle plugin) { return g_host && g_host->PluginExternal(plugin); }
bool PluginHost(PSEPluginHandle plugin) { return g_host && g_host->PluginHost(plugin); }
std::string PluginSettingsKey(PSEPluginHandle plugin) {
    return g_host ? Fetch(g_host->PluginSettingsKey, plugin) : std::string();
}

namespace detail {
std::vector<PSEPluginHandle> PluginsInOrder() {
    std::vector<PSEPluginHandle> out;
    if (!g_host) return out;
    for (;;) {
        const uint32_t n = g_host->PluginsInOrder(out.data(), static_cast<uint32_t>(out.size()));
        if (n <= out.size()) {
            out.resize(n);
            return out;
        }
        out.resize(n + 8);   // registrations in between: ask again
    }
}
}  // namespace detail

// ---- log ----
void LogLine(PSEPluginHandle plugin, PSELogLevel level, const char* text) {
    if (g_host) g_host->LogLine(plugin, level, text);
}

// ---- game-layer side ----
void Frame() {
    if (g_host) g_host->Frame();
}
void Broadcast(uint32_t type, const void* data, uint32_t len) {
    if (g_host) g_host->Broadcast(type, data, len);
}
int RaiseEvent(PSEEventId event, void* data) { return g_host ? g_host->RaiseEvent(event, data) : 0; }

// ---- interface tables ----
const PSELogInterface* LogInterface() { return g_host ? g_host->log : nullptr; }
const PSEHooksInterface* HooksInterface() { return g_host ? g_host->hooks : nullptr; }
const PSEMessagingInterface* MessagingInterface() { return g_host ? g_host->messaging : nullptr; }
const PSETasksInterface* TasksInterface() { return g_host ? g_host->tasks : nullptr; }
const PSESettingsInterface* SettingsInterface() { return g_host ? g_host->settings : nullptr; }
const PSESerializationInterface* SerializationInterface() { return g_host ? g_host->serialization : nullptr; }
const PSEHostInterface* HostInterface() { return g_host; }

// ---- co-save ----
namespace cosave {
bool Save(const char* coSavePath) { return g_host && g_host->CosaveSave(coSavePath); }
bool Load(const char* coSavePath) { return g_host && g_host->CosaveLoad(coSavePath); }
bool LoadBytes(const void* data, size_t len, const char* label) {
    return g_host && g_host->CosaveLoadBytes(data, static_cast<uint32_t>(len), label);
}
void NewGame() {
    if (g_host) g_host->CosaveNewGame();
}
}  // namespace cosave

// ---- plugin load results + pure helpers (plugins.cpp has them in the loader) ----
std::vector<PluginLoadResult> PluginResults() {
    std::vector<PluginLoadResult> out;
    if (!g_host) return out;
    g_host->PluginResults(
        [](const PSEHostPluginResult* c, void* ctx) {
            PluginLoadResult r;
            r.name = c->name ? c->name : "";
            r.displayName = c->displayName ? c->displayName : "";
            r.file = c->file ? c->file : "";
            r.version = c->version;
            r.flags = c->flags;
            r.state = static_cast<PluginState>(c->state);
            r.reason = c->reason ? c->reason : "";
            r.handle = c->handle;
            static_cast<std::vector<PluginLoadResult>*>(ctx)->push_back(std::move(r));
        },
        &out);
    return out;
}

std::string VersionString(uint32_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "%u.%u.%u", PSE_VERSION_MAJOR(v), PSE_VERSION_MINOR(v), PSE_VERSION_PATCH(v));
    return b;
}

const char* PluginStateName(PluginState state) {
    switch (state) {
        case PluginState::Loaded: return "loaded";
        case PluginState::Refused: return "refused";
        case PluginState::Failed: return "failed";
        case PluginState::Unloaded: return "unloaded";
    }
    return "?";
}

}  // namespace pse
