// Main menu CO-OP -> a CO-OP screen (SPLIT SCREEN = the join screen, state 0x3E / ONLINE) and the online screens over
// core/net.h (direct connection with join codes - no server):
//   0x48 CO-OP              SPLIT SCREEN -> 0x3E, ONLINE -> 0x49; Back -> main menu
//   0x49 ONLINE CO-OP       HOST GAME (starts hosting) -> 0x4A, JOIN GAME -> 0x4B; Back -> 0x48
//   0x4A HOST ONLINE GAME   join code (also copied to the clipboard), router port, players + ping;
//                           START (DIFFICULTY -> the campaign's first mission), COPY JOIN CODE, PASTE REPLY CODE;
//                           Back ends the session -> 0x49
//   0x4B JOIN ONLINE GAME   PASTE JOIN CODE (or Ctrl+V) connects; COPY REPLY CODE when the host can't be reached
//                           directly; Back ends the session -> 0x49
// Level loads are mirrored: every level the host loads while players are connected (START, restart, Continue, back to
// the front end) goes to them as "START <level> <difficulty>" and their games load it too (console.cpp's level start).
// Before anything starts both PCs compare plugins + mods (onlinecompat.cpp, docs/sdk/online-compat.md): START and the
// level mirroring only go to players that passed, a joiner ignores START until the host passed, and a refused peer is
// dropped (host) / leaves (joiner) with the differences on both screens.
// The screens are Options sub-screen objects with copied vtables (like versus.cpp / fixmenu.cpp), served by coop.cpp's
// state -> screen hook; their list area is made taller than the two rows so the status text sits inside the panel.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <regex>
#include <string>

#include "core/log.h"
#include "core/net.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/onlinecompat.h"
#include "features/overlay.h"

namespace {
// Streamer mode (settings featStreamerMode, default on; user 2026-10-22): codes and IP addresses are hidden wherever the
// online screens draw them - a join / reply code carries the PC's addresses. COPY JOIN CODE / C still copies the real one.
std::string Masked(const std::string& text) {
    if (!settings::Get().featStreamerMode) return text;
    static const std::regex ip4(R"(\b\d{1,3}(\.\d{1,3}){3}\b)"), ip6(R"(\[[0-9A-Fa-f:]+\])");
    return std::regex_replace(std::regex_replace(text, ip4, "*.*.*.*"), ip6, "[*]");
}
std::string ShownCode(const std::string& code) {
    if (!settings::Get().featStreamerMode || code.size() < 6) return code;
    return code.substr(0, 5) + "-*****-***** (hidden: streamer mode - copy it to send it)";
}
}  // namespace

