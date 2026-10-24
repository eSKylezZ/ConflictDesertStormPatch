// Owner-mode puppet sync (src/features/puppetsync.cpp), out of game: the record wire format, the puppet plan, and a
// replay of the 2-PC session of 2026-10-22 (Mission 3; videos t6pgUAeyDJU / gY4L22a7ye8, online logs 20 / 21) through
// the real Encode / Packets / Receiver / MakePlan over a lossy, reordering network into a puppet that follows the game's
// rules as reverse engineered (authority.cpp):
//   - the puppet mover's hold-state machine FUN_004278b0 (0x41FB6E) lowers the weapon every frame - no input on a
//     puppet's block (hooked out by the fix);
//   - SetMovementState lowers the weapon for some movement states (0x42D2CD), the applier then takes a hold state only
//     where the movement-state table allows a switch (FUN_0042da90);
//   - the owner's shots are replayed on whatever the puppet holds ("EA F"), draining that weapon's magazine.
// The replay runs once as the shipped build behaved (no held item in the records, the hold machine running) and once
// fixed: the first must show the desync the videos show, the second must follow its owner.
#include <cstring>
#include <deque>
#include <random>

#include "features/puppetsync.cpp"  // NOLINT: the unit under test (pure code)
#include "pse_test.h"

using namespace puppetsync;

