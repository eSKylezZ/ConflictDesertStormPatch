// pse.ui's pure parts (src/core/pse_ui_model.cpp): a plugin's settings page built from pse.settings' Describe, value
// stepping / texts, the PLUGINS list from the loader's load results, UTF-8 -> the game fonts' 8-bit text, the per-plugin
// text-hash allocator. Real pse.settings (throw-away registry key), no game.
#include <cstring>

#include "core/pse_ui_model.cpp"  // NOLINT: the unit under test (pure code)
#include "loader_fixture.h"

using namespace pse_fixture;

namespace {
const PSESettingsInterface* S() { return pse::SettingsInterface(); }

struct KeyGuard {
    ~KeyGuard() { DeleteTestKey(); }
};

PSESettingDesc D(const char* key, PSESettingType type, uint32_t flags, int32_t def, const char* group = nullptr) {
    PSESettingDesc d{};
    d.size = sizeof d;
    d.key = key;
    d.type = type;
    d.flags = flags;
    d.defaultInt = def;
    d.label = key;
    d.group = group;
    return d;
}

const char* const kModes[] = {"OFF", "AIMING", "ALWAYS", nullptr};

// One plugin with a setting of every type (+ a hidden one), in two groups.
PSEPluginHandle DeclareDemo(const char* name) {
    PSEPluginHandle h = AddPlugin(name);
    PSESettingDesc b = D("Banner", PSE_SETTING_BOOL, PSE_SETTING_LIVE | PSE_SETTING_LAUNCHER, 1, "BANNER");
    b.label = "SHOW BANNER";
    b.help = "draws a line";
    PSESettingDesc i = D("Height", PSE_SETTING_INT, PSE_SETTING_LIVE, 8, "BANNER");
    i.minInt = 2, i.maxInt = 9, i.step = 3;
    PSESettingDesc c = D("Mode", PSE_SETTING_CHOICE, PSE_SETTING_NEXT_LEVEL, 1, "BANNER");
    c.choices = kModes;
    PSESettingDesc col = D("Colour", PSE_SETTING_COLOUR, PSE_SETTING_LIVE, 0x123456, "LOOK");
    PSESettingDesc s = D("Greeting", PSE_SETTING_STRING, 0, 0, "LOOK");
    s.defaultString = "hi";
    PSESettingDesc hid = D("Secret", PSE_SETTING_INT, PSE_SETTING_HIDDEN, 7);
    hid.maxInt = 10;
    PSESettingDesc r = D("Restart", PSE_SETTING_BOOL, 0, 0);
    for (const PSESettingDesc* d : {&b, &i, &c, &col, &s, &hid, &r}) PSE_REQUIRE(S()->Declare(h, d) == PSE_OK);
    return h;
}
}  // namespace