namespace {
constexpr uint32_t kSetState = 0x47EF20, kBackPressed = 0x478380, kListSelected = 0x4E5D80;
constexpr uint32_t kAlloc = 0x4B83D0, kListCtor = 0x4E5B10, kListReserve = 0x4E56C0, kListAddToPool = 0x4E58C0;
constexpr uint32_t kItemCtor = 0x4E4F90, kItemSet = 0x4E4FB0;
constexpr uint32_t kDrawPanel = 0x456EB0, kDrawTitle = 0x456F40;
constexpr uint32_t kLargeFont = 0x60EDB8, kHudSheet = 0x60EE18, kMainMenuScreen = 0x617A00, kRenderer = 0x63C924;
constexpr uint32_t kMainMenuEnter = 0x4153B0;
constexpr uint32_t kOptCtor = 0x409B90, kOptVtable = 0x5D8510, kOptSetup = 0x409C90, kOptItemDrawCb = 0x409E90;
constexpr uint32_t kStateMainMenu = 1, kStateJoin = 0x3E, kStateDifficulty = 5;
constexpr uint32_t kCampaign = 0x60EDC0, kNextLevel = 0x5E7028, kDispatchMode = 0x60EC8C;
constexpr uint32_t kDifficulty = 0x63C9E0, kSetDifficulty = 0x4F0060;
constexpr uint32_t kUniform = 0x60ED1C, kSetUniform = 0x40A320;   // 0 SAS, 1 US Delta   // [obj+4]: 0 easy, 1 normal, 2 hard
constexpr uint32_t kStateCoop = 0x48, kStateOnline = 0x49, kStateHost = 0x4A, kStateJoinOnline = 0x4B, kStateLobby = 0x4F;
constexpr int kVtableEntries = 21;
enum : uint32_t {
    kTextCoop = 0xC0100001, kTextSplit, kTextOnline, kTextOnlineTitle, kTextHost, kTextJoin, kTextHostTitle,
    kTextJoinTitle, kTextCopyCode, kTextPasteReply, kTextPasteCode, kTextCopyReply, kTextStart
};
enum : uint16_t {
    kItemSplit = 0x870, kItemOnline, kItemHost, kItemJoin, kItemCopyCode, kItemPasteReply, kItemPasteCode, kItemCopyReply,
    kItemStart
};
constexpr int kMaxItems = 3;

struct Screen {
    uint32_t state, title;
    int count;
    uint16_t ids[kMaxItems];
    uint32_t texts[kMaxItems];
    float bottom;   // the panel's bottom edge as laid out (screen +0x14); the status text extends it
    uint8_t object[0x20];
    void* vtable[kVtableEntries];
    bool built;
};
Screen g_screens[] = {
    {kStateCoop, kTextCoop, 2, {kItemSplit, kItemOnline}, {kTextSplit, kTextOnline}, 0.0f, {}, {}, false},
    {kStateOnline, kTextOnlineTitle, 2, {kItemHost, kItemJoin}, {kTextHost, kTextJoin}, 0.0f, {}, {}, false},
    {kStateHost, kTextHostTitle, 3, {kItemStart, kItemCopyCode, kItemPasteReply}, {kTextStart, kTextCopyCode, kTextPasteReply}, 0.0f, {}, {}, false},
    {kStateJoinOnline, kTextJoinTitle, 2, {kItemPasteCode, kItemCopyReply}, {kTextPasteCode, kTextCopyReply}, 0.0f, {}, {}, false},
};

std::string g_notice;    // one line of feedback ("Join code copied" ...)
DWORD g_noticeAt = 0;

template <class T>
T& At(void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}
void SetState(uint32_t s) { reinterpret_cast<void(__cdecl*)(uint32_t)>(kSetState)(s); }
void* ItemAt(void* list, int i) { return At<void**>(list, 0x2C)[i]; }
Screen* Find(void* object) {
    for (Screen& s : g_screens)
        if (s.object == object) return &s;
    return nullptr;
}
void Notice(const char* text) {
    g_notice = text;
    g_noticeAt = GetTickCount();
    dslog::Write("Online menu: %s", text);
}

bool SetClipboard(const std::string& text) {
    if (text.empty() || !OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    bool ok = false;
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1)) {
        memcpy(GlobalLock(h), text.c_str(), text.size() + 1);
        GlobalUnlock(h);
        ok = SetClipboardData(CF_TEXT, h) != nullptr;
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
}

std::string Clipboard() {
    std::string text;
    if (!OpenClipboard(nullptr)) return text;
    if (HANDLE h = GetClipboardData(CF_TEXT)) {
        if (const char* p = static_cast<const char*>(GlobalLock(h))) text = p;
        GlobalUnlock(h);
    }
    CloseClipboard();
    // a code pasted from a chat may come with quotes / spaces / line breaks around it
    const size_t a = text.find_first_not_of(" \t\r\n\"'`");
    const size_t b = text.find_last_not_of(" \t\r\n\"'`");
    return a == std::string::npos ? std::string() : text.substr(a, b - a + 1);
}

// ---- screen class functions (shared, by object) ----
bool __fastcall Create(void* screen, void*) {
    using Alloc = void*(__cdecl*)(uint32_t, uint32_t);
    void* list = reinterpret_cast<Alloc>(kAlloc)(0xA4, 0x1E);
    if (!list) return false;
    reinterpret_cast<void*(__thiscall*)(void*, void*, void*, int)>(kListCtor)(
        list, *reinterpret_cast<void**>(kLargeFont), *reinterpret_cast<void**>(kHudSheet), 1);
    At<void*>(screen, 8) = list;
    const int n = Find(screen)->count;
    if (!reinterpret_cast<int(__thiscall*)(void*, int)>(kListReserve)(list, n)) return false;
    for (int i = 0; i < n; ++i) {
        void* item = reinterpret_cast<Alloc>(kAlloc)(0x24, 0x1E);
        if (item) reinterpret_cast<void*(__thiscall*)(void*)>(kItemCtor)(item);
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kListAddToPool)(list, item);
    }
    return true;
}

