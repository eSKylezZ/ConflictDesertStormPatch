// dsfix.online's side of what dsfix.dll used to reach into directly: the developer console's net commands and
// connection events (console.cpp prints the lines), the lobby name on the loadout screen's title, connected-PC counts
// (customise.cpp, pse_host.cpp) and the connection shut down when the game window closes. Plus the online check's
// list of loaded plugins, which only the loader knows - dsfix.dll hands it over as text (features::CompatLocalPluginsText).
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/net.h"
#include "features/features.h"
#include "features/lobby.h"
#include "features/lobbynet.h"
#include "loader.h"

namespace {
std::vector<std::string>* g_out = nullptr;

void Print(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_out) g_out->emplace_back(buf);
}

void SetClipboard(const std::string& text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1)) {
        memcpy(GlobalLock(h), text.c_str(), text.size() + 1);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_TEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
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
        // the code may contain spaces (pasted with them); trailing options (tests): lan / public / v6 = the addresses
        // to try, noport = the joiner opens no router port, punchonly = it waits for the host's punches (reply code)
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
}  // namespace

std::vector<std::string> features::OnlineConsoleNet(const std::string& arg) {
    std::vector<std::string> out;
    g_out = &out;
    Net(arg);
    g_out = nullptr;
    return out;
}

std::vector<std::string> features::OnlineConsoleEvents() {
    std::vector<std::string> out;
    g_out = &out;
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
    g_out = nullptr;
    return out;
}

void features::OnlineShutdown() { net::Stop(); }   // goodbye to peers, router port closed again

std::string features::LobbyPlayerName(int player) {
    if (player < 0 || player >= lobby::kMaxPlayers) return {};
    const lobby::Table& t = lobbynet::Current();
    return t.slots[player].used ? lobby::CleanName(t.slots[player].name) : std::string();
}

uint32_t features::OnlineConnectedPcMask() {
    uint32_t mask = 0;
    for (const net::Snapshot::Peer& p : net::Snap().peers)
        if (p.connected && p.pc > 0 && p.pc < 32) mask |= 1u << p.pc;
    return mask;
}

int features::OnlineConnectedPeers() {
    int n = 0;
    for (const net::Snapshot::Peer& p : net::Snap().peers) n += p.connected ? 1 : 0;
    return n;
}

// compat.cpp's version texts (the loader's own copy is in plugins.cpp, not compiled here)
std::string pse::VersionString(uint32_t v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%u.%u.%u", PSE_VERSION_MAJOR(v), PSE_VERSION_MINOR(v), PSE_VERSION_PATCH(v));
    return buf;
}

// The loader's plugin list for the online check (onlinecompat.cpp), from dsfix.dll's text: one line per plugin,
// "name<TAB>version<TAB>flags<TAB>hash".
std::vector<pse::compat::PluginEntry> pse::compat::LocalPlugins() {
    std::vector<PluginEntry> out;
    const std::string text = features::CompatLocalPluginsText();
    for (size_t pos = 0; pos < text.size();) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1),
                     c = b == std::string::npos ? b : line.find('\t', b + 1);
        if (c == std::string::npos) continue;
        PluginEntry e;
        e.name = line.substr(0, a);
        e.version = static_cast<uint32_t>(strtoul(line.c_str() + a + 1, nullptr, 10));
        e.flags = static_cast<uint32_t>(strtoul(line.c_str() + b + 1, nullptr, 10));
        e.hash = line.substr(c + 1);
        out.push_back(std::move(e));
    }
    return out;
}
