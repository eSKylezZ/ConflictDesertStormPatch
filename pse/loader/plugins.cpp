// External plugin DLLs (docs/internal/DESIGN.md 4, reference docs/sdk/reference/plugins.md).
//
// LoadPlugins(dir) in four steps:
//   1. every <dir>\*.pse manifest is parsed; the .dll of the same name is mapped as an image RESOURCE (no code runs,
//      no DllMain) and its PSEPlugin_Info data export read from the file's bytes (pointers rebased by hand);
//   2. resolution without running anything: name / info / game / API checks, missing `requires`, versions too low,
//      `conflicts`, cycles -> refused with a reason; the rest ordered topologically (requires, after, optional when
//      present), ties by name;
//   3. in that order: LoadLibraryExW, the real exports, register (an external plugin in the same registry as the
//      internal ones), PSEPlugin_Load inside SEH. A crash or a 0 return = everything the plugin registered is taken
//      back (hooks, byte groups, listeners, subscriptions, interfaces, tasks, co-save callbacks), its handle is
//      disabled (never reused) and the DLL freed;
//   4. PSE_MSG_POST_LOAD, then PSE_MSG_POST_POST_LOAD.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <malloc.h>
#include <map>
#include <mutex>
#include <set>

#include "internal.h"

namespace pse {
namespace {

// ---- small helpers -------------------------------------------------------------------------------------------------

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::vector<std::string> SplitList(const std::string& value) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        if (comma == std::string::npos) comma = value.size();
        std::string item = Trim(value.substr(start, comma - start));
        if (!item.empty()) out.push_back(item);
        start = comma + 1;
    }
    return out;
}

std::string Format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::string s = detail::VFormat(fmt, args);
    va_end(args);
    return s;
}

void Log(PSELogLevel level, const std::string& text) { LogLine(detail::LoaderHandle(), level, text.c_str()); }

std::string Requirement(const PluginRequirement& r) {
    return r.minVersion ? r.name + " >= " + VersionString(r.minVersion) : r.name;
}

// ---- SEH guards (guard.cpp; no C++ objects with destructors in these functions) ------------------------------------

using detail::DescribeFault;
using detail::Fault;

int LoadFilter(EXCEPTION_POINTERS* ep, Fault* f) {
    f->code = ep->ExceptionRecord->ExceptionCode;
    f->address = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

int GuardedLoadLibrary(const wchar_t* path, HMODULE* out, Fault* fault) {
    __try {
        *out = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        return 1;
    } __except (LoadFilter(GetExceptionInformation(), fault)) {
        return 0;
    }
}

std::string WinError(DWORD e) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   e, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::string text = msg ? Trim(detail::Narrow(msg)) : std::string();
    if (msg) LocalFree(msg);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == '.')) text.pop_back();
    return Format("error %lu", e) + (text.empty() ? "" : " (" + text + ")");
}

// ---- PSEPlugin_Info read from the file (image mapped as a resource: nothing runs) -----------------------------------

struct StaticInfo {
    uint32_t size = 0, infoVersion = 0, version = 0, minApiVersion = 0, flags = 0;
    std::string name, displayName;
    bool anyGame = true;
    std::vector<std::string> games;
    bool hasLoad = false;
};

class Image {
public:
    explicit Image(const std::wstring& path) {
        module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!module_) return;
        base_ = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(module_) & ~static_cast<uintptr_t>(3));
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base_);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return;
        nt_ = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base_ + dos->e_lfanew);
        if (nt_->Signature != IMAGE_NT_SIGNATURE || nt_->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
            nt_->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
            !(nt_->FileHeader.Characteristics & IMAGE_FILE_DLL)) {
            nt_ = nullptr;
            return;
        }
        size_ = nt_->OptionalHeader.SizeOfImage;
    }
    ~Image() {
        if (module_) FreeLibrary(module_);
    }
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    bool Mapped() const { return module_ != nullptr; }
    bool Valid() const { return nt_ != nullptr; }

    // RVA of an export by name, 0 = none.
    uint32_t Export(const char* name) const {
        const IMAGE_DATA_DIRECTORY& d = nt_->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!d.VirtualAddress || !In(d.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY))) return 0;
        auto ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base_ + d.VirtualAddress);
        if (!In(ed->AddressOfNames, ed->NumberOfNames * 4ull) || !In(ed->AddressOfNameOrdinals, ed->NumberOfNames * 2ull) ||
            !In(ed->AddressOfFunctions, ed->NumberOfFunctions * 4ull))
            return 0;
        auto names = reinterpret_cast<const uint32_t*>(base_ + ed->AddressOfNames);
        auto ords = reinterpret_cast<const uint16_t*>(base_ + ed->AddressOfNameOrdinals);
        auto funcs = reinterpret_cast<const uint32_t*>(base_ + ed->AddressOfFunctions);
        for (DWORD i = 0; i < ed->NumberOfNames; ++i) {
            std::string n;
            if (!String(names[i], 256, &n) || n != name) continue;
            if (ords[i] >= ed->NumberOfFunctions) return 0;
            uint32_t rva = funcs[ords[i]];
            // Inside the export directory = a forwarder string, not code / data of this DLL.
            if (rva >= d.VirtualAddress && rva < d.VirtualAddress + d.Size) return 0;
            return In(rva, 1) ? rva : 0;
        }
        return 0;
    }

    bool In(uint64_t rva, uint64_t len) const { return rva + len <= size_; }
    template <class T>
    bool Read(uint32_t rva, T* out) const {
        if (!In(rva, sizeof(T))) return false;
        std::memcpy(out, base_ + rva, sizeof(T));
        return true;
    }
    bool String(uint32_t rva, size_t max, std::string* out) const {
        out->clear();
        for (size_t i = 0; i < max; ++i) {
            if (!In(static_cast<uint64_t>(rva) + i, 1)) return false;
            char c = static_cast<char>(base_[rva + i]);
            if (!c) return true;
            out->push_back(c);
        }
        return false;
    }
    // An absolute pointer stored in the file (relative to the preferred base) -> RVA; false when outside the image.
    bool PointerRva(uint32_t va, uint32_t* rva) const {
        uint32_t r = va - nt_->OptionalHeader.ImageBase;
        if (va < nt_->OptionalHeader.ImageBase || !In(r, 1)) return false;
        *rva = r;
        return true;
    }

