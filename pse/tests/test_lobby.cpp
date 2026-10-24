// The co-op lobby table (src/features/lobby.cpp): host rules, per-PC player masks, wire format, joiners' requests.
#include "features/lobby.cpp"  // NOLINT: the unit under test (pure code)
#include "pse_test.h"

using namespace lobby;

PSE_TEST("lobby: players join packed, per-PC masks, a pad joins once, kDeviceAny is a PC's only player") {
    Table t;
    PSE_CHECK_EQ(Add(t, 1, kDeviceKeyboard, "Kyle"), 0);
    PSE_CHECK_EQ(Add(t, 1, 0, "Pad"), 1);
    PSE_CHECK_EQ(Add(t, 1, 0, "Again"), -1);           // that pad is already in
    PSE_CHECK_EQ(Add(t, 1, kDeviceAny, "Any"), -1);    // "any" conflicts with the PC's other players
    PSE_CHECK_EQ(Add(t, 2, kDeviceAny, "Sam"), 2);
    PSE_CHECK_EQ(Add(t, 2, 1, "Two"), -1);             // PC 2's only player has every device
    PSE_CHECK_EQ(Add(t, 3, 0, "Jo"), 3);
    PSE_CHECK_EQ(Add(t, 4, 0, "Full"), -1);
    PSE_CHECK_EQ(t.Count(), 4);
    PSE_CHECK_EQ(t.MaskOf(1), 0x3u);
    PSE_CHECK_EQ(t.MaskOf(2), 0x4u);
    PSE_CHECK_EQ(t.MaskOf(3), 0x8u);
    PSE_CHECK_EQ(t.Pcs(), 3);
    PSE_CHECK_EQ(t.FirstOf(2), 2);
    // default colours: distinct, lowest free
    PSE_CHECK_EQ(int(t.slots[0].colour), 0);
    PSE_CHECK_EQ(int(t.slots[3].colour), 3);
}

PSE_TEST("lobby: leaving packs the later players down; a PC that left loses all its players") {
    Table t;
    Add(t, 1, kDeviceKeyboard, "A");
    Add(t, 2, 0, "B");
    Add(t, 2, 1, "C");
    Add(t, 3, kDeviceAny, "D");
    PSE_CHECK(Remove(t, 1));
    PSE_CHECK_EQ(t.slots[1].name, std::string("C"));
    PSE_CHECK_EQ(t.slots[2].name, std::string("D"));
    PSE_CHECK(!t.slots[3].used);
    PSE_CHECK_EQ(RemovePc(t, 3), 1);
    PSE_CHECK_EQ(t.Count(), 2);
    PSE_CHECK_EQ(RemovePc(t, 2), 1);
    PSE_CHECK_EQ(t.Count(), 1);
    PSE_CHECK_EQ(t.MaskOf(1), 0x1u);
}

PSE_TEST("lobby: names cleaned and capped, colours unique, ready / view, every change bumps the version") {
    Table t;
    Add(t, 1, kDeviceAny, "  \x01Kyle\t ");
    PSE_CHECK_EQ(t.slots[0].name, std::string("Kyle"));
    Add(t, 2, kDeviceAny, "");
    PSE_CHECK_EQ(t.slots[1].name, std::string("Player 2"));
    const uint32_t v = t.version;
    PSE_CHECK(SetName(t, 1, "A very long player name indeed"));
    PSE_CHECK_EQ(t.slots[1].name.size(), kNameMax);
    PSE_CHECK(!SetColour(t, 1, t.slots[0].colour));   // taken
    PSE_CHECK(SetColour(t, 1, 5));
    PSE_CHECK(!t.AllReady());
    PSE_CHECK(SetReady(t, 0, true) && SetReady(t, 1, true));
    PSE_CHECK(t.AllReady());
    PSE_CHECK(SetView(t, 1, View::Own));
    PSE_CHECK(!SetView(t, 1, View::Own));   // no change
    PSE_CHECK(t.version > v + 3);
    PSE_CHECK(!Table{}.AllReady());          // nobody = not ready
}

