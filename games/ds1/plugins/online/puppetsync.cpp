// Owner-mode record format and puppet plan (see puppetsync.h). No game memory here: pse_tests includes this file.
#include "features/puppetsync.h"

#include <algorithm>
#include <cstring>

namespace puppetsync {

Rec Encode(uint16_t id, const View& v) {
    Rec r{};
    r.id = id;
    r.move = static_cast<int16_t>(v.move);
    r.stance = static_cast<int16_t>(v.stance);
    r.hold = static_cast<int16_t>(v.hold);
    r.step = static_cast<int8_t>(v.step);
    r.aimMode = static_cast<int8_t>(v.aimMode == 0 || v.aimMode == 1 ? v.aimMode : -1);
    r.itemHash = v.held.hash;
    r.itemSlot = static_cast<int8_t>(v.held.hash && v.heldSlot >= 0 && v.heldSlot < kInventorySlots ? v.heldSlot : -1);
    r.itemIsWeapon = v.held.hash && v.held.weapon ? 1 : 0;
    r.fireMode = r.itemIsWeapon ? v.held.fireMode : 0;
    for (int k = 0; k < 3; ++k) r.rounds[k] = r.itemIsWeapon ? v.held.rounds[k] : 0;
    return r;
}

int FindSlot(const View& v, uint32_t hash, int prefer) {
    if (!hash) return -1;
    if (prefer >= 0 && prefer < kInventorySlots && v.inventory[prefer].hash == hash) return prefer;
    for (int k = 0; k < kInventorySlots; ++k)
        if (v.inventory[k].hash == hash) return k;
    return -1;
}

Plan MakePlan(const Rec& owner, const View& puppet) {
    Plan p;
    // 1. the held item first: holding another weapon class changes the animation set (FUN_0042ec10 re-runs the
    // chooser), which the movement state / hold state below then play
    const Item* target = &puppet.held;
    if (!owner.itemHash) {
        if (puppet.held.hash) p.select = p.selectNone = true;
    } else if (puppet.held.hash != owner.itemHash) {
        const int slot = FindSlot(puppet, owner.itemHash, owner.itemSlot);
        if (slot >= 0) {
            p.select = true;
            p.selectSlot = slot;
            target = &puppet.inventory[slot];
        } else {
            p.itemMissing = true;
            target = nullptr;
        }
    }
    // 2. every field goes in whenever the puppet differs from its owner, not only when the owner's value changes (a
    // change sent once could be refused by the game - Pivotal's versus host sent everything again every 500 ms)
    p.applierMask = 4 | 8 | 0x10;
    if (owner.move != puppet.move) p.applierMask |= 1;
    if (owner.hold != puppet.hold) p.applierMask |= 2;
    if (owner.stance != puppet.stance) p.applierMask |= 0x40;
    if (owner.step != puppet.step) p.applierMask |= 0x80;
    // 3. rounds / fire mode of the item the puppet will hold (the owner's shots are replayed on it: its count is shown
    // in that view's HUD, an empty magazine there showed RELOAD and the replayed shots stopped)
    if (owner.itemIsWeapon && target && target->hash == owner.itemHash && target->weapon) {
        p.setFireMode = target->fireMode != owner.fireMode;
        p.setRounds = !target->SameRounds(owner.rounds);
    }
    // 4. hold state: always (re-)asserted - the game lowers a puppet's weapon on some movement states and nothing
    // raises it again (no action code runs on a puppet)
    p.setHold = owner.hold >= 0 && owner.hold <= 0x19;
    // 5. aim mode of the block controlling the puppet (other players' blocks get no input on this PC)
    p.setAimMode = (owner.aimMode == 0 || owner.aimMode == 1) && puppet.aimMode >= 0 && puppet.aimMode != owner.aimMode;
    return p;
}

bool Receiver::Take(const uint8_t* d, size_t size, uint32_t epoch) {
    if (size < kHeader || d[0] != 'L' || d[1] != kPacketType) return false;
    const size_t n = d[2];
    uint32_t seq, theirs;
    memcpy(&seq, d + 4, 4);
    memcpy(&theirs, d + 8, 4);
    if (size < kHeader + n * sizeof(Rec)) return false;
    if (theirs != epoch) {   // another mission start's
        ++otherEpochs, lastOtherEpoch = theirs;
        return true;
    }
    for (size_t k = 0; k < n; ++k) {
        Rec r;
        memcpy(&r, d + kHeader + k * sizeof(Rec), sizeof r);
        Received& s = soldiers_[r.id];
        if (seq < s.seq) {   // older than what we have (reordered)
            ++stale;
            continue;
        }
        s.rec = r;
        s.seq = seq;
        s.fresh = true;
    }
    return true;
}

std::vector<std::vector<uint8_t>> Packets(const std::vector<Rec>& recs, int sender, uint32_t seq, uint32_t epoch) {
    std::vector<std::vector<uint8_t>> out;
    for (size_t at = 0; at < recs.size(); at += kPerPacket) {
        const size_t n = std::min(kPerPacket, recs.size() - at);
        std::vector<uint8_t> d(kHeader);
        d[0] = 'L', d[1] = kPacketType, d[2] = static_cast<uint8_t>(n), d[3] = static_cast<uint8_t>(sender);
        memcpy(&d[4], &seq, 4);
        memcpy(&d[8], &epoch, 4);
        const auto* b = reinterpret_cast<const uint8_t*>(recs.data() + at);
        d.insert(d.end(), b, b + n * sizeof(Rec));
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace puppetsync