void __fastcall Setup(void* screen, void*) {
    Screen* s = Find(screen);
    reinterpret_cast<void(__thiscall*)(void*)>(kOptSetup)(screen);
    void* list = At<void*>(screen, 8);
    using Fn = void(__thiscall*)(void*, int, uint32_t, int, int, int, int, uint32_t, int);
    for (int i = 0; i < s->count; ++i) reinterpret_cast<Fn>(kItemSet)(ItemAt(list, i), s->ids[i], s->texts[i], 0, 0x24, 0, 0, kOptItemDrawCb, 0);
    // the class's set-up lists two items: the rest added, the list area (+0x40, row pitch +0x38) and visible rows
    // (+0x42) grown
    for (int i = 2; i < s->count; ++i)
        reinterpret_cast<void(__thiscall*)(void*, void*)>((*reinterpret_cast<void***>(list))[1])(list, ItemAt(list, i));
    At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + (s->count - 2) * At<int16_t>(list, 0x38));
    At<int16_t>(list, 0x42) = static_cast<int16_t>(s->count);
    using Layout = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, s->title, 0, 0, 0x7FFF);
    s->bottom = At<float>(screen, 0x14);   // Draw moves it down to fit the status text
}

void PasteJoinCode() {
    const std::string code = Clipboard();
    if (code.empty()) return Notice("The clipboard is empty - copy the host's join code first");
    if (!net::IsJoinCode(code)) return Notice("The clipboard doesn't hold a join code");
    if (net::Active()) net::Stop();
    onlinecompat::ClearNotice();
    Notice(net::Join(code) ? "Connecting ..." : "Couldn't start - see below");
}

int __fastcall Update(void* screen, void*) {
    const Screen* s = Find(screen);
    const short id = reinterpret_cast<short(__thiscall*)(void*)>(kListSelected)(At<void*>(screen, 8));
    const bool back = reinterpret_cast<int(__cdecl*)(int)>(kBackPressed)(0) != 0;
    const net::Snapshot snap = net::Snap();
    switch (id) {
        case kItemSplit: SetState(kStateJoin); break;
        case kItemOnline: SetState(kStateOnline); break;
        case kItemHost:
            if (net::Active()) net::Stop();
            g_notice.clear();
            onlinecompat::ClearNotice();
            if (net::Host()) SetState(kStateLobby);   // the lobby (lobbymenu.cpp) shows the join code too
            break;
        case kItemJoin:
            if (net::Active()) net::Stop();
            g_notice.clear();
            onlinecompat::ClearNotice();
            SetState(kStateJoinOnline);
            break;
        case kItemCopyCode:
            if (snap.role != net::Snapshot::Role::Host || snap.code.empty()) Notice("Still finding your addresses ...");
            else Notice(SetClipboard(snap.code) ? "Join code copied - send it to your friend" : "Couldn't use the clipboard");
            break;
        case kItemPasteReply: {
            const std::string code = Clipboard();
            if (code.empty()) Notice("The clipboard is empty - copy your friend's reply code first");
            else Notice(net::AddReplyCode(code) ? "Reply code accepted - connecting ..." : "That isn't a reply code for this game");
            break;
        }
        case kItemPasteCode: PasteJoinCode(); break;
        case kItemStart: {
            std::string why;
            if (features::OnlineStartBlocked(&why)) Notice(why.c_str());
            else features::OnlineStartSession();
            break;
        }
        case kItemCopyReply:
            if (snap.role != net::Snapshot::Role::Join || snap.code.empty() || !snap.gathered)
                Notice("No reply code yet - paste the join code first");
            else Notice(SetClipboard(snap.code) ? "Reply code copied - send it to the host" : "Couldn't use the clipboard");
            break;
        default: break;
    }
    // Ctrl+V on the join screen = PASTE JOIN CODE
    static bool paste = false;
    const bool v = features::KeyHeld(VK_CONTROL) && features::KeyHeld('V');
    if (s->state == kStateJoinOnline && v && !paste) PasteJoinCode();
    paste = v;
    // connected and the host's lobby table is here: on to the lobby
    if (s->state == kStateJoinOnline && snap.role == net::Snapshot::Role::Join && features::LobbyOnline() && !back) {
        SetState(kStateLobby);
        return 0;
    }
    if (back) {
        if (s->state == kStateHost || s->state == kStateJoinOnline) {
            net::Stop();
            g_notice.clear();
            onlinecompat::ClearNotice();
        }
        SetState(s->state == kStateCoop ? kStateMainMenu : s->state == kStateOnline ? kStateCoop : kStateOnline);
    }
    return 0;
}

