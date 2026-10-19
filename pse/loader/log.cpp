// pse.log: formatted lines to the sink (DesertStormFix.log) and to <logDir>\<plugin>.log. Any thread.
#include <windows.h>

#include <cstdio>
#include <map>
#include <mutex>

#include "internal.h"

namespace pse {
namespace {

std::mutex g_mutex;                         // one line at a time: sink and files never see interleaved text
std::map<std::string, FILE*> g_files;       // per plugin; opened ("w") on the first line since Init
LARGE_INTEGER g_start{}, g_freq{};

const char* LevelName(PSELogLevel level) {
    switch (level) {
        case PSE_LOG_DEBUG: return "DEBUG";
        case PSE_LOG_INFO: return "INFO";
        case PSE_LOG_WARN: return "WARN";
        case PSE_LOG_ERROR: return "ERROR";
    }
    return "INFO";
}

std::string Stamp() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    unsigned long long ms = g_freq.QuadPart ? static_cast<unsigned long long>((now.QuadPart - g_start.QuadPart) * 1000 /
                                                                              g_freq.QuadPart)
                                            : 0;
    char b[32];
    std::snprintf(b, sizeof b, "[%02llu:%02llu.%03llu]", ms / 60000, ms / 1000 % 60, ms % 1000);
    return b;
}

// Unknown handles still log (as "?"), but get no file.
void Emit(PSEPluginHandle plugin, PSELogLevel level, const std::string& text) {
    const char* name = PluginName(plugin);
    bool known = *name != 0;
    std::string line = Stamp() + " [" + (known ? name : "?") + "] " + LevelName(level) + " " + text;
    std::lock_guard lock(g_mutex);
    const Config& cfg = detail::Cfg();
    if (cfg.logSink) cfg.logSink(line.c_str());
    if (!known || cfg.logDir.empty()) return;
    auto it = g_files.find(name);
    if (it == g_files.end()) {
        std::wstring path = detail::Widen(cfg.logDir + "\\" + name + ".log");
        FILE* f = _wfopen(path.c_str(), L"wb");   // rewritten each start
        it = g_files.emplace(name, f).first;      // nullptr remembered too: don't retry a failing open per line
    }
    if (it->second) {
        std::fputs(line.c_str(), it->second);
        std::fputs("\r\n", it->second);
        std::fflush(it->second);   // lines must survive a game crash
    }
}

void PSE_CALL Write(PSEPluginHandle plugin, PSELogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::string text = detail::VFormat(fmt, args);
    va_end(args);
    Emit(plugin, level, text);
}

void PSE_CALL Report(PSEPluginHandle plugin, int error, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::string text = detail::VFormat(fmt, args);
    va_end(args);
    Emit(plugin, error ? PSE_LOG_ERROR : PSE_LOG_INFO, text);
    const char* name = PluginName(plugin);
    std::string line = std::string("[") + (*name ? name : "?") + "] " + text;
    std::lock_guard lock(g_mutex);
    if (detail::Cfg().reportSink) detail::Cfg().reportSink(line.c_str(), error ? 1 : 0);
}

const PSELogInterface kTable = {sizeof(PSELogInterface), 1, &Write, &Report};

}  // namespace

namespace detail {

void InitLog() {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
}

void ResetLog() {
    std::lock_guard lock(g_mutex);
    for (auto& [name, f] : g_files)
        if (f) std::fclose(f);
    g_files.clear();
}

}  // namespace detail

void LogLine(PSEPluginHandle plugin, PSELogLevel level, const char* text) {
    if (!PluginName(plugin)[0]) plugin = detail::LoaderHandle();
    Emit(plugin, level, text ? text : "");
}

const PSELogInterface* LogInterface() { return &kTable; }

}  // namespace pse