private:
    HMODULE module_ = nullptr;
    const uint8_t* base_ = nullptr;
    const IMAGE_NT_HEADERS32* nt_ = nullptr;
    uint32_t size_ = 0;
};

// "" = ok, else why the info can't be read.
std::string ReadStaticInfo(const std::wstring& dll, StaticInfo* out) {
    Image img(dll);
    if (!img.Mapped()) return "can't read " + detail::Narrow(dll) + ": " + WinError(GetLastError());
    if (!img.Valid()) return detail::Narrow(dll) + " is not a 32-bit x86 DLL";
    uint32_t rva = img.Export("PSEPlugin_Info");
    if (!rva) return "the DLL has no PSEPlugin_Info export (not a PSE plugin?)";
    out->hasLoad = img.Export("PSEPlugin_Load") != 0;
    // Field by field: a plugin built against an older, shorter struct must still be readable.
    uint32_t raw[9] = {};
    if (!img.Read(rva, &raw[0]) || !img.Read(rva + 4, &raw[1])) return "PSEPlugin_Info is outside the DLL image";
    out->size = raw[0];
    out->infoVersion = raw[1];
    if (out->size < sizeof(PSEPluginInfo))
        return Format("PSEPlugin_Info.size is %u, expected at least %u", out->size,
                      static_cast<unsigned>(sizeof(PSEPluginInfo)));
    if (out->infoVersion != 1) return Format("PSEPlugin_Info.infoVersion is %u, this loader reads 1", out->infoVersion);
    for (uint32_t i = 2; i < 9; ++i)
        if (!img.Read(rva + i * 4, &raw[i])) return "PSEPlugin_Info is outside the DLL image";
    uint32_t s;
    if (!raw[2] || !img.PointerRva(raw[2], &s) || !img.String(s, 65, &out->name))
        return "PSEPlugin_Info.name is missing or unreadable";
    if (raw[3] && img.PointerRva(raw[3], &s)) img.String(s, 256, &out->displayName);
    out->version = raw[5];
    out->minApiVersion = raw[6];
    out->flags = raw[8];
    if (raw[7]) {
        out->anyGame = false;
        uint32_t list;
        if (!img.PointerRva(raw[7], &list)) return "PSEPlugin_Info.games is unreadable";
        for (uint32_t i = 0;; ++i) {
            uint32_t p;
            if (i >= 64 || !img.Read(list + i * 4, &p)) return "PSEPlugin_Info.games is not NULL-terminated";
            if (!p) break;
            std::string g;
            if (!img.PointerRva(p, &s) || !img.String(s, 32, &g)) return "PSEPlugin_Info.games is unreadable";
            out->games.push_back(g);
        }
    }
    return {};
}

// ---- state ---------------------------------------------------------------------------------------------------------

struct LoadedPlugin {
    PSEPluginHandle handle = 0;
    HMODULE module = nullptr;
    PSEPluginUnloadFn unload = nullptr;
    size_t result = 0;      // index into g_results
    bool crashed = false;   // faulted in a callback after Load: disabled, no Unload call, DLL left mapped
};

