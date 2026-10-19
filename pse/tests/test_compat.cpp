// Online plugin-set check (pse/loader/compat.cpp, docs/sdk/online-compat.md): encoding, assembling, comparison.
#include <windows.h>

#include <string>
#include <vector>

#include "loader.h"
#include "loader_fixture.h"
#include "pse_test.h"

using namespace pse_fixture;
using pse::compat::ModEntry;
using pse::compat::PluginEntry;
using pse::compat::Set;

namespace {

const std::string kHashA(64, 'a'), kHashB(64, 'b');

Set Base() {
    Set s;
    s.apiVersion = PSE_API_VERSION;
    s.gameId = "ds1";
    s.host = "DesertStormFix 3.3.0-Alpha";
    s.hostFingerprint = "0123456789abcdef";
    s.plugins = {
        {"dsfix", PSE_MAKE_VERSION(3, 3, 0), (uint32_t(PSE_PLUGIN_SIMULATION) | uint32_t(PSE_PLUGIN_INTERNAL)), "="},
        {"dsfix.hud", PSE_MAKE_VERSION(3, 3, 0), (uint32_t(PSE_PLUGIN_CLIENT_ONLY) | uint32_t(PSE_PLUGIN_INTERNAL)), "="},
        {"dsfix.limits", PSE_MAKE_VERSION(3, 3, 0), (uint32_t(PSE_PLUGIN_SIMULATION) | uint32_t(PSE_PLUGIN_INTERNAL)), "="},
        {"kyle.alienfaction", PSE_MAKE_VERSION(1, 2, 0), PSE_PLUGIN_SIMULATION, kHashA},
        {"kyle.hudcolour", PSE_MAKE_VERSION(2, 0, 0), PSE_PLUGIN_CLIENT_ONLY, ""},
    };
    s.mods = {{"GoldShades", kHashA}, {"Mission Custom", kHashB}};
    return s;
}

bool Has(const std::vector<std::string>& lines, const std::string& text) {
    for (const std::string& l : lines)
        if (l.find(text) != std::string::npos) return true;
    return false;
}

std::string All(const std::vector<std::string>& lines) {
    std::string s;
    for (const std::string& l : lines) s += l + " | ";
    return s;
}

// Compare both ways: the verdict must be the same on both PCs.
std::vector<std::string> Diff(const Set& a, const Set& b) {
    std::vector<std::string> ab = pse::compat::Compare(a, b, "the host");
    std::vector<std::string> ba = pse::compat::Compare(b, a, "player 2");
    PSE_CHECK_EQ(ab.empty(), ba.empty());
    return ab;
}

Set RoundTrip(const Set& s, size_t maxLine = 240) {
    pse::compat::Assembler a;
    std::vector<std::string> lines = pse::compat::Encode(s, 0x1234ABCD, maxLine);
    pse::compat::Assembler::Result r = pse::compat::Assembler::Pending;
    for (const std::string& l : lines) {
        PSE_CHECK(l.size() <= std::max<size_t>(maxLine, 80) || l.find(' ', 20) == std::string::npos);
        r = a.Feed(l);
    }
    PSE_CHECK_EQ(r, pse::compat::Assembler::Done);
    return a.set;
}

}  // namespace

PSE_TEST("compat: escape / unescape") {
    std::string out;
    PSE_CHECK_STR(pse::compat::Escape("Mission Custom").c_str(), "Mission%20Custom");
    PSE_CHECK_STR(pse::compat::Escape("a:b%c").c_str(), "a%3Ab%25c");
    PSE_CHECK_STR(pse::compat::Escape("").c_str(), "%");
    PSE_CHECK(pse::compat::Unescape("Mission%20Custom", &out) && out == "Mission Custom");
    PSE_CHECK(pse::compat::Unescape("%", &out) && out.empty());
    PSE_CHECK(!pse::compat::Unescape("bad%2", &out));
    PSE_CHECK(!pse::compat::Unescape("bad%zz", &out));
    const std::string odd = "\xC3\xA9t\xC3\xA9 _x:y 100%";
    PSE_CHECK(pse::compat::Unescape(pse::compat::Escape(odd), &out) && out == odd);
}

