// Frame recorder (see framerec.h). A frame is the time between two calls of the frame-cap hook (framecap.cpp):
//   cap wait (frame limit)  -> [dispatch: logic ... render loop 0x40FD9E (FUN_0040ef70(1)) incl. Present] -> next
// Present (graphics.cpp) reports its own V-Sync wait and the driver's Present time; the render loop's time excludes
// presents made inside it; "logic" is what is left (game update, scripts, sound, level loading work).
// Game state per row: dispatch mode [0x60EC8C] (0 playing, 1 loading a level, 3 other), front-end state [0x617C18],
// cutscene bit [[0x63C948]+0x1B8C] & 1, level name 0x606880.
// Rows are buffered and appended to the file about twice a second (a few KB), the summary when recording stops.
#include "features/framerec.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
constexpr uint32_t kRenderCall = 0x40FD9E, kRenderLoop = 0x40EF70;
constexpr uint32_t kDispatchMode = 0x60EC8C, kFrontEndState = 0x617C18, kGameState = 0x63C948;
constexpr uint32_t kLevelName = 0x606880, kRenderer = 0x63C924;

LARGE_INTEGER g_freq;
bool g_on = false;
FILE* g_file = nullptr;
std::string g_buffer;
double g_start = 0, g_lastBegin = 0, g_lastFlush = 0, g_avg = 0;
uint32_t g_frame = 0;
bool g_inRender = false;

struct Frame {
    double vblank = 0, present = 0, presentInRender = 0, render = 0, overlay = 0, fileMs = 0, texMs = 0;
    int presents = 0, files = 0, missing = 0, textures = 0;
    double texMB = 0;
    std::string fileNames, notes;
} g_cur;

struct Hitch {
    uint32_t frame;
    double ms;
    std::string what;
};
std::vector<double> g_times;
std::vector<Hitch> g_hitches;

template <class T>
T Read(uintptr_t a) { return *reinterpret_cast<const T*>(a); }

const char* Label(int mode, bool frontEnd, bool cutscene, const Frame& f) {
    if (mode == 1 || f.files || f.textures) return "loading";
    if (frontEnd) return "menu";
    if (cutscene) return "cutscene";
    return mode == 0 ? "game" : "other";
}

void Flush() {
    if (g_file && !g_buffer.empty()) {
        fwrite(g_buffer.data(), 1, g_buffer.size(), g_file);
        fflush(g_file);
    }
    g_buffer.clear();
}

void Start() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    while (n > 0 && path[n - 1] != '\\') --n;
    snprintf(path + n, MAX_PATH - n, "DesertStormFix-frames-%04d%02d%02d-%02d%02d%02d.csv", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_file = fopen(path, "wb");
    if (!g_file) {  // game folder not writable (e.g. under Program Files): Documents\DesertStormFix
        char docs[MAX_PATH];
        if (GetEnvironmentVariableA("USERPROFILE", docs, MAX_PATH)) {
            const std::string dir = std::string(docs) + "\\Documents\\DesertStormFix";
            CreateDirectoryA(dir.c_str(), nullptr);
            const std::string name = path + n;
            snprintf(path, MAX_PATH, "%s\\%s", dir.c_str(), name.c_str());
            n = static_cast<DWORD>(dir.size() + 1);
            g_file = fopen(path, "wb");
        }
    }
    if (!g_file) return;
    g_on = true;
    g_start = g_lastBegin = g_lastFlush = framerec::Now();
    g_frame = 0;
    g_avg = 0;
    g_cur = {};
    g_times.clear();
    g_hitches.clear();
    const auto& s = settings::Get();
    const auto* r = Read<const uint8_t*>(kRenderer);
    char head[1024];
    snprintf(head, sizeof head,
             "# DesertStormFix " DS_VERSION " frame recording %04d-%02d-%02d %02d:%02d:%02d\r\n"
             "# back buffer %dx%d, display mode %u (0 fullscreen 1 windowed 2 borderless), MSAA %u, V-Sync setting %d, "
             "synced at %u fps, frame limit %u%s\r\n"
             "# times in ms. frame = start of this frame to the next; logic = frame - cap_wait - vblank_wait - present - "
             "render - overlay (game update, scripts, sound, level loading work)\r\n"
             "frame,time_s,frame_ms,cap_wait_ms,vblank_wait_ms,present_ms,render_ms,logic_ms,overlay_ms,presents,"
             "files,file_ms,files_missing,textures,texture_ms,texture_mb,mode,label,hitch,level,fe_state,cutscene,"
             "file_names,notes\r\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, r ? Read<int>(uintptr_t(r) + 0x40688) : 0,
             r ? Read<int>(uintptr_t(r) + 0x4068C) : 0, s.displayMode, s.antialiasing, s.vsync, features::VsyncFps(),
             s.fpsCap, s.fpsCapToRefresh ? " (or monitor refresh)" : "");
    g_buffer = head;
    Flush();
    dslog::Write("Frame recorder: started (%s)", path + n);
}

