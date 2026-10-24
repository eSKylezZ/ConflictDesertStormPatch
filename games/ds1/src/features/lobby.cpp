// The co-op lobby's player table and its host rules / wire format (see lobby.h). No game memory here: pse_tests
// includes this file.
#include "features/lobby.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace lobby {

int Table::Count() const {
    int n = 0;
    for (const Slot& s : slots) n += s.used;
    return n;
}
uint32_t Table::MaskOf(int pc) const {
    uint32_t m = 0;
    for (int k = 0; k < kMaxPlayers; ++k)
        if (slots[k].used && slots[k].pc == pc) m |= 1u << k;
    return m;
}
int Table::Pcs() const {
    uint32_t seen = 0;
    for (const Slot& s : slots)
        if (s.used && s.pc < 32) seen |= 1u << s.pc;
    int n = 0;
    for (; seen; seen &= seen - 1) ++n;
    return n;
}
bool Table::AllReady() const {
    int n = 0;
    for (const Slot& s : slots) {
        if (!s.used) continue;
        if (!s.ready) return false;
        ++n;
    }
    return n > 0;
}
int Table::FirstOf(int pc) const {
    for (int k = 0; k < kMaxPlayers; ++k)
        if (slots[k].used && slots[k].pc == pc) return k;
    return -1;
}

std::string CleanName(const std::string& name) {
    std::string out;
    for (char c : name)
        if (c >= 0x20 && c < 0x7F) out += c;
    const size_t a = out.find_first_not_of(' '), b = out.find_last_not_of(' ');
    out = a == std::string::npos ? std::string() : out.substr(a, b - a + 1);
    if (out.size() > kNameMax) out.resize(kNameMax);
    return out;
}

namespace {
bool Valid(const Table& t, int player) { return player >= 0 && player < kMaxPlayers && t.slots[player].used; }
bool ColourFree(const Table& t, int colour, int except) {
    for (int k = 0; k < kMaxPlayers; ++k)
        if (k != except && t.slots[k].used && t.slots[k].colour == colour) return false;
    return true;
}
}  // namespace

int Add(Table& t, int pc, int device, const std::string& name) {
    if (pc <= 0 || device < kDeviceAny || device > kDeviceKeyboard) return -1;
    int free = -1;
    for (int k = 0; k < kMaxPlayers; ++k) {
        const Slot& s = t.slots[k];
        if (!s.used) {
            if (free < 0) free = k;
            continue;
        }
        if (s.pc == pc && (s.device == device || s.device == kDeviceAny || device == kDeviceAny)) return -1;
    }
    if (free < 0) return -1;
    Slot& s = t.slots[free];
    s = Slot{};
    s.used = true;
    s.pc = static_cast<uint8_t>(pc);
    s.device = static_cast<int8_t>(device);
    s.name = CleanName(name);
    if (s.name.empty()) s.name = "Player " + std::to_string(free + 1);
    for (int c = 0; c < kColours; ++c)
        if (ColourFree(t, c, free)) {
            s.colour = static_cast<uint8_t>(c);
            break;
        }
    ++t.version;
    return free;
}

bool Remove(Table& t, int player) {
    if (!Valid(t, player)) return false;
    for (int k = player; k + 1 < kMaxPlayers; ++k) t.slots[k] = t.slots[k + 1];
    t.slots[kMaxPlayers - 1] = Slot{};
    ++t.version;
    return true;
}

int RemovePc(Table& t, int pc) {
    int n = 0;
    for (int k = kMaxPlayers - 1; k >= 0; --k)
        if (t.slots[k].used && t.slots[k].pc == pc) Remove(t, k), ++n;
    return n;
}

bool SetName(Table& t, int player, const std::string& name) {
    if (!Valid(t, player)) return false;
    const std::string n = CleanName(name);
    if (n.empty() || n == t.slots[player].name) return false;
    t.slots[player].name = n;
    ++t.version;
    return true;
}
bool SetColour(Table& t, int player, int colour) {
    if (!Valid(t, player) || colour < 0 || colour >= kColours || !ColourFree(t, colour, player)) return false;
    if (t.slots[player].colour == colour) return false;
    t.slots[player].colour = static_cast<uint8_t>(colour);
    ++t.version;
    return true;
}
bool SetView(Table& t, int player, View view) {
    if (!Valid(t, player) || t.slots[player].view == view) return false;
    t.slots[player].view = view;
    ++t.version;
    return true;
}
bool SetReady(Table& t, int player, bool ready) {
    if (!Valid(t, player) || t.slots[player].ready == ready) return false;
    t.slots[player].ready = ready;
    ++t.version;
    return true;
}

