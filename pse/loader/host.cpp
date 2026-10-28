// pse.host (version 0, PRIVATE - host_api.h, docs/sdk/reference/plugins.md "Host plugins"): the loader internals a
// trusted host plugin (DesertStormFix's dsfix.dll) and the game layer use through pse/hostlink. Only reachable through
// the PSELoadInterface.Query of a plugin the loader marked as host (registry.cpp Query).
#include <windows.h>

#include <cstring>

#include "internal.h"

namespace pse {
std::wstring g_pseDirForHost;   // set by Start (startup.cpp)
bool g_hostShutdownDone = false;   // reset by detail::ResetStartup

namespace {

uint32_t CopyOut(const std::string& s, char* buf, uint32_t bufLen) {
    if (buf && bufLen) {
        const size_t n = s.size() < bufLen - 1 ? s.size() : bufLen - 1;
        std::memcpy(buf, s.data(), n);
        buf[n] = 0;
    }
    return static_cast<uint32_t>(s.size());
}

void PSE_CALL SetLogSinkFn(PSEHostLogSink sink) { SetHostLogSink(sink); }
void PSE_CALL SetReportSinkFn(PSEHostReportSink sink) { SetHostReportSink(sink); }
void PSE_CALL LogLineFn(PSEPluginHandle plugin, PSELogLevel level, const char* text) { LogLine(plugin, level, text); }

PSEPluginHandle PSE_CALL RegisterInternalFn(PSEPluginHandle parent, const PSEPluginInfo* info) {
    return RegisterHostedPlugin(info, parent);
}
PSEPluginHandle PSE_CALL FindPluginFn(const char* name) { return FindPlugin(name); }
const char* PSE_CALL PluginNameFn(PSEPluginHandle plugin) { return PluginName(plugin); }
uint32_t PSE_CALL PluginCountFn() { return PluginCount(); }
const PSEPluginInfo* PSE_CALL PluginInfoFn(PSEPluginHandle plugin) { return PluginInfo(plugin); }
int PSE_CALL PluginExternalFn(PSEPluginHandle plugin) { return PluginExternal(plugin) ? 1 : 0; }
int PSE_CALL PluginDisabledFn(PSEPluginHandle plugin) { return PluginDisabled(plugin) ? 1 : 0; }
int PSE_CALL PluginHostFn(PSEPluginHandle plugin) { return PluginHost(plugin) ? 1 : 0; }
uint32_t PSE_CALL PluginsInOrderFn(PSEPluginHandle* out, uint32_t max) {
    const std::vector<PSEPluginHandle> list = detail::PluginsInOrder();
    for (uint32_t i = 0; out && i < max && i < list.size(); ++i) out[i] = list[i];
    return static_cast<uint32_t>(list.size());
}
const PSELoadInterface* PSE_CALL LoadInterfaceForFn(PSEPluginHandle plugin) { return LoadInterfaceFor(plugin); }
uint32_t PSE_CALL PluginSettingsKeyFn(PSEPluginHandle plugin, char* buf, uint32_t bufLen) {
    return CopyOut(PluginSettingsKey(plugin), buf, bufLen);
}
uint32_t PSE_CALL PluginResultsFn(PSEHostResultFn fn, void* ctx) {
    const std::vector<PluginLoadResult> results = PluginResults();   // a copy: fn may log / look things up
    for (const PluginLoadResult& r : results) {
        if (!fn) break;
        PSEHostPluginResult c{};
        c.size = sizeof c;
        c.name = r.name.c_str();
        c.displayName = r.displayName.c_str();
        c.file = r.file.c_str();
        c.version = r.version;
        c.flags = r.flags;
        c.state = static_cast<uint32_t>(r.state);
        c.reason = r.reason.c_str();
        c.handle = r.handle;
        fn(&c, ctx);
    }
    return static_cast<uint32_t>(results.size());
}

void PSE_CALL FrameFn() { Frame(); }
void PSE_CALL BroadcastFn(uint32_t type, const void* data, uint32_t len) { Broadcast(type, data, len); }
int PSE_CALL RaiseEventFn(PSEEventId event, void* data) { return RaiseEvent(event, data); }
int PSE_CALL CosaveSaveFn(const char* path) { return cosave::Save(path) ? 1 : 0; }
int PSE_CALL CosaveLoadFn(const char* path) { return cosave::Load(path) ? 1 : 0; }
int PSE_CALL CosaveLoadBytesFn(const void* data, uint32_t len, const char* label) {
    return cosave::LoadBytes(data, len, label) ? 1 : 0;
}
void PSE_CALL CosaveNewGameFn() { cosave::NewGame(); }
void PSE_CALL ShutdownFn() {
    // WM_DESTROY can come more than once (several windows / a second close).
    if (g_hostShutdownDone || !detail::Initialized()) return;
    g_hostShutdownDone = true;
    Broadcast(PSE_MSG_EXIT, nullptr, 0);
    UnloadPlugins();
}

}  // namespace

const PSEHostInterface* HostInterface() {
    static PSEHostInterface table = {};
    table.size = sizeof table;
    table.version = PSE_HOST_INTERFACE_VERSION;
    table.log = LogInterface();
    table.hooks = HooksInterface();
    table.messaging = MessagingInterface();
    table.tasks = TasksInterface();
    table.settings = SettingsInterface();
    table.serialization = SerializationInterface();
    table.SetLogSink = &SetLogSinkFn;
    table.SetReportSink = &SetReportSinkFn;
    table.LogLine = &LogLineFn;
    table.RegisterInternalPlugin = &RegisterInternalFn;
    table.FindPlugin = &FindPluginFn;
    table.PluginName = &PluginNameFn;
    table.PluginCount = &PluginCountFn;
    table.PluginInfo = &PluginInfoFn;
    table.PluginExternal = &PluginExternalFn;
    table.PluginDisabled = &PluginDisabledFn;
    table.PluginHost = &PluginHostFn;
    table.PluginsInOrder = &PluginsInOrderFn;
    table.LoadInterfaceFor = &LoadInterfaceForFn;
    table.PluginSettingsKey = &PluginSettingsKeyFn;
    table.PluginResults = &PluginResultsFn;
    table.Frame = &FrameFn;
    table.Broadcast = &BroadcastFn;
    table.RaiseEvent = &RaiseEventFn;
    table.CosaveSave = &CosaveSaveFn;
    table.CosaveLoad = &CosaveLoadFn;
    table.CosaveLoadBytes = &CosaveLoadBytesFn;
    table.CosaveNewGame = &CosaveNewGameFn;
    table.Shutdown = &ShutdownFn;
    table.pseDir = g_pseDirForHost.c_str();
    return &table;
}

}  // namespace pse