// ---- status text inside the panel, under the two rows ----
struct Lines {
    void* font;
    int x, y, w, step;
    bool draw;   // false: only count (the panel is sized first)
    int count = 0;
    // One line per call - wrapped at spaces to the panel width (a player's address made the line run off the
    // screen); a word still too wide (an IPv6 address) is broken after a ':' / '.' / '-', else anywhere.
    void Add(const std::string& text, uint32_t argb = 0xFFC8C8C8) {
        std::string rest = text;
        while (!rest.empty()) {
            size_t n = Fit(rest);
            Line(rest.substr(0, n), argb);
            rest.erase(0, n);
            while (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        }
    }
    void Line(const std::string& text, uint32_t argb) {
        if (draw) overlay::DrawLabel(font, text.c_str(), x, y, argb);
        y += step;
        ++count;
    }
    bool Fits(const std::string& s) const { return overlay::TextWidth(font, s.c_str()) <= w; }
    // Length of the longest start of `s` that fits: whole words if possible.
    size_t Fit(const std::string& s) const {
        if (Fits(s)) return s.size();
        size_t best = 0;
        for (size_t i = s.find(' '); i != std::string::npos; i = s.find(' ', i + 1)) {
            if (!Fits(s.substr(0, i))) break;
            best = i;
        }
        if (best) return best;
        size_t n = 1;
        while (n < s.size() && Fits(s.substr(0, n + 1))) ++n;
        for (size_t i = n; i > n / 2; --i)
            if (s[i - 1] == ':' || s[i - 1] == '.' || s[i - 1] == '-' || s[i - 1] == ',') return i;
        return n;
    }
    // a long code split at a dash so it stays inside the panel
    void Code(const std::string& code, uint32_t argb) {
        if (overlay::TextWidth(font, code.c_str()) <= w) return Add(code, argb);
        size_t cut = code.find('-', code.size() / 2 - 3);
        if (cut == std::string::npos) cut = code.size() / 2;
        Add(code.substr(0, cut + 1), argb);
        Add(code.substr(cut + 1), argb);
    }
};

// Returns the baseline below the last line (0 = no text). step 0 = the normal line spacing.
int DrawStatus(const Screen& s, void* list, bool draw, int step = 0) {
    void* font = overlay::MenuFont();
    if (!font || s.state == kStateCoop) return 0;
    const int pitch = At<int16_t>(list, 0x38);
    Lines l{font, At<int16_t>(list, 0x3A), At<int16_t>(list, 0x3C) + pitch * s.count + pitch / 2, At<int16_t>(list, 0x32),
            step ? step : std::max(10, pitch * 11 / 20), draw};
    const uint32_t gold = 0xFFE0D878, white = 0xFFFFFFFF, grey = 0xFFC8C8C8;
    const net::Snapshot snap = net::Snap();
    char buf[160];
    if (s.state == kStateOnline) {
        l.Add("Play over the internet - no server needed.");
        l.Add("The host sends a join code; the friend pastes it.");
        return l.y;
    }
    if (s.state == kStateHost) {
        if (snap.role != net::Snapshot::Role::Host) l.Add("Not hosting.");
        else if (!snap.gathered || snap.code.empty()) l.Add("Finding your addresses ...");
        else {
            l.Add("JOIN CODE (copied to the clipboard):", grey);
            l.Code(ShownCode(snap.code), gold);
            l.Add(snap.routerPort.empty() ? "Router port: not opened (a reply code may be needed)" : "Router port opened: " + Masked(snap.routerPort));
        }
        int n = 0;
        for (size_t i = 0; i < snap.peers.size(); ++i) {
            const auto& p = snap.peers[i];
            if (p.connected) snprintf(buf, sizeof buf, "PLAYER %d: connected - %s, ping %d ms", p.pc, Masked(p.path).c_str(), p.ping);
            else snprintf(buf, sizeof buf, "PLAYER %d: %s", p.pc, p.punching ? "connecting (reply code) ..." : "connecting ...");
            l.Add(buf, p.connected ? white : grey);
            ++n;
        }
        for (const std::string& c : onlinecompat::StatusLines(true, 4))
            l.Add(c, c.find("REFUSED") != std::string::npos || c[0] == ' ' ? gold : grey);
        if (!n && snap.gathered) l.Add("Waiting for players ...");
        else if (n) l.Add("START loads the mission for everyone (each plays their own copy for now).");
    } else {   // join
        const std::vector<std::string> check = onlinecompat::StatusLines(false, 5);
        const bool refused = !check.empty() && check[0].rfind("JOIN REFUSED", 0) == 0;
        if (refused && snap.role == net::Snapshot::Role::None) {
            for (const std::string& c : check) l.Add(c, gold);
            l.Add("Install the same plugins (PSE\\Plugins) and switch on the same mods, then join again.");
        } else if (snap.role == net::Snapshot::Role::None && !net::Active()) {
            l.Add("Copy the host's join code, then PASTE JOIN CODE (or Ctrl+V).");
        } else if (snap.role == net::Snapshot::Role::None) {
            l.Add("The session ended.");
        } else if (!snap.peers.empty() && snap.peers[0].connected) {
            if (g_notice == "Connecting ...") g_notice.clear();
            snprintf(buf, sizeof buf, "CONNECTED to the host - %s, ping %d ms", Masked(snap.peers[0].path).c_str(), snap.peers[0].ping);
            l.Add(buf, white);
            for (const std::string& c : check) l.Add(c, refused || c[0] == ' ' ? gold : grey);
            l.Add("Waiting for the host to START - the mission loads here too.");
            l.Add("(Each player plays their own copy for now: no shared game yet.)");
        } else {
            l.Add("Connecting to the host ...");
            if (snap.gathered && !snap.code.empty()) {
                l.Add("No answer yet? Send the host this REPLY CODE:", grey);
                l.Code(ShownCode(snap.code), gold);
            }
        }
    }
    if (!g_notice.empty() && GetTickCount() - g_noticeAt < 6000) l.Add(g_notice, gold);
    else if (!snap.recent.empty()) l.Add(Masked(snap.recent.back()));
    return l.y;
}

void __fastcall Draw(void* screen, void*) {
    const Screen* s = Find(screen);
    void* list = At<void*>(screen, 8);
    // panel down to just below the status text (the last line's baseline + about half a row)
    int below = DrawStatus(*s, list, false);
    const int pitch = At<int16_t>(list, 0x38);
    // Many lines on a small screen (4:3, 800x600) ran past the Select / Back prompts: closer lines, down to about the
    // font's own height, so the last one stays above the prompt line (0.88 of the screen height).
    int step = 0;
    const int normal = std::max(10, pitch * 11 / 20);
    const int limit = static_cast<int>(*reinterpret_cast<int*>(*reinterpret_cast<uint8_t**>(kRenderer) + 0x4068C) * 0.88f);
    if (below > limit) {
        const int first = At<int16_t>(list, 0x3C) + pitch * s->count + pitch / 2;
        const int lines = std::max(1, (below - first) / normal);
        const int minStep = std::max(8, overlay::TextWidth(overlay::MenuFont(), "M") * 2);
        step = std::max(minStep, (limit - first) / lines);
        below = DrawStatus(*s, list, false, step);
    }
    At<float>(screen, 0x14) = below ? std::max(s->bottom, static_cast<float>(below - pitch * 11 / 20 + pitch / 2)) : s->bottom;
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(kDrawPanel)(screen, 6, 0);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(kDrawTitle)(screen, s->title, 1);
    reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(list))[2])(list);
    DrawStatus(*s, list, true, step);
}

