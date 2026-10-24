#pragma once
// Online co-op, owner mode (authority.cpp): what a soldier's owner PC sends about it every 50 ms, and what another PC
// does to its puppet copy so it shows the same thing. Pure code - no game memory - so pse_tests checks it out of game
// (pse/tests/test_puppet_sync.cpp); authority.cpp reads a View from the game and runs the Plan through the game's own
// functions.
//
// Held item (2-PC videos 2026-10-22, Mission 3): the records carried the hold *state* (+0x2688: lowered / raising /
// ready / reloading) but not *which* item was held (+0x26B0), as in Pivotal's versus records (FUN_0041db30), whose
// clients switched items through separate network messages (FUN_0042ec10's network branch). The puppet kept the rifle
// while its owner went pistol -> grenade -> binoculars -> M203 -> medikit; the owner's pistol shots were replayed on the
// puppet's rifle and its reloads never happened, so the puppet's ammo ran down to a RELOAD prompt.
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace puppetsync {

constexpr uint8_t kPacketType = 10;   // 'L' 10 (9 = before the held item; 5 = before aim mode / step)
constexpr size_t kHeader = 12;        // 'L', type, u8 count, u8 sender player, u32 sequence, u32 epoch
constexpr size_t kPerPacket = 22;     // 12 + 22 x 60 = 1332 B (net.cpp sends at most 1400)

#pragma pack(push, 1)
struct Rec {
    uint16_t id;
    float pos[3], heading, aim[3], health;
    int16_t move, stance, hold;   // +0x2674 movement state, +0x348 stance, +0x2688 hold state
    float pending;                // +0x2B84, the medikit heal still to drip in
    int8_t step;                  // +0x2A60, the puppet mover's turn / step state
    int8_t aimMode;               // the controlling player's block +0x318 (0 normal, 1 aiming), -1 = no player's
    uint32_t itemHash;            // held item +0x26B0 -> definition +0x24 -> name hash +0x80; 0 = nothing held
    int8_t itemSlot;              // its index in the inventory [+0x26B8] (25), -1 = not in it
    uint8_t itemIsWeapon;         // item +4 = 6: the rounds / fire mode below mean something
    uint16_t fireMode;            // weapon +0x5C (1 single, 2 burst, 4 auto, 0x100 grenade launcher)
    int16_t rounds[3];            // weapon +0x54, +0x78, +0x80: the three counters the reload FUN_004a1350 moves
};
#pragma pack(pop)
static_assert(sizeof(Rec) == 60, "record layout is the wire format");
static_assert(kHeader + kPerPacket * sizeof(Rec) <= 1400, "packet over net.cpp's send limit");

struct Item {
    uint32_t hash = 0;   // 0 = empty slot
    bool weapon = false;
    uint16_t fireMode = 0;
    int16_t rounds[3] = {};   // +0x54, +0x78, +0x80
    bool SameRounds(const int16_t* r) const { return rounds[0] == r[0] && rounds[1] == r[1] && rounds[2] == r[2]; }
};
constexpr int kInventorySlots = 25;

// A soldier as the sync sees it (the owner's copy when sending, the puppet when applying).
struct View {
    int move = 0, stance = 0, hold = 0, step = 0;
    int aimMode = -1;     // controlling block's mode, -1 = no player's block
    int heldSlot = -1;    // -1 = nothing held / not in the inventory
    Item held;            // the held item (hash 0 = none)
    Item inventory[kInventorySlots];
};

// Owner side: the record for one owned soldier (pos / heading / aim / health / pending are the caller's).
Rec Encode(uint16_t id, const View& v);

// Puppet side: what to do so the puppet shows the owner's record, in this order.
struct Plan {
    // 1. hold another item: FUN_0042ec10(soldier, item, 1) - selectNone = hold nothing
    bool select = false, selectNone = false;
    int selectSlot = -1;          // the puppet's inventory slot holding the owner's item
    bool itemMissing = false;     // the owner holds an item this puppet doesn't carry (nothing to select)
    // 2. the versus applier FUN_0041eb90's mask: always 4 | 8 | 0x10 (position, heading, aim), plus 1 move, 2 hold,
    //    0x40 stance, 0x80 step where the puppet differs
    uint32_t applierMask = 0;
    // 3. the (newly) held weapon's fire mode (FUN_004a4fb0 - swaps the M203 pools), then its rounds
    bool setFireMode = false, setRounds = false;
    // 4. the hold state set directly (the applier's own path is gated by the movement state table)
    bool setHold = false;
    // 5. the controlling block's aim mode (FUN_004500b0)
    bool setAimMode = false;
};
// Which slot of `v` holds an item with `hash`: `prefer` first (the owner's slot), else the first match; -1 none.
int FindSlot(const View& v, uint32_t hash, int prefer);
Plan MakePlan(const Rec& owner, const View& puppet);

// Records in, newest per soldier out. Packets of another mission start (epoch) are dropped and counted.
struct Received {
    Rec rec{};
    uint32_t seq = 0;   // sender's sequence (0 = none yet)
    bool fresh = false;
};
class Receiver {
public:
    // false = not a record packet of this protocol / truncated
    bool Take(const uint8_t* data, size_t size, uint32_t epoch);
    std::map<uint16_t, Received>& Soldiers() { return soldiers_; }
    uint32_t otherEpochs = 0, lastOtherEpoch = 0, stale = 0;

private:
    std::map<uint16_t, Received> soldiers_;
};
// The packets carrying `recs` (split at kPerPacket).
std::vector<std::vector<uint8_t>> Packets(const std::vector<Rec>& recs, int sender, uint32_t seq, uint32_t epoch);

}  // namespace puppetsync