bool g_done = false;
std::vector<PluginLoadResult> g_results;
std::vector<LoadedPlugin> g_loaded;   // load order
// g_results / g_loaded entries change from other threads only through PluginCrashed (a callback that crashed off the
// game thread); everything that reads or writes them after LoadPlugins holds this lock.
std::recursive_mutex g_stateMutex;
std::map<PSEPluginHandle, std::string> g_crashReasons;   // PluginCrashed: why (also before the plugin's entry exists)

// Takes back everything `plugin` registered. true = nothing of it is left that points into its DLL.
bool Cleanup(PSEPluginHandle plugin) {
    const PSEHooksInterface* hooks = HooksInterface();
    bool clean = true;
    uint32_t n = hooks->ListHooks(nullptr, 0);
    std::vector<PSEHookInfo> list(n);
    n = hooks->ListHooks(list.data(), n);
    list.resize(std::min<size_t>(n, list.size()));
    std::set<std::string> groups;
    for (const PSEHookInfo& h : list) {
        if (h.owner != plugin) continue;
        if (h.kind == PSE_HOOK_BYTES) {
            if (h.name) groups.insert(h.name);
        } else if (hooks->Unhook(plugin, h.id) != PSE_OK) {
            Log(PSE_LOG_ERROR, Format("%s: hook %u at 0x%08X couldn't be removed", PluginName(plugin), h.id, h.va));
            clean = false;
        }
    }
    for (const std::string& g : groups)
        if (hooks->RevertGroup(plugin, g.c_str()) != PSE_OK) {
            Log(PSE_LOG_ERROR, Format("%s: byte group %s couldn't be reverted", PluginName(plugin), g.c_str()));
            clean = false;
        }
    detail::ForgetMessaging(plugin);
    detail::ForgetTasks(plugin);
    detail::ForgetSettings(plugin);
    detail::ForgetCosave(plugin);
    detail::ForgetLua(plugin);   // pse.lua (lua_iface.cpp): its namespace + functions
    detail::ForgetVfs(plugin);   // pse.vfs (vfs.cpp): its providers
    // A host plugin's sub-plugins (pse.host RegisterInternalPlugin) live in its DLL: they go with it.
    for (PSEPluginHandle child : PluginChildren(plugin)) {
        if (PluginDisabled(child)) continue;
        if (!Cleanup(child)) clean = false;
        DisablePlugin(child);
    }
    return clean;
}

// Disable + free. A DLL whose hooks couldn't all be removed stays in memory (the game may still jump into it). A host
// plugin's DLL is never freed: it may have written raw patches the broker doesn't know about (docs/sdk/reference/
// hooks.md "Still raw") that point into it.
void Retire(PSEPluginHandle plugin, HMODULE module, const char* why) {
    const bool host = plugin && PluginHost(plugin);
    bool clean = plugin ? Cleanup(plugin) : true;
    if (plugin) DisablePlugin(plugin);
    if (!module) return;
    if (host) {
        Log(PSE_LOG_WARN, Format("%s: host plugin DLL kept in memory (%s)", PluginName(plugin), why));
        return;
    }
    if (clean) FreeLibrary(module);
    else Log(PSE_LOG_WARN, Format("%s: DLL kept in memory (%s, some of its patches are still in place)",
                                  plugin ? PluginName(plugin) : "?", why));
}

}  // namespace

// ---- manifest ------------------------------------------------------------------------------------------------------

bool ParseVersion(const std::string& text, uint32_t* out) {
    const std::string t = Trim(text);
    unsigned parts[3] = {0, 0, 0};
    int count = 0;
    size_t i = 0;
    while (true) {
        if (i >= t.size() || t[i] < '0' || t[i] > '9' || count == 3) return false;
        unsigned v = 0;
        while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
            v = v * 10 + static_cast<unsigned>(t[i] - '0');
            if (v > 4095) return false;
            ++i;
        }
        parts[count++] = v;
        if (i == t.size()) break;
        if (t[i] != '.') return false;
        ++i;
    }
    if (parts[0] > 255) return false;
    if (out) *out = PSE_MAKE_VERSION(parts[0], parts[1], parts[2]);
    return true;
}

std::string VersionString(uint32_t v) {
    return Format("%u.%u.%u", PSE_VERSION_MAJOR(v), PSE_VERSION_MINOR(v), PSE_VERSION_PATCH(v));
}

