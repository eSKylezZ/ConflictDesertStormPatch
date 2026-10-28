// The co-op lobby over the network (the table and its rules: lobby.h). The host keeps the table: its own first player
// goes in when it starts hosting, a joiner's first player is asked for once the host's table has arrived (again every
// 3 s while it isn't in), more players / names / colours / views / ready come from the lobby screen (lobbymenu.cpp)
// as requests ("LB ..." to the host, lobbynet::Request), and a PC that leaves loses its players. The table goes to
// every PC on each change, every 2 s, and just before every START (the commands to one PC arrive in order, so a joiner
// always has the table the mission starts with). While a mission runs the table is frozen (requests refused).
//
// Owner mode (authority.cpp / lockstep.cpp) asks it which players this PC runs (LobbyLocalMask) and with which device
// each one is read (LobbyDevice).
#include "features/lobbynet.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/net.h"
#include "core/settings.h"
#include "features/features.h"

namespace {

lobby::Table g_table;
bool g_have = false;                   // a joiner: a table from the host has arrived
net::Snapshot::Role g_role = net::Snapshot::Role::None;
uint32_t g_sent = 0xFFFFFFFF;          // host: the version last sent
DWORD g_lastSend = 0, g_lastJoin = 0;

bool Frozen() { return features::LockstepActive(); }   // a mission runs: the players stay as they are

void Broadcast(const char* why) {
    net::SendCommand(lobby::Encode(g_table));
    g_sent = g_table.version;
    g_lastSend = GetTickCount();
    dslog::Write("Lobby: table v%u sent (%s) - %d player(s) on %d PC(s)", g_table.version, why, g_table.Count(),
                 g_table.Pcs());
}

void Log(const char* what) {
    char line[320];
    std::string players;
    for (int k = 0; k < lobby::kMaxPlayers; ++k) {
        const lobby::Slot& s = g_table.slots[k];
        if (!s.used) continue;
        char one[80];
        snprintf(one, sizeof one, " P%d=%s(PC %d, dev %d, %s%s%s)", k + 1, s.name.c_str(), s.pc, s.device,
                 lobby::kColourNames[s.colour], s.view == lobby::View::Own ? ", own view" : "", s.ready ? ", ready" : "");
        players += one;
    }
    snprintf(line, sizeof line, "Lobby: %s v%u:%s", what, g_table.version, players.c_str());
    features::OnlineLogLine(line);
}

int Mine() {
    int n = 0;
    for (const lobby::Slot& s : g_table.slots) n += s.used && s.pc == lobbynet::MyPc();
    return n;
}

}  // namespace

const lobby::Table& lobbynet::Current() { return g_table; }
bool lobbynet::IsHost() { return g_role == net::Snapshot::Role::Host; }
int lobbynet::MyPc() { return g_role == net::Snapshot::Role::Host ? 1 : g_role == net::Snapshot::Role::Join ? net::LocalPlayer() : 0; }

bool lobbynet::Request(const std::string& text) {
    if (Frozen()) return false;
    if (g_role == net::Snapshot::Role::Host) {
        if (!lobby::Apply(g_table, 1, text)) return false;
        Log(("own request " + text.substr(0, 4)).c_str());
        return true;
    }
    if (g_role != net::Snapshot::Role::Join) return false;
    return net::SendCommandTo(1, text);
}

std::string lobbynet::DefaultName(int k) {   // "PlayerName<k+1>" (settings), else the Windows user name
    char key[24];
    snprintf(key, sizeof key, "PlayerName%d", k + 1);
    std::string name;
    if (settings::ReadString(key, name) && !lobby::CleanName(name).empty()) return lobby::CleanName(name);
    char user[64] = {};
    DWORD n = sizeof user;
    if (GetUserNameA(user, &n) && user[0]) name = user;
    else name = "Player";
    name = lobby::CleanName(name);
    if (k > 0) name = lobby::CleanName(name.substr(0, lobby::kNameMax - 2) + " " + std::to_string(k + 1));
    return name;
}

void lobbynet::RememberName(int k, const std::string& name) {
    char key[24];
    snprintf(key, sizeof key, "PlayerName%d", k + 1);
    const std::string clean = lobby::CleanName(name);
    if (!clean.empty() && k >= 0 && k < lobby::kMaxPlayers) settings::WriteString(key, clean);
}