namespace {
// Mission 3 Bradley's kit (definition name hashes are arbitrary but fixed)
enum : uint32_t { kM16 = 0xA1000001, kSig = 0xA1000002, kFrag = 0xA1000003, kSmoke = 0xA1000004, kBinos = 0xA1000005,
                  kMedi = 0xA1000006, kKnife = 0xA1000007 };
Item Weapon(uint32_t hash, uint16_t mode, int16_t a, int16_t b, int16_t c) {
    Item it;
    it.hash = hash, it.weapon = true, it.fireMode = mode;
    it.rounds[0] = a, it.rounds[1] = b, it.rounds[2] = c;
    return it;
}
Item Gear(uint32_t hash) {
    Item it;
    it.hash = hash;
    return it;
}
View Kit(bool shuffled) {   // shuffled: the puppet's copy holds the pistol and frags in other slots (a pick-up order)
    View v;
    v.inventory[0] = Weapon(kM16, 2, 390, 0, 30);
    v.inventory[shuffled ? 4 : 1] = Weapon(kSig, 1, 39, 0, 13);
    v.inventory[shuffled ? 1 : 2] = Weapon(kFrag, 1, 2, 0, 1);
    v.inventory[3] = Weapon(kSmoke, 1, 2, 0, 1);
    v.inventory[shuffled ? 2 : 4] = Gear(kBinos);
    v.inventory[5] = Gear(kMedi);
    v.inventory[6] = Weapon(kKnife, 1, 0, 0, 0);
    v.heldSlot = 0;
    v.held = v.inventory[0];
    v.move = 15, v.stance = 1, v.hold = 2, v.step = 2, v.aimMode = 0;
    return v;
}
void Hold(View& v, int slot) {   // what FUN_0042ec10 does to the fields we track
    v.heldSlot = slot;
    v.held = slot >= 0 ? v.inventory[slot] : Item{};
}
void StoreHeld(View& v) {   // the held item is an object: keep its inventory copy current
    if (v.heldSlot >= 0) v.inventory[v.heldSlot] = v.held;
}

// ---- the puppet as the game treats it ----
bool LowersWeapon(int move) { return move == 87 || move == 88 || move == 78; }   // dive / roll / getting up
bool AllowsHoldSwitch(int move) { return move != 87 && move != 88; }              // FUN_0042da90

struct Puppet {
    View v = Kit(true);
    bool itemSync = true;          // records carry the held item (the fix)
    bool holdMachineRuns = false;  // FUN_004278b0 runs on puppets (the shipped build)
    void Frame() {                 // the game's own update between records
        if (holdMachineRuns) v.hold = 0;   // no input -> event 0 -> lowered
    }
    void Apply(const Rec& r) {
        Rec in = r;
        if (!itemSync) in.itemHash = v.held.hash, in.itemIsWeapon = 0;   // shipped records: nothing about the item
        const Plan p = MakePlan(in, v);
        if (p.select) Hold(v, p.selectNone ? -1 : p.selectSlot);
        if (p.applierMask & 1) {
            v.move = in.move;
            if (LowersWeapon(in.move)) v.hold = 0;   // SetMovementState 0x42D2CD
        }
        if ((p.applierMask & 2) && AllowsHoldSwitch(v.move)) v.hold = in.hold;
        if (p.applierMask & 0x40) v.stance = in.stance;
        if (p.applierMask & 0x80) v.step = in.step;
        if (p.setFireMode) v.held.fireMode = in.fireMode;
        if (p.setRounds) memcpy(v.held.rounds, in.rounds, sizeof in.rounds);
        StoreHeld(v);
        if (p.setHold) v.hold = in.hold;   // KeepHoldState (the shipped build had it too - Frame undid it)
        if (p.setAimMode) v.aimMode = in.aimMode;
    }
    void ReplayShot() {   // "EA F": the puppet's held weapon fires
        if (v.held.weapon && v.held.rounds[2] > 0) --v.held.rounds[2], StoreHeld(v);
    }
};

// ---- the owner: the session's actions as seen in the videos and the host's log ----
struct Owner {
    View v = Kit(false);
    int shots = 0;   // shots fired this frame (each one goes out as "EA F")
    void Fire(int n) {
        for (int k = 0; k < n && v.held.weapon && v.held.rounds[2] > 0; ++k) --v.held.rounds[2], ++shots;
        StoreHeld(v);
    }
    void Reload() {   // FUN_004a1350's effect on the counters
        const int16_t want = 30 - v.held.rounds[2], take = want < v.held.rounds[0] ? want : v.held.rounds[0];
        v.held.rounds[0] -= take, v.held.rounds[2] += take;
        StoreHeld(v);
    }
    // ms since start -> the owner's state changes at that moment
    void At(int ms) {
        switch (ms) {
        case 2000: v.stance = 3, v.move = 2; break;                              // crouch
        case 4000: v.hold = 8; Fire(5); break;                                   // burst
        case 4600: v.hold = 2; break;
        case 6000: Hold(v, 1), v.hold = 1; break;                                // SIG
        case 6500: v.hold = 2; break;
        case 7000: Fire(2); break;
        case 9000: Hold(v, 2), v.hold = 1; break;                                // frag
        case 10000: v.hold = 3; break;                                           // pin out, wind up
        case 10600: v.hold = 4; break;
        case 11000: v.held.rounds[0] = 1, StoreHeld(v), v.hold = 0; break;       // thrown
        case 12000: Hold(v, 3), v.hold = 1; break;                               // smoke
        case 14000: Hold(v, 4), v.hold = 15, v.aimMode = 1; break;               // binoculars
        case 14500: v.hold = 16; break;
        case 18000: Hold(v, 0), v.hold = 1, v.aimMode = 0; break;                // rifle
        case 18400: v.hold = 6; break;                                           // reload
        case 19200: v.hold = 7; break;
        case 19700: Reload(), v.hold = 0; break;
        case 19900: v.hold = 1; break;
        case 20300: v.hold = 2; break;
        case 22000: v.move = 5; break;                                           // crouch-walk
        case 22400: v.move = 6, Fire(3); break;
        case 23000: v.move = 87; break;                                          // dive
        case 23600: v.move = 2, v.hold = 1; break;
        case 24000: v.hold = 2; break;
        case 26000: v.stance = 1, v.move = 15, v.held.fireMode = 0x100, StoreHeld(v); break;   // M203
        case 26500: v.hold = 8; Fire(1); break;
        case 27200: v.hold = 2, v.held.fireMode = 2, StoreHeld(v); break;
        case 30000: Hold(v, 5), v.hold = 0; break;                               // medikit
        case 33000: Hold(v, 0), v.hold = 1; break;                               // rifle
        case 33500: v.hold = 2, v.aimMode = 1; break;
        default: break;
        }
    }
};

struct Result {
    double itemMatch = 0, holdMatch = 0, roundsMatch = 0, poseMatch = 0;
    int worstLagMs = 0;   // longest a changed owner field took to show on the puppet (fields that ever caught up)
    int neverCaught = 0;  // owner states the puppet never showed before the owner changed again
};

// The puppet's view is compared with the owner's state `lagFrames` ago (records every 50 ms + network delay).
struct Snapshot {
    uint32_t item;
    int hold, stance, move, aim;
    int16_t rounds[3];
};
Snapshot Snap(const View& v) {
    Snapshot s{v.held.hash, v.hold, v.stance, v.move, v.aimMode, {}};
    memcpy(s.rounds, v.held.rounds, sizeof s.rounds);
    return s;
}

Result Replay(bool fixed, unsigned seed = 20261022) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> loss(0, 1);
    std::uniform_int_distribution<int> delay(20, 120);
    Owner owner;
    Puppet puppet;
    puppet.itemSync = fixed;
    puppet.holdMachineRuns = !fixed;
    Receiver rx;
    struct InFlight {
        int arriveMs;
        std::vector<uint8_t> data;
    };
    std::deque<InFlight> net;   // (any order: delays differ, so packets overtake each other)
    struct Shot {
        int arriveMs;
    };
    std::deque<Shot> shots;   // "EA F" is reliable (resent): never lost, same delay range
    constexpr int kFrameMs = 16, kEndMs = 36000, kLagFrames = 12;   // compare against the owner ~190 ms earlier
    std::deque<Snapshot> history;
    uint32_t seq = 0, epoch = 0x516479B3;
    int frames = 0, item = 0, hold = 0, rounds = 0, pose = 0, worst = 0, never = 0;
    Snapshot lastOwner = Snap(owner.v);
    int changedAt = 0;
    bool caught = true;
    for (int ms = 0; ms < kEndMs; ms += kFrameMs) {
        for (int t = ms; t < ms + kFrameMs; ++t) owner.At(t);
        for (; owner.shots > 0; --owner.shots) shots.push_back({ms + delay(rng)});
        if (ms % 48 == 0) {   // kRecordMs (50 ms, at frame granularity)
            Rec r = Encode(7, owner.v);
            for (auto& d : Packets({r}, 0, ++seq, epoch))
                if (loss(rng) >= 0.15) net.push_back({ms + delay(rng), d});
        }
        for (auto it = net.begin(); it != net.end();)
            if (it->arriveMs <= ms) rx.Take(it->data.data(), it->data.size(), epoch), it = net.erase(it);
            else ++it;
        for (auto it = shots.begin(); it != shots.end();)
            if (it->arriveMs <= ms) puppet.ReplayShot(), it = shots.erase(it);
            else ++it;
        // AuthorityTick applies records before the game update; the frame is rendered after it
        for (auto& [id, got] : rx.Soldiers())
            if (got.fresh) puppet.Apply(got.rec), got.fresh = false;
        puppet.Frame();

        // what each PC renders this frame
        const Snapshot o = Snap(owner.v), p = Snap(puppet.v);
        history.push_back(o);
        if (history.size() > kLagFrames) history.pop_front();
        auto matches = [&](auto field) {   // the puppet shows any of the owner's recent states
            for (const Snapshot& h : history)
                if (field(h, p)) return true;
            return false;
        };
        ++frames;
        item += matches([](const Snapshot& a, const Snapshot& b) { return a.item == b.item; });
        hold += matches([](const Snapshot& a, const Snapshot& b) { return a.hold == b.hold; });
        rounds += matches([](const Snapshot& a, const Snapshot& b) { return !memcmp(a.rounds, b.rounds, sizeof a.rounds); });
        pose += matches([](const Snapshot& a, const Snapshot& b) {
            return a.stance == b.stance && a.move == b.move && a.aim == b.aim;
        });
        // lag: from an owner change until the puppet shows that state
        if (memcmp(&o, &lastOwner, sizeof o)) {
            if (!caught) ++never;
            lastOwner = o, changedAt = ms, caught = false;
        }
        if (!caught && !memcmp(&o, &p, sizeof o)) {
            caught = true;
            if (ms - changedAt > worst) worst = ms - changedAt;
        }
    }
    if (!caught) ++never;
    Result r;
    r.itemMatch = double(item) / frames, r.holdMatch = double(hold) / frames;
    r.roundsMatch = double(rounds) / frames, r.poseMatch = double(pose) / frames;
    r.worstLagMs = worst, r.neverCaught = never;
    return r;
}
}  // namespace

