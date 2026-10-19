// Developer console: the ` key (the key left of 1, scan code 0x29 on every layout) opens a box over the top of the
// screen in the front end and in missions. Always available in dev builds; in release builds with OPTIONS >
// DESERTSTORMFIX > DEVELOPER CONSOLE (FeatDevConsole, off by default). Commands: help, level <name>, levels, restart,
// mods, net (online connection test, core/net.h), pse plugins / hooks (plugin host, pse/loader), clear. Every command and its output also goes to the log. The console closes when a level starts loading (its
// loading screen waits for a key).
//
// Keyboard: the game reads its keyboard from window messages (window proc 0x411310 -> key table 0x753940), so the
// game window [0x606A60] is subclassed: while the console is open, WM_KEYDOWN / WM_CHAR go to the console and never
// reach the game (key-ups still do, so a key held when the console opened is released in the game too). Our own
// screens read keys through features::KeyHeld, which answers no while the console is open. The mouse and pads still
// reach the game (it isn't paused).
//
// Starting a level = what the game does when a front-end screen's update returns 3 (mission list pick, DIFFICULTY,
// mission end Continue): the game update FUN_0040f830 calls the front-end update FUN_0047d170 at 0x40FCB9; on 3 it
// sets dispatch mode [0x60EC8C] = 1 and the next frame's dispatcher runs the level load 0x40DE70, which unloads the
// running level (0x40D990) and loads the name in the next-level slot *(char**)0x5E7028 (buffer 0x5E6F28, 256 bytes),
// also when it is the same level (an empty name would open the level-select dialog instead). After the 3 the game
// picks the front-end state: [0x60696C] != 0 -> main menu (1), else campaign index -1 -> in game (0xE).
// The call at 0x40FCB9 is hooked: with a level pending and the front end idle ([0x617C20] = 0, no transition
// [0x617C32] / [0x617C33] - the state setter ignores calls during one), the name goes into the slot, the campaign is
// reset like the CHEATS mission list does (FUN_004576C0 on [0x60EDC0], [0x60EE14] = 1, FUN_004475B0(1)), [0x60696C] =
// 1 for FrontEnd.dll, the scene name [0x606880] is cleared (features that key on it - loadout screen, time of day -
// see a level change even on a restart) and 3 is returned. Single player unless a co-op session is running: the
// split-screen code takes the player count from the session as for any level load.
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/pse_host.h"
#include "core/net.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/overlay.h"
#include "loader.h"

namespace {
constexpr uint32_t kGameWindow = 0x606A60, kRenderer = 0x63C924, kLevelName = 0x606880, kNextLevel = 0x5E7028;
constexpr uint32_t kFrontEndUpdateCall = 0x40FCB9, kFrontEndUpdate = 0x47D170;
constexpr uint32_t kCampaign = 0x60EDC0, kCampaignReset = 0x4576C0, kDirectLevel = 0x60EE14, kDebugKeys = 0x4475B0;
constexpr uint32_t kToMainMenu = 0x60696C, kFeBusy = 0x617C20, kFeAnimIn = 0x617C32, kFeAnimOut = 0x617C33;
constexpr uint32_t kFrontEndState = 0x617C18, kDispatchMode = 0x60EC8C;
constexpr int kTexW = 2048, kTexH = 1024, kShownLines = 12, kMaxLines = 300;

bool g_open = false, g_hooked = false;
HWND g_wnd = nullptr;
WNDPROC g_prevProc = nullptr;
bool g_eatChar = false;       // the WM_CHAR of the toggle key
std::string g_input;
std::deque<std::string> g_lines;
std::vector<std::string> g_history;
int g_historyPos = 0;          // == size: the line being typed
int g_scroll = 0;              // lines scrolled back from the newest
std::string g_pending;         // level to start
std::string g_lastLoaded;      // level name of the last level load (restart)
bool g_plainRestart = false;   // g_pending is the pause menu's RESTART LEVEL (works without the console)
bool g_dirty = true;
void* g_tex = nullptr;
void* g_texDevice = nullptr;
int g_w = 0, g_h = 0, g_texUsedW = 0, g_drawW = 0;   // g_drawW: back-buffer width this frame
std::vector<uint32_t> g_pixels;
DWORD g_lastBlink = 0;
bool g_caret = true;
std::vector<std::string>* g_capture = nullptr;   // ConsoleRun (devserver.cpp): the lines one command prints

template <class T>
T& Global(uint32_t va) {
    return *reinterpret_cast<T*>(va);
}

bool Enabled() {
#ifndef DS_DIST
    return true;
#else
    return settings::Get().featDevConsole;
#endif
}

void Print(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    dslog::Write("Console: %s", buf);
    if (g_capture) g_capture->emplace_back(buf);
    g_lines.emplace_back(buf);
    while (static_cast<int>(g_lines.size()) > kMaxLines) g_lines.pop_front();
    g_scroll = 0;
    g_dirty = true;
}

std::string GameDir() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir(path);
    return dir.substr(0, dir.find_last_of("\\/"));
}

