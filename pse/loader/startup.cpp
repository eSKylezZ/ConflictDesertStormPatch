// Start-up of pse_loader.dll (docs/sdk/layout.md): exe detection through the game layers in PSE\Games, the loader
// services, the game layer's Init, the plugins in PSE\Plugins (host plugins first). Plus the log routing: every loader
// / plugin line goes to the sink a host plugin sets (DesertStormFix: DesertStormFix.log); lines from before that are
// kept and handed over when it does.
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "internal.h"

namespace pse {
extern std::wstring g_pseDirForHost;   // host.cpp
extern bool g_hostShutdownDone;

namespace {

// ---- log routing ---------------------------------------------------------------------------------------------------

std::mutex g_logMutex;
PSEHostLogSink g_hostLog = nullptr;
PSEHostReportSink g_hostReport = nullptr;
void (*g_fallbackLog)(const char*) = nullptr;
void (*g_fallbackReport)(const char*, int) = nullptr;
bool g_collecting = true;                          // until Start returns: keep lines for a host sink
std::vector<std::string> g_backlog;
std::vector<std::pair<std::string, int>> g_reportBacklog;
constexpr size_t kBacklogMax = 4000;

void LogSink(const char* line) {
    std::lock_guard lock(g_logMutex);
    if (g_hostLog) {
        g_hostLog(line);
        return;
    }
    if (g_collecting && g_backlog.size() < kBacklogMax) g_backlog.emplace_back(line);
    if (g_fallbackLog) g_fallbackLog(line);
}

void ReportSink(const char* line, int error) {
    std::lock_guard lock(g_logMutex);
    if (g_hostReport) {
        g_hostReport(line, error);
        return;
    }
    if (g_collecting && g_reportBacklog.size() < kBacklogMax) g_reportBacklog.emplace_back(line, error);
    if (g_fallbackReport) g_fallbackReport(line, error);
}

// Lines from before pse::Init (no plugin registry yet): logged as the loader once it exists, else straight out.
std::vector<std::pair<PSELogLevel, std::string>> g_early;
void Early(PSELogLevel level, const std::string& text) { g_early.emplace_back(level, text); }
void FlushEarly(bool initialized) {
    for (const auto& [level, text] : g_early) {
        if (initialized) {
            LogLine(detail::LoaderHandle(), level, text.c_str());
        } else {
            const std::string line = std::string("[pse] ") + (level >= PSE_LOG_ERROR ? "ERROR " : level == PSE_LOG_WARN ? "WARN " : "INFO ") + text;
            LogSink(line.c_str());
        }
    }
    g_early.clear();
}

std::string Format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::string s = detail::VFormat(fmt, args);
    va_end(args);
    return s;
}

std::string WinError(DWORD e) {
    char buf[256] = {};
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, buf, sizeof buf,
                             nullptr);
    while (n && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
    return Format("error %lu%s%s", e, n ? ": " : "", buf);
}

// ---- game layers ---------------------------------------------------------------------------------------------------

bool g_started = false;
HMODULE g_layerModule = nullptr;

int DetectFilter(EXCEPTION_POINTERS* ep, detail::Fault* f) {
    f->code = ep->ExceptionRecord->ExceptionCode;
    f->address = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
int GuardedDetect(PSEGameDetectFn fn, const PSEGameDetectInfo* d, int* result, detail::Fault* fault) {
    __try {
        *result = fn(d);
        return 1;
    } __except (DetectFilter(GetExceptionInformation(), fault)) {
        return 0;
    }
}
int GuardedLoadLibrary(const wchar_t* path, HMODULE* out, detail::Fault* fault) {
    __try {
        *out = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        return 1;
    } __except (DetectFilter(GetExceptionInformation(), fault)) {
        return 0;
    }
}

bool ValidId(const char* id) {
    if (!id || !*id || std::strlen(id) > 16) return false;
    for (const char* p = id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) return false;
    return true;
}

}  // namespace

void SetHostLogSink(PSEHostLogSink sink) {
    std::lock_guard lock(g_logMutex);
    g_hostLog = sink;
    if (!sink) return;
    for (const std::string& line : g_backlog) sink(line.c_str());
    g_backlog.clear();
}

void SetHostReportSink(PSEHostReportSink sink) {
    std::lock_guard lock(g_logMutex);
    g_hostReport = sink;
    if (!sink) return;
    for (const auto& [line, error] : g_reportBacklog) sink(line.c_str(), error);
    g_reportBacklog.clear();
}

std::string ExeSha256(const std::wstring& path) {
    const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    LARGE_INTEGER size{};
    std::vector<uint8_t> data;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0x40 && size.QuadPart < (256ll << 20)) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr) || got != data.size()) data.clear();
    }
    CloseHandle(f);
    if (data.size() < 0x40) return "";
    // IMAGE_FILE_HEADER.Characteristics = e_lfanew + 4 + 18: the 4 GB patch's Large Address Aware bit doesn't count.
    uint32_t lfanew = 0;
    std::memcpy(&lfanew, &data[0x3C], 4);
    if (static_cast<uint64_t>(lfanew) + 4 + 20 <= data.size()) data[lfanew + 4 + 18] &= static_cast<uint8_t>(~0x20);
    std::string hex = compat::Sha256Hex(data.data(), data.size());
    for (char& c : hex) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return hex;
}

