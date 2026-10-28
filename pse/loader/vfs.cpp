// pse.vfs (pse/include/pse/pse_vfs.h, docs/sdk/reference/vfs.md): the provider registry. Game-neutral: the game layer
// (DS1: DesertStormFix's src/features/assetstore.cpp for now) hooks the game's file lookups into it and sets the
// name hash; providers (the asset store, a plugin's generated files) answer.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "internal.h"
#include "pse/pse_vfs.h"

namespace pse {
namespace {

struct Provider {
    PSEVfsProviderId id;
    PSEPluginHandle owner;
    int32_t priority;
    uint64_t order;          // registration order (equal priority)
    std::string name;
    PSEVfsProvider cb;       // copy (name points at `name`)
    void* ctx;
    bool external;
};

std::mutex g_mutex;
std::vector<Provider> g_providers;   // asking order: priority desc, then registration order
PSEVfsProviderId g_nextId = 1;
uint64_t g_order = 0;
PSEVfsHashFn g_hash = nullptr;
PSEPluginHandle g_hashOwner = 0;

std::vector<Provider> Snapshot() {
    std::lock_guard lock(g_mutex);
    return g_providers;
}

bool ValidQuery(const PSEVfsQuery* q) {
    return q && q->size >= sizeof(PSEVfsQuery) && (q->name || q->nameHash);
}

// The query with its hash filled in (when the asker gave only the name).
PSEVfsQuery Normalised(const PSEVfsQuery* q) {
    PSEVfsQuery n = *q;
    n.size = sizeof(PSEVfsQuery);
    if (!n.nameHash && n.name) {
        PSEVfsHashFn h;
        {
            std::lock_guard lock(g_mutex);
            h = g_hash;
        }
        if (h) n.nameHash = h(n.name);
    }
    return n;
}

// One provider's Locate (external owners under the crash guard). 1 = it answered.
int CallLocate(const Provider& p, const PSEVfsQuery* q, PSEVfsLocation* out) {
    if (!p.external) return p.cb.Locate(q, out, p.ctx);
    if (PluginDisabled(p.owner)) return 0;
    detail::Fault fault;
    int result = 0;
    if (!detail::GuardedVfsLocate(p.cb.Locate, q, out, p.ctx, &result, &fault)) {
        detail::PluginCrashed(p.owner, "a pse.vfs provider (Locate)", fault);
        return 0;
    }
    return result;
}

int32_t CallRead(const Provider& p, const PSEVfsQuery* q, uint32_t offset, void* buf, uint32_t len) {
    if (!p.external) return p.cb.Read(q, offset, buf, len, p.ctx);
    if (PluginDisabled(p.owner)) return -1;
    detail::Fault fault;
    int32_t result = -1;
    if (!detail::GuardedVfsRead(p.cb.Read, q, offset, buf, len, p.ctx, &result, &fault)) {
        detail::PluginCrashed(p.owner, "a pse.vfs provider (Read)", fault);
        return -1;
    }
    return result;
}

// The first provider that has the file; *which = it. False = none.
bool Locate(const PSEVfsQuery* q, PSEVfsLocation* out, Provider* which) {
    for (const Provider& p : Snapshot()) {
        PSEVfsLocation loc{};
        loc.size = sizeof loc;
        if (!CallLocate(p, q, &loc)) continue;
        loc.path[sizeof loc.path - 1] = 0;
        loc.source[sizeof loc.source - 1] = 0;
        loc.size = sizeof loc;
        loc.provider = p.owner;
        if (out) *out = loc;
        if (which) *which = p;
        return true;
    }
    return false;
}

// Reads `len` bytes of the located file into buf (provider's Read, else its disk file).
bool ReadLocated(const Provider& p, const PSEVfsQuery* q, const PSEVfsLocation& loc, void* buf, uint32_t len) {
    if (p.cb.Read) return CallRead(p, q, 0, buf, len) == static_cast<int32_t>(len);
    if (!loc.path[0]) return false;
    HANDLE f = CreateFileW(detail::Widen(loc.path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(loc.offset);
    DWORD got = 0;
    const bool ok = SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) &&
                    (len == 0 || (ReadFile(f, buf, len, &got, nullptr) && got == len));
    CloseHandle(f);
    return ok;
}

PSEResult PSE_CALL RegisterProvider(PSEPluginHandle plugin, const PSEVfsProvider* provider, int32_t priority, void* ctx,
                                    PSEVfsProviderId* outId) {
    if (!detail::ValidPlugin(plugin) || !provider || provider->size < sizeof(PSEVfsProvider) || !provider->Locate)
        return PSE_E_INVALID_ARG;
    Provider p;
    p.owner = plugin;
    p.priority = priority;
    p.name = provider->name ? provider->name : PluginName(plugin);
    p.cb = *provider;
    p.cb.size = sizeof(PSEVfsProvider);
    p.ctx = ctx;
    p.external = PluginExternal(plugin);   // internal (sub-)plugins are called directly, like every service
    {
        std::lock_guard lock(g_mutex);
        p.id = g_nextId++;
        p.order = g_order++;
        g_providers.push_back(p);
        std::stable_sort(g_providers.begin(), g_providers.end(), [](const Provider& a, const Provider& b) {
            return a.priority != b.priority ? a.priority > b.priority : a.order < b.order;
        });
        for (Provider& q : g_providers) q.cb.name = q.name.c_str();   // (strings moved with the vector)
    }
    if (outId) *outId = p.id;
    char line[256];
    std::snprintf(line, sizeof line, "pse.vfs: provider %s registered by %s (priority %d)", p.name.c_str(),
                  PluginName(plugin), priority);
    LogLine(detail::LoaderHandle(), PSE_LOG_INFO, line);
    return PSE_OK;
}

PSEResult PSE_CALL UnregisterProvider(PSEPluginHandle plugin, PSEVfsProviderId id) {
    std::lock_guard lock(g_mutex);
    auto it = std::find_if(g_providers.begin(), g_providers.end(),
                           [&](const Provider& p) { return p.id == id && p.owner == plugin; });
    if (it == g_providers.end()) return PSE_E_NOT_FOUND;
    g_providers.erase(it);
    for (Provider& q : g_providers) q.cb.name = q.name.c_str();
    return PSE_OK;
}

int PSE_CALL Exists(PSEPluginHandle plugin, const PSEVfsQuery* q) {
    if (!detail::ValidPlugin(plugin) || !ValidQuery(q)) return 0;
    const PSEVfsQuery n = Normalised(q);
    return Locate(&n, nullptr, nullptr) ? 1 : 0;
}

PSEResult PSE_CALL ResolvePath(PSEPluginHandle plugin, const PSEVfsQuery* q, PSEVfsLocation* out) {
    if (!detail::ValidPlugin(plugin) || !ValidQuery(q) || !out || out->size < sizeof(PSEVfsLocation))
        return PSE_E_INVALID_ARG;
    const PSEVfsQuery n = Normalised(q);
    return Locate(&n, out, nullptr) ? PSE_OK : PSE_E_NOT_FOUND;
}

PSEResult PSE_CALL Read(PSEPluginHandle plugin, const PSEVfsQuery* q, void* buf, uint32_t bufLen, uint32_t* outLen) {
    if (!detail::ValidPlugin(plugin) || !ValidQuery(q)) return PSE_E_INVALID_ARG;
    const PSEVfsQuery n = Normalised(q);
    PSEVfsLocation loc{};
    Provider p;
    if (!Locate(&n, &loc, &p)) return PSE_E_NOT_FOUND;
    if (outLen) *outLen = loc.length;
    if (!buf || bufLen < loc.length) return PSE_E_NO_MEMORY;
    return ReadLocated(p, &n, loc, buf, loc.length) ? PSE_OK : PSE_E_ACCESS;
}

PSEResult PSE_CALL ReadAlloc(PSEPluginHandle plugin, const PSEVfsQuery* q, void** outData, uint32_t* outLen) {
    if (!detail::ValidPlugin(plugin) || !ValidQuery(q) || !outData) return PSE_E_INVALID_ARG;
    *outData = nullptr;
    const PSEVfsQuery n = Normalised(q);
    PSEVfsLocation loc{};
    Provider p;
    if (!Locate(&n, &loc, &p)) return PSE_E_NOT_FOUND;
    if (outLen) *outLen = loc.length;
    void* mem = HeapAlloc(GetProcessHeap(), 0, loc.length ? loc.length : 1);
    if (!mem) return PSE_E_NO_MEMORY;
    if (!ReadLocated(p, &n, loc, mem, loc.length)) {
        HeapFree(GetProcessHeap(), 0, mem);
        return PSE_E_ACCESS;
    }
    *outData = mem;
    return PSE_OK;
}

void PSE_CALL Free(void* data) {
    if (data) HeapFree(GetProcessHeap(), 0, data);
}

PSEResult PSE_CALL SetNameHash(PSEPluginHandle plugin, PSEVfsHashFn fn) {
    if (!detail::ValidPlugin(plugin) || !fn) return PSE_E_INVALID_ARG;
    std::lock_guard lock(g_mutex);
    if (g_hash) return PSE_E_EXISTS;
    g_hash = fn;
    g_hashOwner = plugin;
    return PSE_OK;
}

uint32_t PSE_CALL NameHash(const char* name) {
    PSEVfsHashFn h;
    {
        std::lock_guard lock(g_mutex);
        h = g_hash;
    }
    return h && name ? h(name) : 0;
}

uint32_t PSE_CALL ListProviders(PSEVfsProviderInfo* out, uint32_t max) {
    std::lock_guard lock(g_mutex);
    const uint32_t n = static_cast<uint32_t>(g_providers.size());
    for (uint32_t i = 0; i < n && i < max && out; ++i) {
        if (out[i].size < sizeof(PSEVfsProviderInfo)) continue;
        out[i].id = g_providers[i].id;
        out[i].owner = g_providers[i].owner;
        out[i].priority = g_providers[i].priority;
        out[i].name = g_providers[i].name.c_str();
    }
    return n;
}

const PSEVfsInterface kTable = {sizeof(PSEVfsInterface), PSE_VFS_VERSION, &RegisterProvider, &UnregisterProvider,
                                &Exists, &ResolvePath, &Read, &ReadAlloc, &Free, &SetNameHash, &NameHash,
                                &ListProviders};

}  // namespace

namespace detail {

void ForgetVfs(PSEPluginHandle plugin) {
    std::lock_guard lock(g_mutex);
    std::erase_if(g_providers, [&](const Provider& p) { return p.owner == plugin; });
    for (Provider& q : g_providers) q.cb.name = q.name.c_str();
    if (g_hashOwner == plugin) g_hash = nullptr, g_hashOwner = 0;
}

void ResetVfs() {
    std::lock_guard lock(g_mutex);
    g_providers.clear();
    g_nextId = 1;
    g_order = 0;
    g_hash = nullptr;
    g_hashOwner = 0;
}

}  // namespace detail

const PSEVfsInterface* VfsInterface() { return &kTable; }

}  // namespace pse