PSE_TEST("lobby: table round-trips through the wire format; bad text refused") {
    Table t;
    Add(t, 1, kDeviceKeyboard, "Kyle K");
    Add(t, 3, 2, "a,b|c 9");
    SetView(t, 1, View::Own);
    SetReady(t, 0, true);
    Table back;
    PSE_REQUIRE(Decode(Encode(t), back));
    PSE_CHECK_EQ(back.version, t.version);
    PSE_CHECK_EQ(back.Count(), 2);
    PSE_CHECK_EQ(back.slots[0].name, std::string("Kyle K"));
    PSE_CHECK_EQ(back.slots[1].name, std::string("a,b|c 9"));
    PSE_CHECK_EQ(int(back.slots[1].device), 2);
    PSE_CHECK_EQ(int(back.slots[1].pc), 3);
    PSE_CHECK(back.slots[1].view == View::Own);
    PSE_CHECK(back.slots[0].ready && !back.slots[1].ready);
    PSE_CHECK(Encode(back) == Encode(t));
    Table junk;
    PSE_CHECK(!Decode("LB T 1 - -", junk));               // too few slots
    PSE_CHECK(!Decode("LB T 1 0,0,0,0,0,41 - - -", junk));   // PC 0
    PSE_CHECK(!Decode("LB X 1 - - - -", junk));
    PSE_CHECK(Decode("LB T 7 - - - -", junk) && junk.Count() == 0 && junk.version == 7);
}

PSE_TEST("lobby: joiners' requests touch only their own PC's players") {
    Table t;
    Add(t, 1, kDeviceAny, "Host");
    PSE_CHECK(Apply(t, 2, JoinRequest(0, "Pad A")));
    PSE_CHECK(Apply(t, 2, JoinRequest(1, "Pad B")));
    PSE_CHECK_EQ(t.MaskOf(2), 0x6u);
    PSE_CHECK(!Apply(t, 2, "LB R 0 1"));            // the host's player
    PSE_CHECK(Apply(t, 2, "LB R 1 1"));
    PSE_CHECK(Apply(t, 2, "LB N 2 " + Hex("Renamed")));
    PSE_CHECK_EQ(t.slots[2].name, std::string("Renamed"));
    PSE_CHECK(Apply(t, 2, "LB V 2 1") && t.slots[2].view == View::Own);
    PSE_CHECK(Apply(t, 2, "LB C 2 6") && t.slots[2].colour == 6);
    PSE_CHECK(!Apply(t, 3, "LB L 1"));              // not PC 3's
    PSE_CHECK(Apply(t, 2, "LB L 1"));
    PSE_CHECK_EQ(t.Count(), 2);
    PSE_CHECK_EQ(t.slots[1].name, std::string("Renamed"));   // packed down
    PSE_CHECK(!Apply(t, 2, "LB Q 1 1"));
    PSE_CHECK(!Apply(t, 2, "garbage"));
}

PSE_TEST("lobby: a PC's keyboard + pad player narrows to one device so a second local player can join") {
    Table t;
    Add(t, 1, kDeviceAny, "Host");
    Add(t, 2, kDeviceAny, "Sam");
    PSE_CHECK_EQ(Add(t, 2, 1, "Pad"), -1);                     // Sam still has every device
    PSE_CHECK(!Apply(t, 2, "LB D 0 8"));                         // not PC 2's player
    PSE_CHECK(Apply(t, 2, "LB D 1 8"));                          // Sam: keyboard only
    PSE_CHECK(Apply(t, 2, JoinRequest(1, "Pad")));
    PSE_CHECK(!Apply(t, 2, "LB D 2 8"));                         // the keyboard is Sam's
    PSE_CHECK(!Apply(t, 2, "LB D 1 -1"));                        // "any" while the PC has another player
    PSE_CHECK(Apply(t, 2, "LB D 2 3"));
    PSE_CHECK_EQ(int(t.slots[2].device), 3);
    PSE_CHECK_EQ(kPalette[t.slots[2].colour] != 0, true);
}
