// DS1 co-save data (src/core/pse_saves_format.cpp): the chunk layouts of dsfix.loadout / dsfix.gear / dsfix.vehicles,
// the old ".sav.dsfix" sidecar reader, and a sidecar loaded through pse::cosave::LoadBytes reaching the plugins'
// load callbacks with the same bytes they write themselves. Also pse::cosave::LoadBytes on its own.
#include <cstdio>

#include "core/pse_saves_format.cpp"  // NOLINT: the unit under test (pure code)
#include "loader_fixture.h"

using namespace pse_fixture;

namespace {
const PSESerializationInterface* Z() { return pse::SerializationInterface(); }

struct Got {
    PSEPluginHandle plugin = 0;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> chunks;
    int loads = 0, reverts = 0;
};
void PSE_CALL OnLoad(PSESaveReader* r, void* ctx) {
    auto* g = static_cast<Got*>(ctx);
    ++g->loads;
    uint32_t tag, version, len;
    while (Z()->NextChunk(r, &tag, &version, &len)) {
        PSE_CHECK_EQ(version, 1u);
        std::vector<uint8_t> b(len);
        Z()->Read(r, b.data(), len);
        g->chunks.emplace_back(tag, std::move(b));
    }
}
void PSE_CALL OnRevert(void* ctx) { ++static_cast<Got*>(ctx)->reverts; }
void Listen(Got& g, const char* name) {
    g.plugin = AddPlugin(name);
    Z()->SetCallbacks(g.plugin, nullptr, &OnLoad, &OnRevert, &g);
}

const char kSidecar[] =
    "1|US_WPN_MP5SilencedSubMG|120|30|1\n"
    "3|US_WPN_SAW_Silenced|200|0|0\r\n"
    "G|0|HAT|UK_CAP01\n"
    "G|0|SPINE1|-\n"
    "G|2|FACE|IR_BEARD02_SUNGLASSES_CIG\n"
    "V|US_VEH_M2A1_Bradley|US_VEH_M1A1_Abrams\n"
    "garbage line\n"
    "V|broken\n";
}  // namespace

PSE_TEST("ds1 saves: chunk encode / decode round trips") {
    std::vector<pse_saves::SavedWeapon> kits(2);
    kits[0].role = 1;
    kits[0].id = "US_WPN_MP5SilencedSubMG";
    kits[0].reserve = 120;
    kits[0].magazine = 30;
    kits[0].held = true;
    kits[1].role = 3;
    kits[1].id = std::string(300, 'x');
    auto kb = pse_saves::EncodeKits(kits);
    PSE_CHECK_EQ(kb.size(), (size_t)(4 + (4 + 2 + 23 + 4 + 4 + 1) + (4 + 2 + 300 + 9)));
    std::vector<pse_saves::SavedWeapon> k2;
    PSE_CHECK(pse_saves::DecodeKits(kb.data(), kb.size(), &k2));
    PSE_REQUIRE(k2.size() == 2);
    PSE_CHECK_STR(k2[0].id.c_str(), "US_WPN_MP5SilencedSubMG");
    PSE_CHECK_EQ(k2[0].reserve, 120);
    PSE_CHECK(k2[0].held && !k2[1].held);
    PSE_CHECK_EQ(k2[1].id.size(), (size_t)300);

    pse_saves::SavedGear gear;
    gear[0] = {{"HAT", "UK_CAP01"}, {"SPINE1", ""}};
    gear[2] = {};
    auto gb = pse_saves::EncodeGear(gear);
    pse_saves::SavedGear g2;
    PSE_CHECK(pse_saves::DecodeGear(gb.data(), gb.size(), &g2));
    PSE_CHECK_EQ(g2.size(), (size_t)2);
    PSE_CHECK_STR(g2[0][1].connector.c_str(), "SPINE1");
    PSE_CHECK(g2[0][1].model.empty());

    pse_saves::SavedVehicles v{{"A", "B"}, {"C", "D"}};
    auto vb = pse_saves::EncodeVehicles(v);
    pse_saves::SavedVehicles v2;
    PSE_CHECK(pse_saves::DecodeVehicles(vb.data(), vb.size(), &v2));
    PSE_CHECK(v2 == v);

    // Damage: truncated / absurd counts are refused, nothing past the damage is invented.
    std::vector<pse_saves::SavedWeapon> k3;
    PSE_CHECK(!pse_saves::DecodeKits(kb.data(), kb.size() - 1, &k3));
    PSE_CHECK_EQ(k3.size(), (size_t)1);
    const uint8_t huge[] = {0xFF, 0xFF, 0xFF, 0x7F};
    pse_saves::SavedGear g3;
    PSE_CHECK(!pse_saves::DecodeGear(huge, sizeof huge, &g3));
    PSE_CHECK(!pse_saves::DecodeVehicles(nullptr, 0, &v2));
}