PSE_TEST("ui model: page from Describe - every type, headings, hidden left out") {
    Fresh();
    KeyGuard guard;
    const PSEPluginHandle other = AddPlugin("other.plugin");
    PSESettingDesc x = D("X", PSE_SETTING_BOOL, 0, 0);
    PSE_REQUIRE(S()->Declare(other, &x) == PSE_OK);
    const PSEPluginHandle h = DeclareDemo("ui.demo");
    const std::vector<pse_ui::Row> rows = pse_ui::BuildPage(S(), h);
    // BANNER heading, 3 rows, LOOK heading, 2 rows, then Restart (no group: no heading)
    PSE_REQUIRE(rows.size() == 8u);
    PSE_CHECK(rows[0].kind == pse_ui::RowKind::Heading);
    PSE_CHECK_STR(rows[0].label.c_str(), "BANNER");
    PSE_CHECK(rows[1].kind == pse_ui::RowKind::Bool);
    PSE_CHECK_STR(rows[1].label.c_str(), "SHOW BANNER");
    PSE_CHECK_STR(rows[1].help.c_str(), "draws a line");
    PSE_CHECK(rows[2].kind == pse_ui::RowKind::Int);
    PSE_CHECK_EQ(rows[2].min, 2);
    PSE_CHECK_EQ(rows[2].max, 9);
    PSE_CHECK_EQ(rows[2].step, 3);
    PSE_CHECK(rows[3].kind == pse_ui::RowKind::Choice);
    PSE_CHECK_EQ(rows[3].choices.size(), 3u);
    PSE_CHECK_EQ(rows[3].max, 2);
    PSE_CHECK(rows[4].kind == pse_ui::RowKind::Heading);
    PSE_CHECK_STR(rows[4].label.c_str(), "LOOK");
    PSE_CHECK(rows[5].kind == pse_ui::RowKind::Colour);
    PSE_CHECK(rows[6].kind == pse_ui::RowKind::String);
    PSE_CHECK_STR(rows[7].key.c_str(), "Restart");
    for (const pse_ui::Row& r : rows) {
        PSE_CHECK(r.key != "Secret");
        PSE_CHECK(r.key != "X");   // another plugin's
        PSE_CHECK_EQ(r.owner, h);
    }
    PSE_CHECK_EQ(pse_ui::VisibleSettings(S(), h), 6u);
    PSE_CHECK_EQ(pse_ui::VisibleSettings(S(), other), 1u);
    PSE_CHECK(pse_ui::BuildPage(S(), 0).empty());
    PSE_CHECK(pse_ui::BuildPage(nullptr, h).empty());
}

PSE_TEST("ui model: stepping, value texts, markers") {
    Fresh();
    KeyGuard guard;
    const std::vector<pse_ui::Row> rows = pse_ui::BuildPage(S(), DeclareDemo("ui.step"));
    const pse_ui::Row &b = rows[1], &i = rows[2], &c = rows[3], &col = rows[5], &s = rows[6], &h = rows[0];
    PSE_CHECK_EQ(pse_ui::StepValue(b, 1, 1), 0);
    PSE_CHECK_EQ(pse_ui::StepValue(b, 0, -1), 1);
    PSE_CHECK_EQ(pse_ui::StepValue(i, 2, 1), 5);
    PSE_CHECK_EQ(pse_ui::StepValue(i, 8, 1), 9);    // stops at max (not a multiple of the step)
    PSE_CHECK_EQ(pse_ui::StepValue(i, 9, 1), 9);
    PSE_CHECK_EQ(pse_ui::StepValue(i, 3, -1), 2);   // stops at min
    PSE_CHECK_EQ(pse_ui::StepValue(c, 2, 1), 0);    // choices wrap
    PSE_CHECK_EQ(pse_ui::StepValue(c, 0, -1), 2);
    PSE_CHECK_EQ(pse_ui::StepValue(col, 0x123456, 1), 0x123456);
    PSE_CHECK_EQ(pse_ui::StepValue(i, 5, 0), 5);
    PSE_CHECK(pse_ui::Spinner(b) && pse_ui::Spinner(i) && pse_ui::Spinner(c));
    PSE_CHECK(!pse_ui::Spinner(col) && !pse_ui::Spinner(s) && !pse_ui::Spinner(h));
    PSE_CHECK(pse_ui::Editable(col) && !pse_ui::Editable(s) && !pse_ui::Editable(h));
    PSE_CHECK_STR(pse_ui::ValueText(b, 1).c_str(), "ON");
    PSE_CHECK_STR(pse_ui::ValueText(b, 0).c_str(), "OFF");
    PSE_CHECK_STR(pse_ui::ValueText(i, -5).c_str(), "-5");
    PSE_CHECK_STR(pse_ui::ValueText(c, 1).c_str(), "AIMING");
    PSE_CHECK_STR(pse_ui::ValueText(c, 7).c_str(), "?");
    PSE_CHECK_STR(pse_ui::ValueText(col, 0xAB12CD).c_str(), "#AB12CD");
    PSE_CHECK_STR(pse_ui::ValueText(s, 0, "hey").c_str(), "hey");
    PSE_CHECK_STR(pse_ui::WhenMarker(PSE_SETTING_LIVE), "");
    PSE_CHECK_STR(pse_ui::WhenMarker(PSE_SETTING_NEXT_LEVEL), " +");
    PSE_CHECK_STR(pse_ui::WhenMarker(0), " *");
    PSE_CHECK(pse_ui::NeedsRestart(0) && !pse_ui::NeedsRestart(PSE_SETTING_LIVE) &&
              !pse_ui::NeedsRestart(PSE_SETTING_NEXT_LEVEL));
    // a single-value int / choice is no spinner
    pse_ui::Row one = i;
    one.max = one.min;
    PSE_CHECK(!pse_ui::Spinner(one));
}

