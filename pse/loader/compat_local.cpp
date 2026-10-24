// The online plugin-set check's local side (compat.cpp has the encoding and the comparison): the running loader's
// plugins with their hashes. Loader state - pse_core only.
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>

#include "internal.h"

namespace pse::compat {

// ---- the running loader's plugins ----------------------------------------------------------------------------------

namespace {
using FingerprintFn = const char*(PSE_CALL*)(void);
// The plugin's PSEPlugin_SimFingerprint() under SEH (plugin code; a crash = no fingerprint, the file hash is used).
const char* CallFingerprint(FingerprintFn fn) {
    __try {
        return fn();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// "fp:" + SHA-256 of the plugin's declared simulation fingerprint, or "" when the loaded DLL exports none.
std::string DeclaredFingerprint(const std::string& path) {
    HMODULE m = GetModuleHandleW(detail::Widen(path).c_str());
    if (!m) return {};
    auto fn = reinterpret_cast<FingerprintFn>(GetProcAddress(m, "PSEPlugin_SimFingerprint"));
    if (!fn) return {};
    const char* s = CallFingerprint(fn);
    if (!s || !*s) return {};
    Sha256 h;
    h.Add(s, strnlen(s, 4096));
    const std::string hex = h.Hex();
    return hex.empty() ? std::string() : kFingerprintPrefix + hex;
}
}  // namespace

std::vector<PluginEntry> LocalPlugins() {
    // External DLL paths from the load results; hashed once per file (a session can start many times).
    static std::map<std::string, std::string> cache;   // path -> SHA-256
    std::map<PSEPluginHandle, std::string> files;
    for (const PluginLoadResult& r : PluginResults())
        if (r.handle && r.state == PluginState::Loaded) files[r.handle] = r.file;
    std::vector<PluginEntry> out;
    for (PSEPluginHandle h : detail::PluginsInOrder()) {   // disabled (crashed / unloaded) plugins are left out
        const PSEPluginInfo* info = PluginInfo(h);
        if (!info) continue;
        PluginEntry e;
        e.name = PluginName(h);
        e.version = info->version;
        e.flags = info->flags;
        // Built in: compiled into the loader / game layer / a host plugin (sub-plugins), or the host plugin itself
        // (DesertStormFix's dsfix.dll) - covered by the host's simulation fingerprint, not by a file hash, so a dev
        // build and the release package of the same source still connect (docs/sdk/online-compat.md).
        if (!PluginExternal(h) || PluginHost(h)) {
            e.hash = kBuiltIn;
        } else if (Simulation(info->flags)) {
            auto f = files.find(h);
            if (f != files.end()) {
                auto c = cache.find(f->second);
                if (c == cache.end()) {
                    // A declared simulation fingerprint (PSEPlugin_SimFingerprint, docs/sdk/online-compat.md) stands
                    // for the source, so a dev and a release build of one plugin connect; else the file's SHA-256.
                    std::string hex = DeclaredFingerprint(f->second);
                    if (hex.empty() && !Sha256File(detail::Widen(f->second), &hex)) hex = "unreadable";
                    c = cache.emplace(f->second, hex).first;
                }
                e.hash = c->second;
            } else {
                e.hash = "unknown";
            }
        }
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const PluginEntry& a, const PluginEntry& b) { return a.name < b.name; });
    return out;
}

}  // namespace pse::compat