void Build(Screen& s) {
    reinterpret_cast<void(__thiscall*)(void*)>(kOptCtor)(s.object);
    std::memcpy(s.vtable, reinterpret_cast<void*>(kOptVtable), sizeof s.vtable);
    At<void**>(s.object, 0) = s.vtable;
    s.vtable[6] = reinterpret_cast<void*>(kMainMenuEnter);   // enter runs the set-up every time
    s.vtable[1] = reinterpret_cast<void*>(&Create);
    s.vtable[3] = reinterpret_cast<void*>(&Setup);
    s.vtable[4] = reinterpret_cast<void*>(&Update);
    s.vtable[5] = reinterpret_cast<void*>(&Draw);
    s.built = Create(s.object, nullptr);
    dslog::Write(s.built ? "Online menu: screen 0x%X built" : "[fail] Online menu: screen 0x%X not built", s.state);
}
}  // namespace

void* features::OnlineScreen(uint32_t state) {
    if (state == kStateLobby) return features::LobbyScreen();
    for (Screen& s : g_screens) {
        if (s.state != state) continue;
        if (!s.built && *reinterpret_cast<void**>(kMainMenuScreen)) Build(s);
        return s.built ? s.object : nullptr;
    }
    return nullptr;
}

uint32_t features::CoopMenuState() { return kStateCoop; }