int lobbynet::LocalIndex(int player) {
    if (player < 0 || player >= lobby::kMaxPlayers || !g_table.slots[player].used || g_table.slots[player].pc != MyPc())
        return -1;
    int k = 0;
    for (int i = 0; i < player; ++i) k += g_table.slots[i].used && g_table.slots[i].pc == MyPc();
    return k;
}

void features::OnFrameLobby() {
    const net::Snapshot snap = net::Snap();
    if (snap.role != g_role) {   // a new session (or none): start over
        g_role = snap.role;
        g_table = lobby::Table{};
        g_have = false;
        g_sent = 0xFFFFFFFF;
        g_lastJoin = 0;
        if (g_role == net::Snapshot::Role::Host) {
            lobby::Add(g_table, 1, lobby::kDeviceAny, lobbynet::DefaultName(0));
            Log("hosting - own player");
        }
    }
    const DWORD now = GetTickCount();
    if (g_role == net::Snapshot::Role::Host) {
        if (!Frozen()) {   // PCs no longer connected lose their players
            for (int k = lobby::kMaxPlayers - 1; k >= 0; --k) {
                const lobby::Slot& s = g_table.slots[k];
                if (!s.used || s.pc == 1) continue;
                bool live = false;
                for (const auto& p : snap.peers) live |= p.connected && p.pc == s.pc;
                if (!live && lobby::RemovePc(g_table, s.pc)) Log("a PC left");
            }
            if (!Mine()) lobby::Add(g_table, 1, lobby::kDeviceAny, lobbynet::DefaultName(0));   // never without one
        }
        bool anyone = false;
        for (const auto& p : snap.peers) anyone |= p.connected;
        if (anyone && (g_table.version != g_sent || now - g_lastSend >= 2000))
            Broadcast(g_table.version != g_sent ? "changed" : "repeat");
    } else if (g_role == net::Snapshot::Role::Join && !Frozen()) {
        // this PC's first player: asked for once the host's table is here, again while it isn't in it
        const bool connected = !snap.peers.empty() && snap.peers[0].connected;
        if (connected && g_have && !Mine() && now - g_lastJoin >= 3000) {
            g_lastJoin = now;
            net::SendCommandTo(1, lobby::JoinRequest(lobby::kDeviceAny, lobbynet::DefaultName(0)));
        }
    }
}

bool features::LobbyCommand(int from, const std::string& text) {
    if (text.compare(0, 3, "LB ") != 0) return false;
    if (g_role == net::Snapshot::Role::Host) {
        if (Frozen()) {
            dslog::Write("Lobby: request from PC %d refused - a mission runs: %s", from, text.c_str());
        } else if (lobby::Apply(g_table, from, text)) {
            Log(("PC " + std::to_string(from) + " - " + text.substr(3, 1)).c_str());
        }
    } else if (from == 1) {
        lobby::Table t;
        if (lobby::Decode(text, t) && (!g_have || t.version >= g_table.version || t.Count() != g_table.Count())) {
            const bool changed = !g_have || lobby::Encode(t) != lobby::Encode(g_table);
            g_table = t;
            g_have = true;
            if (changed) Log("the host's table");
        }
    }
    return true;
}

// Before the host's START: the table every PC starts the mission with (in order before START on each connection).
void features::LobbySendBeforeStart() {
    if (g_role == net::Snapshot::Role::Host) Broadcast("start");
}

bool features::LobbyOnline() {
    return g_role == net::Snapshot::Role::Host || (g_role == net::Snapshot::Role::Join && g_have);
}
int features::LobbyPlayers() { return LobbyOnline() ? g_table.Count() : 0; }
int features::LobbyPcs() {   // no table in a session: one player per PC
    return LobbyOnline() ? g_table.Pcs() : net::LocalPlayer() > 0 ? features::CoopPlayers() : 0;
}
uint32_t features::LobbyLocalMask() {
    if (!LobbyOnline()) {   // no table: one player per PC (player n = PC n + 1), no session = every player is this PC's
        const int pc = net::LocalPlayer();
        return pc > 0 ? 1u << (pc - 1) : 0xFu;
    }
    return g_table.MaskOf(lobbynet::MyPc());
}
// The first player of every PC - the one whose number a PC's packets carry (lockstep.cpp waits for / listens to these)
uint32_t features::LobbySenderMask(int players) {
    if (!LobbyOnline()) return players >= 32 ? 0xFFFFFFFFu : (1u << players) - 1;
    uint32_t m = 0;
    for (int k = 0; k < lobby::kMaxPlayers; ++k)
        if (g_table.slots[k].used && g_table.FirstOf(g_table.slots[k].pc) == k) m |= 1u << k;
    return m;
}
bool features::LobbyLocal(int player) { return player >= 0 && player < 32 && (LobbyLocalMask() >> player) & 1; }
int features::LobbyDevice(int player) {
    if (!LobbyOnline() || player < 0 || player >= lobby::kMaxPlayers || !g_table.slots[player].used)
        return lobby::kDeviceAny;
    return g_table.slots[player].device;
}
int features::LobbyFirstLocal() {
    const uint32_t m = LobbyLocalMask();
    for (int k = 0; k < lobby::kMaxPlayers; ++k)
        if ((m >> k) & 1) return k;
    return 0;
}

