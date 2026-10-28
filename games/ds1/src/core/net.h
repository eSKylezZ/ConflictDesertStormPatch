#pragma once

// Online connection without any server of ours: the host gets a join code (its addresses), a friend pastes it and
// connects. The host's router port is opened automatically (PCP / NAT-PMP / UPnP) and the public address comes from
// public STUN servers; when neither side can be reached directly the joiner shows a reply code, and once the host
// pastes it both sides send at once (UDP hole punching). IPv6 addresses go into the codes too (no NAT there).
//
// One UDP port per game (27616, the next free one when several copies run on one PC), served by a worker thread -
// the game thread never blocks and the connection stays up while the game window is in the background.
// No game data is exchanged yet: keep-alive pings, round-trip time and short text messages (net say) - the transport
// that co-op input sync will use.
#include <cstdint>
#include <string>
#include <vector>

namespace net {
enum class Map { Auto, Pcp, NatPmp, Upnp, None };   // how the host's router port is opened (Auto: in that order)
enum class Paths { All, Lan, Public, V6 };          // which addresses a joiner tries / puts in its reply code (tests)

struct Event {
    enum Kind { Info, Code, ReplyCode, Message } kind;
    std::string text;   // Code / ReplyCode: the code itself
};

bool Host(Map map = Map::Auto);
// tests: openPort false = the joiner leaves its router alone; hello false = it only answers the host's punches (the
// reply code is the only way in)
bool Join(const std::string& code, Paths paths = Paths::All, bool openPort = true, bool hello = true);
bool AddReplyCode(const std::string& code);   // host: a joiner's reply code -> hole punching towards it
bool Say(const std::string& text);            // to every connected peer (reliable)
// Game commands: reliable, in order, to every connected peer ("START Mission3.dll 1" ...); not shown as messages.
struct Command {
    int from;   // player number: 1 = the host, 2.. = joiners
    std::string text;
};
bool SendCommand(const std::string& text);
// The same to one player only (1 = the host, 2.. = joiners as in Command::from); false = not connected.
bool SendCommandTo(int player, const std::string& text);
// Host: ends one joiner's connection (BYE; the others stay). Joiners use Stop().
bool Drop(int player);
// Game commands waiting for an acknowledgement from that player (0 = everything sent so far arrived).
int Unacked(int player);
// Unreliable game data (lockstep inputs, world hashes): up to ~1380 bytes, to every connected peer; may be lost or
// arrive twice / out of order - the payload says what it is.
struct Raw {
    int from;   // player number
    std::vector<uint8_t> data;
};
int SendRaw(const void* data, int len);       // peers it went to
bool SendRawTo(int player, const void* data, int len);   // one player (1 = host, 2.. joiners); false = not connected
std::vector<Raw> TakeRaw();
int LocalPlayer();                            // this PC's number: 1 = host, 2.. = joiner (host-assigned), 0 = no session
std::vector<Command> TakeCommands();          // game thread
void Stop();                                  // goodbye to peers, router port closed again; blocks up to ~2 s
bool Active();
std::vector<std::string> Status();
std::vector<Event> TakeEvents();              // game thread: what happened since the last call

// For the menus: the session as it is now.
struct Snapshot {
    enum class Role { None, Host, Join } role = Role::None;
    bool gathered = false;      // addresses found: `code` is ready
    std::string code;           // host: the join code; joiner: its reply code
    std::string routerPort;     // "NAT-PMP 1.2.3.4:27616", "" = none
    bool strictNat = false;
    struct Peer {
        uint32_t id;            // the connection's nonce: another id at the same player number = a new connection
        bool connected;
        std::string path;       // "same network (LAN)", "internet - hole punched" ...
        int ping;               // ms, -1 = not measured yet
        bool punching;
        int pc;                 // its PC number: host 1, joiners 2, 3 ... (stable while connected)
    };
    std::vector<Peer> peers;    // host: joiners (player 2..); joiner: the host
    std::vector<std::string> recent;   // the last few info lines (newest last)
};
Snapshot Snap();
bool IsJoinCode(const std::string& text);   // a well-formed join code (not a reply code)
}  // namespace net