void Stop() {
    if (!g_on) return;
    g_on = false;
    std::vector<double> sorted = g_times;
    std::sort(sorted.begin(), sorted.end());
    char line[512];
    if (!sorted.empty()) {
        double sum = 0;
        for (double t : sorted) sum += t;
        const auto pct = [&](double p) { return sorted[std::min(sorted.size() - 1, size_t(sorted.size() * p))]; };
        snprintf(line, sizeof line,
                 "# summary: %zu frames, %.1f s, avg %.3f ms (%.1f fps), median %.3f, 99%% %.3f, 99.9%% %.3f, max %.3f, "
                 "hitches %zu\r\n",
                 sorted.size(), sum / 1000, sum / sorted.size(), 1000.0 * sorted.size() / sum, pct(0.5), pct(0.99),
                 pct(0.999), sorted.back(), g_hitches.size());
        g_buffer += line;
        std::sort(g_hitches.begin(), g_hitches.end(), [](const Hitch& a, const Hitch& b) { return a.ms > b.ms; });
        for (size_t i = 0; i < g_hitches.size() && i < 20; ++i) {
            snprintf(line, sizeof line, "# hitch frame %u: %.2f ms - %s\r\n", g_hitches[i].frame, g_hitches[i].ms,
                     g_hitches[i].what.c_str());
            g_buffer += line;
        }
        dslog::Write("Frame recorder: stopped - %zu frames, avg %.3f ms, 99%% %.3f ms, %zu hitches", sorted.size(),
                     sum / sorted.size(), pct(0.99), g_hitches.size());
    }
    Flush();
    fclose(g_file);
    g_file = nullptr;
}

void CsvText(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) out += c == '"' ? '\'' : c;
    out += '"';
}

using RenderFn = void(__cdecl*)(int);
void __cdecl RenderLoop(int a) {
    if (!g_on) return reinterpret_cast<RenderFn>(kRenderLoop)(a);
    const double t0 = framerec::Now();
    const double presentBefore = g_cur.present + g_cur.vblank;
    g_inRender = true;
    reinterpret_cast<RenderFn>(kRenderLoop)(a);
    g_inRender = false;
    const double inside = g_cur.present + g_cur.vblank - presentBefore;
    g_cur.presentInRender += inside;
    g_cur.render += framerec::Now() - t0 - inside;
}
}  // namespace

double framerec::Now() {
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(g_freq.QuadPart);
}

bool framerec::Recording() { return g_on; }

void framerec::Toggle() {
    if (g_on) Stop();
    else Start();
}

