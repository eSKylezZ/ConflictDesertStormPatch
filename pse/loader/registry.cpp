// Plugin registry, start-up / reset, PSELoadInterface + Query, small shared helpers.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#include "internal.h"
#include "lua_iface.h"

namespace pse {
namespace {

struct Plugin {
    PSEPluginInfo info;          // copy of the caller's struct; internal: PSE_PLUGIN_INTERNAL set, strings stay theirs
    std::string name;
    PSELoadInterface load;
    // External plugins (plugins.cpp): the strings are copied - the DLL may be unloaded while its record stays.
    bool external = false;
    bool disabled = false;       // failed / unloaded: handle kept (never reused), invisible to FindPlugin
    // Phase 2 layout (docs/sdk/layout.md): a host plugin / game layer gets "pse.host" through its Query; a hosted
    // (sub-)plugin was registered by its parent through pse.host - internal semantics, strings copied (the parent's DLL
    // can be kept from running), retired with the parent.
    bool host = false;
    PSEPluginHandle parent = 0;
    std::vector<std::string> adopts;     // manifest "adopts": co-save chunks of these (no longer loaded) plugin names
    std::string displayName, author;
    std::vector<std::string> games;
    std::vector<const char*> gamePtrs;   // NULL-terminated, points into games
};

std::mutex g_mutex;              // guards g_plugins (PluginName is called from any thread through pse.log)
std::deque<Plugin> g_plugins;    // handle = index + 1; deque keeps element addresses stable on push_back
Config g_config;
bool g_initialized = false;
PSEPluginHandle g_loader = 0;

bool ValidName(const char* name) {
    size_t n = std::strlen(name);
    if (n == 0 || n > 64) return false;   // the co-save stores the name with a u8 length
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

const void* PSE_CALL Query(PSEPluginHandle plugin, const char* id, uint32_t minVersion) {
    if (!detail::ValidPlugin(plugin)) return nullptr;
    // The private host table: only through the Query of a host plugin / game layer, never through the messaging
    // interface registry (host.cpp, docs/sdk/reference/plugins.md "Host plugins").
    if (id && std::strcmp(id, PSE_HOST_INTERFACE) == 0)
        return PluginHost(plugin) && minVersion <= PSE_HOST_INTERFACE_VERSION ? HostInterface() : nullptr;
    // pse.lua's private binding table (lua_iface.h / lua_bridge.h): only for whoever owns the Lua state - host plugins,
    // game layers, built-in plugins - never for ordinary external plugins.
    if (id && std::strcmp(id, PSE_LUA_HOST_INTERFACE) == 0)
        return (PluginHost(plugin) || !PluginExternal(plugin)) && minVersion <= 1 ? lua::HostTable() : nullptr;
    return detail::QueryAny(id, minVersion);
}

const char* const kLoaderGames[] = {nullptr};
const PSEPluginInfo kLoaderInfo = {sizeof(PSEPluginInfo), 1, "pse", "Pivotal Script Extender", "PSE",
                                   PSE_MAKE_VERSION(0, 1, 0), PSE_API_VERSION, kLoaderGames, PSE_PLUGIN_CLIENT_ONLY};

}  // namespace

namespace detail {

const Config& Cfg() { return g_config; }
bool Initialized() { return g_initialized; }
PSEPluginHandle LoaderHandle() { return g_loader; }

std::vector<PSEPluginHandle> PluginsInOrder() {
    std::lock_guard lock(g_mutex);
    std::vector<PSEPluginHandle> out;
    for (size_t i = 0; i < g_plugins.size(); ++i)
        if (!g_plugins[i].disabled) out.push_back(static_cast<PSEPluginHandle>(i + 1));
    return out;
}

bool ValidPlugin(PSEPluginHandle plugin) {
    std::lock_guard lock(g_mutex);
    return plugin != 0 && plugin <= g_plugins.size() && !g_plugins[plugin - 1].disabled;
}

const void* QueryAny(const char* id, uint32_t minVersion) {
    if (!id) return nullptr;
    struct Builtin {
        const char* id;
        const void* (*table)();
    };
    static const Builtin kBuiltins[] = {
        {PSE_LOG_INTERFACE, [] { return static_cast<const void*>(LogInterface()); }},
        {PSE_HOOKS_INTERFACE, [] { return static_cast<const void*>(HooksInterface()); }},
        {PSE_MESSAGING_INTERFACE, [] { return static_cast<const void*>(MessagingInterface()); }},
        {PSE_TASKS_INTERFACE, [] { return static_cast<const void*>(TasksInterface()); }},
        {PSE_SETTINGS_INTERFACE, [] { return static_cast<const void*>(SettingsInterface()); }},
        {PSE_SERIALIZATION_INTERFACE, [] { return static_cast<const void*>(SerializationInterface()); }},
        {PSE_LUA_INTERFACE, [] { return static_cast<const void*>(lua::Interface()); }},   // lua_iface.cpp
        {PSE_VFS_INTERFACE, [] { return static_cast<const void*>(VfsInterface()); }},    // vfs.cpp
    };
    for (const Builtin& b : kBuiltins) {
        if (std::strcmp(b.id, id) != 0) continue;
        const void* t = b.table();
        // Every loader table starts with {u32 size, u32 version}.
        return static_cast<const uint32_t*>(t)[1] >= minVersion ? t : nullptr;
    }
    return QueryRegisteredInterface(id, minVersion);
}

}  // namespace detail

bool Init(const Config& config) {
    if (g_initialized) return true;
    bool valid = config.gameId && *config.gameId && std::strlen(config.gameId) <= 16;
    for (const char* p = config.gameId; valid && *p; ++p)
        valid = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9');
    if (!valid) {   // no default game: settings and plugins' game lists would quietly be another game's
        if (config.logSink) config.logSink("[pse] ERROR Init refused: no valid game id (Config::gameId)");
        return false;
    }
    g_config = config;
    if (g_config.registryRoot.empty()) g_config.registryRoot = std::string("Software\\PSE\\") + g_config.gameId;
    g_initialized = true;
    detail::InitLog();
    detail::InitTasks();
    hooks::SetEnvironment(&PluginName, &LogLine);
    g_loader = RegisterInternalPlugin(&kLoaderInfo);
    return true;
}

void ResetForTests() {
    detail::ResetPlugins();
    detail::ResetStartup();
    hooks::ResetForTests();
    detail::ResetCosave();
    detail::ResetLua();
    detail::ResetVfs();
    detail::ResetSettings();
    detail::ResetTasks();
    detail::ResetMessaging();
    detail::ResetLog();
    {
        std::lock_guard lock(g_mutex);
        g_plugins.clear();
    }
    g_loader = 0;
    g_initialized = false;
    g_config = Config{};
}

namespace {
PSEPluginHandle Register(const PSEPluginInfo* info, bool external, PSEPluginHandle parent = 0) {
    if (!g_initialized || !info || info->size < sizeof(PSEPluginInfo) || !info->name || !ValidName(info->name))
        return 0;
    std::lock_guard lock(g_mutex);
    for (const Plugin& p : g_plugins)
        if (!p.disabled && p.name == info->name) return 0;
    Plugin& p = g_plugins.emplace_back();
    p.info = *info;
    p.info.size = sizeof(PSEPluginInfo);
    p.name = info->name;
    p.external = external;
    p.parent = parent;
    if (external || parent) {
        if (external) p.info.flags &= ~static_cast<uint32_t>(PSE_PLUGIN_INTERNAL);
        else p.info.flags |= PSE_PLUGIN_INTERNAL;
        p.info.name = p.name.c_str();
        p.displayName = info->displayName ? info->displayName : "";
        p.author = info->author ? info->author : "";
        p.info.displayName = p.displayName.c_str();
        p.info.author = p.author.c_str();
        if (info->games) {
            for (const char* const* g = info->games; *g; ++g) p.games.emplace_back(*g);
            for (const std::string& g : p.games) p.gamePtrs.push_back(g.c_str());
            p.gamePtrs.push_back(nullptr);
            p.info.games = p.gamePtrs.data();
        }
    } else {
        p.info.flags |= PSE_PLUGIN_INTERNAL;
    }
    PSEPluginHandle handle = static_cast<PSEPluginHandle>(g_plugins.size());
    p.load = {sizeof(PSELoadInterface), PSE_API_VERSION, g_config.gameId, handle, &Query};
    return handle;
}
}  // namespace

PSEPluginHandle RegisterInternalPlugin(const PSEPluginInfo* info) { return Register(info, false); }
PSEPluginHandle RegisterExternalPlugin(const PSEPluginInfo* info) { return Register(info, true); }

PSEPluginHandle RegisterHostedPlugin(const PSEPluginInfo* info, PSEPluginHandle parent) {
    if (!detail::ValidPlugin(parent)) return 0;
    return Register(info, false, parent);
}

PSEPluginHandle PluginParent(PSEPluginHandle plugin) {
    std::lock_guard lock(g_mutex);
    return plugin != 0 && plugin <= g_plugins.size() ? g_plugins[plugin - 1].parent : 0;
}

std::vector<PSEPluginHandle> PluginChildren(PSEPluginHandle parent) {
    std::lock_guard lock(g_mutex);
    std::vector<PSEPluginHandle> out;
    for (size_t i = 0; parent && i < g_plugins.size(); ++i)
        if (g_plugins[i].parent == parent) out.push_back(static_cast<PSEPluginHandle>(i + 1));
    return out;
}

void SetPluginHost(PSEPluginHandle plugin) {
    std::lock_guard lock(g_mutex);
    if (plugin != 0 && plugin <= g_plugins.size()) g_plugins[plugin - 1].host = true;
}

void SetPluginAdopts(PSEPluginHandle plugin, const std::vector<std::string>& names) {
    std::lock_guard lock(g_mutex);
    if (plugin != 0 && plugin <= g_plugins.size()) g_plugins[plugin - 1].adopts = names;
}

PSEPluginHandle PluginAdopting(const char* name) {
    std::lock_guard lock(g_mutex);
    for (size_t i = 0; name && i < g_plugins.size(); ++i) {
        if (g_plugins[i].disabled) continue;
        for (const std::string& a : g_plugins[i].adopts)
            if (a == name) return static_cast<PSEPluginHandle>(i + 1);
    }
    return 0;
}

bool PluginHost(PSEPluginHandle plugin) {
    std::lock_guard lock(g_mutex);
    return plugin != 0 && plugin <= g_plugins.size() && g_plugins[plugin - 1].host;
}

void DisablePlugin(PSEPluginHandle plugin) {
    std::lock_guard lock(g_mutex);
    if (plugin == 0 || plugin > g_plugins.size()) return;
    // The record (name, info) stays: logs and the plugin list still name it. External records hold only our own
    // string copies, so nothing points into the DLL once it is freed.
    g_plugins[plugin - 1].disabled = true;
}

bool ValidPluginName(const char* name) { return name && ValidName(name); }

bool PluginDisabled(PSEPluginHandle plugin) {
    std::lock_guard lock(g_mutex);
    return plugin != 0 && plugin <= g_plugins.size() && g_plugins[plugin - 1].disabled;
}

bool PluginExternal(PSEPluginHandle plugin) {
    std::lock_guard lock(g_mutex);
    return plugin != 0 && plugin <= g_plugins.size() && g_plugins[plugin - 1].external;
}

const PSELoadInterface* LoadInterfaceFor(PSEPluginHandle plugin) {
    std::lock_guard lock(g_mutex);
    if (plugin == 0 || plugin > g_plugins.size()) return nullptr;
    return &g_plugins[plugin - 1].load;
}

const char* PluginName(PSEPluginHandle plugin) {
    std::lock_guard lock(g_mutex);
    if (plugin == 0 || plugin > g_plugins.size()) return "";
    return g_plugins[plugin - 1].name.c_str();
}

PSEPluginHandle FindPlugin(const char* name) {
    if (!name) return 0;
    std::lock_guard lock(g_mutex);
    for (size_t i = 0; i < g_plugins.size(); ++i)
        if (!g_plugins[i].disabled && g_plugins[i].name == name) return static_cast<PSEPluginHandle>(i + 1);
    return 0;
}

uint32_t PluginCount() {
    std::lock_guard lock(g_mutex);
    return static_cast<uint32_t>(g_plugins.size());
}

const PSEPluginInfo* PluginInfo(PSEPluginHandle plugin) {
    std::lock_guard lock(g_mutex);
    if (plugin == 0 || plugin > g_plugins.size()) return nullptr;
    return &g_plugins[plugin - 1].info;
}

}  // namespace pse