PSE_TEST("compat: encode -> assemble round trip, lines fit the command channel") {
    Set s = Base();
    for (int i = 0; i < 60; ++i)   // many plugins + mods: several lines
        s.plugins.push_back({"dsfix.feature" + std::to_string(i), PSE_MAKE_VERSION(3, 3, 0),
                             (uint32_t(PSE_PLUGIN_SIMULATION) | uint32_t(PSE_PLUGIN_INTERNAL)), "="});
    for (int i = 0; i < 6; ++i) s.plugins.push_back({"ext.p" + std::to_string(i), 1, PSE_PLUGIN_SIMULATION, kHashB});
    for (int i = 0; i < 20; ++i) s.mods.push_back({"Mod " + std::to_string(i), kHashA});
    std::vector<std::string> lines = pse::compat::Encode(s, 7, 240);
    PSE_CHECK(lines.size() > 5);
    for (const std::string& l : lines) {
        PSE_CHECK(l.size() <= 240);
        PSE_CHECK(l.compare(0, 7, "PSEC 7 ") == 0);
    }
    Set back = RoundTrip(s);
    PSE_CHECK_EQ(back.plugins.size(), s.plugins.size());
    PSE_CHECK_EQ(back.mods.size(), s.mods.size());
    PSE_CHECK_STR(back.host.c_str(), s.host.c_str());
    PSE_CHECK_STR(back.mods[1].name.c_str(), "Mission Custom");
    PSE_CHECK_STR(back.plugins[3].hash.c_str(), kHashA.c_str());
    PSE_CHECK(back.plugins[4].hash.empty());
    PSE_CHECK_EQ(back.plugins[1].flags, static_cast<uint32_t>(PSE_PLUGIN_CLIENT_ONLY | PSE_PLUGIN_INTERNAL));
    PSE_CHECK(pse::compat::Compare(s, back, "x").empty());
}

PSE_TEST("compat: assembler - other lines, restarts, damage") {
    pse::compat::Assembler a;
    PSE_CHECK_EQ(a.Feed("START Mission1.dll 1"), pse::compat::Assembler::NotOurs);
    std::vector<std::string> one = pse::compat::Encode(Base(), 1), two = pse::compat::Encode(Base(), 2);
    // a resent list (new id) replaces a half-received one; leftovers of the old id are ignored
    PSE_CHECK_EQ(a.Feed(one[0]), pse::compat::Assembler::Pending);
    PSE_CHECK_EQ(a.Feed(two[0]), pse::compat::Assembler::Pending);
    PSE_CHECK_EQ(a.Feed(one[1]), pse::compat::Assembler::Pending);
    for (size_t i = 1; i + 1 < two.size(); ++i) PSE_CHECK_EQ(a.Feed(two[i]), pse::compat::Assembler::Pending);
    PSE_CHECK_EQ(a.Feed(two.back()), pse::compat::Assembler::Done);
    // a missing middle line: the end line's counts don't match
    pse::compat::Assembler b;
    for (size_t i = 0; i < two.size(); ++i)
        if (i != 1) b.Feed(two[i]);
    PSE_CHECK(!b.error.empty());
    PSE_CHECK_EQ(b.Feed("PSEC zz B"), pse::compat::Assembler::Bad);
    PSE_CHECK_EQ(b.Feed("PSEC 2 P name-only"), pse::compat::Assembler::Pending);   // not building: ignored
}

PSE_TEST("compat: identical sets match; client-only plugins may differ") {
    Set a = Base(), b = Base();
    PSE_CHECK(Diff(a, b).empty());
    b.plugins.erase(b.plugins.begin() + 4);                                         // kyle.hudcolour missing
    b.plugins[1].version = PSE_MAKE_VERSION(9, 0, 0);                                // client-only, other version
    b.plugins.push_back({"other.photomode", 1, PSE_PLUGIN_CLIENT_ONLY, ""});        // only there
    PSE_CHECK_STR(All(Diff(a, b)).c_str(), "");
}

PSE_TEST("compat: simulation plugin missing / extra / version / file / kind") {
    Set a = Base(), b = Base();
    b.plugins.erase(b.plugins.begin() + 3);   // the host lacks kyle.alienfaction
    std::vector<std::string> d = pse::compat::Compare(a, b, "the host");
    PSE_CHECK(Has(d, "the host is missing kyle.alienfaction 1.2.0"));
    d = pse::compat::Compare(b, a, "player 2");
    PSE_CHECK(Has(d, "missing kyle.alienfaction 1.2.0 (player 2 has it)"));

    b = Base();
    b.plugins[2].version = PSE_MAKE_VERSION(1, 0, 0);
    a.plugins[2].version = PSE_MAKE_VERSION(1, 0, 1);
    d = Diff(a, b);
    PSE_CHECK(Has(d, "dsfix.limits 1.0.1 here vs 1.0.0 on the host"));

    a = Base(), b = Base();
    b.plugins[3].hash = kHashB;
    d = Diff(a, b);
    PSE_CHECK(Has(d, "kyle.alienfaction 1.2.0: a different file here than on the host (SHA-256 aaaaaaaa vs bbbbbbbb)"));

    a = Base(), b = Base();
    b.plugins[3].hash = "=";   // built in on the other side
    PSE_CHECK(Has(Diff(a, b), "a plugin DLL here, built in on the host"));

    a = Base(), b = Base();   // unflagged = simulation; flags disagree
    b.plugins[3].flags = PSE_PLUGIN_CLIENT_ONLY;
    b.plugins[3].hash.clear();
    PSE_CHECK(Has(Diff(a, b), "kyle.alienfaction 1.2.0: simulation here, client-only on the host"));
    a.plugins[3].flags = 0;   // no flag at all: still simulation
    PSE_CHECK(Has(Diff(a, b), "simulation here, client-only"));
    PSE_CHECK(pse::compat::Simulation(0));
    PSE_CHECK(pse::compat::Simulation(PSE_PLUGIN_SIMULATION | PSE_PLUGIN_CLIENT_ONLY));
    PSE_CHECK(!pse::compat::Simulation(PSE_PLUGIN_CLIENT_ONLY));
}