bool ParseManifest(const std::string& text, PluginManifest* out, std::string* error) {
    PluginManifest m;
    auto fail = [&](int line, const std::string& what) {
        if (error) *error = Format("line %d: ", line) + what;
        return false;
    };
    size_t pos = 0;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) pos = 3;   // UTF-8 BOM
    bool haveName = false;
    for (int lineNo = 1; pos <= text.size(); ++lineNo) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = Trim(line);
        if (line.empty()) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) return fail(lineNo, "expected \"key = value\"");
        const std::string key = Lower(Trim(line.substr(0, eq)));
        const std::string value = Trim(line.substr(eq + 1));
        auto names = [&](std::vector<std::string>* into) {
            for (const std::string& n : SplitList(value)) {
                if (!ValidPluginName(n.c_str())) return fail(lineNo, "\"" + n + "\" is not a valid plugin name");
                into->push_back(n);
            }
            return true;
        };
        if (key == "name") {
            if (haveName) return fail(lineNo, "name is given twice");
            if (!ValidPluginName(value.c_str()))
                return fail(lineNo, "\"" + value + "\" is not a valid plugin name ([a-z0-9_.-], 1-64 characters)");
            m.name = value;
            haveName = true;
        } else if (key == "requires") {
            for (const std::string& item : SplitList(value)) {
                size_t n = 0;
                while (n < item.size() && item[n] != ' ' && item[n] != '\t' && item[n] != '>' && item[n] != '=' &&
                       item[n] != '<')
                    ++n;
                PluginRequirement r;
                r.name = item.substr(0, n);
                if (!ValidPluginName(r.name.c_str()))
                    return fail(lineNo, "\"" + r.name + "\" is not a valid plugin name");
                const std::string rest = Trim(item.substr(n));
                if (!rest.empty()) {
                    if (rest.compare(0, 2, ">=") != 0)
                        return fail(lineNo, "\"" + item + "\": only \"name >= version\" is supported");
                    if (!ParseVersion(rest.substr(2), &r.minVersion))
                        return fail(lineNo, "\"" + item + "\": bad version (major.minor[.patch])");
                }
                m.required.push_back(r);
            }
        } else if (key == "optional") {
            if (!names(&m.optional)) return false;
        } else if (key == "conflicts") {
            if (!names(&m.conflicts)) return false;
        } else if (key == "after") {
            if (!names(&m.after)) return false;
        } else if (key == "host") {
            const std::string v = Lower(value);
            if (v == "true" || v == "yes" || v == "1") m.host = true;
            else if (v == "false" || v == "no" || v == "0") m.host = false;
            else return fail(lineNo, "host = true or false");
        } else {
            m.warnings.push_back(Format("line %d: unknown key \"%s\" (ignored)", lineNo, key.c_str()));
        }
    }
    if (!haveName) {
        if (error) *error = "no \"name = ...\" line";
        return false;
    }
    if (out) *out = std::move(m);
    return true;
}

// ---- resolution ----------------------------------------------------------------------------------------------------

