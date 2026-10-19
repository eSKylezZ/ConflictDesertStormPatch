// pse.messaging: listeners + Dispatch, the cross-plugin interface registry, prioritised events.
//
// Callbacks may subscribe, unsubscribe, register listeners or raise / dispatch again while they run: every delivery
// walks a snapshot taken under the lock and runs without it. A subscription removed during a Raise is marked dead,
// so it is skipped even though it is still in that Raise's snapshot.
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>

#include "internal.h"

namespace pse {
namespace {

struct Listener {
    PSEPluginHandle owner;
    std::string sender;   // "" = from anyone
    PSEMessageCallback cb;
    void* ctx;
    bool external;        // owner is an external plugin: called under the crash guard (guard.cpp)
};

struct RegisteredInterface {
    uint32_t version;
    const void* table;
    PSEPluginHandle owner;
};

struct Subscription {
    PSESubscriptionId id;
    PSEPluginHandle owner;
    PSEEventId event;
    PSEEventCallback cb;
    void* ctx;
    int32_t priority;
    bool alive = true;
    bool external = false;   // owner is an external plugin: called under the crash guard (guard.cpp)
};
using SubPtr = std::shared_ptr<Subscription>;

const char* const kBuiltinEvents[] = {nullptr,              "pse.frame",          "pse.pre_render",
                                      "pse.post_render",    "pse.end_scene",      "pse.present",
                                      "pse.level_load_begin", "pse.level_load_end", "pse.level_unload"};
constexpr PSEEventId kLastBuiltin = PSE_EVENT_LEVEL_UNLOAD;

std::recursive_mutex g_mutex;
std::vector<Listener> g_listeners;
std::map<std::string, std::vector<RegisteredInterface>> g_interfaces;
std::map<std::string, PSEEventId> g_eventNames;          // dynamic ones
PSEEventId g_nextEvent = PSE_EVENT_FIRST_DYNAMIC;
std::map<PSEEventId, std::vector<SubPtr>> g_subs;        // per event, sorted: priority desc, then subscription order
PSESubscriptionId g_nextSub = 1;

bool KnownEvent(PSEEventId e) {
    return (e >= 1 && e <= kLastBuiltin) || (e >= PSE_EVENT_FIRST_DYNAMIC && e < g_nextEvent);
}

// For crash reports: "a PSE_MSG_POST_LOAD message", "a message listener (type 0x10002 from dsfix)" ...
std::string MessageWhere(const std::string& sender, uint32_t type) {
    if (sender == "pse") {
        switch (type) {
            case PSE_MSG_POST_LOAD: return "its PSE_MSG_POST_LOAD listener";
            case PSE_MSG_POST_POST_LOAD: return "its PSE_MSG_POST_POST_LOAD listener";
            default: break;
        }
    }
    char b[96];
    std::snprintf(b, sizeof b, "a message listener (type 0x%X from %s)", type, sender.c_str());
    return b;
}

std::string EventWhere(PSEEventId event) {
    if (event >= 1 && event <= kLastBuiltin) return std::string("a ") + kBuiltinEvents[event] + " event subscriber";
    std::lock_guard lock(g_mutex);
    for (const auto& [name, id] : g_eventNames)
        if (id == event) return "a " + name + " event subscriber";
    return "an event subscriber (event " + std::to_string(event) + ")";
}

bool IsLoaderInterfaceId(const char* id) {
    for (const char* b : {PSE_LOG_INTERFACE, PSE_HOOKS_INTERFACE, PSE_MESSAGING_INTERFACE, PSE_TASKS_INTERFACE,
                          PSE_SETTINGS_INTERFACE, PSE_SERIALIZATION_INTERFACE})
        if (std::strcmp(b, id) == 0) return true;
    return false;
}

PSEResult PSE_CALL RegisterListener(PSEPluginHandle plugin, const char* sender, PSEMessageCallback cb, void* ctx) {
    if (!detail::ValidPlugin(plugin) || !cb) return PSE_E_INVALID_ARG;
    const bool external = PluginExternal(plugin);
    std::lock_guard lock(g_mutex);
    g_listeners.push_back({plugin, sender ? sender : "", cb, ctx, external});
    return PSE_OK;
}

PSEResult PSE_CALL Dispatch(PSEPluginHandle plugin, uint32_t type, const void* data, uint32_t dataLen,
                            const char* receiver) {
    if (!detail::ValidPlugin(plugin)) return PSE_E_INVALID_ARG;
    PSEPluginHandle target = 0;
    if (receiver) {
        target = FindPlugin(receiver);
        if (!target) return PSE_E_NOT_FOUND;
    }
    std::string sender = PluginName(plugin);
    std::vector<Listener> snapshot;
    {
        std::lock_guard lock(g_mutex);
        for (const Listener& l : g_listeners)
            if ((target == 0 || l.owner == target) && (l.sender.empty() || l.sender == sender)) snapshot.push_back(l);
    }
    PSEMessage msg = {sizeof(PSEMessage), sender.c_str(), type, data, dataLen};
    for (const Listener& l : snapshot) {
        if (!l.external) {
            l.cb(&msg, l.ctx);
            continue;
        }
        if (PluginDisabled(l.owner)) continue;   // crashed earlier in this round (or meanwhile)
        detail::Fault fault;
        if (!detail::GuardedMessage(l.cb, &msg, l.ctx, &fault))
            detail::PluginCrashed(l.owner, MessageWhere(sender, type).c_str(), fault);
    }
    return PSE_OK;
}

PSEResult PSE_CALL RegisterInterface(PSEPluginHandle plugin, const char* id, uint32_t version, const void* table) {
    if (!detail::ValidPlugin(plugin) || !id || !*id || !table) return PSE_E_INVALID_ARG;
    if (IsLoaderInterfaceId(id)) return PSE_E_EXISTS;
    std::lock_guard lock(g_mutex);
    auto& list = g_interfaces[id];
    for (const RegisteredInterface& r : list)
        if (r.version == version) return PSE_E_EXISTS;
    list.push_back({version, table, plugin});
    return PSE_OK;
}

const void* PSE_CALL QueryInterface(PSEPluginHandle plugin, const char* id, uint32_t minVersion) {
    if (!detail::ValidPlugin(plugin)) return nullptr;
    return detail::QueryAny(id, minVersion);
}

PSEEventId PSE_CALL EventId(const char* name) {
    if (!name || !*name) return 0;
    for (PSEEventId e = 1; e <= kLastBuiltin; ++e)
        if (std::strcmp(kBuiltinEvents[e], name) == 0) return e;
    std::lock_guard lock(g_mutex);
    auto it = g_eventNames.find(name);
    if (it != g_eventNames.end()) return it->second;
    PSEEventId id = g_nextEvent++;
    g_eventNames.emplace(name, id);
    return id;
}

PSESubscriptionId PSE_CALL Subscribe(PSEPluginHandle plugin, PSEEventId event, PSEEventCallback cb, void* ctx,
                                     int32_t priority) {
    if (!detail::ValidPlugin(plugin) || !cb) return 0;
    const bool external = PluginExternal(plugin);
    std::lock_guard lock(g_mutex);
    if (!KnownEvent(event)) return 0;
    auto sub = std::make_shared<Subscription>(Subscription{g_nextSub++, plugin, event, cb, ctx, priority, true, external});
    auto& list = g_subs[event];
    // After every subscription with priority >= ours: ties keep subscription order.
    auto pos = list.begin();
    while (pos != list.end() && (*pos)->priority >= priority) ++pos;
    list.insert(pos, std::move(sub));
    return g_nextSub - 1;
}

PSEResult PSE_CALL Unsubscribe(PSEPluginHandle plugin, PSESubscriptionId id) {
    if (!detail::ValidPlugin(plugin) || id == 0) return PSE_E_INVALID_ARG;
    std::lock_guard lock(g_mutex);
    for (auto& [event, list] : g_subs) {
        for (auto it = list.begin(); it != list.end(); ++it) {
            if ((*it)->id != id) continue;
            if ((*it)->owner != plugin) return PSE_E_NOT_FOUND;   // only the owner may remove it
            (*it)->alive = false;
            list.erase(it);
            return PSE_OK;
        }
    }
    return PSE_E_NOT_FOUND;
}

int PSE_CALL Raise(PSEPluginHandle plugin, PSEEventId event, void* data) {
    if (!detail::ValidPlugin(plugin)) return 0;
    std::vector<SubPtr> snapshot;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_subs.find(event);
        if (it == g_subs.end()) return 0;
        snapshot = it->second;
    }
    for (const SubPtr& s : snapshot) {
        if (!s->alive) continue;   // also false once its plugin crashed (ForgetMessaging)
        int r;
        if (!s->external) {
            r = s->cb(event, data, s->ctx);
        } else {
            detail::Fault fault;
            if (!detail::GuardedEvent(s->cb, event, data, s->ctx, &r, &fault)) {
                detail::PluginCrashed(s->owner, EventWhere(event).c_str(), fault);
                continue;   // a crashed subscriber doesn't stop the event: the rest still get it
            }
        }
        if (r != 0) return 1;
    }
    return 0;
}

const PSEMessagingInterface kTable = {sizeof(PSEMessagingInterface), 1, &RegisterListener, &Dispatch,
                                      &RegisterInterface, &QueryInterface, &EventId, &Subscribe, &Unsubscribe, &Raise};

}  // namespace