PSE_TEST("ds1 saves: the old sidecar is read like the old reader did") {
    pse_saves::Sidecar s = pse_saves::ParseSidecar(kSidecar);
    PSE_REQUIRE(s.kits.size() == 2);
    PSE_CHECK_EQ(s.kits[0].role, 1);
    PSE_CHECK_EQ(s.kits[0].magazine, 30);
    PSE_CHECK(s.kits[0].held);
    PSE_CHECK_STR(s.kits[1].id.c_str(), "US_WPN_SAW_Silenced");
    PSE_CHECK_EQ(s.gear.size(), (size_t)2);
    PSE_CHECK_STR(s.gear[0][0].model.c_str(), "UK_CAP01");
    PSE_CHECK(s.gear[0][1].model.empty());   // "-" = nothing
    PSE_CHECK_STR(s.gear[2][0].model.c_str(), "IR_BEARD02_SUNGLASSES_CIG");
    PSE_CHECK_EQ(s.vehicles.size(), (size_t)1);
    PSE_CHECK_STR(s.vehicles["US_VEH_M2A1_Bradley"].c_str(), "US_VEH_M1A1_Abrams");
    PSE_CHECK(pse_saves::ParseSidecar("").kits.empty());
}

PSE_TEST("ds1 saves: co-save / sidecar paths") {
    PSE_CHECK_STR(pse_saves::CoSavePathFor("C:\\s\\Rescue 1.sav").c_str(), "C:\\s\\Rescue 1.pse");
    PSE_CHECK_STR(pse_saves::CoSavePathFor("C:\\s\\Rescue 1.SAV").c_str(), "C:\\s\\Rescue 1.pse");
    PSE_CHECK_STR(pse_saves::CoSavePathFor("x").c_str(), "x.pse");
    PSE_CHECK_STR(pse_saves::SidecarPathFor("C:\\s\\a.sav").c_str(), "C:\\s\\a.sav.dsfix");
}

PSE_TEST("ds1 saves: a sidecar through LoadBytes reaches each plugin's load callback; others revert") {
    Fresh();
    Got loadout, gear, vehicles, other;
    Listen(loadout, "dsfix.loadout");
    Listen(gear, "dsfix.gear");
    Listen(vehicles, "dsfix.vehicles");
    Listen(other, "co.other");
    pse_saves::Sidecar s = pse_saves::ParseSidecar(kSidecar);
    std::vector<uint8_t> image = pse_saves::SidecarToCosave(s);
    PSE_CHECK(pse::cosave::LoadBytes(image.data(), image.size(), "test.sav.dsfix"));
    PSE_CHECK_EQ(loadout.loads, 1);
    PSE_CHECK_EQ(gear.loads, 1);
    PSE_CHECK_EQ(vehicles.loads, 1);
    PSE_CHECK_EQ(other.reverts, 1);
    PSE_REQUIRE(loadout.chunks.size() == 1 && gear.chunks.size() == 1 && vehicles.chunks.size() == 1);
    PSE_CHECK_EQ(loadout.chunks[0].first, pse_saves::kKitsTag);
    PSE_CHECK(loadout.chunks[0].second == pse_saves::EncodeKits(s.kits));
    PSE_CHECK(gear.chunks[0].second == pse_saves::EncodeGear(s.gear));
    PSE_CHECK_EQ(vehicles.chunks[0].first, pse_saves::kVehiclesTag);
    PSE_CHECK(vehicles.chunks[0].second == pse_saves::EncodeVehicles(s.vehicles));

    // An empty sidecar = a valid image with no chunks: everyone reverts.
    Got l2;
    Fresh();
    Listen(l2, "dsfix.loadout");
    std::vector<uint8_t> empty = pse_saves::SidecarToCosave({});
    PSE_CHECK_EQ(empty.size(), (size_t)8);
    PSE_CHECK(pse::cosave::LoadBytes(empty.data(), empty.size(), "empty"));
    PSE_CHECK_EQ(l2.reverts, 1);
    // A damaged image: warning, everyone reverts, false.
    PSE_CHECK(!pse::cosave::LoadBytes(image.data(), image.size() - 1, "cut"));
    PSE_CHECK_EQ(l2.reverts, 2);
    PSE_CHECK(!pse::cosave::LoadBytes(nullptr, 0, nullptr));
}

PSE_TEST("ds1 saves: chunks from a sidecar of a plugin not installed are kept on the next save") {
    Fresh();
    Got loadout;
    Listen(loadout, "dsfix.loadout");   // dsfix.gear / dsfix.vehicles not installed this time
    std::vector<uint8_t> image = pse_saves::SidecarToCosave(pse_saves::ParseSidecar(kSidecar));
    pse::cosave::LoadBytes(image.data(), image.size(), "old");
    const std::string path = TempPath("ds1-sidecar-keep.pse");
    DeleteFileA(path.c_str());
    PSE_CHECK(pse::cosave::Save(path.c_str()));
    Fresh();
    Got gear, vehicles;
    Listen(gear, "dsfix.gear");
    Listen(vehicles, "dsfix.vehicles");
    PSE_CHECK(pse::cosave::Load(path.c_str()));
    PSE_CHECK_EQ(gear.chunks.size(), (size_t)1);
    PSE_CHECK_EQ(vehicles.chunks.size(), (size_t)1);
    DeleteFileA(path.c_str());
}