// Console "lobby ..." (the lobby screen does the same with the players' devices): list, add <pad 0..7 | kb>,
// remove <player>
std::vector<std::string> features::LobbyConsole(const std::string& arg) {
    std::vector<std::string> out;
    char buf[160];
    if (arg.empty() || arg == "list") {
        snprintf(buf, sizeof buf, "lobby v%u: %d player(s) on %d PC(s) - this PC is %d, its players mask %X",
                 g_table.version, g_table.Count(), g_table.Pcs(), lobbynet::MyPc(), LobbyLocalMask());
        out.push_back(buf);
        for (int k = 0; k < lobby::kMaxPlayers; ++k) {
            const lobby::Slot& s = g_table.slots[k];
            if (!s.used) continue;
            const std::string dev = s.device == lobby::kDeviceAny        ? "kb+pad"
                                    : s.device == lobby::kDeviceKeyboard ? "keyboard"
                                                                         : "pad " + std::to_string(s.device);
            snprintf(buf, sizeof buf, "  P%d %-15s PC %d  device %s  %s  %s%s", k + 1, s.name.c_str(), s.pc, dev.c_str(),
                     lobby::kColourNames[s.colour], s.view == lobby::View::Own ? "own view" : "split", s.ready ? ", ready" : "");
            out.push_back(buf);
        }
        return out;
    }
    if (g_role == net::Snapshot::Role::None) return {"no online session (net host / net join first)"};
    if (arg.compare(0, 4, "add ") == 0) {
        const std::string d = arg.substr(4);
        const int device = d == "kb" ? lobby::kDeviceKeyboard : std::atoi(d.c_str());
        if (device < 0 || device > lobby::kDeviceKeyboard) return {"lobby add <pad 0..7 | kb>"};
        // this PC's "keyboard + first pad" player keeps the keyboard (or pad 0 when the keyboard is the one joining)
        for (int k = 0; k < lobby::kMaxPlayers; ++k)
            if (g_table.slots[k].used && g_table.slots[k].pc == lobbynet::MyPc() && g_table.slots[k].device == lobby::kDeviceAny)
                lobbynet::Request("LB D " + std::to_string(k) + " " +
                                  std::to_string(device == lobby::kDeviceKeyboard ? 0 : lobby::kDeviceKeyboard));
        const bool ok = lobbynet::Request(lobby::JoinRequest(device, lobbynet::DefaultName(Mine())));
        return {ok ? (lobbynet::IsHost() ? "added" : "asked the host") : "not added"};
    }
    if (arg.compare(0, 7, "remove ") == 0) {
        const int player = std::atoi(arg.substr(7).c_str()) - 1;
        return {lobbynet::Request("LB L " + std::to_string(player)) ? (lobbynet::IsHost() ? "removed" : "asked the host")
                                                                    : "not removed"};
    }
    if (arg == "yes") {   // the mod sync's question (modsync.cpp) - tests, a player without the screen's keys
        if (!features::ModSyncWaitingForYes()) return {"no question waiting"};
        features::ModSyncAccept();
        return {"getting the host's mods"};
    }
    if (arg == "leave") {   // what the lobby's Esc does: session ended, back to ONLINE CO-OP (tests: a background copy)
        features::LobbyLeave();
        return {"left the lobby"};
    }
    if (arg.compare(0, 6, "ready ") == 0) {   // a player of this PC ready (tests: a pad nobody can press)
        const int player = std::atoi(arg.substr(6).c_str()) - 1;
        return {lobbynet::Request("LB R " + std::to_string(player) + " 1") ? "ready" : "not changed"};
    }
    return {"lobby [list] | add <pad 0..7 | kb> | remove <player 1..4> | ready <player 1..4> | leave"};
}