bool features::OnlineStartBlocked(std::string* why) {
    const net::Snapshot snap = net::Snap();
    bool anyone = false;
    for (const auto& p : snap.peers) anyone |= p.connected;
    if (!anyone) {
        if (why) *why = "Wait for a player to connect first";
        return true;
    }
    std::string reason;   // every connected player must have the same plugins + mods (onlinecompat.cpp)
    if (!onlinecompat::AllPassed(snap, &reason)) {
        if (why) *why = reason;
        return true;
    }
    return false;
}

// A co-op session for the lobby's players (coop.cpp): CO-OPERATIVE CAMPAIGN -> campaign -> difficulty -> co-op mission
// list; the level load is mirrored to the players (OnFrameOnline), owner mode (authority.cpp) keeps the games together.
void features::OnlineStartSession() {
    const net::Snapshot snap = net::Snap();
    int players = features::LobbyPlayers();   // every PC's local players, packed (lobbynet.cpp)
    if (players < 2) {
        players = 1;
        for (const auto& p : snap.peers) players += p.connected;
    }
    features::BeginOnlineSession(players, true);
}

std::vector<std::string> features::OnlineStatusLines(bool host) {
    std::vector<std::string> out;
    const net::Snapshot snap = net::Snap();
    char buf[200];
    if (host) {
        if (snap.role != net::Snapshot::Role::Host) out.push_back("Not hosting.");
        else if (!snap.gathered || snap.code.empty()) out.push_back("Finding your addresses ...");
        else {
            out.push_back("JOIN CODE: " + ShownCode(snap.code));
            out.push_back(snap.routerPort.empty() ? "Router port not opened - a friend may need to send a reply code"
                                                  : "Router port opened: " + Masked(snap.routerPort));
        }
        for (const auto& p : snap.peers) {
            if (p.connected) snprintf(buf, sizeof buf, "PC %d connected - %s, ping %d ms", p.pc, Masked(p.path).c_str(), p.ping);
            else snprintf(buf, sizeof buf, "PC %d %s", p.pc, p.punching ? "connecting (reply code) ..." : "connecting ...");
            out.push_back(buf);
        }
    } else if (!snap.peers.empty() && snap.peers[0].connected) {
        snprintf(buf, sizeof buf, "Connected to the host - %s, ping %d ms", Masked(snap.peers[0].path).c_str(), snap.peers[0].ping);
        out.push_back(buf);
    } else {
        out.push_back(snap.role == net::Snapshot::Role::None ? "The session ended." : "Connecting to the host ...");
    }
    for (const std::string& c : onlinecompat::StatusLines(host, 4)) out.push_back(c);
    if (host)
        for (const std::string& l : features::ModSyncHostLines()) out.push_back(l);
    if (!g_notice.empty() && GetTickCount() - g_noticeAt < 6000) out.push_back(g_notice);
    return out;
}

