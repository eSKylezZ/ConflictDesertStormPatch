// Settings key migration (loader.h MigrateRegistryValues; DesertStormFix's Enhancements key -> Plugins\dsfix) and
// PluginSettingsKey, against throw-away HKCU keys under Software\PSE-test-<pid> (deleted at the end of each test).
#include "loader_fixture.h"

using namespace pse_fixture;

namespace {
std::string Root() { return TestRegistryRoot(); }
std::string OldKey() { return Root() + "\\Old\\Enhancements"; }
std::string StoreKey() { return Root() + "\\VirtualStore\\Enhancements"; }
std::string NewKey() { return Root() + "\\Plugins\\dsfix"; }

void PutDword(const std::string& key, const char* name, DWORD v) {
    RegSetKeyValueA(HKEY_CURRENT_USER, key.c_str(), name, REG_DWORD, &v, sizeof v);
}
void PutString(const std::string& key, const char* name, const char* v) {
    RegSetKeyValueA(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, v, (DWORD)strlen(v) + 1);
}
void PutBinary(const std::string& key, const char* name, const void* data, DWORD size) {
    RegSetKeyValueA(HKEY_CURRENT_USER, key.c_str(), name, REG_BINARY, data, size);
}
bool Dword(const std::string& key, const char* name, DWORD* v) {
    DWORD size = sizeof *v;
    return RegGetValueA(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_DWORD, nullptr, v, &size) == ERROR_SUCCESS;
}
std::string String(const std::string& key, const char* name) {
    char buf[256] = {};
    DWORD size = sizeof buf;
    if (RegGetValueA(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return "<none>";
    return buf;
}
bool Exists(const std::string& key, const char* name) {
    return RegGetValueA(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_ANY, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}
int ValueCount(const std::string& key) {
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return -1;
    DWORD n = 0;
    RegQueryInfoKeyA(k, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &n, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(k);
    return (int)n;
}

pse::MigrationResult Run() {
    return pse::MigrateRegistryValues({{HKEY_CURRENT_USER, OldKey(), 0}, {HKEY_CURRENT_USER, StoreKey(), 0}},
                                      HKEY_CURRENT_USER, NewKey(), "MigratedFromEnhancements");
}

struct KeyGuard {
    KeyGuard() { DeleteTestKey(); }
    ~KeyGuard() { DeleteTestKey(); }
};
}  // namespace

PSE_TEST("settings migration: copies every value and type, sets the marker, leaves the old key alone") {
    KeyGuard guard;
    PutDword(OldKey(), "FpsCap", 144);
    PutDword(OldKey(), "FeatMods", 0);
    PutString(OldKey(), "LoadoutSP0", "0;1;US_WPN_M16A2;-;-");
    const unsigned char layout[] = {1, 2, 3, 4, 0, 0xFF, 7};
    PutBinary(OldKey(), "PadLayout1", layout, sizeof layout);
    PutString(OldKey(), "", "default value");   // the key's default value isn't a setting

    pse::MigrationResult r = Run();
    PSE_CHECK(r.ran);
    PSE_CHECK(r.ok);
    PSE_CHECK_EQ(r.sourcesFound, 1);   // the VirtualStore source doesn't exist
    PSE_CHECK_EQ(r.copied.size(), (size_t)4);
    PSE_CHECK(r.kept.empty() && r.failed.empty());

    DWORD v = 0;
    PSE_CHECK(Dword(NewKey(), "FpsCap", &v) && v == 144);
    PSE_CHECK(Dword(NewKey(), "FeatMods", &v) && v == 0);
    PSE_CHECK_STR(String(NewKey(), "LoadoutSP0").c_str(), "0;1;US_WPN_M16A2;-;-");
    unsigned char back[16] = {};
    DWORD size = sizeof back, type = 0;
    PSE_CHECK(RegGetValueA(HKEY_CURRENT_USER, NewKey().c_str(), "PadLayout1", RRF_RT_ANY, &type, back, &size) ==
              ERROR_SUCCESS);
    PSE_CHECK_EQ(type, (DWORD)REG_BINARY);
    PSE_CHECK_EQ(size, (DWORD)sizeof layout);
    PSE_CHECK(memcmp(back, layout, sizeof layout) == 0);
    PSE_CHECK(Dword(NewKey(), "MigratedFromEnhancements", &v) && v == 1);
    PSE_CHECK_EQ(ValueCount(NewKey()), 5);   // 4 values + the marker, no default value

    // Old key untouched.
    PSE_CHECK_EQ(ValueCount(OldKey()), 5);
    PSE_CHECK(Dword(OldKey(), "FpsCap", &v) && v == 144);
    PSE_CHECK(!Exists(OldKey(), "MigratedFromEnhancements"));
}

PSE_TEST("settings migration: runs once - the marker stops later copies") {
    KeyGuard guard;
    PutDword(OldKey(), "FpsCap", 144);
    PSE_CHECK(Run().ran);
    PutDword(NewKey(), "FpsCap", 60);       // changed in the new place since
    PutDword(OldKey(), "PerfOverlay", 1);   // an old build wrote the old key afterwards
    pse::MigrationResult r = Run();
    PSE_CHECK(!r.ran);
    PSE_CHECK(r.ok);
    DWORD v = 0;
    PSE_CHECK(Dword(NewKey(), "FpsCap", &v) && v == 60);
    PSE_CHECK(!Exists(NewKey(), "PerfOverlay"));
}

PSE_TEST("settings migration: values already in the new key win; first source wins; nothing to copy still marks") {
    KeyGuard guard;
    PutDword(NewKey(), "FpsCap", 60);          // set through pse.settings before the migration ran
    PutDword(OldKey(), "FpsCap", 144);
    PutDword(OldKey(), "VSync", 0);
    PutDword(StoreKey(), "VSync", 1);          // second source: VSync already taken from the first
    PutString(StoreKey(), "UnlockedWeaponsSP", "US_WPN_MP5SD;");
    pse::MigrationResult r = Run();
    PSE_CHECK(r.ran && r.ok);
    PSE_CHECK_EQ(r.sourcesFound, 2);
    PSE_CHECK_EQ(r.kept.size(), (size_t)1);
    PSE_CHECK_STR(r.kept[0].c_str(), "FpsCap");
    PSE_CHECK_EQ(r.copied.size(), (size_t)2);
    DWORD v = 7;
    PSE_CHECK(Dword(NewKey(), "FpsCap", &v) && v == 60);
    PSE_CHECK(Dword(NewKey(), "VSync", &v) && v == 0);
    PSE_CHECK_STR(String(NewKey(), "UnlockedWeaponsSP").c_str(), "US_WPN_MP5SD;");

    // No old key at all: marker only.
    DeleteTestKey();
    r = Run();
    PSE_CHECK(r.ran && r.ok);
    PSE_CHECK_EQ(r.sourcesFound, 0);
    PSE_CHECK(r.copied.empty());
    PSE_CHECK(Dword(NewKey(), "MigratedFromEnhancements", &v) && v == 1);
    PSE_CHECK_EQ(ValueCount(NewKey()), 1);
}

PSE_TEST("settings migration: bad arguments do nothing") {
    KeyGuard guard;
    PutDword(OldKey(), "FpsCap", 144);
    PSE_CHECK(!pse::MigrateRegistryValues({{HKEY_CURRENT_USER, OldKey(), 0}}, HKEY_CURRENT_USER, NewKey(), "").ran);
    PSE_CHECK(!pse::MigrateRegistryValues({{HKEY_CURRENT_USER, OldKey(), 0}}, HKEY_CURRENT_USER, "", "M").ran);
    PSE_CHECK(!pse::MigrateRegistryValues({{HKEY_CURRENT_USER, OldKey(), 0}}, nullptr, NewKey(), "M").ran);
    PSE_CHECK_EQ(ValueCount(NewKey()), -1);
}

PSE_TEST("settings migration: migrated values are read by pse.settings Declare") {
    KeyGuard guard;
    Fresh();   // registryRoot = TestRegistryRoot(), so plugin "dsfix" lives at NewKey()
    PSEPluginHandle me = AddPlugin("dsfix");
    PSE_CHECK_STR(pse::PluginSettingsKey(me).c_str(), NewKey().c_str());
    PSE_CHECK(pse::PluginSettingsKey(0).empty());
    PutDword(OldKey(), "FpsCap", 144);
    PutDword(OldKey(), "MenuBarColour", 0x123456);
    PSE_CHECK(Run().ok);

    const PSESettingsInterface* s = pse::SettingsInterface();
    PSESettingDesc d{};
    d.size = sizeof d;
    d.key = "FpsCap";
    d.type = PSE_SETTING_INT;
    d.defaultInt = 240;
    d.minInt = 0;
    d.maxInt = 1000;
    PSE_CHECK_EQ(s->Declare(me, &d), PSE_OK);
    d.key = "MenuBarColour";
    d.type = PSE_SETTING_COLOUR;
    d.defaultInt = 0x1D3909;
    PSE_CHECK_EQ(s->Declare(me, &d), PSE_OK);
    PSE_CHECK_EQ(s->GetInt(me, "FpsCap"), 144);
    PSE_CHECK_EQ(s->GetInt(me, "MenuBarColour"), 0x123456);
    // A write goes to the new key only.
    PSE_CHECK_EQ(s->SetInt(me, "FpsCap", 60), PSE_OK);
    DWORD v = 0;
    PSE_CHECK(Dword(NewKey(), "FpsCap", &v) && v == 60);
    PSE_CHECK(Dword(OldKey(), "FpsCap", &v) && v == 144);
}