PSE_TEST("compat: host build, fingerprint, API, protocol, game") {
    Set a = Base(), b = Base();
    b.hostFingerprint = "fedcba9876543210";
    std::vector<std::string> d = Diff(a, b);
    PSE_REQUIRE(d.size() == 1);   // built-in plugins are covered by that one line, not 3 more
    PSE_CHECK(Has(d, "DesertStormFix 3.3.0-Alpha: a different build here than on the host (simulation fingerprint "
                     "01234567 vs fedcba98)"));
    b = Base();
    b.host = "DesertStormFix 3.2.0";
    PSE_CHECK(Has(Diff(a, b), "DesertStormFix 3.3.0-Alpha here vs DesertStormFix 3.2.0 on the host"));
    b = Base();
    b.apiVersion = 1;
    PSE_CHECK(Has(Diff(a, b), "PSE loader API 0 here vs 1 on the host"));
    b = Base();
    b.protocol = 2;
    d = Diff(a, b);
    PSE_REQUIRE(d.size() == 1);
    PSE_CHECK(Has(d, "online check version 1 here vs 2"));
    b = Base();
    b.gameId = "ds2";
    PSE_CHECK(Has(Diff(a, b), "a different game: ds1 here, ds2 on the host"));
}

PSE_TEST("compat: mods by name (any case) + content hash") {
    Set a = Base(), b = Base();
    b.mods[0].name = "goldshades";   // case doesn't matter
    PSE_CHECK(Diff(a, b).empty());
    b.mods[1].hash = kHashA;
    PSE_CHECK(Has(Diff(a, b), "mod Mission Custom differs (its files aren't the same)"));
    b = Base();
    b.mods.pop_back();
    PSE_CHECK(Has(pse::compat::Compare(a, b, "the host"), "mod Mission Custom is switched on here, not on the host"));
    PSE_CHECK(Has(pse::compat::Compare(b, a, "player 2"), "missing mod Mission Custom (player 2 has it switched on)"));
}

PSE_TEST("compat: SHA-256") {
    PSE_CHECK_STR(pse::compat::Sha256Hex("abc", 3).c_str(),
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    PSE_CHECK_STR(pse::compat::Sha256Hex("", 0).c_str(),
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    pse::compat::Sha256 h;   // incremental = one shot
    h.Add("a", 1);
    h.Add("bc", 2);
    const std::string inc = h.Hex();
    PSE_CHECK_STR(inc.c_str(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string path = TempPath("sha.bin");
    HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    std::string big(200000, 'x');
    DWORD n = 0;
    WriteFile(f, big.data(), static_cast<DWORD>(big.size()), &n, nullptr);
    CloseHandle(f);
    std::string hex;
    PSE_CHECK(pse::compat::Sha256File(std::wstring(path.begin(), path.end()), &hex));
    PSE_CHECK_STR(hex.c_str(), pse::compat::Sha256Hex(big.data(), big.size()).c_str());
    DeleteFileA(path.c_str());
    PSE_CHECK(!pse::compat::Sha256File(L"C:\\no\\such\\file.dll", &hex));
}

PSE_TEST("compat: local plugin list - built in, external hashed, client-only without hash, disabled left out") {
    Fresh();
    PSEPluginHandle core = AddPlugin("dsfix");   // fixture: CLIENT_ONLY internal
    (void)core;
    static PSEPluginInfo sim = {sizeof(PSEPluginInfo), 1, "dsfix.limits", "", "", PSE_MAKE_VERSION(3, 3, 0), 0,
                                nullptr, PSE_PLUGIN_SIMULATION};
    pse::RegisterInternalPlugin(&sim);
    static PSEPluginInfo gone = {sizeof(PSEPluginInfo), 1, "ext.gone", "", "", 1, 0, nullptr, PSE_PLUGIN_SIMULATION};
    PSEPluginHandle g = pse::RegisterExternalPlugin(&gone);
    pse::DisablePlugin(g);
    std::vector<PluginEntry> list = pse::compat::LocalPlugins();
    std::string names;
    for (const PluginEntry& e : list) names += e.name + " ";
    PSE_CHECK_STR(names.c_str(), "dsfix dsfix.limits pse ");   // sorted; ext.gone disabled
    PSE_REQUIRE(list.size() == 3);
    PSE_CHECK_STR(list[1].hash.c_str(), "=");
    PSE_CHECK(list[1].flags & PSE_PLUGIN_INTERNAL);
}
