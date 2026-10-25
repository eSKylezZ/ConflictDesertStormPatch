// dsfix kit (kit.h): the host-side pieces core/patch.cpp and core/log.h expect, on the public SDK.
#include "kit.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/pse_host.h"
#include "core/simrevision.h"
#include "generated/patch_tables.h"
#include "loader.h"

namespace kit {
namespace {
const PSELoadInterface* g_pse = nullptr;
PSEPluginHandle g_self = 0;
const PSELogInterface* g_log = nullptr;
const PSEHooksInterface* g_hooks = nullptr;
const PSESettingsInterface* g_settings = nullptr;
const PSEMessagingInterface* g_messaging = nullptr;
std::string g_name;

constexpr char kOldPlugin[] = "dsfix";
constexpr char kImportedMarker[] = "ImportedFromDsfix";

struct Watch {
    std::string key;
    void (*changed)(bool on);
};
std::vector<Watch> g_watches;

std::string KeyOf(const char* plugin) {
    return std::string("Software\\PSE\\") + (g_pse && g_pse->gameId ? g_pse->gameId : "ds1") + "\\Plugins\\" + plugin;
}

// One-time copy of `value` from dsfix's key into ours (only when ours has none), before Declare reads it. The marker
// is set once per plugin, after its first import pass; settings a later version adds are copied while it is absent.
void ImportOld(const char* value, const char* from = kOldPlugin) {
    HKEY key;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, KeyOf(g_name.c_str()).c_str(), 0, nullptr, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    DWORD v = 0, size = sizeof v;
    const bool imported =
        RegQueryValueExA(key, kImportedMarker, nullptr, nullptr, reinterpret_cast<BYTE*>(&v), &size) == ERROR_SUCCESS;
    size = sizeof v;
    const bool haveOwn = RegQueryValueExA(key, value, nullptr, nullptr, reinterpret_cast<BYTE*>(&v), &size) == ERROR_SUCCESS;
    if (!imported && !haveOwn) {
        DWORD old = 0;
        size = sizeof old;
        if (RegGetValueA(HKEY_CURRENT_USER, KeyOf(from).c_str(), value, RRF_RT_REG_DWORD, nullptr, &old, &size) ==
            ERROR_SUCCESS) {
            const bool ok =
                RegSetValueExA(key, value, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&old), sizeof old) == ERROR_SUCCESS;
            dslog::Write("%s: %s %lu copied from plugin %s's settings%s", g_name.c_str(), value, old, from,
                         ok ? "" : " - FAILED");
        }
    }
    RegCloseKey(key);
}

void MarkImported() {
    HKEY key;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, KeyOf(g_name.c_str()).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;
    const DWORD one = 1;
    RegSetValueExA(key, kImportedMarker, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof one);
    RegCloseKey(key);
}

void PSE_CALL OnChanged(const char* key, void*) {
    for (const Watch& w : g_watches)
        if (w.changed && w.key == key) w.changed(GetBool(key));
}

void PSE_CALL OnPostLoad(const PSEMessage* msg, void*) {
    if (msg->type == PSE_MSG_POST_LOAD) MarkImported();
}
}  // namespace

bool Load(const PSELoadInterface* pse) {
    if (!pse || pse->size < sizeof(PSELoadInterface) || !pse->Query) return false;
    g_pse = pse;
    g_self = pse->plugin;
    g_log = static_cast<const PSELogInterface*>(pse->Query(g_self, PSE_LOG_INTERFACE, 1));
    g_hooks = static_cast<const PSEHooksInterface*>(pse->Query(g_self, PSE_HOOKS_INTERFACE, 1));
    g_settings = static_cast<const PSESettingsInterface*>(pse->Query(g_self, PSE_SETTINGS_INTERFACE, 1));
    g_messaging = static_cast<const PSEMessagingInterface*>(pse->Query(g_self, PSE_MESSAGING_INTERFACE, 1));
    // The name is ours (PSEPlugin_Info.name); the hooks list knows it only once we own something.
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&Load), &self);
    if (auto info = reinterpret_cast<const PSEPluginInfo*>(GetProcAddress(self, "PSEPlugin_Info")); info && info->name)
        g_name = info->name;
    if (!g_log) return false;
    if (!g_hooks || !g_settings || !g_messaging) {
        g_log->Write(g_self, PSE_LOG_ERROR, "%s: needs pse.log, pse.hooks, pse.settings and pse.messaging",
                     g_name.c_str());
        return false;
    }
    g_settings->OnChange(g_self, &OnChanged, nullptr);
    g_messaging->RegisterListener(g_self, "pse", &OnPostLoad, nullptr);
    return true;
}

