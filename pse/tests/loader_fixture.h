#pragma once
// Shared set-up for the loader service tests: fresh loader, captured log / report lines, throw-away registry key.
#include <windows.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "loader.h"
#include "pse_test.h"

namespace pse_fixture {

struct Captured {
    std::mutex m;
    std::vector<std::string> lines;
    std::vector<std::pair<std::string, int>> reports;
};
inline Captured& Cap() {
    static Captured c;
    return c;
}
inline void Sink(const char* line) {
    std::lock_guard lock(Cap().m);
    Cap().lines.emplace_back(line);
}
inline void ReportSink(const char* line, int error) {
    std::lock_guard lock(Cap().m);
    Cap().reports.emplace_back(line, error);
}
inline std::vector<std::string> Lines() {
    std::lock_guard lock(Cap().m);
    return Cap().lines;
}
inline bool AnyLineContains(const char* text) {
    for (const std::string& l : Lines())
        if (l.find(text) != std::string::npos) return true;
    return false;
}

// HKCU key no real install uses; deleted by DeleteTestKey().
inline std::string TestRegistryRoot() { return "Software\\PSE-test-" + std::to_string(GetCurrentProcessId()); }
inline void DeleteTestKey() { RegDeleteTreeA(HKEY_CURRENT_USER, TestRegistryRoot().c_str()); }

inline pse::Config Fresh(const std::string& logDir = "") {
    pse::ResetForTests();
    {
        std::lock_guard lock(Cap().m);
        Cap().lines.clear();
        Cap().reports.clear();
    }
    pse::Config c;
    c.gameId = "ds1";
    c.registryRoot = TestRegistryRoot();
    c.logDir = logDir;
    c.logSink = &Sink;
    c.reportSink = &ReportSink;
    pse::Init(c);
    return c;
}

// A plugin info with static storage per name.
inline PSEPluginHandle AddPlugin(const char* name, uint32_t version = PSE_MAKE_VERSION(1, 0, 0)) {
    static std::vector<std::unique_ptr<PSEPluginInfo>> infos;
    auto info = std::make_unique<PSEPluginInfo>();
    *info = {sizeof(PSEPluginInfo), 1, name, name, "test", version, 0, nullptr, PSE_PLUGIN_CLIENT_ONLY};
    PSEPluginHandle h = pse::RegisterInternalPlugin(info.get());
    infos.push_back(std::move(info));
    return h;
}

inline std::string TempPath(const char* name) {
    char dir[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    return std::string(dir) + "pse-test-" + std::to_string(GetCurrentProcessId()) + "-" + name;
}

}  // namespace pse_fixture