bool SetDevice(Table& t, int player, int device) {
    if (!Valid(t, player) || device < kDeviceAny || device > kDeviceKeyboard || t.slots[player].device == device) return false;
    for (int k = 0; k < kMaxPlayers; ++k) {
        const Slot& o = t.slots[k];
        if (k == player || !o.used || o.pc != t.slots[player].pc) continue;
        if (o.device == device || o.device == kDeviceAny || device == kDeviceAny) return false;
    }
    t.slots[player].device = static_cast<int8_t>(device);
    ++t.version;
    return true;
}

std::string Hex(const std::string& s) {
    static const char* d = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) out += d[c >> 4], out += d[c & 15];
    return out.empty() ? "-" : out;
}
std::string Unhex(const std::string& s) {
    if (s == "-") return {};
    std::string out;
    for (size_t k = 0; k + 1 < s.size(); k += 2) out += static_cast<char>(std::strtoul(s.substr(k, 2).c_str(), nullptr, 16));
    return out;
}

std::string Encode(const Table& t) {
    std::string out = "LB T " + std::to_string(t.version);
    for (const Slot& s : t.slots) {
        if (!s.used) {
            out += " -";
            continue;
        }
        char buf[64];
        snprintf(buf, sizeof buf, " %u,%d,%u,%u,%u,", s.pc, s.device, s.colour, static_cast<unsigned>(s.view),
                 s.ready ? 1u : 0u);
        out += buf + Hex(s.name);
    }
    return out;
}

bool Decode(const std::string& text, Table& out) {
    std::istringstream in(text);
    std::string lb, t;
    Table r;
    if (!(in >> lb >> t >> r.version) || lb != "LB" || t != "T") return false;
    for (Slot& s : r.slots) {
        std::string f;
        if (!(in >> f)) return false;
        if (f == "-") continue;
        unsigned pc, colour, view, ready;
        int device;
        char name[64] = {};
        if (sscanf_s(f.c_str(), "%u,%d,%u,%u,%u,%63s", &pc, &device, &colour, &view, &ready, name,
                     static_cast<unsigned>(sizeof name)) != 6)
            return false;
        if (pc == 0 || pc > 255 || device < kDeviceAny || device > kDeviceKeyboard || colour >= kColours || view > 1)
            return false;
        s.used = true;
        s.pc = static_cast<uint8_t>(pc);
        s.device = static_cast<int8_t>(device);
        s.colour = static_cast<uint8_t>(colour);
        s.view = static_cast<View>(view);
        s.ready = ready != 0;
        s.name = CleanName(Unhex(name));
    }
    out = r;
    return true;
}

std::string JoinRequest(int device, const std::string& name) {
    return "LB J " + std::to_string(device) + " " + Hex(CleanName(name));
}

bool Apply(Table& t, int fromPc, const std::string& request) {
    std::istringstream in(request);
    std::string lb, op;
    if (!(in >> lb >> op) || lb != "LB") return false;
    if (op == "J") {
        int device;
        std::string name;
        if (!(in >> device >> name)) return false;
        return Add(t, fromPc, device, Unhex(name)) >= 0;
    }
    int player;
    if (!(in >> player) || !Valid(t, player) || t.slots[player].pc != fromPc) return false;   // only its own players
    if (op == "L") return Remove(t, player);
    std::string arg;
    if (!(in >> arg)) return false;
    if (op == "N") return SetName(t, player, Unhex(arg));
    if (op == "C") return SetColour(t, player, std::atoi(arg.c_str()));
    if (op == "V") return SetView(t, player, std::atoi(arg.c_str()) ? View::Own : View::Split);
    if (op == "R") return SetReady(t, player, std::atoi(arg.c_str()) != 0);
    if (op == "D") return SetDevice(t, player, std::atoi(arg.c_str()));
    return false;
}

}  // namespace lobby