// The lobby's own notices (copied / pasted ...) on the same line as the online screens'
void features::OnlineNotice(const char* text) { Notice(text); }
// A joiner's game wants to start a level by itself (MISSION SUCCESSFUL > Continue, a menu): in a session only the host's
// START loads levels (2-home session 2026-10-22: the joiner's own Continue loaded Mission 2 with the old mission start,
// the host's START came a moment later, the joiner loaded it again and waited for good). Held back; the host's START
// follows when the host continues.
bool features::OnlineJoinerHoldsLevel() {
    const net::Snapshot snap = net::Snap();
    if (snap.role != net::Snapshot::Role::Join) return false;
    bool connected = false;
    for (const auto& p : snap.peers) connected |= p.connected;
    if (!connected) return false;
    static DWORD told = 0;
    if (GetTickCount() - told > 3000) {
        told = GetTickCount();
        dslog::Write("Online: this PC's own level start held back - the host continues for everyone");
        features::OnlineLogLine("Online: this PC's own level start held back - waiting for the host's START");
    }
    return true;
}
bool features::OnlineCopyJoinCode() {
    const net::Snapshot snap = net::Snap();
    if (snap.role != net::Snapshot::Role::Host || snap.code.empty()) return false;
    return SetClipboard(snap.code);
}
bool features::OnlinePasteReplyCode() { return net::AddReplyCode(Clipboard()); }

const char* features::OnlineText(uint32_t hash) {
    switch (hash) {
        case kTextCoop: return "CO-OP";
        case kTextSplit: return "SPLIT SCREEN";
        case kTextOnline: return "ONLINE";
        case kTextOnlineTitle: return "ONLINE CO-OP";
        case kTextHost: return "HOST GAME";
        case kTextJoin: return "JOIN GAME";
        case kTextHostTitle: return "HOST ONLINE GAME";
        case kTextJoinTitle: return "JOIN ONLINE GAME";
        case kTextCopyCode: return "COPY JOIN CODE";
        case kTextPasteReply: return "PASTE REPLY CODE";
        case kTextPasteCode: return "PASTE JOIN CODE";
        case kTextCopyReply: return "COPY REPLY CODE";
        case kTextStart: return "START";
        default: return nullptr;
    }
}