StartResult Start(const StartOptions& o) {
    StartResult r;
    if (g_started) {
        r.why = "pse::Start called twice";
        return r;
    }
    g_started = true;
    g_fallbackLog = o.fallbackLog;
    g_fallbackReport = o.fallbackReport;
    g_pseDirForHost = o.pseDir;
    const std::string pseDirText = detail::Narrow(o.pseDir);
    auto vanilla = [&](const std::string& why) {
        r.why = why;
        Early(PSE_LOG_WARN, "start-up: " + why + " - the game runs without PSE plugins");
        FlushEarly(false);
        std::lock_guard lock(g_logMutex);
        g_collecting = false;
        g_backlog.clear();
        g_reportBacklog.clear();
        return r;
    };

    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    Early(PSE_LOG_INFO, Format("start-up: Pivotal Script Extender, API %u, folder %s", PSE_API_VERSION,
                               pseDirText.c_str()));
    r.exeSha256 = ExeSha256(o.exePath);
    Early(PSE_LOG_INFO, "start-up: exe " + detail::Narrow(o.exePath) + " SHA-256 " +
                            (r.exeSha256.empty() ? std::string("(unreadable)") : r.exeSha256) +
                            Format(", base 0x%08X, PE time stamp 0x%08X", o.imageBase, o.peTimestamp));

    // ---- which game layer recognises the exe ----
    const std::wstring gamesDir = o.pseDir + L"\\Games";
    std::vector<std::wstring> layers;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((gamesDir + L"\\pse_*.dll").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) layers.push_back(fd.cFileName);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    std::sort(layers.begin(), layers.end(),
              [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    if (layers.empty()) return vanilla("no game layer (" + detail::Narrow(gamesDir) + "\\pse_*.dll)");

    const PSEGameLayerInfo* info = nullptr;
    PSEGameInitFn init = nullptr;
    PSEGameDetectInfo detect{sizeof(PSEGameDetectInfo), r.exeSha256.c_str(), o.imageBase, o.peTimestamp,
                             o.exePath.c_str()};
    for (const std::wstring& file : layers) {
        const std::wstring path = gamesDir + L"\\" + file;
        const std::string name = detail::Narrow(file);
        HMODULE m = nullptr;
        detail::Fault fault;
        if (!GuardedLoadLibrary(path.c_str(), &m, &fault)) {
            Early(PSE_LOG_ERROR, "start-up: game layer " + name + " crashed while loading: " + detail::DescribeFault(fault));
            continue;
        }
        if (!m) {
            Early(PSE_LOG_ERROR, "start-up: game layer " + name + " couldn't be loaded: " + WinError(GetLastError()));
            continue;
        }
        auto li = reinterpret_cast<const PSEGameLayerInfo*>(GetProcAddress(m, "PSEGame_Info"));
        auto det = reinterpret_cast<PSEGameDetectFn>(GetProcAddress(m, "PSEGame_Detect"));
        auto ini = reinterpret_cast<PSEGameInitFn>(GetProcAddress(m, "PSEGame_Init"));
        if (!li || !det || !ini || li->size < sizeof(PSEGameLayerInfo) || li->infoVersion != 1 || !ValidId(li->gameId) ||
            !li->plugin || li->plugin->size < sizeof(PSEPluginInfo) || !ValidPluginName(li->plugin->name)) {
            Early(PSE_LOG_ERROR, "start-up: " + name + " is not a PSE game layer (PSEGame_Info / Detect / Init)");
            FreeLibrary(m);
            continue;
        }
        int mine = 0;
        if (!GuardedDetect(det, &detect, &mine, &fault)) {
            Early(PSE_LOG_ERROR, "start-up: " + name + " crashed in PSEGame_Detect: " + detail::DescribeFault(fault));
            FreeLibrary(m);
            continue;
        }
        if (mine != 1 || g_layerModule) {
            if (mine == 1) Early(PSE_LOG_WARN, "start-up: " + name + " also recognises the exe - the first layer wins");
            else Early(PSE_LOG_INFO, "start-up: " + name + " (" + li->gameId + ") doesn't recognise this exe");
            FreeLibrary(m);
            continue;
        }
        g_layerModule = m;
        info = li;
        init = ini;
        r.gameLayer = path;
        Early(PSE_LOG_INFO, "start-up: game layer " + name + " recognises the exe: game " + li->gameId);
    }
    if (!g_layerModule) return vanilla("no game layer recognises this exe");

    // ---- loader services, then the layer, then the plugins ----
    Config c;
    c.gameId = info->gameId;   // the layer stays loaded: its strings stay valid
    c.registryRoot = o.registryRoot.empty() ? std::string("Software\\PSE\\") + info->gameId : o.registryRoot;
    c.logSink = &LogSink;
    c.reportSink = &ReportSink;
    Init(c);
    FlushEarly(true);
    r.gameId = info->gameId;

    const PSEPluginHandle layer = RegisterInternalPlugin(info->plugin);
    if (!layer) {
        r.why = std::string("the game layer's plugin ") + info->plugin->name + " couldn't be registered";
        LogLine(detail::LoaderHandle(), PSE_LOG_ERROR, ("start-up: " + r.why + " - no plugins loaded").c_str());
        std::lock_guard lock(g_logMutex);
        g_collecting = false;
        return r;
    }
    SetPluginHost(layer);
    int ok = 0;
    detail::Fault fault;
    if (!detail::GuardedLoadFn(init, LoadInterfaceFor(layer), &ok, &fault) || !ok) {
        r.why = std::string("game layer ") + info->plugin->name +
                (ok ? "" : " PSEGame_Init failed") + (fault.code ? " crashed in PSEGame_Init: " + detail::DescribeFault(fault) : "");
        LogLine(detail::LoaderHandle(), PSE_LOG_ERROR, ("start-up: " + r.why + " - no plugins loaded").c_str());
        DisablePlugin(layer);
        std::lock_guard lock(g_logMutex);
        g_collecting = false;
        return r;
    }
    std::vector<std::string> hosts;
    for (const char* const* h = info->hostPlugins; h && *h; ++h) hosts.emplace_back(*h);
    r.running = true;
    r.pluginsLoaded = LoadPlugins(o.pseDir + L"\\Plugins", hosts);
    Broadcast(PSE_MSG_GAME_ENTRY, nullptr, 0);
    QueryPerformanceCounter(&t1);
    LogLine(detail::LoaderHandle(), PSE_LOG_INFO,
            Format("start-up: done in %.1f ms (%u plugin DLL(s)) - the game's entry point runs next",
                   static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart),
                   r.pluginsLoaded)
                .c_str());
    std::lock_guard lock(g_logMutex);
    g_collecting = false;
    if (!g_hostLog) g_backlog.clear();
    if (!g_hostReport) g_reportBacklog.clear();
    return r;
}

namespace detail {
void ResetStartup() {
    {
        std::lock_guard lock(g_logMutex);
        g_hostLog = nullptr;
        g_hostReport = nullptr;
        g_fallbackLog = nullptr;
        g_fallbackReport = nullptr;
        g_collecting = true;
        g_backlog.clear();
        g_reportBacklog.clear();
    }
    g_early.clear();
    if (g_layerModule) FreeLibrary(g_layerModule);
    g_layerModule = nullptr;
    g_started = false;
    g_hostShutdownDone = false;
    g_pseDirForHost.clear();
}
}  // namespace detail

}  // namespace pse