PSEPluginHandle Self() { return g_self; }
const char* Name() { return g_name.c_str(); }
const char* GameId() { return g_pse && g_pse->gameId ? g_pse->gameId : ""; }
const PSELogInterface* Log() { return g_log; }
const PSEHooksInterface* Hooks() { return g_hooks; }
const PSESettingsInterface* Settings() { return g_settings; }
const PSEMessagingInterface* Messaging() { return g_messaging; }
const void* Query(const char* id, uint32_t minVersion) {
    return g_pse ? g_pse->Query(g_self, id, minVersion) : nullptr;
}

void ImportFrom(const char* plugin, const char* value) { ImportOld(value, plugin); }
const PSELoadInterface* LoadInterface() { return g_pse; }

bool DeclareBool(const char* key, bool defaultOn, const char* label, const char* help, const char* group,
                 void (*changed)(bool on), uint32_t flags) {
    if (!g_settings) return false;
    ImportOld(key);
    PSESettingDesc d = {};
    d.size = sizeof d;
    d.key = key;
    d.type = PSE_SETTING_BOOL;
    d.flags = flags;
    d.defaultInt = defaultOn ? 1 : 0;
    d.minInt = 0;
    d.maxInt = 1;
    d.label = label;
    d.help = help;
    d.group = group;
    if (g_settings->Declare(g_self, &d) != PSE_OK) return false;
    g_watches.push_back({key, changed});
    return true;
}

bool GetBool(const char* key) { return g_settings && g_settings->GetInt(g_self, key) != 0; }

bool DeclareInt(const char* key, int defaultValue, int min, int max, uint32_t flags) {
    if (!g_settings) return false;
    ImportOld(key);
    PSESettingDesc d = {};
    d.size = sizeof d;
    d.key = key;
    d.type = PSE_SETTING_INT;
    d.flags = flags;
    d.defaultInt = defaultValue;
    d.minInt = min;
    d.maxInt = max;
    d.label = key;
    return g_settings->Declare(g_self, &d) == PSE_OK;
}
int GetInt(const char* key) { return g_settings ? g_settings->GetInt(g_self, key) : 0; }
bool SetInt(const char* key, int value) { return g_settings && g_settings->SetInt(g_self, key, value) == PSE_OK; }

bool SetBool(const char* key, bool on) {
    return g_settings && g_settings->SetInt(g_self, key, on ? 1 : 0) == PSE_OK;
}

const DsfixServicesV1* Services() {
    static const DsfixServicesV1* s = nullptr;
    static bool asked = false;
    if (!asked && g_messaging) {
        asked = true;
        s = static_cast<const DsfixServicesV1*>(g_messaging->QueryInterface(g_self, DSFIX_SERVICES_INTERFACE, 1));
        if (s && s->size < sizeof(DsfixServicesV1)) s = nullptr;
        if (!s && g_log)
            g_log->Write(g_self, PSE_LOG_ERROR, "%s: no " DSFIX_SERVICES_INTERFACE " 1 (DesertStormFix's dsfix.dll missing "
                         "or older)", g_name.c_str());
    }
    return s;
}

bool OnFrame(void (*fn)()) {
    if (!g_messaging || !fn) return false;
    return g_messaging->Subscribe(
               g_self, PSE_EVENT_FRAME,
               [](PSEEventId, void*, void* ctx) -> int {
                   reinterpret_cast<void (*)()>(ctx)();
                   return 0;
               },
               reinterpret_cast<void*>(fn), 0) != 0;
}

DWORD DevDword(const char* name, DWORD fallback) {
#ifdef DS_DIST
    (void)name;
    return fallback;
#else
    DWORD v = 0, size = sizeof v;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", name, RRF_RT_REG_DWORD, nullptr, &v, &size) !=
        ERROR_SUCCESS)
        return fallback;
    return v;
#endif
}

}  // namespace kit

// ---- what core/patch.cpp and core/log.h expect from the host ---------------------------------------------------------