std::vector<size_t> ResolvePluginOrder(std::vector<PluginCandidate>& c) {
    const size_t n = c.size();
    auto alive = [&](size_t i) { return c[i].refusal.empty(); };

    // Duplicate names, names already taken by a registered (internal) plugin.
    std::map<std::string, std::vector<size_t>> byName;
    for (size_t i = 0; i < n; ++i)
        if (alive(i)) byName[c[i].manifest.name].push_back(i);
    for (auto& [name, list] : byName) {
        if (list.size() > 1)
            for (size_t i : list) c[i].refusal = Format("%zu plugins are named %s", list.size(), name.c_str());
        else if (FindPlugin(name.c_str()))
            c[list[0]].refusal = "a plugin named " + name + " is already loaded (built in)";
    }
    auto candidate = [&](const std::string& name) -> int {   // index of the (alive or refused) candidate, -1 none
        for (size_t i = 0; i < n; ++i)
            if (c[i].manifest.name == name) return static_cast<int>(i);
        return -1;
    };
    auto available = [&](const std::string& name, uint32_t* version) {
        int i = candidate(name);
        if (i >= 0 && alive(static_cast<size_t>(i)) && byName[name].size() == 1) {
            *version = c[static_cast<size_t>(i)].version;
            return true;
        }
        if (PSEPluginHandle h = FindPlugin(name.c_str())) {
            *version = PluginInfo(h)->version;
            return true;
        }
        return false;
    };
    auto requirements = [&] {
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t i = 0; i < n; ++i) {
                if (!alive(i)) continue;
                for (const PluginRequirement& r : c[i].manifest.required) {
                    uint32_t v = 0;
                    std::string why;
                    if (r.name == c[i].manifest.name) why = "requires itself";
                    else if (!available(r.name, &v))
                        why = candidate(r.name) >= 0 ? "requires " + Requirement(r) + ", which is not loaded (" +
                                                           r.name + ": " + c[static_cast<size_t>(candidate(r.name))].refusal + ")"
                                                     : "requires " + Requirement(r) + ", which is not installed";
                    else if (v < r.minVersion)
                        why = "requires " + Requirement(r) + ", found " + r.name + " " + VersionString(v);
                    if (why.empty()) continue;
                    c[i].refusal = why;
                    changed = true;
                    break;
                }
            }
        }
    };

    requirements();
    // Conflicts against one snapshot: two plugins that name each other are both refused.
    std::vector<std::string> conflictRefusal(n);
    for (size_t i = 0; i < n; ++i) {
        if (!alive(i)) continue;
        for (const std::string& other : c[i].manifest.conflicts) {
            uint32_t v;
            if (other == c[i].manifest.name || !available(other, &v)) continue;
            conflictRefusal[i] = "conflicts with " + other + (FindPlugin(other.c_str()) ? " (built in)" : "");
            break;
        }
    }
    for (size_t i = 0; i < n; ++i)
        if (!conflictRefusal[i].empty()) c[i].refusal = conflictRefusal[i];
    requirements();

    // Topological order. Edges dep -> plugin for requires, after and optional (only between candidates still alive).
    std::vector<size_t> order;
    std::vector<bool> placed(n, false);
    std::vector<std::string> cycle(n);
    while (true) {
        auto deps = [&](size_t i) {
            std::vector<size_t> out;
            auto add = [&](const std::string& name) {
                int d = candidate(name);
                if (d >= 0 && static_cast<size_t>(d) != i && alive(static_cast<size_t>(d)) &&
                    std::find(out.begin(), out.end(), static_cast<size_t>(d)) == out.end())
                    out.push_back(static_cast<size_t>(d));
            };
            for (const PluginRequirement& r : c[i].manifest.required) add(r.name);
            for (const std::string& a : c[i].manifest.after) add(a);
            for (const std::string& o : c[i].manifest.optional) add(o);
            return out;
        };
        // Kahn over the alive, not yet placed candidates; ready ones by name.
        std::set<std::pair<std::string, size_t>> ready;
        std::vector<int> waiting(n, 0);
        std::vector<std::vector<size_t>> users(n);
        for (size_t i = 0; i < n; ++i) {
            if (!alive(i) || placed[i]) continue;
            for (size_t d : deps(i))
                if (!placed[d]) {
                    ++waiting[i];
                    users[d].push_back(i);
                }
            if (!waiting[i]) ready.insert({c[i].manifest.name, i});
        }
        while (!ready.empty()) {
            size_t i = ready.begin()->second;
            ready.erase(ready.begin());
            placed[i] = true;
            order.push_back(i);
            for (size_t u : users[i])
                if (--waiting[u] == 0) ready.insert({c[u].manifest.name, u});
        }
        std::vector<size_t> rest;
        for (size_t i = 0; i < n; ++i)
            if (alive(i) && !placed[i]) rest.push_back(i);
        if (rest.empty()) break;
        // Everything left waits on a cycle. Refuse the plugins ON a cycle (with the path), then whoever requires
        // them; plugins that only waited through `after` / `optional` get placed in the next round.
        bool refused = false;
        for (size_t start : rest) {
            // DFS from start back to start through unplaced alive candidates.
            std::vector<size_t> path;
            std::vector<bool> seen(n, false);
            std::function<bool(size_t)> dfs = [&](size_t i) -> bool {
                for (size_t d : deps(i)) {
                    if (placed[d]) continue;
                    if (d == start) return true;
                    if (seen[d]) continue;
                    seen[d] = true;
                    path.push_back(d);
                    if (dfs(d)) return true;
                    path.pop_back();
                }
                return false;
            };
            if (!dfs(start)) continue;
            std::string text = c[start].manifest.name;
            for (size_t p : path) text += " -> " + c[p].manifest.name;
            text += " -> " + c[start].manifest.name;
            cycle[start] = "dependency cycle: " + text;
            refused = true;
        }
        for (size_t i : rest)
            if (!cycle[i].empty()) c[i].refusal = cycle[i];
        if (!refused) {   // can't happen (a graph where every node waits contains a cycle) - never loop forever
            for (size_t i : rest) c[i].refusal = "unresolvable load order";
            break;
        }
        requirements();
    }
    return order;
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

// ---- loading -------------------------------------------------------------------------------------------------------