PSE_TEST("ui model: launcher rows") {
    PSESettingDesc d = D("A", PSE_SETTING_BOOL, PSE_SETTING_LAUNCHER, 0);
    PSE_CHECK(pse_ui::LauncherSetting(d));
    d.flags |= PSE_SETTING_HIDDEN;
    PSE_CHECK(!pse_ui::LauncherSetting(d));
    d.flags = PSE_SETTING_LAUNCHER;
    d.type = PSE_SETTING_COLOUR;
    PSE_CHECK(!pse_ui::LauncherSetting(d));
    d.type = PSE_SETTING_STRING;
    PSE_CHECK(!pse_ui::LauncherSetting(d));
    d.type = PSE_SETTING_CHOICE;
    PSE_CHECK(pse_ui::LauncherSetting(d));
    d.flags = 0;
    PSE_CHECK(!pse_ui::LauncherSetting(d));
}

PSE_TEST("ui model: PLUGINS list from load results") {
    std::vector<pse::PluginLoadResult> res(3);
    res[0].name = "dsfix", res[0].displayName = "DesertStormFix", res[0].version = PSE_MAKE_VERSION(3, 3, 0);
    res[0].state = pse::PluginState::Loaded, res[0].handle = 2;
    res[1].name = "example.ui_demo", res[1].version = PSE_MAKE_VERSION(1, 2, 3);
    res[1].state = pse::PluginState::Loaded, res[1].handle = 9;
    res[2].name = "bad.one", res[2].state = pse::PluginState::Refused, res[2].reason = "requires x >= 2";
    const auto count = [](PSEPluginHandle h, void*) -> size_t { return h == 9 ? 4u : 99u; };
    const std::vector<pse_ui::PluginEntry> list = pse_ui::BuildPluginList(res, {"dsfix"}, count, nullptr);
    PSE_REQUIRE(list.size() == 3u);
    PSE_CHECK(list[0].host);
    PSE_CHECK_EQ(list[0].settings, 0u);   // host: its own menus
    PSE_CHECK_STR(list[0].version.c_str(), "3.3.0");
    PSE_CHECK_STR(list[1].displayName.c_str(), "example.ui_demo");   // no display name: the name
    PSE_CHECK_EQ(list[1].settings, 4u);
    PSE_CHECK_STR(list[1].version.c_str(), "1.2.3");
    PSE_CHECK(list[2].state == pse::PluginState::Refused);
    PSE_CHECK_STR(list[2].reason.c_str(), "requires x >= 2");
    PSE_CHECK_STR(list[2].version.c_str(), "");
    PSE_CHECK_EQ(list[2].settings, 0u);
    PSE_CHECK_STR(pse_ui::StateText(pse::PluginState::Failed), "FAILED");
}