// ---- wire format ----
PSE_TEST("puppet sync: record encodes the held item, its slot, rounds and fire mode") {
    View v = Kit(false);
    Hold(v, 1);
    v.hold = 8, v.aimMode = 1, v.move = 5, v.stance = 3;
    const Rec r = Encode(7, v);
    PSE_CHECK_EQ(r.id, uint16_t(7));
    PSE_CHECK_EQ(r.itemHash, uint32_t(kSig));
    PSE_CHECK_EQ(r.itemSlot, int8_t(1));
    PSE_CHECK_EQ(r.itemIsWeapon, uint8_t(1));
    PSE_CHECK_EQ(r.fireMode, uint16_t(1));
    PSE_CHECK_EQ(r.rounds[0], int16_t(39));
    PSE_CHECK_EQ(r.rounds[2], int16_t(13));
    PSE_CHECK_EQ(r.hold, int16_t(8));
    PSE_CHECK_EQ(r.aimMode, int8_t(1));
    Hold(v, 4);   // binoculars: equipment, no rounds
    const Rec g = Encode(7, v);
    PSE_CHECK_EQ(g.itemHash, uint32_t(kBinos));
    PSE_CHECK_EQ(g.itemIsWeapon, uint8_t(0));
    PSE_CHECK_EQ(g.rounds[2], int16_t(0));
    v.aimMode = 2;   // AI-driven block mode isn't sent
    PSE_CHECK_EQ(Encode(7, v).aimMode, int8_t(-1));
}

