// pse.settings against a throw-away HKCU key (Software\PSE-test-<pid>, deleted at the end of each test).
#include "loader_fixture.h"

using namespace pse_fixture;

namespace {
const PSESettingsInterface* S() { return pse::SettingsInterface(); }

PSESettingDesc Desc(const char* key, PSESettingType type, int32_t def, int32_t mn = 0, int32_t mx = 0) {
    PSESettingDesc d{};
    d.size = sizeof d;
    d.key = key;
    d.type = type;
    d.defaultInt = def;
    d.minInt = mn;
    d.maxInt = mx;
    d.label = key;
    return d;
}

std::string KeyOf(const char* plugin) { return TestRegistryRoot() + "\\Plugins\\" + plugin; }
void PutDword(const char* plugin, const char* name, DWORD v) {
    RegSetKeyValueA(HKEY_CURRENT_USER, KeyOf(plugin).c_str(), name, REG_DWORD, &v, sizeof v);
}
void PutString(const char* plugin, const char* name, const char* v) {
    RegSetKeyValueA(HKEY_CURRENT_USER, KeyOf(plugin).c_str(), name, REG_SZ, v, (DWORD)strlen(v) + 1);
}
bool GetDword(const char* plugin, const char* name, DWORD* v) {
    DWORD size = sizeof *v;
    return RegGetValueA(HKEY_CURRENT_USER, KeyOf(plugin).c_str(), name, RRF_RT_REG_DWORD, nullptr, v, &size) ==
           ERROR_SUCCESS;
}

struct Changes {
    std::vector<std::string> keys;
};
void PSE_CALL Changed(const char* key, void* ctx) { static_cast<Changes*>(ctx)->keys.emplace_back(key); }

// Cleans up the registry even when a PSE_REQUIRE throws.
struct KeyGuard {
    KeyGuard() { DeleteTestKey(); }
    ~KeyGuard() { DeleteTestKey(); }
};
}  // namespace

PSE_TEST("settings: defaults, stored values, clamping per type") {
    KeyGuard guard;
    Fresh();
    PSEPluginHandle p = AddPlugin("set.a");
    PutDword("set.a", "Stored", 77);
    PutDword("set.a", "TooBig", 5000);
    PutDword("set.a", "TooSmall", (DWORD)-50);
    PutDword("set.a", "Flag", 9);
    PutDword("set.a", "Colour", 0xAB123456);
    PutDword("set.a", "Mode", 12);
    PutString("set.a", "WrongType", "123");   // REG_SZ for an int: ignored -> default

    static const char* const kChoices[] = {"OFF", "LOW", "HIGH", nullptr};
    auto d = Desc("Fresh", PSE_SETTING_INT, 40, 0, 90);
    PSE_CHECK_EQ(S()->Declare(p, &d), PSE_OK);
    d = Desc("Stored", PSE_SETTING_INT, 1, 0, 100);
    PSE_CHECK_EQ(S()->Declare(p, &d), PSE_OK);
    d = Desc("TooBig", PSE_SETTING_INT, 1, 0, 100);
    S()->Declare(p, &d);
    d = Desc("TooSmall", PSE_SETTING_INT, 1, -10, 100);
    S()->Declare(p, &d);
    d = Desc("Flag", PSE_SETTING_BOOL, 0);
    S()->Declare(p, &d);
    d = Desc("Colour", PSE_SETTING_COLOUR, 0xFFFFFF);
    S()->Declare(p, &d);
    d = Desc("Mode", PSE_SETTING_CHOICE, 1);
    d.choices = kChoices;
    S()->Declare(p, &d);
    d = Desc("WrongType", PSE_SETTING_INT, 3, 0, 1000);
    S()->Declare(p, &d);
    d = Desc("BadDefault", PSE_SETTING_INT, 500, 0, 10);   // default clamped too
    S()->Declare(p, &d);

    PSE_CHECK_EQ(S()->GetInt(p, "Fresh"), 40);
    PSE_CHECK_EQ(S()->GetInt(p, "Stored"), 77);
    PSE_CHECK_EQ(S()->GetInt(p, "TooBig"), 100);
    PSE_CHECK_EQ(S()->GetInt(p, "TooSmall"), -10);
    PSE_CHECK_EQ(S()->GetInt(p, "Flag"), 1);
    PSE_CHECK_EQ(S()->GetInt(p, "Colour"), 0x123456);
    PSE_CHECK_EQ(S()->GetInt(p, "Mode"), 2);
    PSE_CHECK_EQ(S()->GetInt(p, "WrongType"), 3);
    PSE_CHECK_EQ(S()->GetInt(p, "BadDefault"), 10);
    PSE_CHECK_EQ(S()->GetInt(p, "Undeclared"), 0);
    DWORD v = 0;
    PSE_CHECK(!GetDword("set.a", "Fresh", &v));   // Declare doesn't write
}