// Every frame: the host mirrors its level loads to the connected players; a player's game loads what the host loads.
void features::OnFrameOnline() {
    static bool wasLoading = false;
    const bool loading = *reinterpret_cast<int8_t*>(kDispatchMode) == 1;
    const net::Snapshot snap = net::Snap();
    if (loading && !wasLoading && snap.role == net::Snapshot::Role::Host) {
        bool anyone = false;
        for (const auto& p : snap.peers) anyone |= p.connected;
        const char* level = *reinterpret_cast<char**>(kNextLevel);
        const auto* diff = *reinterpret_cast<const uint8_t* const*>(kDifficulty);
        if (anyone && level && level[0]) {
            char cmd[512];
            const std::string host = features::HostSyncToken();   // hostsync.cpp: the host's simulation settings
            snprintf(cmd, sizeof cmd, "START %s %d %u %d %d %d %s", level, diff ? *reinterpret_cast<const int*>(diff + 4) : 1,
                     features::LockstepSeed(), settings::Get().skipCutscenes ? 1 : 0, features::CoopPlayers(),
                     *reinterpret_cast<const int*>(kUniform), host.c_str());
            features::ApplyHostSyncToken(host.c_str(), true);
            features::LobbySendBeforeStart();   // every PC starts with the same players (commands arrive in order)
            // only to players whose plugins + mods matched (onlinecompat.cpp); the others are refused anyway
            for (size_t i = 0; i < snap.peers.size(); ++i) {
                if (!snap.peers[i].connected) continue;
                const int player = snap.peers[i].pc;
                if (onlinecompat::PeerState(player) == onlinecompat::State::Ok) net::SendCommandTo(player, cmd);
                else dslog::Write("[fail] Online: START not sent to player %d - plugins / mods not checked or different",
                                  player);
            }
        }
    }
    wasLoading = loading;
    onlinecompat::Frame(snap);
    features::OnFrameOnlinePause();
    {   // back in the front end while the session is still up (a mission left through the online menu, the end screen's
        // MAIN MENU, the host going back): on to the lobby instead of the main menu
        bool anyone = false;
        for (const auto& p : snap.peers) anyone |= p.connected;
        if (anyone && snap.role != net::Snapshot::Role::None && features::LobbyOnline() &&
            *reinterpret_cast<uint32_t*>(0x617C18) == 1 && *reinterpret_cast<uint32_t*>(0x617C20) == 0 && !loading) {
            dslog::Write("Online: back in the menus with the session up - the lobby");
            SetState(kStateLobby);
        }
    }
    features::OnFrameLobby();
    features::OnFrameModSync();
    for (const net::Command& c : net::TakeCommands()) {
        if (onlinecompat::Command(c)) continue;   // "PSEC ...": the plugin / mod lists
        if (features::LobbyCommand(c.from, c.text)) continue;   // "LB ...": the lobby's players
        if (features::LoadoutCommand(c.from, c.text)) continue;   // "LO ...": the loadout screen online
        if (features::ModSyncCommand(c.from, c.text)) continue;   // "MS ...": the host's mods to a joiner
        if (c.text.compare(0, 3, "EA ") == 0) {   // the host's damage / shot events (authority.cpp)
            features::AuthorityCommand(c.text);   // (every PC sends its own soldiers' damage / shots)
            // 3-4 players: a joiner's command goes to the host only - passed on to the other joiners (it comes back to
            // the joiner that sent it too; every handler ignores what that PC already has / owns)
            if (net::LocalPlayer() == 1 && c.from > 1 && features::LobbyPcs() > 2) net::SendCommand(c.text);
            continue;
        }
        char level[256] = {};
        char host[160] = {};
        int difficulty = 1, skip = 0, players = 2, uniform = 0;
        uint32_t seed = 0;
        if (c.from != 1 || snap.role != net::Snapshot::Role::Join ||
            sscanf(c.text.c_str(), "START %255s %d %u %d %d %d %159s", level, &difficulty, &seed, &skip, &players, &uniform,
                   host) < 1)
            continue;
        if (onlinecompat::PeerState(1) != onlinecompat::State::Ok) {
            features::OnlineLogLine("[fail] Online: the host's START ignored - plugins / mods not checked or different");
            dslog::Write("[fail] Online: START from the host ignored - the plugin / mod check hasn't passed");
            continue;
        }
        reinterpret_cast<void(__cdecl*)(int)>(kSetUniform)(uniform);   // the host's uniform (skills, skins, speech)
        features::ApplyHostSyncToken(host, false);                       // time of day, voices, followers, cheats (hostsync.cpp)
        features::LockstepSetSeed(seed, skip);
        const bool mission = _strnicmp(level, "FrontEnd", 8) != 0;
        if (mission && features::CoopPlayers() != players) features::BeginOnlineSession(players, false);
        if (void* d = *reinterpret_cast<void**>(kDifficulty))
            reinterpret_cast<void(__thiscall*)(void*, int)>(kSetDifficulty)(d, difficulty);
        dslog::Write("Online: the host loads %s (difficulty %d) - loading it here", level, difficulty);
        features::ConsoleStartLevel(level);
    }
}