namespace pse {
const PSEHooksInterface* HooksInterface() { return kit::Hooks(); }
// Moved code asks the loader-side accessors (loader.h) as dsfix.dll did: the plugin's own interfaces answer.
const PSEMessagingInterface* MessagingInterface() { return kit::Messaging(); }
const PSESettingsInterface* SettingsInterface() { return kit::Settings(); }
const PSESerializationInterface* SerializationInterface() {
    static const auto* t = static_cast<const PSESerializationInterface*>(kit::Query(PSE_SERIALIZATION_INTERFACE, 1));
    return t;
}
const PSELoadInterface* LoadInterfaceFor(PSEPluginHandle) { return kit::LoadInterface(); }
// lua_bridge.h logs as plugin "pse" through these; a kit plugin writes the line under its own name.
PSEPluginHandle FindPlugin(const char*) { return kit::Self(); }
void LogLine(PSEPluginHandle, PSELogLevel level, const char* text) {
    if (const PSELogInterface* log = kit::Log()) log->Write(kit::Self(), level, "%s", text);
}

// Owner names for patch.cpp's log lines: our own, else the broker's records (a plugin that owns nothing has no name
// there - it never shows up in a conflict line then either).
const char* PluginName(PSEPluginHandle plugin) {
    if (plugin == kit::Self()) return kit::Name();
    static thread_local std::string name;
    if (const PSEHooksInterface* h = kit::Hooks()) {
        std::vector<PSEHookInfo> all(h->ListHooks(nullptr, 0) + 8);
        for (auto& i : all) i.size = sizeof i;
        const uint32_t n = h->ListHooks(all.data(), static_cast<uint32_t>(all.size()));
        for (uint32_t k = 0; k < n && k < all.size(); ++k)
            if (all[k].owner == plugin && all[k].ownerName) return (name = all[k].ownerName).c_str();
    }
    return "";
}
}  // namespace pse

PSEPluginHandle pse_host::Core() { return kit::Self(); }
// Moved code that took a sub-plugin's handle for its hooks' owner (patch::OwnerScope) owns them as this plugin now.
PSEPluginHandle pse_host::Find(const char*) { return kit::Self(); }

// ---- the online check's simulation fingerprint (pse_api.h PSEPlugin_SimFingerprint) ----------------------------------
// Every dsfix kit plugin declares the same rule as dsfix.dll's own fingerprint (plugins/online/onlinecompat.cpp): DesertStormFix
// version + dsfix::kSimRevision + the generated patch tables, plus its own name and version. A dev build and the release
// package of one source give the same string, so they connect online; the loader compares its SHA-256.
extern "C" __declspec(dllexport) const char* PSE_CALL PSEPlugin_SimFingerprint(void) {
    static std::string fp;
    if (fp.empty()) {
        uint64_t h = 1469598103934665603ull;   // FNV-1a 64 over the patch tables
        auto add = [&h](const void* data, size_t len) {
            for (size_t i = 0; i < len; ++i) h = (h ^ static_cast<const uint8_t*>(data)[i]) * 1099511628211ull;
        };
        for (const gen::PatchGroup& g : gen::kAllGroups) {
            add(g.name, strlen(g.name) + 1);
            for (size_t i = 0; i < g.count; ++i) {
                const BytePatch& p = g.patches[i];
                const uint32_t fields[3] = {p.va, p.len, p.anyOriginal ? 1u : 0u};
                add(fields, sizeof fields);
                add(p.original, p.len);
                add(p.patched, p.len);
            }
        }
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&PSEPlugin_SimFingerprint), &self);
        auto info = reinterpret_cast<const PSEPluginInfo*>(GetProcAddress(self, "PSEPlugin_Info"));
        char b[256];
        snprintf(b, sizeof b, "dsfix-kit DesertStormFix %s rev %u plugin %s %u.%u.%u tables %016llx", DS_VERSION,
                 dsfix::kSimRevision, info && info->name ? info->name : "?",
                 info ? PSE_VERSION_MAJOR(info->version) : 0, info ? PSE_VERSION_MINOR(info->version) : 0,
                 info ? PSE_VERSION_PATCH(info->version) : 0, static_cast<unsigned long long>(h));
        fp = b;
    }
    return fp.c_str();
}

#ifndef DS_DIST
void dslog::Write(const char* fmt, ...) {
    const PSELogInterface* log = kit::Log();
    if (!log) return;
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    log->Write(kit::Self(), PSE_LOG_INFO, "%s", line);
}
#endif