PSE_TEST("ui model: UTF-8 to the game fonts' text") {
    using pse_ui::ToGameText;
    PSE_CHECK_STR(ToGameText("Plain ASCII 123").c_str(), "Plain ASCII 123");
    PSE_CHECK_STR(ToGameText("caf\xC3\xA9").c_str(), "caf\xE9");                       // in Windows-1252
    PSE_CHECK_STR(ToGameText("caf\xC3\xA9", true).c_str(), "CAF\xC9");                 // upper case
    PSE_CHECK_STR(ToGameText("\xE2\x80\x93 \xE2\x80\x9Cq\xE2\x80\x9D").c_str(), "\x96 \x93q\x94");   // 1252 has them
    PSE_CHECK_STR(ToGameText("\xC5\x81\xC3\xB3" "d\xC5\xBA").c_str(), "L\xF3" "dz");        // Lodz: Latin Ext-A plain
    PSE_CHECK_STR(ToGameText("\xE2\x98\x83!").c_str(), "?!");                           // snowman: not in the font
    PSE_CHECK_STR(ToGameText("a\x01" "b\tc").c_str(), "a b c");                          // icon slots / tab: spaces
    PSE_CHECK_STR(ToGameText("one\ntwo").c_str(), "one");                               // one line
    PSE_CHECK_STR(ToGameText("\xFF\xC3").c_str(), "??");                                // malformed
    PSE_CHECK_STR(ToGameText("\xC0\xAF").c_str(), "?");                              // overlong '/'
    PSE_CHECK_STR(ToGameText("\xC5\x93uvre", true).c_str(), "\x8C" "UVRE");             // oe -> OE (1252 0x8C)
    PSE_CHECK_STR(ToGameText("abcdef", false, nullptr, nullptr, 3).c_str(), "abc");
    PSE_CHECK_STR(ToGameText(nullptr).c_str(), "");
    // a font without the accented letters / typographic dashes: plain forms, else '?'
    const auto ascii = [](unsigned char c, void*) { return c < 0x80; };
    PSE_CHECK_STR(ToGameText("Caf\xC3\xA9 \xE2\x80\x93 \xC3\x86ther \xE2\x80\xA6", false, ascii, nullptr).c_str(),
                  "Cafe - AEther ...");
    PSE_CHECK_STR(ToGameText("\xC3\xBC" "ber", true, ascii, nullptr).c_str(), "UBER");
    PSE_CHECK_STR(ToGameText("\xC2\xB1", false, ascii, nullptr).c_str(), "?");          // no plain form
    const auto noQ = [](unsigned char c, void*) { return c != 'Q' && c < 0x80; };
    PSE_CHECK_STR(ToGameText("Q", false, noQ, nullptr).c_str(), "?");
}

PSE_TEST("ui model: text hashes per plugin + key") {
    pse_ui::TextTable t;
    const uint32_t a = t.Register(3, "hello", "Hello");
    const uint32_t b = t.Register(3, "bye", "Bye");
    const uint32_t c = t.Register(4, "hello", "Other");
    PSE_CHECK(a && b && c && a != b && a != c);
    PSE_CHECK(pse_ui::TextTable::Ours(a) && pse_ui::TextTable::Ours(c));
    PSE_CHECK(!pse_ui::TextTable::Ours(0xC00B0001));
    PSE_CHECK_EQ(a, 0xC4003001u);
    PSE_CHECK_EQ(c, 0xC4004001u);
    PSE_CHECK_EQ(t.Register(3, "hello", "Hello again"), a);   // same key: same hash, new text
    PSE_CHECK_STR(t.Find(a)->c_str(), "Hello again");
    PSE_CHECK_EQ(t.Hash(3, "bye"), b);
    PSE_CHECK_EQ(t.Hash(3, "nope"), 0u);
    PSE_CHECK(t.Find(0xC4003999u) == nullptr);
    PSE_CHECK_EQ(t.Register(0, "x", "y"), 0u);
    PSE_CHECK_EQ(t.Register(3, "", "y"), 0u);
    pse_ui::TextTable full;
    for (uint32_t i = 0; i < pse_ui::TextTable::kPerPlugin; ++i) PSE_REQUIRE(full.Register(5, std::to_string(i), "t"));
    PSE_CHECK_EQ(full.Register(5, "one-too-many", "t"), 0u);
    PSE_CHECK(full.Register(5, "0", "again") != 0u);   // an existing key still works
}