bool EndsWithI(const std::string& s, const char* tail) {
    const size_t n = strlen(tail);
    return s.size() >= n && _stricmp(s.c_str() + s.size() - n, tail) == 0;
}

// ---- levels ----
struct Level {
    std::string name, source;   // source: "game" or the mod's folder name (+ " (dllpatch)")
};

bool GameLevelName(const char* file) {
    for (const char* p : {"Mission", "Training", "MP_", "FrontEnd", "Outro"})
        if (_strnicmp(file, p, strlen(p)) == 0) return true;
    return false;
}

std::vector<Level> AllLevels() {
    std::vector<Level> out;
    for (const char* pattern : {"\\*.dll", "\\*.lua"}) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((GameDir() + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (pattern[3] == 'l' || GameLevelName(fd.cFileName)) out.push_back({fd.cFileName, "game"});
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    for (const features::ModLevel& m : features::ModLevels())
        out.push_back({m.name, m.mod + (m.built ? " (dllpatch)" : "")});
    std::sort(out.begin(), out.end(), [](const Level& a, const Level& b) {
        if (a.source != b.source) return a.source == "game" || (b.source != "game" && a.source < b.source);
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

// "mission6" / "Mission6.dll" / "myop.dll" / "ambush.lua" -> the file name as found (a mod's copy wins, like the
// game's loaders), "" = no such level.
std::string Resolve(std::string name, std::string& source) {
    while (!name.empty() && (name.back() == ' ' || name.back() == '"')) name.pop_back();
    while (!name.empty() && (name.front() == ' ' || name.front() == '"')) name.erase(0, 1);
    if (name.empty()) return {};
    std::vector<std::string> tries;
    if (EndsWithI(name, ".dll") || EndsWithI(name, ".lua")) tries.push_back(name);
    else tries = {name + ".dll", name + ".lua"};
    const std::vector<Level> all = AllLevels();
    for (const std::string& t : tries) {
        for (const Level& l : all)   // mods first: their copy is what the game loads
            if (l.source != "game" && _stricmp(l.name.c_str(), t.c_str()) == 0) {
                source = l.source;
                return l.name;
            }
        for (const Level& l : all)
            if (l.source == "game" && _stricmp(l.name.c_str(), t.c_str()) == 0) {
                source = "game";
                return l.name;
            }
    }
    return {};
}

std::string CurrentLevel() {
    char scene[32] = {};
    memcpy(scene, reinterpret_cast<const char*>(kLevelName), sizeof scene - 1);
    return scene;
}

// ---- commands ----
void Help() {
    Print("help              this list");
    Print("level <name>      start a level now: mission3, Mission6.dll, Training1, FrontEnd, myop.dll, ambush.lua");
    Print("levels            the levels that can be started (game folder + mods)");
    Print("restart           start the current level again");
    Print("mods              the mods in use");
    Print("net host          host an online session: shows a join code (copied to the clipboard)");
    Print("net join <code>   join with a host's code (Ctrl+V pastes) [lan | public | v6] [noport]");
    Print("net reply <code>  host: a joiner's reply code -> both sides punch through their routers");
    Print("net say <text>    message to the other side;  net status;  net stop");
    Print("pse plugins       PSE plugins: name, version, built in / external, state, why not loaded");
    Print("pse hooks [text]  PSE hook broker records (kind, owner, name, address, length, priority, chain)");
    Print("clear             empty this window");
    Print("Tab completes level names, Up / Down = history, PgUp / PgDn scroll, Esc or ` closes");
}

void ListLevels() {
    const std::vector<Level> all = AllLevels();
    if (all.empty()) {
        Print("no levels found");
        return;
    }
    std::string source, line;
    auto flush = [&] {
        if (!line.empty()) Print("  %s", line.c_str());
        line.clear();
    };
    for (const Level& l : all) {
        if (l.source != source) {
            flush();
            source = l.source;
            Print("%s:", source == "game" ? "game folder" : ("mod " + source).c_str());
        }
        std::string n = l.name;
        if (source == "game" && EndsWithI(n, ".dll")) n.resize(n.size() - 4);   // the game's: start them by this name
        if (!line.empty() && line.size() + n.size() + 2 > 96) flush();
        line += (line.empty() ? "" : "  ") + n;
    }
    flush();
}

void StartLevel(const std::string& arg) {
    if (features::FreeCamActive()) {
        Print("leave the free camera first (F6)");
        return;
    }
    std::string source;
    const std::string name = Resolve(arg, source);
    if (name.empty()) {
        Print("no level \"%s\" - 'levels' lists them", arg.c_str());
        return;
    }
    if (name.size() >= 255) {
        Print("level name too long");
        return;
    }
    g_pending = name;
    g_plainRestart = false;
    Print("starting %s (%s)%s", name.c_str(), source.c_str(),
          features::CoopPlayers() ? " - co-op session: split screen" : "");
}

// The level being played: the last level load's name, or (booted straight into it by the start-up options) the scene
// name, "Mission3.env" -> "Mission3". "" in the front end.
std::string LevelToRestart() {
    std::string level = g_lastLoaded;
    if (level.empty()) {
        level = CurrentLevel();
        if (level.find('.') != std::string::npos) level.resize(level.find('.'));
    }
    if (_strnicmp(level.c_str(), "FrontEnd", 8) == 0) level.clear();
    return level;
}

void SetClipboard(const std::string& text) {
    if (!OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1)) {
        memcpy(GlobalLock(h), text.c_str(), text.size() + 1);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_TEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

std::string GetClipboard() {
    std::string text;
    if (!OpenClipboard(g_wnd)) return text;
    if (HANDLE h = GetClipboardData(CF_TEXT)) {
        if (const char* p = static_cast<const char*>(GlobalLock(h))) text = p;
        GlobalUnlock(h);
    }
    CloseClipboard();
    return text;
}

// ---- pse (plugin host, pse/loader) ----
const char* HookKindName(PSEHookKind k) {
    switch (k) {
        case PSE_HOOK_BYTES: return "bytes";
        case PSE_HOOK_CALL_SITE: return "call";
        case PSE_HOOK_FUNCTION: return "function";
        case PSE_HOOK_INDIRECT_CALL: return "indirect";
        case PSE_HOOK_POINTER: return "pointer";
    }
    return "?";
}

bool ContainsI(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
               return tolower(static_cast<unsigned char>(a)) == tolower(static_cast<unsigned char>(b));
           }) != hay.end();
}

void PsePlugins() {
    const std::vector<pse::PluginLoadResult> results = pse::PluginResults();
    auto resultOf = [&](PSEPluginHandle h) -> const pse::PluginLoadResult* {
        for (const pse::PluginLoadResult& r : results)
            if (r.handle == h) return &r;
        return nullptr;
    };
    int count = 0;
    for (PSEPluginHandle h = 1; h <= pse::PluginCount(); ++h) {
        const PSEPluginInfo* info = pse::PluginInfo(h);
        if (!info) continue;
        const bool external = pse::PluginExternal(h);
        const pse::PluginLoadResult* r = external ? resultOf(h) : nullptr;
        const char* state = r ? pse::PluginStateName(r->state) : pse::PluginDisabled(h) ? "disabled" : "loaded";
        Print("  %-24s %-9s %-8s %s%s%s", pse::PluginName(h), pse::VersionString(info->version).c_str(),
              external ? "external" : "built in", state, r && !r->reason.empty() ? " - " : "",
              r ? r->reason.c_str() : "");
        ++count;
    }
    for (const pse::PluginLoadResult& r : results) {
        if (r.handle) continue;   // listed above
        Print("  %-24s %-9s %-8s %s - %s", r.name.c_str(), r.version ? pse::VersionString(r.version).c_str() : "?",
              "external", pse::PluginStateName(r.state), r.reason.c_str());
        ++count;
    }
    Print("%d plugin(s)", count);
}

void PseHooks(const std::string& filter) {
    const PSEHooksInterface* hooks = pse::HooksInterface();
    std::vector<PSEHookInfo> list(hooks->ListHooks(nullptr, 0));
    list.resize(std::min<size_t>(hooks->ListHooks(list.data(), static_cast<uint32_t>(list.size())), list.size()));
    int shown = 0;
    for (const PSEHookInfo& h : list) {
        char va[16];
        snprintf(va, sizeof va, "0x%08X", h.va);
        const std::string owner = h.ownerName ? h.ownerName : "?", name = h.name ? h.name : "";
        if (!ContainsI(owner, filter) && !ContainsI(name, filter) && !ContainsI(va, filter) &&
            !ContainsI(HookKindName(h.kind), filter))
            continue;
        if (h.kind == PSE_HOOK_BYTES)
            Print("  %-8s %-16s %-24s %s len %u", HookKindName(h.kind), owner.c_str(), name.c_str(), va, h.len);
        else
            Print("  %-8s %-16s id %-21u %s len %u  priority %d  chain %u", HookKindName(h.kind), owner.c_str(), h.id,
                  va, h.len, h.priority, h.chainPosition);
        ++shown;
    }
    Print("%d of %u record(s)%s", shown, static_cast<unsigned>(list.size()),
          filter.empty() ? " - 'pse hooks <text>' filters by owner, name, address or kind" : "");
}

void Pse(const std::string& arg) {
    const size_t sp = arg.find(' ');
    const std::string sub = arg.substr(0, sp);
    const std::string rest = sp == std::string::npos ? std::string() : arg.substr(arg.find_first_not_of(' ', sp) == std::string::npos ? arg.size() : arg.find_first_not_of(' ', sp));
    if (_stricmp(sub.c_str(), "plugins") == 0) PsePlugins();
    else if (_stricmp(sub.c_str(), "hooks") == 0) PseHooks(rest);
    else Print("pse plugins | hooks [text]");
}

void Net(const std::string& arg) {
    std::string sub = arg, rest;
    if (const size_t sp = arg.find(' '); sp != std::string::npos) {
        sub = arg.substr(0, sp);
        rest = arg.substr(std::min(arg.size(), arg.find_first_not_of(' ', sp)));
    }
    auto is = [&](const char* c) { return _stricmp(sub.c_str(), c) == 0; };
    if (is("host")) {
        net::Map m = net::Map::Auto;
        if (_stricmp(rest.c_str(), "pcp") == 0) m = net::Map::Pcp;
        else if (_stricmp(rest.c_str(), "natpmp") == 0) m = net::Map::NatPmp;
        else if (_stricmp(rest.c_str(), "upnp") == 0) m = net::Map::Upnp;
        else if (_stricmp(rest.c_str(), "none") == 0) m = net::Map::None;
        else if (!rest.empty()) return Print("net host [pcp | natpmp | upnp | none] - how to open the router port (default: try all)");
        net::Host(m);
    } else if (is("join") || is("reply")) {
        // the code may contain spaces (pasted with them); a trailing lan / public / v6 picks the addresses to try
        // trailing options (tests): lan / public / v6, noport = the joiner opens no router port, punchonly = it waits
        // for the host's punches (reply code)
        std::string code = rest;
        net::Paths paths = net::Paths::All;
        bool openPort = true, hello = true;
        for (size_t sp; is("join") && (sp = code.find_last_of(' ')) != std::string::npos;) {
            const std::string last = code.substr(sp + 1);
            if (_stricmp(last.c_str(), "lan") == 0) paths = net::Paths::Lan;
            else if (_stricmp(last.c_str(), "public") == 0) paths = net::Paths::Public;
            else if (_stricmp(last.c_str(), "v6") == 0) paths = net::Paths::V6;
            else if (_stricmp(last.c_str(), "noport") == 0) openPort = false;
            else if (_stricmp(last.c_str(), "punchonly") == 0) hello = false;
            else break;
            code.resize(sp);
        }
        if (code.empty()) return Print(is("join") ? "net join <code> - the host's join code" : "net reply <code> - the joiner's reply code");
        if (is("join")) net::Join(code, paths, openPort, hello);
        else net::AddReplyCode(code);
    } else if (is("say")) {
        if (rest.empty()) return Print("net say <text>");
        if (net::Say(rest)) Print("you: %s", rest.c_str());
    } else if (is("status") || sub.empty()) {
        for (const std::string& l : net::Status()) Print("%s", l.c_str());
    } else if (is("stop")) {
        net::Stop();
    } else Print("net host | join <code> | reply <code> | say <text> | status | stop");
}

void Execute(const std::string& line) {
    Print("> %s", line.c_str());
    std::string cmd = line, arg;
    const size_t sp = line.find(' ');
    if (sp != std::string::npos) {
        cmd = line.substr(0, sp);
        arg = line.substr(line.find_first_not_of(' ', sp) == std::string::npos ? line.size() : line.find_first_not_of(' ', sp));
    }
    if (cmd.empty()) return;
    if (_stricmp(cmd.c_str(), "help") == 0 || cmd == "?") Help();
    else if (_stricmp(cmd.c_str(), "levels") == 0) ListLevels();
    else if (_stricmp(cmd.c_str(), "level") == 0 || _stricmp(cmd.c_str(), "map") == 0) {
        if (arg.empty()) Print("level <name> - 'levels' lists them");
        else StartLevel(arg);
    } else if (_stricmp(cmd.c_str(), "restart") == 0) {
        const std::string level = LevelToRestart();
        if (level.empty()) Print("no level to restart");
        else StartLevel(level);
    } else if (_stricmp(cmd.c_str(), "mods") == 0) {
        const auto& mods = features::LoadedMods();
        if (!features::ModsFolderEnabled()) Print("the Mods folder is off (OPTIONS > DESERTSTORMFIX > MODS)");
        else if (mods.empty()) Print("no mods in use");
        else
            for (const std::string& m : mods) Print("  %s", m.c_str());
    } else if (_stricmp(cmd.c_str(), "net") == 0) {
        Net(arg);
    } else if (_stricmp(cmd.c_str(), "pse") == 0) {
        Pse(arg);
    } else if (_stricmp(cmd.c_str(), "clear") == 0 || _stricmp(cmd.c_str(), "cls") == 0) {
        g_lines.clear();
        g_dirty = true;
    } else Print("unknown command \"%s\" - 'help' lists them", cmd.c_str());
}

void Complete() {
    const size_t sp = g_input.find(' ');
    if (sp == std::string::npos) {
        for (const char* c : {"help", "level ", "levels", "restart", "mods", "net ", "pse ", "clear"})
            if (!g_input.empty() && _strnicmp(c, g_input.c_str(), g_input.size()) == 0) {
                g_input = c;
                return;
            }
        return;
    }
    const std::string cmd = g_input.substr(0, sp);
    if (_stricmp(cmd.c_str(), "pse") == 0) {
        const std::string sub = g_input.substr(sp + 1);
        if (sub.find(' ') == std::string::npos)
            for (const char* c : {"plugins", "hooks "})
                if (_strnicmp(c, sub.c_str(), sub.size()) == 0) {
                    g_input = "pse " + std::string(c);
                    return;
                }
        return;
    }
    if (_stricmp(cmd.c_str(), "level") != 0 && _stricmp(cmd.c_str(), "map") != 0) return;
    const std::string prefix = g_input.substr(sp + 1);
    std::vector<std::string> hits;
    for (const Level& l : AllLevels())
        if (_strnicmp(l.name.c_str(), prefix.c_str(), prefix.size()) == 0 &&
            std::find_if(hits.begin(), hits.end(), [&](const std::string& h) { return _stricmp(h.c_str(), l.name.c_str()) == 0; }) == hits.end())
            hits.push_back(l.name);
    if (hits.empty()) return;
    // longest common prefix; the full name when it is the only one
    std::string common = hits[0];
    for (const std::string& h : hits) {
        size_t i = 0;
        while (i < common.size() && i < h.size() && tolower(static_cast<unsigned char>(common[i])) == tolower(static_cast<unsigned char>(h[i]))) ++i;
        common.resize(i);
    }
    if (common.size() > prefix.size()) g_input = cmd + " " + common;
    else if (hits.size() > 1) {
        std::string line;
        for (const std::string& h : hits) {
            if (!line.empty() && line.size() + h.size() > 96) {
                Print("  %s", line.c_str());
                line.clear();
            }
            line += (line.empty() ? "" : "  ") + h;
        }
        Print("  %s", line.c_str());
    }
}

void SetOpen(bool open) {
    if (open == g_open) return;
    g_open = open;
    g_dirty = true;
    dslog::Write("Console: %s", open ? "opened" : "closed");
    if (open && g_lines.empty()) Print("DesertStormFix developer console - 'help' lists the commands");
}

// The key left of 1 by position (scan code 0x29: ` on US keyboards, also UK / German ...); VK_OEM_3 only for a key
// message without a scan code (VK_OEM_3 is the ' key on UK layouts).
bool ToggleKey(WPARAM wp, LPARAM lp) {
    const UINT scan = (lp >> 16) & 0x1FF;
    return scan == 0x29 || (!scan && wp == VK_OEM_3);
}

LRESULT CALLBACK ConsoleProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (Enabled()) {
        switch (msg) {
            case WM_KEYDOWN:
                if (ToggleKey(wp, lp)) {
                    if (!(lp & 0x40000000)) SetOpen(!g_open);   // not on auto-repeat
                    g_eatChar = true;
                    return 0;
                }
                g_eatChar = false;   // (a layout where the toggle key types nothing)
                if (!g_open) break;
                g_dirty = true;
                g_caret = true;
                g_lastBlink = GetTickCount();
                switch (wp) {
                    case VK_ESCAPE: SetOpen(false); break;
                    case VK_RETURN: {
                        const std::string line = g_input;
                        g_input.clear();
                        if (!line.empty() && (g_history.empty() || g_history.back() != line)) g_history.push_back(line);
                        g_historyPos = static_cast<int>(g_history.size());
                        Execute(line);
                        break;
                    }
                    case VK_BACK:
                        if (!g_input.empty()) g_input.pop_back();
                        break;
                    case VK_UP:
                        if (g_historyPos > 0) g_input = g_history[--g_historyPos];
                        break;
                    case VK_DOWN:
                        if (g_historyPos < static_cast<int>(g_history.size())) ++g_historyPos;
                        g_input = g_historyPos < static_cast<int>(g_history.size()) ? g_history[g_historyPos] : std::string();
                        break;
                    case VK_PRIOR: g_scroll = std::min(g_scroll + kShownLines / 2, std::max(0, static_cast<int>(g_lines.size()) - kShownLines)); break;
                    case VK_NEXT: g_scroll = std::max(0, g_scroll - kShownLines / 2); break;
                    case VK_TAB: Complete(); break;
                    default: break;
                }
                return 0;
            case WM_KEYUP:
                if (ToggleKey(wp, lp)) return 0;
                break;   // other key-ups reach the game (keys held when the console opened)
            case WM_CHAR:
                if (g_eatChar) {   // the toggle key's own character
                    g_eatChar = false;
                    return 0;
                }
                if (!g_open) break;
                if (wp == 0x16) {   // Ctrl+V
                    for (char c : GetClipboard())
                        if (c >= 0x20 && c < 0x7F && g_input.size() < 200) g_input += c;
                    g_dirty = true;
                    return 0;
                }
                if (wp >= 0x20 && wp < 0x7F && wp != '`' && g_input.size() < 200) {
                    g_input += static_cast<char>(wp);
                    g_dirty = true;
                }
                return 0;
            case WM_DEADCHAR:   // the toggle key is a dead key on some layouts (^ on German ones)
                if (g_eatChar || g_open) {
                    g_eatChar = false;
                    return 0;
                }
                break;
            default: break;
        }
    }
    if (msg == WM_DESTROY) {
        net::Stop();            // goodbye to peers, router port closed again
        pse_host::Shutdown();   // PSE_MSG_EXIT + external plugins unloaded
    }
    return CallWindowProcA(g_prevProc, wnd, msg, wp, lp);
}

// ---- level start (the game update's front-end call) ----
int g_lastFrontEndResult = 0;
int __cdecl FrontEndUpdate() {
    const int r = reinterpret_cast<int(__cdecl*)()>(kFrontEndUpdate)();
    g_lastFrontEndResult = r;
    if (g_pending.empty() || r != 0) return r;
    if (Global<uint32_t>(kFeBusy) || Global<uint8_t>(kFeAnimIn) || Global<uint8_t>(kFeAnimOut)) return r;   // next frame
    const std::string name = g_pending;
    const bool plain = g_plainRestart;
    g_pending.clear();
    g_plainRestart = false;
    strcpy_s(Global<char*>(kNextLevel), 256, name.c_str());
    if (void* campaign = Global<void*>(kCampaign)) reinterpret_cast<void(__thiscall*)(void*)>(kCampaignReset)(campaign);
    if (!plain) {   // the mission list's extras; the game's own restart (MISSION FAILED > RESTART LEVEL) skips them
        Global<uint32_t>(kDirectLevel) = 1;
        reinterpret_cast<void(__cdecl*)(int)>(kDebugKeys)(1);
    }
    Global<uint32_t>(kToMainMenu) = _strnicmp(name.c_str(), "FrontEnd", 8) == 0 ? 1 : 0;
    Global<char>(kLevelName) = 0;
    // closed: the loading screen waits for a key (PLEASE PRESS ANY KEY), and the level should get the keyboard
    g_open = false;
    dslog::Write("Console: level load %s (front-end state 0x%X)", name.c_str(), Global<uint32_t>(kFrontEndState));
    return 3;
}

// ---- drawing: GDI into a texture (like the performance overlay), redrawn when something changed ----
void Draw(int screenW, int screenH) {
    const int texW = std::min(kTexW, screenW);
    const float scale = static_cast<float>(texW) / screenW;
    const int px = std::max(11, static_cast<int>(std::clamp(screenH / 42, 13, 34) * scale));
    const int lineH = px + px / 4, pad = px / 2;
    const int h = std::min(kTexH, pad * 2 + lineH * (kShownLines + 2) + px / 2);
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), kTexW, -kTexH, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        DeleteDC(dc);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HFONT font = CreateFontA(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    memset(bits, 0, size_t(kTexW) * kTexH * 4);
    auto text = [&](int y, COLORREF c, const std::string& t) {
        SetTextColor(dc, c);
        TextOutA(dc, pad, y, t.c_str(), static_cast<int>(t.size()));
    };
    int y = pad;
    char title[160];
    snprintf(title, sizeof title, "DEVELOPER CONSOLE  -  %s  -  help, Esc closes%s",
             CurrentLevel().empty() ? "loading" : CurrentLevel().c_str(),
             g_scroll ? "  (scrolled back - PgDn)" : "");
    text(y, RGB(255, 190, 70), title);
    y += lineH;
    const int n = static_cast<int>(g_lines.size());
    const int last = n - g_scroll, first = std::max(0, last - kShownLines);
    y += lineH * (kShownLines - (last - first));
    for (int i = first; i < last; ++i, y += lineH) {
        const std::string& l = g_lines[i];
        text(y, l.rfind("> ", 0) == 0 ? RGB(150, 210, 255) : RGB(225, 225, 225), l);
    }
    text(y + px / 4, RGB(255, 255, 255), "] " + g_input + (g_caret ? "_" : ""));
    // GDI leaves alpha 0: panel = translucent dark blue, text opaque by its brightness; amber line along the bottom
    auto* px32 = static_cast<uint32_t*>(bits);
    g_pixels.assign(size_t(texW) * h, 0);
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < texW; ++xx) {
            uint32_t c = px32[size_t(yy) * kTexW + xx] & 0xFFFFFF;
            if (yy >= h - std::max(2, px / 8)) c = 0xFFBE46;
            const uint32_t lum = std::max({c >> 16 & 255, c >> 8 & 255, c & 255});
            const uint32_t a = 0xD0 + (0xFF - 0xD0) * lum / 255;
            if (!lum) c = 0x0A1018;
            g_pixels[size_t(yy) * texW + xx] = a << 24 | c;
        }
    g_texUsedW = texW;
    g_w = texW;
    g_h = h;
    SelectObject(dc, oldFont);
    DeleteObject(font);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// Queued from the overlay's top draw: over the menus, the loadout / briefing screen and the other late draws.
void TopDraw() {
    if (!g_open || !g_tex || g_w <= 0 || g_h <= 0 || g_drawW <= 0) return;
    const float scale = static_cast<float>(g_drawW) / g_w;
    overlay::QueueScreenImage(g_tex, 0, 0, static_cast<float>(g_drawW), g_h * scale, static_cast<float>(g_w) / kTexW,
                              static_cast<float>(g_h) / kTexH);
}
}  // namespace

bool features::ConsoleOpen() { return g_open; }
void features::ConsoleStartLevel(const std::string& level) { StartLevel(level); }   // online.cpp: the host's level

// Pause menu RESTART LEVEL (pausemenu.cpp): from the start, intro included - the game's own restart (MISSION FAILED)
// also sets campaign +0x13CC, which API +0x3F4 reports as a loaded save, and every mission DLL then skips its whole
// start-up (objectives, AI) waiting for save data that never comes.
bool features::RestartLevel() {
    const std::string level = LevelToRestart();
    if (level.empty() || !g_hooked) return false;
    StartLevel(level);
    if (g_pending.empty()) return false;
    g_plainRestart = true;
    return true;
}

// devserver.cpp (dev test pipe): runs one console line as if typed, returns the lines it printed. Declared here and in
// devserver.cpp only (not features.h) - nothing else needs it.
namespace features {
std::vector<std::string> ConsoleRun(const std::string& line);
}
std::vector<std::string> features::ConsoleRun(const std::string& line) {
    std::vector<std::string> out;
    g_capture = &out;
    Execute(line);
    g_capture = nullptr;
    return out;
}

void features::ApplyConsole() {
    if (g_hooked) return;
    overlay::SetTopDraw(&TopDraw);
    g_hooked = patch::HookCall(kFrontEndUpdateCall, reinterpret_cast<const void*>(&FrontEndUpdate), kFrontEndUpdate);
    dslog::Write(g_hooked ? "Console: ready (` opens it%s)" : "[fail] Console: level start hook not applied%s",
#ifndef DS_DIST
                 ", dev build: always on"
#else
                 ""
#endif
    );
}

void features::OnFrameConsole() {
    // the level being loaded (every load goes through dispatch mode 1 with the name in the slot): for restart
    if (Global<int8_t>(kDispatchMode) == 1) {
        const char* next = Global<char*>(kNextLevel);
        if (next && next[0] && _stricmp(next, g_lastLoaded.c_str()) != 0) g_lastLoaded = next;
    }
    const HWND wnd = Global<HWND>(kGameWindow);
    if (wnd && wnd != g_wnd && IsWindow(wnd)) {
        g_prevProc = reinterpret_cast<WNDPROC>(SetWindowLongA(wnd, GWL_WNDPROC, reinterpret_cast<LONG>(&ConsoleProc)));
        g_wnd = g_prevProc ? wnd : nullptr;
    }
    for (const net::Event& e : net::TakeEvents()) {
        if (e.kind == net::Event::Code) {
            SetClipboard(e.text);
            Print("JOIN CODE (copied to the clipboard): %s", e.text.c_str());
            Print("your friend types: net join <code>  (Ctrl+V pastes)");
        } else if (e.kind == net::Event::ReplyCode) {
            SetClipboard(e.text);
            Print("not connected yet - send the host this REPLY CODE (copied): %s", e.text.c_str());
            Print("the host types: net reply <code>");
        } else Print("%s%s", e.kind == net::Event::Message ? "" : "net: ", e.text.c_str());
    }
    if (!Enabled()) {
        g_open = false;
        if (!g_plainRestart) g_pending.clear();
        return;
    }
    g_drawW = 0;
    if (!g_open) return;
    void* dev = overlay::CurrentDevice();
    const auto* r = Global<const uint8_t*>(kRenderer);
    if (!dev || !r) return;
    const int W = *reinterpret_cast<const int*>(r + 0x40688), H = *reinterpret_cast<const int*>(r + 0x4068C);
    if (W <= 0 || H <= 0) return;
    if (dev != g_texDevice) {
        g_tex = overlay::CreateTexture(kTexW, kTexH);
        g_texDevice = g_tex ? dev : nullptr;
        g_dirty = true;
        if (!g_tex) return;
    }
    const DWORD now = GetTickCount();
    if (now - g_lastBlink >= 500) {
        g_lastBlink = now;
        g_caret = !g_caret;
        g_dirty = true;
    }
    if (g_dirty || g_texUsedW != std::min(kTexW, W)) {
        g_dirty = false;
        Draw(W, H);
        if (g_w > 0 && g_h > 0) overlay::UploadTexture(g_tex, g_pixels.data(), g_w, g_h);
    }
    g_drawW = W;
}

int features::LastFrontEndResult() { return g_lastFrontEndResult; }