PSE_TEST("settings: Declare validation") {
    KeyGuard guard;
    Fresh();
    PSEPluginHandle p = AddPlugin("set.v");
    auto d = Desc("Good_Key1", PSE_SETTING_INT, 0, 0, 1);
    PSE_CHECK_EQ(S()->Declare(p, &d), PSE_OK);
    PSE_CHECK_EQ(S()->Declare(p, &d), PSE_E_EXISTS);
    PSE_CHECK_EQ(S()->Declare(0, &d), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(S()->Declare(p, nullptr), PSE_E_INVALID_ARG);
    d = Desc("bad key", PSE_SETTING_INT, 0, 0, 1);
    PSE_CHECK_EQ(S()->Declare(p, &d), PSE_E_INVALID_ARG);
    d = Desc("Inverted", PSE_SETTING_INT, 0, 5, 1);
    PSE_CHECK_EQ(S()->Declare(p, &d), PSE_E_INVALID_ARG);
    d = Desc("NoChoices", PSE_SETTING_CHOICE, 0);
    PSE_CHECK_EQ(S()->Declare(p, &d), PSE_E_INVALID_ARG);
    d = Desc("BadType", (PSESettingType)42, 0);
    PSE_CHECK_EQ(S()->Declare(p, &d), PSE_E_INVALID_ARG);
    d = Desc("Small", PSE_SETTING_INT, 0, 0, 1);
    d.size = 8;
    PSE_CHECK_EQ(S()->Declare(p, &d), PSE_E_INVALID_ARG);
    // Same key, other plugin: separate settings.
    PSEPluginHandle q = AddPlugin("set.w");
    d = Desc("Good_Key1", PSE_SETTING_INT, 1, 0, 1);
    PSE_CHECK_EQ(S()->Declare(q, &d), PSE_OK);
    PSE_CHECK_EQ(S()->GetInt(p, "Good_Key1"), 0);
    PSE_CHECK_EQ(S()->GetInt(q, "Good_Key1"), 1);
}

PSE_TEST("settings: SetInt writes, clamps, fires OnChange only on a change") {
    KeyGuard guard;
    Fresh();
    PSEPluginHandle p = AddPlugin("set.b"), other = AddPlugin("set.other");
    auto d = Desc("Cap", PSE_SETTING_INT, 240, 30, 500);
    S()->Declare(p, &d);
    static const char* const kChoices[] = {"A", "B", nullptr};
    d = Desc("Pick", PSE_SETTING_CHOICE, 0);
    d.choices = kChoices;
    S()->Declare(p, &d);
    Changes mine, theirs;
    PSE_CHECK_EQ(S()->OnChange(p, &Changed, &mine), PSE_OK);
    PSE_CHECK_EQ(S()->OnChange(other, &Changed, &theirs), PSE_OK);
    PSE_CHECK_EQ(S()->OnChange(p, nullptr, nullptr), PSE_E_INVALID_ARG);

    PSE_CHECK_EQ(S()->SetInt(p, "Cap", 144), PSE_OK);
    PSE_CHECK_EQ(S()->GetInt(p, "Cap"), 144);
    DWORD v = 0;
    PSE_CHECK(GetDword("set.b", "Cap", &v) && v == 144);
    PSE_CHECK_EQ(S()->SetInt(p, "Cap", 144), PSE_OK);   // same: no callback
    PSE_CHECK_EQ(S()->SetInt(p, "Cap", 9999), PSE_OK);  // clamped to 500
    PSE_CHECK_EQ(S()->GetInt(p, "Cap"), 500);
    PSE_CHECK_EQ(S()->SetInt(p, "Cap", 600), PSE_OK);   // clamps to 500 again: no change
    PSE_CHECK_EQ(S()->SetInt(p, "Pick", 7), PSE_OK);
    PSE_CHECK_EQ(S()->GetInt(p, "Pick"), 1);
    PSE_CHECK((mine.keys == std::vector<std::string>{"Cap", "Cap", "Pick"}));
    PSE_CHECK(theirs.keys.empty());
    PSE_CHECK_EQ(S()->SetInt(p, "Nope", 1), PSE_E_NOT_FOUND);

    // Persisted: a new start reads it back.
    Fresh();
    p = AddPlugin("set.b");
    d = Desc("Cap", PSE_SETTING_INT, 240, 30, 500);
    S()->Declare(p, &d);
    PSE_CHECK_EQ(S()->GetInt(p, "Cap"), 500);
}

PSE_TEST("settings: strings - default, UTF-8 round trip, truncation, type checks") {
    KeyGuard guard;
    Fresh();
    PSEPluginHandle p = AddPlugin("set.s");
    auto d = Desc("Name", PSE_SETTING_STRING, 0);
    d.defaultString = "Bradley";
    PSE_CHECK_EQ(S()->Declare(p, &d), PSE_OK);
    auto i = Desc("Num", PSE_SETTING_INT, 3, 0, 9);
    S()->Declare(p, &i);
    char buf[64];
    PSE_CHECK_EQ(S()->GetString(p, "Name", buf, sizeof buf), 7u);
    PSE_CHECK_STR(buf, "Bradley");
    Changes ch;
    S()->OnChange(p, &Changed, &ch);
    const char* utf8 = "Fol\xC3\xA9y \xE2\x9C\x93";   // "Foléy ✓"
    PSE_CHECK_EQ(S()->SetString(p, "Name", utf8), PSE_OK);
    PSE_CHECK_EQ(S()->SetString(p, "Name", utf8), PSE_OK);   // no change
    PSE_CHECK_EQ(ch.keys.size(), (size_t)1);
    char tiny[4];
    PSE_CHECK_EQ(S()->GetString(p, "Name", tiny, sizeof tiny), (uint32_t)strlen(utf8));
    PSE_CHECK_STR(tiny, "Fol");
    PSE_CHECK_EQ(S()->GetString(p, "Name", nullptr, 0), (uint32_t)strlen(utf8));
    PSE_CHECK_EQ(S()->SetString(p, "Num", "x"), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(S()->SetInt(p, "Name", 1), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(S()->SetString(p, "Missing", "x"), PSE_E_NOT_FOUND);
    PSE_CHECK_EQ(S()->SetString(p, "Name", nullptr), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(S()->GetString(p, "Missing", buf, sizeof buf), 0u);
    PSE_CHECK_STR(buf, "");
    PSE_CHECK_EQ(S()->GetInt(p, "Name"), 0);

    Fresh();
    p = AddPlugin("set.s");
    S()->Declare(p, &d);
    S()->GetString(p, "Name", buf, sizeof buf);
    PSE_CHECK_STR(buf, utf8);
}

PSE_TEST("settings: Describe enumerates in declaration order") {
    KeyGuard guard;
    Fresh();
    PSEPluginHandle a = AddPlugin("set.d1"), b = AddPlugin("set.d2");
    static const char* const kChoices[] = {"X", "Y", nullptr};
    auto d = Desc("A1", PSE_SETTING_BOOL, 1);
    d.help = "help text";
    d.group = "GROUP";
    S()->Declare(a, &d);
    d = Desc("B1", PSE_SETTING_CHOICE, 0);
    d.choices = kChoices;
    S()->Declare(b, &d);
    d = Desc("A2", PSE_SETTING_INT, 0, 0, 5);
    S()->Declare(a, &d);

    const PSESettingDesc* out = nullptr;
    PSEPluginHandle owner = 0;
    std::vector<std::string> all;
    for (uint32_t i = 0; S()->Describe(0, i, &out, &owner) == PSE_OK; ++i) all.emplace_back(out->key);
    PSE_CHECK((all == std::vector<std::string>{"A1", "B1", "A2"}));
    PSE_CHECK_EQ(S()->Describe(a, 1, &out, &owner), PSE_OK);
    PSE_CHECK_STR(out->key, "A2");
    PSE_CHECK_EQ(owner, a);
    PSE_CHECK_EQ(S()->Describe(a, 0, &out, nullptr), PSE_OK);
    PSE_CHECK_STR(out->help, "help text");
    PSE_CHECK_STR(out->group, "GROUP");
    PSE_CHECK_EQ(out->size, (uint32_t)sizeof(PSESettingDesc));
    PSE_CHECK_EQ(S()->Describe(b, 0, &out, &owner), PSE_OK);
    PSE_REQUIRE(out->choices);
    PSE_CHECK_STR(out->choices[1], "Y");
    PSE_CHECK(out->choices[2] == nullptr);
    PSE_CHECK_EQ(S()->Describe(a, 2, &out, &owner), PSE_E_NOT_FOUND);
    PSE_CHECK_EQ(S()->Describe(0, 3, &out, &owner), PSE_E_NOT_FOUND);
    PSE_CHECK_EQ(S()->Describe(0, 0, nullptr, &owner), PSE_E_INVALID_ARG);
}

#include "internal.h"

PSE_TEST("settings: a disabled plugin's declarations and change callbacks are dropped") {
    pse_fixture::Fresh();
    const PSESettingsInterface* s = pse::SettingsInterface();
    const PSEPluginHandle gone = pse_fixture::AddPlugin("test.gone");
    const PSEPluginHandle stays = pse_fixture::AddPlugin("test.stays");
    PSESettingDesc d{};
    d.size = sizeof d;
    d.key = "Value";
    d.type = PSE_SETTING_INT;
    d.minInt = 0;
    d.maxInt = 10;
    PSE_CHECK_EQ(s->Declare(gone, &d), PSE_OK);
    PSE_CHECK_EQ(s->Declare(stays, &d), PSE_OK);
    static int calls = 0;
    calls = 0;
    auto cb = [](const char*, void*) { ++calls; };
    s->OnChange(gone, cb, nullptr);
    s->OnChange(stays, cb, nullptr);

    pse::detail::ForgetSettings(gone);
    PSE_CHECK(s->SetInt(gone, "Value", 3) != PSE_OK);   // no longer declared
    PSE_CHECK_EQ(s->SetInt(stays, "Value", 3), PSE_OK);
    PSE_CHECK_EQ(calls, 1);                              // only the remaining plugin's callback
    const PSESettingDesc* out = nullptr;
    PSEPluginHandle owner = 0;
    PSE_CHECK_EQ(s->Describe(0, 0, &out, &owner), PSE_OK);
    PSE_CHECK_EQ(owner, stays);
    PSE_CHECK(s->Describe(0, 1, &out, &owner) != PSE_OK);
    pse_fixture::DeleteTestKey();
}
