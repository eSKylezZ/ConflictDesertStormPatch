#pragma once
// The co-op lobby's player table (user, 2026-10-22: one lobby for local split screen + online, up to 4 players across
// PCs, each PC with one or more local players). Pure code - the net / menu glue is in lobbynet.cpp - so pse_tests
// checks the rules out of game (pse/tests/test_lobby.cpp).
//
// Player n (0..3) = table slot n = the game's input block n (0x60F5B8 + n * 0x478) = the n-th player of the split /
// squad set-up. Slots are kept packed (players 0..Count()-1): a player leaving moves the later ones down, as the local
// join screen does - only before the mission starts; the table is frozen while one runs.
//
// The host owns the table. A joiner asks (Request, "LB ..." commands, only for its own PC's slots); the host applies the
// request and sends the table to everyone (Encode, "LB T ...", also every 2 s).
#include <cstdint>
#include <string>
#include <vector>

namespace lobby {

constexpr int kMaxPlayers = 4;
constexpr int kColours = 8;   // kPalette entries; a new player gets the lowest one no one wears
// 0xRRGGBB: the first four are the DualSense light bar's player colours (padio.cpp), then orange, yellow, cyan, purple
constexpr uint32_t kPalette[kColours] = {0x2E5BD8, 0xD03232, 0x2EA844, 0xD85AB8, 0xE08A20, 0xD8C030, 0x30B8C8, 0x8050D0};
constexpr const char* kColourNames[kColours] = {"BLUE", "RED", "GREEN", "PINK", "ORANGE", "YELLOW", "CYAN", "PURPLE"};
// Device of a local player: a joystick 0..7, the keyboard & mouse, or both (a PC's only player: keyboard + first pad,
// as owner mode always sampled)
constexpr int kDeviceAny = -1, kDeviceKeyboard = 8;
enum class View : uint8_t { Split = 0, Own = 1 };   // the split showing every player / this player's view full screen

struct Slot {
    bool used = false;
    uint8_t pc = 0;          // the owning PC's number (net: host 1, joiners 2, 3 ...)
    int8_t device = kDeviceAny;
    uint8_t colour = 0;      // palette index
    View view = View::Split;
    bool ready = false;
    std::string name;        // printable ASCII, at most kNameMax
};
constexpr size_t kNameMax = 15;

struct Table {
    uint32_t version = 0;    // +1 per change: a joiner keeps the newest
    Slot slots[kMaxPlayers];
    int Count() const;
    uint32_t MaskOf(int pc) const;   // bit n = player n belongs to that PC
    int Pcs() const;                 // how many different PCs have players
    bool AllReady() const;           // every used slot ready (and at least one)
    int FirstOf(int pc) const;       // that PC's first player, -1 none
};

std::string CleanName(const std::string& name);   // printable, trimmed, cut to kNameMax ("" stays "")

// ---- host rules ----
// A new player on a PC with that device. -1 = the table is full, or that PC already has a player on that device (a
// pad joins once; kDeviceAny conflicts with every device of the same PC). Colour: the lowest palette entry no one uses.
int Add(Table& t, int pc, int device, const std::string& name);
bool Remove(Table& t, int player);   // later players move down
int RemovePc(Table& t, int pc);      // a PC that left: its players removed, returns how many
bool SetName(Table& t, int player, const std::string& name);
bool SetColour(Table& t, int player, int colour);   // refused when another player wears it
bool SetView(Table& t, int player, View view);
bool SetReady(Table& t, int player, bool ready);
// Another device for a player: refused when another player of the same PC has it (or has kDeviceAny / it is
// kDeviceAny while the PC has other players)
bool SetDevice(Table& t, int player, int device);

// ---- wire ----
// "LB T <version> <slot> <slot> <slot> <slot>", slot = "-" or "pc,device,colour,view,ready,<name as hex>"
std::string Encode(const Table& t);
bool Decode(const std::string& text, Table& out);

// A joiner's (or a local menu's) request, applied by the host for the PC that sent it:
//   "LB J <device> <hexname>"  join      "LB L <player>"            leave
//   "LB N <player> <hexname>"  name      "LB C <player> <colour>"   colour
//   "LB V <player> <0|1>"      view      "LB R <player> <0|1>"      ready
//   "LB D <player> <device>"   device
// A request for another PC's player is refused. Returns false for unknown / refused requests.
bool Apply(Table& t, int fromPc, const std::string& request);
std::string JoinRequest(int device, const std::string& name);
std::string Hex(const std::string& s);
std::string Unhex(const std::string& s);

}  // namespace lobby
