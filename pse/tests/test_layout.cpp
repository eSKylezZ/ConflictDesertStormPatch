// Phase 2 layout (docs/sdk/layout.md): host plugins + pse.host forwarders (pse/hostlink, from inside a real plugin
// DLL: pse/tests/host_plugin), the two load rounds, the DS1 game layer's detection, and the whole start-up sequence
// pse_loader.dll runs (pse::Start) with scratch PSE folders.
#include <windows.h>

#include <string>
#include <vector>

#include "exe_image.h"
#include "internal.h"
#include "loader.h"
#include "loader_fixture.h"
#include "pse_test.h"

using namespace pse_fixture;

namespace {

struct Probe {
    uint32_t size, version;
    uint32_t(PSE_CALL* SubHandle)();
    int(PSE_CALL* SelfIsHost)();
    int(PSE_CALL* SinkHas)(const char*);
    const char*(PSE_CALL* LocalHash)(const char*);
    uint32_t(PSE_CALL* ResultCount)();
    const char*(PSE_CALL* SettingsKey)();
    uint32_t(PSE_CALL* InOrderCount)();
};
const Probe* GetProbe() {
    return static_cast<const Probe*>(pse::MessagingInterface()->QueryInterface(pse::FindPlugin("pse"), "test.host.probe", 1));
}

bool Exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// A scratch folder tree (sub folders allowed), removed afterwards.
struct Tree {
    std::string dir;
    explicit Tree(const char* tag) : dir(TempPath((std::string("layout-") + tag).c_str())) {
        Remove(dir);
        CreateDirectoryA(dir.c_str(), nullptr);
    }
    ~Tree() { Remove(dir); }
    static void Remove(const std::string& d) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((d + "\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const std::string n = fd.cFileName;
                if (n == "." || n == "..") continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) Remove(d + "\\" + n);
                else DeleteFileA((d + "\\" + n).c_str());
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryA(d.c_str());
    }
    std::string Sub(const std::string& rel) {
        std::string p = dir;
        size_t start = 0;
        while (start <= rel.size()) {
            const size_t slash = rel.find('\\', start);
            p += "\\" + rel.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            CreateDirectoryA(p.c_str(), nullptr);
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        return p;
    }
    // stem.dll (+ stem.pse, or the given manifest text) from srcDir into rel.
    void Plugin(const std::string& srcDir, const char* stem, const std::string& rel, const char* manifest = nullptr) {
        const std::string to = Sub(rel);
        CopyFileA((srcDir + "\\" + stem + ".dll").c_str(), (to + "\\" + stem + ".dll").c_str(), FALSE);
        if (manifest) Write(to + "\\" + stem + ".pse", manifest);
        else CopyFileA((srcDir + "\\" + stem + ".pse").c_str(), (to + "\\" + stem + ".pse").c_str(), FALSE);
    }
    static void Write(const std::string& path, const std::string& text) {
        HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD n = 0;
        WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
        CloseHandle(h);
    }
    std::wstring Wide(const std::string& s) const { return std::wstring(s.begin(), s.end()); }
};

const pse::PluginLoadResult* Result(const std::vector<pse::PluginLoadResult>& all, const char* name) {
    for (const auto& r : all)
        if (r.name == name) return &r;
    return nullptr;
}

void RequireHostPlugin() {
    if (!Exists(std::string(PSE_PLUGIN_DIR) + "\\test.host.dll")) PSE_SKIP("test.host.dll not built");
}

struct EnvVar {
    const char* name;
    EnvVar(const char* n, const char* v) : name(n) { SetEnvironmentVariableA(n, v); }
    ~EnvVar() { SetEnvironmentVariableA(name, nullptr); }
};

}  // namespace

// ---- host plugins (LoadPlugins with a host list) --------------------------------------------------------------------

PSE_TEST("layout: manifest host key") {
    pse::PluginManifest m;
    std::string error;
    PSE_REQUIRE(pse::ParseManifest("name = a\nhost = true\n", &m, &error));
    PSE_CHECK(m.host);
    PSE_REQUIRE(pse::ParseManifest("name = a\nhost = no\n", &m, &error));
    PSE_CHECK(!m.host);
    PSE_CHECK(!pse::ParseManifest("name = a\nhost = maybe\n", &m, &error));
}

PSE_TEST("layout: host plugin gets pse.host, its forwarders reach the loader") {
    RequireHostPlugin();
    Fresh();
    Tree t("host");
    t.Plugin(PSE_PLUGIN_DIR, "test.host", "Plugins");
    PSE_CHECK_EQ(pse::LoadPlugins(t.Wide(t.dir + "\\Plugins"), {"test.host"}), 1u);
    const PSEPluginHandle host = pse::FindPlugin("test.host");
    const PSEPluginHandle sub = pse::FindPlugin("test.host.sub");
    PSE_REQUIRE(host && sub);
    PSE_CHECK(pse::PluginExternal(host) && pse::PluginHost(host));
    PSE_CHECK(!pse::PluginExternal(sub) && !pse::PluginHost(sub));
    PSE_CHECK(pse::PluginInfo(sub)->flags & PSE_PLUGIN_INTERNAL);   // sub-plugins keep internal semantics
    PSE_CHECK_EQ(pse::PluginParent(sub), host);
    const Probe* probe = GetProbe();
    PSE_REQUIRE(probe);
    PSE_CHECK_EQ(probe->SubHandle(), sub);
    PSE_CHECK_EQ(probe->SelfIsHost(), 1);
    // compat::LocalPlugins inside the plugin (hostlink): the host and its sub-plugin count as built in (fingerprint),
    // not by DLL hash - a dev and a release build of one host must connect online.
    PSE_CHECK_STR(probe->LocalHash("test.host"), pse::compat::kBuiltIn);
    PSE_CHECK_STR(probe->LocalHash("test.host.sub"), pse::compat::kBuiltIn);
    PSE_CHECK_EQ(probe->ResultCount(), static_cast<uint32_t>(pse::PluginResults().size()));
    PSE_CHECK_EQ(probe->InOrderCount(), static_cast<uint32_t>(pse::detail::PluginsInOrder().size()));
    PSE_CHECK_STR(probe->SettingsKey(), pse::PluginSettingsKey(host).c_str());
    // The loader side agrees.
    for (const pse::compat::PluginEntry& e : pse::compat::LocalPlugins())
        if (e.name == "test.host") PSE_CHECK_STR(e.hash.c_str(), pse::compat::kBuiltIn);
    // pse.host is never reachable through the interface registry, nor for other plugins.
    PSE_CHECK(!pse::MessagingInterface()->QueryInterface(host, PSE_HOST_INTERFACE, 0));
    const PSELoadInterface* li = pse::LoadInterfaceFor(pse::FindPlugin("pse"));
    PSE_CHECK(!li->Query(li->plugin, PSE_HOST_INTERFACE, 0));
    PSE_CHECK(pse::LoadInterfaceFor(host)->Query(host, PSE_HOST_INTERFACE, 0) == pse::HostInterface());
    DeleteTestKey();
}

PSE_TEST("layout: host = true needs the game layer's allow list") {
    RequireHostPlugin();
    Fresh();
    Tree t("notallowed");
    t.Plugin(PSE_PLUGIN_DIR, "test.host", "Plugins");
    PSE_CHECK_EQ(pse::LoadPlugins(t.Wide(t.dir + "\\Plugins"), {"dsfix"}), 0u);
    const auto results = pse::PluginResults();
    const pse::PluginLoadResult* r = Result(results, "test.host");
    PSE_REQUIRE(r);
    PSE_CHECK(r->state == pse::PluginState::Refused);
    PSE_CHECK(r->reason.find("doesn't allow test.host as a host plugin") != std::string::npos);
    PSE_CHECK(!pse::FindPlugin("test.host.sub"));
    DeleteTestKey();
}

PSE_TEST("layout: no host = true -> no pse.host (an ordinary plugin)") {
    RequireHostPlugin();
    Fresh();
    Tree t("nohostkey");
    t.Plugin(PSE_PLUGIN_DIR, "test.host", "Plugins", "name = test.host\n");
    PSE_CHECK_EQ(pse::LoadPlugins(t.Wide(t.dir + "\\Plugins"), {"test.host"}), 0u);
    const auto results = pse::PluginResults();
    const pse::PluginLoadResult* r = Result(results, "test.host");
    PSE_REQUIRE(r);
    PSE_CHECK(r->state == pse::PluginState::Failed);   // its Load returns 0 without pse.host
    PSE_CHECK(AnyLineContains("no pse.host for test.host"));
    DeleteTestKey();
}

PSE_TEST("layout: other plugins load after the host and may require its sub-plugins") {
    RequireHostPlugin();
    if (!Exists(std::string(PSE_PLUGIN_DIR) + "\\example.hello.dll")) PSE_SKIP("example plugins not built");
    Fresh();
    Tree t("rounds");
    // "a.*" sorts before "test.host": only the host round puts the host first.
    t.Plugin(PSE_PLUGIN_DIR, "example.hello", "Plugins", "name = example.hello\nrequires = test.host.sub >= 1.0\n");
    t.Plugin(PSE_PLUGIN_DIR, "test.host", "Plugins");
    PSE_CHECK_EQ(pse::LoadPlugins(t.Wide(t.dir + "\\Plugins"), {"test.host"}), 2u);
    const auto results = pse::PluginResults();
    PSE_REQUIRE(results.size() == 2);
    PSE_CHECK_STR(results[0].name.c_str(), "test.host");
    PSE_CHECK_STR(results[1].name.c_str(), "example.hello");
    PSE_CHECK(results[1].state == pse::PluginState::Loaded);
    DeleteTestKey();
}

PSE_TEST("layout: a failed host takes its sub-plugins with it, its DLL stays mapped") {
    RequireHostPlugin();
    Fresh();
    Tree t("hostfail");
    t.Plugin(PSE_PLUGIN_DIR, "test.host", "Plugins");
    EnvVar fail("TEST_HOST_FAIL", "1");
    PSE_CHECK_EQ(pse::LoadPlugins(t.Wide(t.dir + "\\Plugins"), {"test.host"}), 0u);
    PSE_CHECK(!pse::FindPlugin("test.host"));
    PSE_CHECK(!pse::FindPlugin("test.host.sub"));   // retired with its parent
    PSE_CHECK(GetModuleHandleA("test.host.dll") != nullptr);   // never freed (raw patches may point into it)
    PSE_CHECK(AnyLineContains("host plugin DLL kept in memory"));
    Fresh();   // forget it, then free the copy by hand (the loader never does), so later tests map a fresh one
    if (HMODULE leaked = GetModuleHandleA("test.host.dll")) FreeLibrary(leaked);
    DeleteTestKey();
}

PSE_TEST("layout: Unload of a host plugin keeps it registered and mapped") {
    RequireHostPlugin();
    Fresh();
    Tree t("hostunload");
    t.Plugin(PSE_PLUGIN_DIR, "test.host", "Plugins");
    PSE_REQUIRE(pse::LoadPlugins(t.Wide(t.dir + "\\Plugins"), {"test.host"}) == 1u);
    pse::HostInterface()->Shutdown();   // what DesertStormFix's WM_DESTROY does
    PSE_CHECK(pse::FindPlugin("test.host") != 0);
    PSE_CHECK(pse::FindPlugin("test.host.sub") != 0);
    PSE_CHECK(GetModuleHandleA("test.host.dll") != nullptr);
    pse::HostInterface()->Shutdown();   // once only
    DeleteTestKey();
}

// ---- the DS1 game layer ---------------------------------------------------------------------------------------------

PSE_TEST("layout: pse_ds1.dll detects exactly the known DesertStorm.exe") {
    HMODULE m = LoadLibraryA(PSE_GAME_LAYER_DLL);
    PSE_REQUIRE(m);
    auto info = reinterpret_cast<const PSEGameLayerInfo*>(GetProcAddress(m, "PSEGame_Info"));
    auto detect = reinterpret_cast<PSEGameDetectFn>(GetProcAddress(m, "PSEGame_Detect"));
    PSE_REQUIRE(info && detect && GetProcAddress(m, "PSEGame_Init"));
    PSE_CHECK_STR(info->gameId, "ds1");
    PSE_CHECK_STR(info->plugin->name, "pse.game.ds1");
    PSE_REQUIRE(info->hostPlugins && info->hostPlugins[0]);
    PSE_CHECK_STR(info->hostPlugins[0], "dsfix");
    PSE_CHECK(info->hostPlugins[1] == nullptr);
    const char* known = "D712F5EC82C14DA66B098332B3CC9C99689091F46317112EA35DFD393DC3E164";
    PSEGameDetectInfo d{sizeof d, known, 0x400000, 0, L"DesertStorm.exe"};
    PSE_CHECK_EQ(detect(&d), 1);
    std::string lower(known);
    for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    d.exeSha256 = lower.c_str();
    PSE_CHECK_EQ(detect(&d), 1);   // case doesn't matter
    d.exeSha256 = "0000000000000000000000000000000000000000000000000000000000000000";
    PSE_CHECK_EQ(detect(&d), 0);
    d.exeSha256 = known;
    d.imageBase = 0x10000000;   // relocated - not the image DesertStormFix's addresses are for
    PSE_CHECK_EQ(detect(&d), 0);
    d.exeSha256 = "";
    PSE_CHECK_EQ(detect(&d), 0);
    FreeLibrary(m);
}

PSE_TEST("layout image: the mapped exe hashes to what the DS1 layer detects") {
    PSE_REQUIRE_EXE();
    const std::wstring path(exe::Path().begin(), exe::Path().end());
    const std::string sha = pse::ExeSha256(path);
    PSE_CHECK_STR(sha.c_str(), "D712F5EC82C14DA66B098332B3CC9C99689091F46317112EA35DFD393DC3E164");
    HMODULE m = LoadLibraryA(PSE_GAME_LAYER_DLL);
    PSE_REQUIRE(m);
    auto detect = reinterpret_cast<PSEGameDetectFn>(GetProcAddress(m, "PSEGame_Detect"));
    // The PE header of the image mapped at 0x400000 (exe_image maps the headers too).
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(static_cast<uintptr_t>(exe::kImageBase));
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(static_cast<uintptr_t>(exe::kImageBase) + dos->e_lfanew);
    PSEGameDetectInfo d{sizeof d, sha.c_str(), exe::kImageBase, nt->FileHeader.TimeDateStamp, path.c_str()};
    PSE_CHECK_EQ(detect(&d), 1);
    FreeLibrary(m);
    PSE_CHECK_STR(pse::ExeSha256(L"C:\\no\\such\\file.exe").c_str(), "");
}

// ---- start-up sequence (pse::Start = PSELoader_Start) ---------------------------------------------------------------

namespace {
std::vector<std::string> g_fallback;
void Fallback(const char* line) { g_fallback.emplace_back(line); }
bool FallbackHas(const char* text) {
    for (const std::string& l : g_fallback)
        if (l.find(text) != std::string::npos) return true;
    return false;
}
std::vector<uint32_t> g_msgs;
}  // namespace

PSE_TEST("layout image: start-up - detect, game layer, host first, then the rest, log hand-over") {
    PSE_REQUIRE_EXE();
    if (!Exists(std::string(PSE_FAKE_DSFIX_DIR) + "\\dsfix.dll")) PSE_SKIP("the stand-in dsfix.dll is not built");
    pse::ResetForTests();
    g_fallback.clear();
    Tree t("start");
    const std::string games = t.Sub("Games");
    PSE_REQUIRE(CopyFileA(PSE_GAME_LAYER_DLL, (games + "\\pse_ds1.dll").c_str(), FALSE));
    t.Plugin(PSE_FAKE_DSFIX_DIR, "dsfix", "Plugins");
    const bool hello = Exists(std::string(PSE_PLUGIN_DIR) + "\\example.hello.dll");
    if (hello) t.Plugin(PSE_PLUGIN_DIR, "example.hello", "Plugins");

    pse::StartOptions o;
    o.pseDir = t.Wide(t.dir);
    o.exePath = std::wstring(exe::Path().begin(), exe::Path().end());
    o.imageBase = exe::kImageBase;
    o.registryRoot = TestRegistryRoot();
    o.fallbackLog = &Fallback;
    const pse::StartResult r = pse::Start(o);
    PSE_REQUIRE(r.running);
    PSE_CHECK_STR(r.gameId.c_str(), "ds1");
    PSE_CHECK_STR(r.exeSha256.c_str(), "D712F5EC82C14DA66B098332B3CC9C99689091F46317112EA35DFD393DC3E164");
    PSE_CHECK_EQ(r.pluginsLoaded, hello ? 2u : 1u);
    // The game layer: built-in plugin + host privileges, its public interface registered.
    const PSEPluginHandle layer = pse::FindPlugin("pse.game.ds1");
    PSE_REQUIRE(layer);
    PSE_CHECK(!pse::PluginExternal(layer) && pse::PluginHost(layer));
    PSE_CHECK(pse::MessagingInterface()->QueryInterface(layer, "pse.game.ds1", 1) != nullptr);
    PSE_CHECK(pse::MessagingInterface()->QueryInterface(layer, "pse.game.ds1.host", 0) != nullptr);
    // ... and its event hooks (pse/games/ds1/ds1_events.cpp) on the mapped exe, owned by it: frame call, Level_Init
    // call, Save_Game entry, Direct3DCreate8 import slot (all eight: test_ds1_layer_events.cpp).
    {
        const PSEHooksInterface* h = pse::HooksInterface();
        std::vector<PSEHookInfo> list(h->ListHooks(nullptr, 0));
        for (auto& i : list) i.size = sizeof i;
        list.resize(h->ListHooks(list.data(), static_cast<uint32_t>(list.size())));
        for (uint32_t va : {0x4116ECu, 0x40E05Du, 0x415650u, 0x5D8308u}) {
            bool found = false;
            for (const auto& i : list) found = found || (i.va == va && i.owner == layer);
            PSE_CHECK(found);
        }
    }
    // The host: loaded first, external + host, its sub-plugin internal; the settings key under the game id.
    const auto results = pse::PluginResults();
    PSE_REQUIRE(!results.empty());
    PSE_CHECK_STR(results[0].name.c_str(), "dsfix");
    const PSEPluginHandle dsfix = pse::FindPlugin("dsfix");
    PSE_REQUIRE(dsfix);
    PSE_CHECK(pse::PluginExternal(dsfix) && pse::PluginHost(dsfix));
    PSE_CHECK(pse::FindPlugin("dsfix.sub") && !pse::PluginExternal(pse::FindPlugin("dsfix.sub")));
    if (hello) PSE_CHECK(pse::FindPlugin("example.hello") && !pse::PluginHost(pse::FindPlugin("example.hello")));
    // Log routing: the lines from before the host set its sink (exe hash, layer detection) were handed to it.
    const Probe* probe = GetProbe();
    PSE_REQUIRE(probe);
    PSE_CHECK_EQ(probe->SinkHas("start-up: game layer pse_ds1.dll recognises the exe: game ds1"), 1);
    PSE_CHECK_EQ(probe->SinkHas("SHA-256 D712F5EC"), 1);
    PSE_CHECK_EQ(probe->SinkHas("start-up: done in"), 1);   // after the host's Load: straight to its sink
    PSE_CHECK(FallbackHas("recognises the exe"));            // the fallback saw them too (before the sink)
    PSE_CHECK(!FallbackHas("start-up: done in"));
    // Once per process.
    PSE_CHECK(!pse::Start(o).running);
    pse::ResetForTests();
    exe::Restore();   // the layer's hooks are in the image (the broker forgot them, the bytes stay)
    DeleteTestKey();
}

PSE_TEST("layout: start-up - unknown exe or no game layer = vanilla, nothing loaded") {
    pse::ResetForTests();
    g_fallback.clear();
    Tree t("vanilla");
    t.Sub("Plugins");
    pse::StartOptions o;
    o.pseDir = t.Wide(t.dir);
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    o.exePath = self;   // pse_tests.exe: no layer knows it
    o.imageBase = exe::kImageBase;
    o.registryRoot = TestRegistryRoot();
    o.fallbackLog = &Fallback;
    pse::StartResult r = pse::Start(o);
    PSE_CHECK(!r.running);
    PSE_CHECK(r.why.find("no game layer") != std::string::npos);
    PSE_CHECK_EQ(pse::PluginCount(), 0u);   // pse::Init never ran
    PSE_CHECK(FallbackHas("the game runs without PSE plugins"));

    pse::ResetForTests();
    g_fallback.clear();
    const std::string games = t.Sub("Games");
    PSE_REQUIRE(CopyFileA(PSE_GAME_LAYER_DLL, (games + "\\pse_ds1.dll").c_str(), FALSE));
    r = pse::Start(o);
    PSE_CHECK(!r.running);
    PSE_CHECK(r.why.find("no game layer recognises this exe") != std::string::npos);
    PSE_CHECK(FallbackHas("pse_ds1.dll (ds1) doesn't recognise this exe"));
    PSE_CHECK_EQ(pse::PluginCount(), 0u);
    PSE_CHECK(GetModuleHandleA("pse_ds1.dll") == nullptr);   // a layer that said no is freed again
    pse::ResetForTests();
    DeleteTestKey();
    (void)g_msgs;
}