namespace detail {

const void* QueryRegisteredInterface(const char* id, uint32_t minVersion) {
    std::lock_guard lock(g_mutex);
    auto it = g_interfaces.find(id);
    if (it == g_interfaces.end()) return nullptr;
    const RegisteredInterface* best = nullptr;
    for (const RegisteredInterface& r : it->second)
        if (r.version >= minVersion && (!best || r.version > best->version)) best = &r;
    return best ? best->table : nullptr;
}

void ForgetMessaging(PSEPluginHandle plugin) {
    std::lock_guard lock(g_mutex);
    std::erase_if(g_listeners, [&](const Listener& l) { return l.owner == plugin; });
    for (auto it = g_interfaces.begin(); it != g_interfaces.end();) {
        std::erase_if(it->second, [&](const RegisteredInterface& r) { return r.owner == plugin; });
        it = it->second.empty() ? g_interfaces.erase(it) : std::next(it);
    }
    for (auto& [event, list] : g_subs)
        std::erase_if(list, [&](const SubPtr& s) {
            if (s->owner != plugin) return false;
            s->alive = false;   // a Raise running right now skips it too
            return true;
        });
}

void ResetMessaging() {
    std::lock_guard lock(g_mutex);
    g_listeners.clear();
    g_interfaces.clear();
    g_eventNames.clear();
    g_nextEvent = PSE_EVENT_FIRST_DYNAMIC;
    for (auto& [event, list] : g_subs)
        for (auto& s : list) s->alive = false;
    g_subs.clear();
    g_nextSub = 1;
}

}  // namespace detail

void Broadcast(uint32_t type, const void* data, uint32_t len) { Dispatch(detail::LoaderHandle(), type, data, len, nullptr); }
int RaiseEvent(PSEEventId event, void* data) { return Raise(detail::LoaderHandle(), event, data); }
const PSEMessagingInterface* MessagingInterface() { return &kTable; }

}  // namespace pse