void framerec::BeginFrame(double capWaitMs) {
    if (!g_on) return;
    const double now = Now();
    const double frameMs = now - g_lastBegin;
    g_lastBegin = now;
    {  // the row for the frame that ends here (the first one began when recording started)
        const Frame& f = g_cur;
        const int mode = Read<int8_t>(kDispatchMode);
        const uint32_t fe = Read<uint32_t>(kFrontEndState);
        char level[20] = {};
        memcpy(level, reinterpret_cast<const char*>(kLevelName), sizeof level - 1);
        const bool frontEnd = _strnicmp(level, "FrontEnd", 8) == 0 || !level[0];
        const auto* state = Read<const uint8_t*>(kGameState);
        const bool cutscene = state && (Read<uint32_t>(uintptr_t(state) + 0x1B8C) & 1);
        const double logic = std::max(0.0, frameMs - capWaitMs - f.vblank - f.present - f.render - f.overlay);
        const bool hitch = g_avg > 0 && frameMs > g_avg * 2.5 && frameMs > 12.0;
        const char* label = Label(mode, frontEnd, cutscene, f);
        char row[512];
        snprintf(row, sizeof row, "%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%.3f,%d,%d,%.3f,%.2f,%d,%s,%d,%s,0x%X,%d,",
                 g_frame, (now - g_start) / 1000, frameMs, capWaitMs, f.vblank, f.present, f.render, logic, f.overlay,
                 f.presents, f.files, f.fileMs, f.missing, f.textures, f.texMs, f.texMB, mode, label, hitch ? 1 : 0,
                 level, fe, cutscene ? 1 : 0);
        g_buffer += row;
        CsvText(g_buffer, f.fileNames);
        g_buffer += ',';
        CsvText(g_buffer, f.notes);
        g_buffer += "\r\n";
        g_times.push_back(frameMs);
        if (hitch) {
            // the biggest share names the hitch
            const std::pair<double, const char*> parts[] = {
                {logic, "logic"}, {f.render, "render"}, {f.present, "present (driver / GPU)"},
                {f.vblank, "V-Sync wait"}, {capWaitMs, "frame-limit wait"}, {f.overlay, "overlay"}};
            const auto* top = &parts[0];
            for (const auto& p : parts)
                if (p.first > top->first) top = &p;
            char what[256];
            snprintf(what, sizeof what, "%s %.1f ms, %s%s%s", top->second, top->first, label,
                     f.files ? ", files " : "", f.files ? f.fileNames.substr(0, 120).c_str() : "");
            g_hitches.push_back({g_frame, frameMs, what});
        }
        // running average of normal frames (hitches don't pull it up)
        if (!hitch) g_avg = g_avg ? g_avg * 0.95 + frameMs * 0.05 : frameMs;
    }
    ++g_frame;
    g_cur = {};
    if (now - g_lastFlush > 500) {
        g_lastFlush = now;
        Flush();
    }
}

void framerec::AddRender(double ms) { g_cur.render += ms; }

void framerec::AddPresent(double vblankWaitMs, double presentMs) {
    if (!g_on) return;
    g_cur.vblank += vblankWaitMs;
    g_cur.present += presentMs;
    ++g_cur.presents;
}

void framerec::AddFile(const char* name, double ms, bool found) {
    if (!g_on) return;
    ++g_cur.files;
    g_cur.fileMs += ms;
    if (!found) ++g_cur.missing;
    if (g_cur.fileNames.size() < 200) {
        if (!g_cur.fileNames.empty()) g_cur.fileNames += ' ';
        g_cur.fileNames += name;
        if (!found) g_cur.fileNames += "(missing)";
    }
}

void framerec::AddTexture(uint32_t w, uint32_t h, uint32_t format, double ms) {
    if (!g_on) return;
    ++g_cur.textures;
    g_cur.texMs += ms;
    const double bpp = format == 21 || format == 22 ? 4 : format == 23 || format == 25 || format == 26 ? 2 : 1;
    g_cur.texMB += w * h * bpp / (1024.0 * 1024.0);
}

void framerec::AddOverlay(double ms) {
    if (g_on) g_cur.overlay += ms;
}

void framerec::Note(const char* text) {
    if (!g_on) return;
    if (!g_cur.notes.empty()) g_cur.notes += "; ";
    g_cur.notes += text;
}

void framerec::ApplyHooks() {
    static bool hooked = false;
    if (hooked) return;
    hooked = patch::HookCall(kRenderCall, reinterpret_cast<const void*>(&RenderLoop), kRenderLoop);
    dslog::Write("Frame recorder: render loop hook %s", hooked ? "installed" : "FAILED");
}