PSE_TEST("puppet sync: packets split at 22 records, stay under net.cpp's 1400 bytes, round-trip") {
    std::vector<Rec> recs;
    for (uint16_t k = 0; k < 50; ++k) recs.push_back(Encode(k, Kit(false)));
    const auto packets = Packets(recs, 1, 77, 0xABCD);
    PSE_CHECK_EQ(packets.size(), size_t(3));
    for (const auto& d : packets) PSE_CHECK(d.size() <= 1400);
    Receiver rx;
    for (const auto& d : packets) PSE_CHECK(rx.Take(d.data(), d.size(), 0xABCD));
    PSE_CHECK_EQ(rx.Soldiers().size(), size_t(50));
    PSE_CHECK_EQ(rx.Soldiers()[49].rec.itemHash, uint32_t(kM16));
    PSE_CHECK_EQ(rx.Soldiers()[49].seq, uint32_t(77));
    // truncated / foreign packets are refused
    PSE_CHECK(!rx.Take(packets[0].data(), packets[0].size() - 1, 0xABCD));
    std::vector<uint8_t> old = packets[0];
    old[1] = 9;   // the shipped 'L' 9
    PSE_CHECK(!rx.Take(old.data(), old.size(), 0xABCD));
}

PSE_TEST("puppet sync: newest record wins when packets arrive out of order; other mission starts dropped") {
    View v = Kit(false);
    Receiver rx;
    Hold(v, 1);
    const auto newer = Packets({Encode(7, v)}, 0, 10, 1);
    Hold(v, 0);
    const auto older = Packets({Encode(7, v)}, 0, 9, 1);
    rx.Take(newer[0].data(), newer[0].size(), 1);
    rx.Take(older[0].data(), older[0].size(), 1);
    PSE_CHECK_EQ(rx.Soldiers()[7].rec.itemHash, uint32_t(kSig));
    PSE_CHECK_EQ(rx.stale, uint32_t(1));
    const auto other = Packets({Encode(8, v)}, 0, 11, 2);
    rx.Take(other[0].data(), other[0].size(), 1);
    PSE_CHECK(!rx.Soldiers().count(8));
    PSE_CHECK_EQ(rx.otherEpochs, uint32_t(1));
    PSE_CHECK_EQ(rx.lastOtherEpoch, uint32_t(2));
}