uint32_t LoadPlugins(const std::wstring& dir, const std::vector<std::string>& hostPlugins) {
    if (!detail::Initialized()) return 0;
    if (g_done) {
        Log(PSE_LOG_WARN, "plugins: LoadPlugins called twice - ignored");
        return 0;
    }
    g_done = true;
    const std::string dirText = detail::Narrow(dir);

    struct Entry {
        std::wstring manifestPath, dllPath;
        StaticInfo info;
    };
    std::vector<Entry> entries;
    std::vector<PluginCandidate> cands;

    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd{};
    HANDLE find = dir.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((dir + L"\\*.pse").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    if (files.empty())
        Log(PSE_LOG_INFO, "plugins: no *.pse manifests in " + (dirText.empty() ? std::string("(no folder)") : dirText));

    for (const std::wstring& file : files) {
        Entry e;
        e.manifestPath = dir + L"\\" + file;
        e.dllPath = dir + L"\\" + file.substr(0, file.size() - 4) + L".dll";
        PluginCandidate pc;
        const std::string manifestName = detail::Narrow(file);
        // Manifest text.
        std::string text;
        HANDLE h = CreateFileW(e.manifestPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            pc.refusal = "can't read the manifest: " + WinError(GetLastError());
        } else {
            LARGE_INTEGER size;
            if (GetFileSizeEx(h, &size) && size.QuadPart < 0x10000) {
                text.resize(static_cast<size_t>(size.QuadPart));
                DWORD got = 0;
                if (!text.empty() && (!ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &got, nullptr) ||
                                      got != text.size()))
                    pc.refusal = "can't read the manifest: " + WinError(GetLastError());
            } else {
                pc.refusal = "the manifest is larger than 64 KB";
            }
            CloseHandle(h);
        }
        std::string error;
        if (pc.refusal.empty() && !ParseManifest(text, &pc.manifest, &error)) pc.refusal = manifestName + " " + error;
        if (pc.manifest.name.empty()) pc.manifest.name = "(" + manifestName + ")";   // for the result list only
        for (const std::string& w : pc.manifest.warnings) Log(PSE_LOG_WARN, "plugins: " + manifestName + " " + w);
        if (pc.refusal.empty() && GetFileAttributesW(e.dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
            pc.refusal = "no " + detail::Narrow(file.substr(0, file.size() - 4)) + ".dll next to the manifest";
        if (pc.refusal.empty()) pc.refusal = ReadStaticInfo(e.dllPath, &e.info);
        if (pc.refusal.empty()) {
            const StaticInfo& in = e.info;
            pc.version = in.version;
            if (in.name != pc.manifest.name)
                pc.refusal = "PSEPlugin_Info.name is \"" + in.name + "\", the manifest says \"" + pc.manifest.name + "\"";
            else if (!in.hasLoad)
                pc.refusal = "the DLL has no PSEPlugin_Load export";
            else if (in.minApiVersion > PSE_API_VERSION)
                pc.refusal = Format("needs loader API %u, this loader has %u", in.minApiVersion, PSE_API_VERSION);
            else if (!in.anyGame &&
                     std::find(in.games.begin(), in.games.end(), detail::Cfg().gameId) == in.games.end()) {
                std::string list;
                for (const std::string& g : in.games) list += (list.empty() ? "" : ", ") + g;
                pc.refusal = "made for " + (list.empty() ? std::string("no game") : list) + "; this game is " +
                             detail::Cfg().gameId;
            }
        }
        entries.push_back(std::move(e));
        cands.push_back(std::move(pc));
    }

    // Host plugins (manifest "host = true" + on the game layer's list) first, in a round of their own; then the rest,
    // resolved against everything registered by then (so they may require a host's sub-plugins, "dsfix.hud").
    std::vector<bool> isHost(cands.size(), false);
    for (size_t i = 0; i < cands.size(); ++i) {
        if (!cands[i].manifest.host || !cands[i].refusal.empty()) continue;
        if (std::find(hostPlugins.begin(), hostPlugins.end(), cands[i].manifest.name) == hostPlugins.end())
            cands[i].refusal = "host = true, but the game layer doesn't allow " + cands[i].manifest.name +
                               " as a host plugin";
        else
            isHost[i] = true;
    }
    auto resolveRound = [&](bool hosts) {
        std::vector<size_t> idx;
        for (size_t i = 0; i < cands.size(); ++i)
            if (hosts ? isHost[i] : !isHost[i]) idx.push_back(i);   // refused ones too: their reasons are quoted
        std::vector<PluginCandidate> sub;
        for (size_t i : idx) {
            sub.push_back(cands[i]);
            if (hosts || !sub.back().refusal.empty()) continue;
            // A requirement on a host plugin that didn't make it: say so (the resolver would say "not installed").
            for (const PluginRequirement& req : sub.back().manifest.required) {
                if (FindPlugin(req.name.c_str())) continue;
                for (size_t j = 0; j < cands.size(); ++j)
                    if (cands[j].manifest.host && cands[j].manifest.name == req.name) {
                        sub.back().refusal = "requires " + Requirement(req) + ", a host plugin that is not loaded";
                        break;
                    }
                if (!sub.back().refusal.empty()) break;
            }
        }
        const std::vector<size_t> o = ResolvePluginOrder(sub);
        for (size_t k = 0; k < idx.size(); ++k) cands[idx[k]].refusal = sub[k].refusal;
        std::vector<size_t> out;
        for (size_t k : o) out.push_back(idx[k]);
        return out;
    };

    auto makeResult = [&](size_t i) {
        PluginLoadResult r;
        r.name = cands[i].manifest.name;
        r.displayName = entries[i].info.displayName;
        r.file = detail::Narrow(entries[i].info.name.empty() ? entries[i].manifestPath : entries[i].dllPath);
        r.version = cands[i].version;
        r.flags = entries[i].info.flags;
        return r;
    };

    uint32_t loaded = 0;
    auto loadRound = [&](const std::vector<size_t>& order) {
        for (size_t i : order) {
            PluginLoadResult r = makeResult(i);
            Entry& e = entries[i];
            auto finish = [&](PluginState state, const std::string& reason) {
                r.state = state;
                r.reason = reason;
                if (state == PluginState::Loaded)
                    Log(PSE_LOG_INFO, "plugins: loaded " + r.name + " " + VersionString(r.version) + " (" + r.file + ")");
                else
                    Log(PSE_LOG_ERROR, "plugins: " + r.name + " " + PluginStateName(state) + " - " + reason);
                std::lock_guard lock(g_stateMutex);
                g_results.push_back(r);
            };
            // A requirement may have failed to load since the plan was made.
            std::string missing;
            for (const PluginRequirement& req : cands[i].manifest.required)
                if (!FindPlugin(req.name.c_str())) {
                    missing = "requires " + Requirement(req) + ", which failed to load";
                    break;
                }
            if (!missing.empty()) {
                finish(PluginState::Refused, missing);
                continue;
            }

            HMODULE module = nullptr;
            Fault fault;
            if (!GuardedLoadLibrary(e.dllPath.c_str(), &module, &fault)) {
                finish(PluginState::Failed, "crashed while loading the DLL (DllMain): " + DescribeFault(fault));
                continue;
            }
            if (!module) {
                finish(PluginState::Failed, "LoadLibrary failed: " + WinError(GetLastError()));
                continue;
            }
            auto info = reinterpret_cast<const PSEPluginInfo*>(GetProcAddress(module, "PSEPlugin_Info"));
            auto load = reinterpret_cast<PSEPluginLoadFn>(GetProcAddress(module, "PSEPlugin_Load"));
            auto unload = reinterpret_cast<PSEPluginUnloadFn>(GetProcAddress(module, "PSEPlugin_Unload"));
            if (!info || !load || info->size < sizeof(PSEPluginInfo) || !info->name || r.name != info->name ||
                info->version != r.version) {
                FreeLibrary(module);
                finish(PluginState::Failed, "the loaded DLL's exports differ from the file that was checked");
                continue;
            }
            PSEPluginHandle handle = RegisterExternalPlugin(info);
            if (!handle) {
                FreeLibrary(module);
                finish(PluginState::Failed, "couldn't register the plugin (name taken?)");
                continue;
            }
            if (isHost[i]) SetPluginHost(handle);   // before its Load: its Query answers pse.host from the start
            r.handle = handle;
            int ok = 0;
            if (!detail::GuardedLoadFn(load, LoadInterfaceFor(handle), &ok, &fault)) {
                const std::string where = DescribeFault(fault);   // before the DLL goes: names it
                Retire(handle, module, "crashed in PSEPlugin_Load");
                finish(PluginState::Failed, "crashed in PSEPlugin_Load: " + where);
                continue;
            }
            if (PluginDisabled(handle)) {   // a callback of its own, reached from inside its Load, faulted
                std::string why;
                {
                    std::lock_guard lock(g_stateMutex);
                    why = g_crashReasons[handle];
                }
                if (!isHost[i]) FreeLibrary(module);   // cleaned up by PluginCrashed; Load returned, none of its code runs
                finish(PluginState::Failed, why + " (during PSEPlugin_Load)");
                continue;
            }
            if (!ok) {
                Retire(handle, module, "PSEPlugin_Load returned 0");
                finish(PluginState::Failed, "PSEPlugin_Load returned 0 (the plugin's log says why)");
                continue;
            }
            finish(PluginState::Loaded, "");
            {
                std::lock_guard lock(g_stateMutex);
                g_loaded.push_back({handle, module, unload, g_results.size() - 1});
            }
            ++loaded;
        }
    };
    loadRound(resolveRound(true));
    loadRound(resolveRound(false));

    // Refused before loading: after the rest, by name.
    std::vector<size_t> refused;
    for (size_t i = 0; i < cands.size(); ++i)
        if (!cands[i].refusal.empty()) refused.push_back(i);
    std::sort(refused.begin(), refused.end(),
              [&](size_t a, size_t b) { return cands[a].manifest.name < cands[b].manifest.name; });
    for (size_t i : refused) {
        PluginLoadResult r = makeResult(i);
        r.state = PluginState::Refused;
        r.reason = cands[i].refusal;
        Log(PSE_LOG_ERROR, "plugins: " + r.name + " refused - " + r.reason);
        std::lock_guard lock(g_stateMutex);
        g_results.push_back(r);
    }
    if (!files.empty())
        Log(PSE_LOG_INFO, Format("plugins: %u of %zu loaded from %s", loaded, files.size(), dirText.c_str()));

    Broadcast(PSE_MSG_POST_LOAD, nullptr, 0);
    Broadcast(PSE_MSG_POST_POST_LOAD, nullptr, 0);
    return loaded;
}

void UnloadPlugins() {
    std::vector<LoadedPlugin> loaded;
    {
        std::lock_guard lock(g_stateMutex);
        loaded = g_loaded;
    }
    for (auto it = loaded.rbegin(); it != loaded.rend(); ++it) {
        bool crashed;
        {
            std::lock_guard lock(g_stateMutex);   // another plugin's Unload may have made it crash just now
            crashed = std::any_of(g_loaded.begin(), g_loaded.end(),
                                  [&](const LoadedPlugin& l) { return l.handle == it->handle && l.crashed; });
        }
        const std::string name = PluginName(it->handle);
        if (crashed) {
            // Already disabled and cleaned up by PluginCrashed: no Unload call into code that faulted, and the DLL
            // stays mapped (the process is about to end anyway).
            Log(PSE_LOG_INFO, "plugins: " + name + " crashed earlier - not unloaded");
            continue;
        }
        Fault fault;
        std::string why;
        if (it->unload && !detail::GuardedUnloadFn(it->unload, &fault)) {
            why = "crashed in PSEPlugin_Unload: " + DescribeFault(fault);
            Log(PSE_LOG_ERROR, name + ": " + why);
            LogInterface()->Report(detail::LoaderHandle(), 1, "[fail] PSE plugin %s %s", name.c_str(), why.c_str());
        }
        // A host plugin (dsfix.dll) stays: Shutdown runs from its own window procedure, its sub-plugins' hooks keep
        // working until the process ends, and nothing of it is freed.
        if (!PluginHost(it->handle)) Retire(it->handle, it->module, "unloaded");
        {
            std::lock_guard lock(g_stateMutex);
            if (it->result < g_results.size()) {
                g_results[it->result].state = PluginState::Unloaded;
                if (!why.empty()) g_results[it->result].reason = why;
            }
        }
        Log(PSE_LOG_INFO, "plugins: unloaded " + name);
    }
    std::lock_guard lock(g_stateMutex);
    g_loaded.clear();
}

std::vector<PluginLoadResult> PluginResults() {
    std::lock_guard lock(g_stateMutex);
    return g_results;
}

namespace detail {
void ResetPlugins() {
    std::lock_guard lock(g_stateMutex);
    // Tests only: crashed plugins' DLLs too (nothing of theirs runs any more), so the next test maps a fresh copy.
    for (auto it = g_loaded.rbegin(); it != g_loaded.rend(); ++it) FreeLibrary(it->module);
    g_loaded.clear();
    g_results.clear();
    g_crashReasons.clear();
    g_done = false;
}

void PluginCrashed(PSEPluginHandle plugin, const char* where, const Fault& fault) {
    std::lock_guard lock(g_stateMutex);
    const std::string name = PluginName(plugin);
    const std::string what = DescribeFault(fault);
    if (PluginDisabled(plugin)) {   // two of its callbacks faulted at once (two threads): the first one handled it
        Log(PSE_LOG_ERROR, "plugins: " + name + " (already disabled) crashed again in " + where + ": " + what);
        return;
    }
    const std::string reason = std::string("crashed in ") + where + ": " + what;
    g_crashReasons[plugin] = reason;
    Log(PSE_LOG_ERROR, "plugins: " + name + " " + reason + " - disabled; its hooks, listeners, tasks, settings and "
                       "co-save callbacks are removed, the game continues");
    LogInterface()->Report(LoaderHandle(), 1, "[fail] PSE plugin %s %s - disabled", name.c_str(), reason.c_str());
    // Cleanup before DisablePlugin (the broker needs the handle valid to unhook it) - like a failed Load. Unlike a
    // failed Load the DLL is NOT freed: its code may still be running (an outer callback of the same plugin further
    // up this stack, a thread of its own).
    if (!Cleanup(plugin)) Log(PSE_LOG_WARN, name + ": some of its patches could not be removed");
    DisablePlugin(plugin);
    bool found = false;
    for (LoadedPlugin& l : g_loaded) {
        if (l.handle != plugin) continue;
        l.crashed = true;
        found = true;
        if (l.result < g_results.size()) {
            g_results[l.result].state = PluginState::Failed;
            g_results[l.result].reason = reason;
        }
    }
    if (!found)   // crashed during LoadPlugins before its own entry was added (a callback reached from its Load)
        for (PluginLoadResult& r : g_results)
            if (r.handle == plugin) {
                r.state = PluginState::Failed;
                r.reason = reason;
            }
}
}  // namespace detail

}  // namespace pse