// ---- plan ----
PSE_TEST("puppet sync: plan selects the owner's item by slot, falls back to its hash, reports a missing one") {
    View owner = Kit(false), puppet = Kit(false);
    Hold(owner, 1);
    Plan p = MakePlan(Encode(7, owner), puppet);
    PSE_CHECK(p.select && !p.selectNone);
    PSE_CHECK_EQ(p.selectSlot, 1);
    PSE_CHECK(!p.setRounds && !p.setFireMode);   // the slot's copy already matches
    // the puppet carries the SIG in another slot
    View shuffled = Kit(true);
    p = MakePlan(Encode(7, owner), shuffled);
    PSE_CHECK(p.select);
    PSE_CHECK_EQ(p.selectSlot, 4);
    // the puppet lost it (owner picked one up only on his PC)
    shuffled.inventory[4] = Item{};
    p = MakePlan(Encode(7, owner), shuffled);
    PSE_CHECK(!p.select && p.itemMissing);
    // same item held: nothing to select
    Hold(puppet, 1);
    PSE_CHECK(!MakePlan(Encode(7, owner), puppet).select);
    // owner holds nothing
    Hold(owner, -1);
    p = MakePlan(Encode(7, owner), puppet);
    PSE_CHECK(p.select && p.selectNone);
}

PSE_TEST("puppet sync: plan sets rounds / fire mode of the item the puppet will hold, and the aim mode") {
    View owner = Kit(false), puppet = Kit(false);
    owner.held.rounds[2] = 21, owner.held.fireMode = 0x100, StoreHeld(owner);
    owner.aimMode = 1;
    Plan p = MakePlan(Encode(7, owner), puppet);
    PSE_CHECK(p.setRounds && p.setFireMode && p.setAimMode && p.setHold);
    PSE_CHECK_EQ(p.applierMask & 0x1C, 0x1Cu);   // position, heading, aim always
    // switching to the SIG: compared against the SIG the puppet will hold, not its rifle
    Hold(owner, 1);
    owner.held.rounds[2] = 4, StoreHeld(owner);
    p = MakePlan(Encode(7, owner), puppet);
    PSE_CHECK(p.select && p.setRounds && !p.setFireMode);
    // equipment: no rounds
    Hold(owner, 5);
    p = MakePlan(Encode(7, owner), puppet);
    PSE_CHECK(p.select && !p.setRounds && !p.setFireMode);
    // a puppet without a player's block can't take an aim mode
    puppet.aimMode = -1;
    PSE_CHECK(!MakePlan(Encode(7, owner), puppet).setAimMode);
}

// ---- the session replay ----
PSE_TEST("puppet sync: session replay - the shipped build shows the videos' desync") {
    const Result r = Replay(false);
    std::printf("    shipped: item %.0f%%, hold %.0f%%, rounds %.0f%%, stance/move/aim %.0f%%, never caught %d\n",
                r.itemMatch * 100, r.holdMatch * 100, r.roundsMatch * 100, r.poseMatch * 100, r.neverCaught);
    PSE_CHECK(r.itemMatch < 0.7);    // rifle while the owner held SIG / grenades / binoculars / medikit
    PSE_CHECK(r.holdMatch < 0.3);    // weapon lowered while the owner held it ready
    PSE_CHECK(r.roundsMatch < 0.5);  // magazine drained by shots of other weapons, reloads never done
    PSE_CHECK(r.neverCaught > 10);
}

PSE_TEST("puppet sync: session replay - fixed, the puppet follows its owner through loss and reordering") {
    for (unsigned seed : {20261022u, 1u, 2u, 3u, 4u}) {
        const Result r = Replay(true, seed);
        std::printf("    fixed (seed %u): item %.1f%%, hold %.1f%%, rounds %.1f%%, stance/move/aim %.1f%%, worst lag %d ms, "
                    "never caught %d\n", seed, r.itemMatch * 100, r.holdMatch * 100, r.roundsMatch * 100,
                    r.poseMatch * 100, r.worstLagMs, r.neverCaught);
        PSE_CHECK(r.itemMatch > 0.97);
        PSE_CHECK(r.holdMatch > 0.95);
        PSE_CHECK(r.roundsMatch > 0.95);
        PSE_CHECK(r.poseMatch > 0.95);
        PSE_CHECK(r.worstLagMs <= 400);   // a few lost records in a row at most
    }
}
