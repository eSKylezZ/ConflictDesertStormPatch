// External plugins (pse/loader/plugins.cpp): manifest parser, dependency resolution, and loading the example DLLs
// (pse/examples, built into PSE_PLUGIN_DIR by CMake before this exe).
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "loader.h"
#include "loader_fixture.h"
#include "pse_test.h"

using namespace pse_fixture;

namespace {

// ---- helpers -------------------------------------------------------------------------------------------------------

pse::PluginManifest Parse(const char* text) {
    pse::PluginManifest m;
    std::string error;
    bool ok = pse::ParseManifest(text, &m, &error);
    if (!ok) m.name = "!" + error;   // marks a failed parse for the checks below
    return m;
}

std::string ParseError(const char* text) {
    pse::PluginManifest m;
    std::string error;
    return pse::ParseManifest(text, &m, &error) ? std::string("(parsed)") : error;
}

pse::PluginCandidate Cand(const char* manifest, uint32_t version = PSE_MAKE_VERSION(1, 0, 0)) {
    pse::PluginCandidate c;
    std::string error;
    pse::ParseManifest(manifest, &c.manifest, &error);
    c.version = version;
    return c;
}

std::vector<std::string> Names(const std::vector<pse::PluginCandidate>& c, const std::vector<size_t>& order) {
    std::vector<std::string> out;
    for (size_t i : order) out.push_back(c[i].manifest.name);
    return out;
}

std::string Joined(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : " ") + x;
    return s;
}

std::string PluginDir() { return PSE_PLUGIN_DIR; }

bool Exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// A fresh folder with copies of the named example plugins (DLL + manifest; manifest text replaceable).
struct Scratch {
    std::string dir;
    explicit Scratch(const char* tag) {
        dir = TempPath((std::string("plugins-") + tag).c_str());
        Clear();
        CreateDirectoryA(dir.c_str(), nullptr);
    }
    ~Scratch() { Clear(); }
    void Clear() {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileA((dir + "\\" + fd.cFileName).c_str());
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryA(dir.c_str());
    }
    void Copy(const char* stem, const char* manifestText = nullptr) {
        std::string src = PluginDir() + "\\" + stem;
        CopyFileA((src + ".dll").c_str(), (dir + "\\" + stem + ".dll").c_str(), FALSE);
        if (manifestText) Write(std::string(stem) + ".pse", manifestText);
        else CopyFileA((src + ".pse").c_str(), (dir + "\\" + stem + ".pse").c_str(), FALSE);
    }
    void Write(const std::string& file, const std::string& text) {
        HANDLE h = CreateFileA((dir + "\\" + file).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD n = 0;
        WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
        CloseHandle(h);
    }
    std::wstring Wide() const { return std::wstring(dir.begin(), dir.end()); }
};

void RequireExamples() {
    if (!Exists(PluginDir() + "\\example.hello.dll") || !Exists(PluginDir() + "\\example.hello.pse"))
        PSE_SKIP("example plugins not built in " + PluginDir());
}

const pse::PluginLoadResult* Result(const std::vector<pse::PluginLoadResult>& all, const char* name) {
    for (const auto& r : all)
        if (r.name == name) return &r;
    return nullptr;
}

size_t LineIndex(const char* text) {
    std::vector<std::string> lines = Lines();
    for (size_t i = 0; i < lines.size(); ++i)
        if (lines[i].find(text) != std::string::npos) return i;
    return SIZE_MAX;
}

size_t CountLines(const char* text) {
    size_t n = 0;
    for (const std::string& l : Lines())
        if (l.find(text) != std::string::npos) ++n;
    return n;
}

}  // namespace

// ---- manifest ------------------------------------------------------------------------------------------------------

PSE_TEST("plugins: manifest - all keys, comments, whitespace") {
    pse::PluginManifest m = Parse(
        "\xEF\xBB\xBF# a comment\r\n"
        "name       = dsfix.online   # trailing comment\r\n"
        "\r\n"
        "requires   = dsfix.splitscreen >= 1.0, pse.game.ds1>=1.2.3 ,plain\n"
        "optional   = dsfix.discord\n"
        "conflicts  = someone.otheronline, x\n"
        "after      = dsfix.splitscreen\n"
        "Requires   = more\n");   // keys are case-insensitive, repeated list keys add up
    PSE_CHECK_STR(m.name.c_str(), "dsfix.online");
    PSE_REQUIRE(m.required.size() == 4);
    PSE_CHECK_STR(m.required[0].name.c_str(), "dsfix.splitscreen");
    PSE_CHECK_EQ(m.required[0].minVersion, PSE_MAKE_VERSION(1, 0, 0));
    PSE_CHECK_STR(m.required[1].name.c_str(), "pse.game.ds1");
    PSE_CHECK_EQ(m.required[1].minVersion, PSE_MAKE_VERSION(1, 2, 3));
    PSE_CHECK_STR(m.required[2].name.c_str(), "plain");
    PSE_CHECK_EQ(m.required[2].minVersion, 0u);
    PSE_CHECK_STR(m.required[3].name.c_str(), "more");
    PSE_CHECK_EQ(m.optional.size(), 1u);
    PSE_CHECK_EQ(m.conflicts.size(), 2u);
    PSE_CHECK_EQ(m.after.size(), 1u);
    PSE_CHECK(m.warnings.empty());
}

PSE_TEST("plugins: manifest - errors and warnings") {
    PSE_CHECK(ParseError("").find("no \"name") != std::string::npos);
    PSE_CHECK(ParseError("# only a comment\n").find("no \"name") != std::string::npos);
    PSE_CHECK(ParseError("name = a\nname = b\n").find("line 2: name is given twice") != std::string::npos);
    PSE_CHECK(ParseError("name = Upper.Case\n").find("not a valid plugin name") != std::string::npos);
    PSE_CHECK(ParseError("name = has space\n").find("not a valid plugin name") != std::string::npos);
    PSE_CHECK(ParseError("name = a\njust text\n").find("line 2: expected") != std::string::npos);
    PSE_CHECK(ParseError("name = a\nrequires = b > 1.0\n").find("only \"name >= version\"") != std::string::npos);
    PSE_CHECK(ParseError("name = a\nrequires = b <= 1.0\n").find("only") != std::string::npos);
    PSE_CHECK(ParseError("name = a\nrequires = b >= 1.x\n").find("bad version") != std::string::npos);
    PSE_CHECK(ParseError("name = a\nrequires = b >= 256.0\n").find("bad version") != std::string::npos);
    PSE_CHECK(ParseError("name = a\nafter = B\n").find("line 2") != std::string::npos);
    std::string longName(65, 'a');
    PSE_CHECK(ParseError(("name = " + longName + "\n").c_str()).find("not a valid") != std::string::npos);

    pse::PluginManifest m = Parse("name = a\nversion = 3\nrequires = ,b,\n");
    PSE_CHECK_STR(m.name.c_str(), "a");
    PSE_REQUIRE(m.warnings.size() == 1);
    PSE_CHECK(m.warnings[0].find("unknown key \"version\"") != std::string::npos);
    PSE_CHECK_EQ(m.required.size(), 1u);   // empty list items are skipped
}

PSE_TEST("plugins: versions") {
    uint32_t v = 0;
    PSE_CHECK(pse::ParseVersion("1", &v) && v == PSE_MAKE_VERSION(1, 0, 0));
    PSE_CHECK(pse::ParseVersion("1.2", &v) && v == PSE_MAKE_VERSION(1, 2, 0));
    PSE_CHECK(pse::ParseVersion(" 255.4095.4095 ", &v) && v == PSE_MAKE_VERSION(255, 4095, 4095));
    PSE_CHECK(!pse::ParseVersion("", &v));
    PSE_CHECK(!pse::ParseVersion("1.", &v));
    PSE_CHECK(!pse::ParseVersion("1.2.3.4", &v));
    PSE_CHECK(!pse::ParseVersion("1.4096", &v));
    PSE_CHECK(!pse::ParseVersion("-1", &v));
    PSE_CHECK_STR(pse::VersionString(PSE_MAKE_VERSION(3, 10, 2)).c_str(), "3.10.2");
}

// ---- resolution ----------------------------------------------------------------------------------------------------

PSE_TEST("plugins: resolve - order by dependencies, ties by name") {
    Fresh();
    std::vector<pse::PluginCandidate> c = {
        Cand("name = zeta\n"),
        Cand("name = client\nrequires = lib >= 1.0\n"),
        Cand("name = lib\n"),
        Cand("name = alpha\nafter = zeta\n"),        // after: ordering only
        Cand("name = opt\noptional = client, absent\n"),  // optional: after it when present, fine when absent
    };
    std::vector<size_t> order = pse::ResolvePluginOrder(c);
    PSE_CHECK_STR(Joined(Names(c, order)).c_str(), "lib client opt zeta alpha");
    for (const auto& x : c) PSE_CHECK(x.refusal.empty());
}

PSE_TEST("plugins: resolve - missing requirement, version too low, cascade") {
    Fresh();
    std::vector<pse::PluginCandidate> c = {
        Cand("name = a\nrequires = missing\n"),
        Cand("name = b\nrequires = lib >= 2.1\n"),
        Cand("name = lib\n", PSE_MAKE_VERSION(2, 0, 5)),
        Cand("name = c\nrequires = a\n"),              // a is refused -> c too
        Cand("name = d\nrequires = d\n"),
    };
    std::vector<size_t> order = pse::ResolvePluginOrder(c);
    PSE_CHECK_STR(Joined(Names(c, order)).c_str(), "lib");
    PSE_CHECK(c[0].refusal.find("requires missing, which is not installed") != std::string::npos);
    PSE_CHECK(c[1].refusal.find("requires lib >= 2.1.0, found lib 2.0.5") != std::string::npos);
    PSE_CHECK(c[3].refusal.find("requires a, which is not loaded") != std::string::npos);
    PSE_CHECK(c[4].refusal.find("requires itself") != std::string::npos);
}

PSE_TEST("plugins: resolve - conflicts") {
    Fresh();
    std::vector<pse::PluginCandidate> c = {
        Cand("name = one\nconflicts = two\n"),   // one-way: only the plugin that names the other is refused
        Cand("name = two\n"),
        Cand("name = x\nconflicts = y\n"),       // both ways: both refused
        Cand("name = y\nconflicts = x\n"),
        Cand("name = z\nconflicts = nothere, z\n"),   // absent / itself: no conflict
        Cand("name = needsone\nrequires = one\n"),
    };
    std::vector<size_t> order = pse::ResolvePluginOrder(c);
    PSE_CHECK_STR(Joined(Names(c, order)).c_str(), "two z");
    PSE_CHECK(c[0].refusal.find("conflicts with two") != std::string::npos);
    PSE_CHECK(c[2].refusal.find("conflicts with y") != std::string::npos);
    PSE_CHECK(c[3].refusal.find("conflicts with x") != std::string::npos);
    PSE_CHECK(c[5].refusal.find("requires one") != std::string::npos);
}

PSE_TEST("plugins: resolve - cycles") {
    Fresh();
    std::vector<pse::PluginCandidate> c = {
        Cand("name = a\nrequires = b\n"),
        Cand("name = b\nafter = c\n"),
        Cand("name = c\nrequires = a\n"),       // a -> b -> c -> a
        Cand("name = user\nrequires = c\n"),    // requires a cycle member: refused
        Cand("name = later\nafter = a\n"),      // only ordered after one: still loads
        Cand("name = free\n"),
    };
    std::vector<size_t> order = pse::ResolvePluginOrder(c);
    PSE_CHECK_STR(Joined(Names(c, order)).c_str(), "free later");
    PSE_CHECK(c[0].refusal.find("dependency cycle: a -> b -> c -> a") != std::string::npos);
    PSE_CHECK(c[1].refusal.find("dependency cycle") != std::string::npos);
    PSE_CHECK(c[2].refusal.find("dependency cycle") != std::string::npos);
    PSE_CHECK(c[3].refusal.find("requires c") != std::string::npos);
}

PSE_TEST("plugins: resolve - built-in plugins, duplicates, pre-refused") {
    Fresh();
    AddPlugin("dsfix", PSE_MAKE_VERSION(3, 3, 0));
    std::vector<pse::PluginCandidate> c = {
        Cand("name = ok\nrequires = dsfix >= 3.0, pse\nconflicts = nobody\n"),
        Cand("name = toonew\nrequires = dsfix >= 4\n"),
        Cand("name = dsfix\n"),
        Cand("name = twin\n"),
        Cand("name = twin\n"),
        Cand("name = conflictsbuiltin\nconflicts = dsfix\n"),
        Cand("name = pre\n"),
    };
    c[6].refusal = "already refused";
    std::vector<size_t> order = pse::ResolvePluginOrder(c);
    PSE_CHECK_STR(Joined(Names(c, order)).c_str(), "ok");
    PSE_CHECK(c[1].refusal.find("found dsfix 3.3.0") != std::string::npos);
    PSE_CHECK(c[2].refusal.find("already loaded (built in)") != std::string::npos);
    PSE_CHECK(c[3].refusal.find("2 plugins are named twin") != std::string::npos);
    PSE_CHECK(c[4].refusal.find("2 plugins are named twin") != std::string::npos);
    PSE_CHECK(c[5].refusal.find("conflicts with dsfix (built in)") != std::string::npos);
    PSE_CHECK_STR(c[6].refusal.c_str(), "already refused");
}

// ---- loading the example DLLs --------------------------------------------------------------------------------------

PSE_TEST("plugins: missing or empty folder is fine, PostLoad still sent") {
    Fresh();
    PSEPluginHandle me = AddPlugin("watcher");
    static std::vector<uint32_t> got;
    got.clear();
    pse::MessagingInterface()->RegisterListener(me, "pse", [](const PSEMessage* m, void*) { got.push_back(m->type); },
                                                nullptr);
    PSE_CHECK_EQ(pse::LoadPlugins(L"C:\\no\\such\\folder\\PSE\\Plugins"), 0u);
    PSE_CHECK(pse::PluginResults().empty());
    PSE_REQUIRE(got.size() == 2);
    PSE_CHECK_EQ(got[0], static_cast<uint32_t>(PSE_MSG_POST_LOAD));
    PSE_CHECK_EQ(got[1], static_cast<uint32_t>(PSE_MSG_POST_POST_LOAD));
    PSE_CHECK_EQ(pse::LoadPlugins(L""), 0u);   // second call: ignored
    PSE_CHECK(AnyLineContains("called twice"));
    pse::UnloadPlugins();   // nothing loaded: nothing happens
    Fresh();
    PSE_CHECK_EQ(pse::LoadPlugins(L""), 0u);   // empty path = no folder
}

PSE_TEST("plugins: hello + hello_client load, interface, frames, settings, unload order") {
    RequireExamples();
    Fresh();
    Scratch s("ok");
    s.Copy("example.hello");
    s.Copy("example.hello_client");

    PSE_CHECK_EQ(pse::LoadPlugins(s.Wide()), 2u);
    std::vector<pse::PluginLoadResult> res = pse::PluginResults();
    PSE_REQUIRE(res.size() == 2);
    PSE_CHECK_STR(res[0].name.c_str(), "example.hello");   // dependency first
    PSE_CHECK_STR(res[1].name.c_str(), "example.hello_client");
    for (const auto& r : res) {
        PSE_CHECK(r.state == pse::PluginState::Loaded);
        PSE_CHECK_STR(r.reason.c_str(), "");
        PSE_CHECK_EQ(r.version, PSE_MAKE_VERSION(1, 0, 0));
        PSE_CHECK(r.handle != 0);
        PSE_CHECK(r.file.find(".dll") != std::string::npos);
    }
    PSE_CHECK_STR(res[0].displayName.c_str(), "Hello (example plugin)");

    // Registered as external plugins with copied strings.
    PSEPluginHandle hello = pse::FindPlugin("example.hello");
    PSE_REQUIRE(hello != 0);
    PSE_CHECK(pse::PluginExternal(hello));
    const PSEPluginInfo* info = pse::PluginInfo(hello);
    PSE_REQUIRE(info);
    PSE_CHECK(!(info->flags & PSE_PLUGIN_INTERNAL));
    PSE_REQUIRE(info->games && info->games[0]);
    PSE_CHECK_STR(info->games[0], "ds1");
    PSE_REQUIRE(info->games[1]);
    PSE_CHECK_STR(info->games[1], "ds2");   // the same DLL runs on Desert Storm II
    PSE_CHECK(info->games[2] == nullptr);

    // hello's interface, called from here.
    struct HelloV1 {
        uint32_t size;
        int(PSE_CALL* Add)(int, int);
    };
    PSEPluginHandle me = AddPlugin("tester");
    auto h = static_cast<const HelloV1*>(pse::MessagingInterface()->QueryInterface(me, "example.hello", 1));
    PSE_REQUIRE(h);
    PSE_CHECK_EQ(h->size, static_cast<uint32_t>(sizeof(HelloV1)));
    PSE_CHECK_EQ(h->Add(40, 2), 42);
    PSE_CHECK(pse::MessagingInterface()->QueryInterface(me, "example.hello", 2) == nullptr);

    // hello_client asked hello in PostLoad.
    struct ClientV1 {
        uint32_t size;
        int(PSE_CALL* Answer)(void);
    };
    auto cl = static_cast<const ClientV1*>(pse::MessagingInterface()->QueryInterface(me, "example.hello_client", 1));
    PSE_REQUIRE(cl);
    PSE_CHECK_EQ(cl->Answer(), 5);
    PSE_CHECK(AnyLineContains("example.hello says 2 + 3 = 5"));

    // The declared setting, and the frame subscription that reads it.
    const PSESettingsInterface* st = pse::SettingsInterface();
    PSE_CHECK_EQ(st->GetInt(hello, "LogEveryFrames"), 600);
    PSE_CHECK_EQ(st->SetInt(hello, "LogEveryFrames", 3), PSE_OK);
    for (int i = 0; i < 9; ++i) pse::Frame();
    PSE_CHECK_EQ(CountLines("hello: frame "), 3u);
    PSE_CHECK(AnyLineContains("hello: frame 9"));

    // Unload: dependents first, then their dependencies; DLLs freed, handles gone.
    pse::UnloadPlugins();
    size_t clientBye = LineIndex("hello_client: unloading"), helloBye = LineIndex("hello: unloading after 9 frames");
    PSE_CHECK(clientBye != SIZE_MAX && helloBye != SIZE_MAX);
    PSE_CHECK(clientBye < helloBye);
    size_t clientGone = LineIndex("unloaded example.hello_client"), helloGone = SIZE_MAX;
    std::vector<std::string> lines = Lines();
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string tail = "unloaded example.hello";
        if (lines[i].size() >= tail.size() && lines[i].compare(lines[i].size() - tail.size(), tail.size(), tail) == 0)
            helloGone = i;
    }
    PSE_CHECK(clientGone < helloGone && helloGone != SIZE_MAX);
    res = pse::PluginResults();
    for (const auto& r : res) PSE_CHECK(r.state == pse::PluginState::Unloaded);
    PSE_CHECK(pse::FindPlugin("example.hello") == 0);
    PSE_CHECK(pse::MessagingInterface()->QueryInterface(me, "example.hello", 1) == nullptr);
    PSE_CHECK(GetModuleHandleA("example.hello.dll") == nullptr);
    PSE_CHECK(GetModuleHandleA("example.hello_client.dll") == nullptr);
    pse::Frame();   // no subscription left pointing into a freed DLL
    DeleteTestKey();
}

PSE_TEST("plugins: bad_crash is disabled cleanly, the others load") {
    RequireExamples();
    Fresh();
    Scratch s("crash");
    s.Copy("example.hello");
    s.Copy("example.bad_crash");

    PSE_CHECK_EQ(pse::LoadPlugins(s.Wide()), 1u);   // the process is still alive to get here
    std::vector<pse::PluginLoadResult> res = pse::PluginResults();
    const pse::PluginLoadResult* bad = Result(res, "example.bad_crash");
    PSE_REQUIRE(bad);
    PSE_CHECK(bad->state == pse::PluginState::Failed);
    PSE_CHECK(bad->reason.find("crashed in PSEPlugin_Load: exception 0xC0000005 at example.bad_crash.dll+0x") !=
              std::string::npos);
    PSE_CHECK(bad->handle != 0);
    PSE_CHECK(AnyLineContains("bad_crash: about to crash"));   // it did run, and register things, first
    const pse::PluginLoadResult* hello = Result(res, "example.hello");
    PSE_REQUIRE(hello);
    PSE_CHECK(hello->state == pse::PluginState::Loaded);

    // Everything it registered is gone; its handle is dead and never handed out again.
    PSE_CHECK(pse::PluginDisabled(bad->handle));
    PSE_CHECK(pse::FindPlugin("example.bad_crash") == 0);
    PSE_CHECK_STR(pse::PluginName(bad->handle), "example.bad_crash");   // still named in logs
    std::vector<PSEHookInfo> hooks(pse::HooksInterface()->ListHooks(nullptr, 0) + 1);
    uint32_t n = pse::HooksInterface()->ListHooks(hooks.data(), static_cast<uint32_t>(hooks.size()));
    for (uint32_t i = 0; i < n && i < hooks.size(); ++i) PSE_CHECK(hooks[i].owner != bad->handle);
    PSE_CHECK(AnyLineContains("(site restored)"));   // its pointer hook was taken off
    PSE_CHECK(AnyLineContains("bad_crash: reverted"));  // and its byte group put back
    PSEPluginHandle me = AddPlugin("tester");
    PSE_CHECK(me > bad->handle);
    PSE_CHECK(pse::MessagingInterface()->QueryInterface(me, "example.bad_crash", 1) == nullptr);
    PSE_CHECK(pse::LoadInterfaceFor(bad->handle)->Query(bad->handle, PSE_LOG_INTERFACE, 1) == nullptr);
    PSE_CHECK(GetModuleHandleA("example.bad_crash.dll") == nullptr);   // freed
    for (int i = 0; i < 3; ++i) pse::Frame();                          // its frame subscription is gone

    pse::UnloadPlugins();
    res = pse::PluginResults();
    PSE_CHECK(Result(res, "example.bad_crash")->state == pse::PluginState::Failed);
    PSE_CHECK(Result(res, "example.hello")->state == pse::PluginState::Unloaded);
    DeleteTestKey();
}

PSE_TEST("plugins: refused before loading - missing dependency, version, name, game, no DLL") {
    RequireExamples();
    {
        Fresh();
        Scratch s("nodep");
        s.Copy("example.hello_client");   // hello missing
        PSE_CHECK_EQ(pse::LoadPlugins(s.Wide()), 0u);
        auto res = pse::PluginResults();
        PSE_REQUIRE(res.size() == 1);
        PSE_CHECK(res[0].state == pse::PluginState::Refused);
        PSE_CHECK(res[0].reason.find("requires example.hello >= 1.0.0, which is not installed") != std::string::npos);
        PSE_CHECK_EQ(res[0].handle, 0u);
        PSE_CHECK(GetModuleHandleA("example.hello_client.dll") == nullptr);   // never ran
    }
    {
        Fresh();
        Scratch s("version");
        s.Copy("example.hello");
        s.Copy("example.hello_client", "name = example.hello_client\nrequires = example.hello >= 1.5\n");
        PSE_CHECK_EQ(pse::LoadPlugins(s.Wide()), 1u);
        auto res = pse::PluginResults();
        const pse::PluginLoadResult* r = Result(res, "example.hello_client");
        PSE_REQUIRE(r);
        PSE_CHECK(r->reason.find("requires example.hello >= 1.5.0, found example.hello 1.0.0") != std::string::npos);
        pse::UnloadPlugins();
    }
    {
        Fresh();
        Scratch s("name");
        s.Copy("example.hello", "name = example.other\n");
        s.Write("lonely.pse", "name = lonely\n");   // no lonely.dll
        s.Write("broken.pse", "name = broken\nrequires = x >\n");
        PSE_CHECK_EQ(pse::LoadPlugins(s.Wide()), 0u);
        auto res = pse::PluginResults();
        PSE_REQUIRE(res.size() == 3);
        PSE_CHECK(Result(res, "example.other")->reason.find("PSEPlugin_Info.name is \"example.hello\"") != std::string::npos);
        PSE_CHECK(Result(res, "lonely")->reason.find("no lonely.dll next to the manifest") != std::string::npos);
        const pse::PluginLoadResult* broken = Result(res, "(broken.pse)");
        PSE_REQUIRE(broken);
        PSE_CHECK(broken->reason.find("broken.pse line 2") != std::string::npos);
        // Refused ones are listed by name.
        PSE_CHECK(res[0].name < res[1].name && res[1].name < res[2].name);
    }
    {
        Fresh();   // the game id must be in the plugin's list (hello: ds1 only)
        pse::Config cfg;
        cfg.gameId = "cv";
        cfg.registryRoot = TestRegistryRoot();
        cfg.logSink = &Sink;
        pse::ResetForTests();
        pse::Init(cfg);
        Scratch s("game");
        s.Copy("example.hello");
        s.Copy("example.hello_client");   // any game, but needs hello
        PSE_CHECK_EQ(pse::LoadPlugins(s.Wide()), 0u);
        auto res = pse::PluginResults();
        PSE_CHECK(Result(res, "example.hello")->reason.find("made for ds1, ds2; this game is cv") != std::string::npos);
        PSE_CHECK(Result(res, "example.hello_client")->reason.find("which is not loaded") != std::string::npos);
    }
    Fresh();
    DeleteTestKey();
}

// ---- crash guard: external plugins that fault AFTER their Load (pse/examples/crash_later) ----------------------------

namespace {

bool HookedBy(PSEPluginHandle plugin) {
    std::vector<PSEHookInfo> hooks(pse::HooksInterface()->ListHooks(nullptr, 0) + 1);
    uint32_t n = pse::HooksInterface()->ListHooks(hooks.data(), static_cast<uint32_t>(hooks.size()));
    for (uint32_t i = 0; i < n && i < hooks.size(); ++i)
        if (hooks[i].owner == plugin) return true;
    return false;
}

// Loads hello + one crash_later variant; returns the crash plugin's handle (still valid when it hasn't crashed yet).
PSEPluginHandle LoadWithHello(Scratch& s, const char* stem) {
    s.Copy("example.hello");
    s.Copy(stem);
    pse::LoadPlugins(s.Wide());
    for (const auto& r : pse::PluginResults())
        if (r.name == stem) return r.handle;
    return 0;
}

// The common "disabled cleanly" checks after a late crash.
void CheckCrashed(PSEPluginHandle h, const char* name, const char* where) {
    PSE_CHECK(pse::PluginDisabled(h));
    PSE_CHECK(pse::FindPlugin(name) == 0);
    PSE_CHECK(!HookedBy(h));   // its pointer hook and byte group were taken back
    std::vector<pse::PluginLoadResult> res = pse::PluginResults();
    const pse::PluginLoadResult* r = Result(res, name);
    PSE_REQUIRE(r);
    PSE_CHECK(r->state == pse::PluginState::Failed);
    PSE_CHECK(r->reason.find(std::string("crashed in ") + where) != std::string::npos);
    PSE_CHECK(r->reason.find("exception 0xC0000005 at ") != std::string::npos);
    PSE_CHECK(r->reason.find(std::string(name) + ".dll+0x") != std::string::npos);   // names the faulting module
    PSE_CHECK(AnyLineContains("- disabled; its hooks"));
    bool reported = false;
    {
        std::lock_guard lock(Cap().m);
        for (const auto& [line, error] : Cap().reports)
            if (error && line.find(name) != std::string::npos && line.find("crashed in") != std::string::npos)
                reported = true;
    }
    PSE_CHECK(reported);   // also in the mod report (reportSink)
    PSEPluginHandle me = AddPlugin("checker");
    PSE_CHECK(pse::MessagingInterface()->QueryInterface(me, name, 1) == nullptr);    // its interface is gone
    PSE_CHECK(GetModuleHandleA((std::string(name) + ".dll").c_str()) != nullptr);   // left mapped on purpose
}

}  // namespace

PSE_TEST("plugins: crash guard - PostLoad listener") {
    RequireExamples();
    Fresh();
    Scratch s("crash-postload");
    PSEPluginHandle h = LoadWithHello(s, "example.crash_postload");   // the crash happens inside LoadPlugins
    PSE_REQUIRE(h != 0);
    CheckCrashed(h, "example.crash_postload", "its PSE_MSG_POST_LOAD listener");
    PSE_CHECK(AnyLineContains("crash_later: crashing in PostLoad"));
    PSE_CHECK(!AnyLineContains("crash_later: PostPostLoad reached"));   // its later listener never ran
    PSE_CHECK(pse::FindPlugin("example.hello") != 0);                   // the others carry on
    for (int i = 0; i < 3; ++i) pse::Frame();
    PSE_CHECK(!AnyLineContains("second frame subscriber ran"));
    pse::UnloadPlugins();
    PSE_CHECK(AnyLineContains("example.crash_postload crashed earlier - not unloaded"));
    PSE_CHECK(!AnyLineContains("crash_later: unloading"));   // no Unload call into it
    PSE_CHECK(Result(pse::PluginResults(), "example.hello")->state == pse::PluginState::Unloaded);
    Fresh();   // ResetPlugins frees the DLL for the next test
    DeleteTestKey();
}

PSE_TEST("plugins: crash guard - frame event, the rest of the event still runs") {
    RequireExamples();
    Fresh();
    Scratch s("crash-frame");
    PSEPluginHandle h = LoadWithHello(s, "example.crash_frame");
    PSE_REQUIRE(h != 0);
    PSE_CHECK(!pse::PluginDisabled(h));   // loads fine
    PSE_CHECK(HookedBy(h));
    PSEPluginHandle hello = pse::FindPlugin("example.hello");
    PSE_CHECK_EQ(pse::SettingsInterface()->SetInt(hello, "LogEveryFrames", 1), PSE_OK);
    pse::Frame();   // crash_frame (priority 100) faults, hello (0) still gets the frame
    CheckCrashed(h, "example.crash_frame", "a pse.frame event subscriber");
    PSE_CHECK(AnyLineContains("hello: frame 1"));
    PSE_CHECK(!AnyLineContains("second frame subscriber ran"));   // its own later subscriber skipped
    pse::Frame();
    PSE_CHECK(AnyLineContains("hello: frame 2"));
    PSE_CHECK_EQ(CountLines("crashing in a frame event"), 1u);   // never called again
    Fresh();
    DeleteTestKey();
}

PSE_TEST("plugins: crash guard - task, its later task skipped") {
    RequireExamples();
    Fresh();
    Scratch s("crash-task");
    PSEPluginHandle h = LoadWithHello(s, "example.crash_task");
    PSE_REQUIRE(h != 0);
    pse::Frame();
    CheckCrashed(h, "example.crash_task", "a pse.tasks task");
    PSE_CHECK(AnyLineContains("crash_later: crashing in a task"));
    PSE_CHECK(!AnyLineContains("crash_later: second task ran"));
    pse::Frame();
    PSE_CHECK(!AnyLineContains("crash_later: second task ran"));
    Fresh();
    DeleteTestKey();
}

PSE_TEST("plugins: crash guard - settings, co-save, messages, nested event, unload") {
    RequireExamples();
    struct Case {
        const char* at;
        const char* where;
    };
    const Case cases[] = {
        {"setting", "a settings change callback (Value)"},
        {"save", "its co-save save callback"},
        {"load", "its co-save load callback"},
        {"revert", "its co-save revert callback"},
        {"message", "a message listener (type 0x4242 from tester)"},
        {"nested", "a example.crash.nested event subscriber"},
    };
    const std::string cosave = TempPath("crash.pse");
    for (const Case& c : cases) {
        SetEnvironmentVariableA("PSE_CRASH_AT", c.at);
        Fresh();
        Scratch s((std::string("crash-") + c.at).c_str());
        PSEPluginHandle h = LoadWithHello(s, "example.crash_callbacks");
        PSE_REQUIRE(h != 0);
        PSE_CHECK(!pse::PluginDisabled(h));
        PSEPluginHandle me = AddPlugin("tester");
        const std::string at = c.at;
        if (at == "setting") pse::SettingsInterface()->SetInt(h, "Value", 7);
        if (at == "save") PSE_CHECK(pse::cosave::Save(cosave.c_str()));   // the save itself still works
        if (at == "load") {
            PSE_CHECK(pse::cosave::Save(cosave.c_str()));
            PSE_CHECK(pse::cosave::Load(cosave.c_str()));
        }
        if (at == "revert") pse::cosave::NewGame();
        if (at == "message") pse::MessagingInterface()->Dispatch(me, 0x4242, nullptr, 0, nullptr);
        if (at == "nested") pse::Frame();
        CheckCrashed(h, "example.crash_callbacks", c.where);
        if (at == "nested") PSE_CHECK(AnyLineContains("outer frame callback carried on"));   // the outer call survived
        pse::Frame();   // nothing of it is called any more
        pse::UnloadPlugins();
        Fresh();
    }
    DeleteFileA(cosave.c_str());
    {   // a crash in PSEPlugin_Unload: logged + reported, the other plugins still unloaded
        SetEnvironmentVariableA("PSE_CRASH_AT", "unload");
        Fresh();
        Scratch s("crash-unload");
        PSEPluginHandle h = LoadWithHello(s, "example.crash_callbacks");
        PSE_REQUIRE(h != 0);
        pse::UnloadPlugins();
        PSE_CHECK(AnyLineContains("crashed in PSEPlugin_Unload: exception 0xC0000005"));
        auto res = pse::PluginResults();
        PSE_CHECK(Result(res, "example.hello")->state == pse::PluginState::Unloaded);
        PSE_CHECK(Result(res, "example.crash_callbacks")->state == pse::PluginState::Unloaded);
        PSE_CHECK(Result(res, "example.crash_callbacks")->reason.find("PSEPlugin_Unload") != std::string::npos);
    }
    SetEnvironmentVariableA("PSE_CRASH_AT", nullptr);
    Fresh();
    DeleteTestKey();
}

namespace {
int g_perfCalls = 0;
int PSE_CALL PerfCallback(PSEEventId, void*, void*) {
    ++g_perfCalls;
    return 0;
}
}  // namespace

PSE_TEST("plugins: crash guard - cost of PSE_EVENT_FRAME with 5 subscribers (internal direct vs external guarded)") {
    Fresh();
    static PSEPluginInfo ext[5];
    static std::string names[5];
    PSEPluginHandle internal[5], external[5];
    for (int i = 0; i < 5; ++i) {
        internal[i] = AddPlugin(("perf.int" + std::to_string(i)).c_str());
        names[i] = "perf.ext" + std::to_string(i);
        ext[i] = {sizeof(PSEPluginInfo), 1, names[i].c_str(), "", "", 1, 0, nullptr, PSE_PLUGIN_CLIENT_ONLY};
        external[i] = pse::RegisterExternalPlugin(&ext[i]);
        PSE_REQUIRE(external[i] != 0);
    }
    const PSEMessagingInterface* m = pse::MessagingInterface();
    const int n = 200000;
    auto measure = [&](PSEPluginHandle* owners) {
        std::vector<PSESubscriptionId> ids;
        for (int i = 0; i < 5; ++i) ids.push_back(m->Subscribe(owners[i], PSE_EVENT_FRAME, &PerfCallback, nullptr, 0));
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        double best = 1e9;
        for (int round = 0; round < 5; ++round) {   // best of 5 (other processes on the PC)
            QueryPerformanceCounter(&a);
            for (int i = 0; i < n; ++i) pse::RaiseEvent(PSE_EVENT_FRAME, nullptr);
            QueryPerformanceCounter(&b);
            best = std::min(best, double(b.QuadPart - a.QuadPart) * 1e9 / double(f.QuadPart) / n);
        }
        for (int i = 0; i < 5; ++i) m->Unsubscribe(owners[i], ids[i]);
        return best;
    };
    g_perfCalls = 0;
    const double direct = measure(internal);
    PSE_CHECK_EQ(g_perfCalls, 5 * n * 5);
    const double guarded = measure(external);
    PSE_CHECK_EQ(g_perfCalls, 2 * 5 * n * 5);
    std::printf("    PSE_EVENT_FRAME, 5 subscribers: internal (direct) %.1f ns, external (guarded) %.1f ns per Raise "
                "-> %.1f ns more per guarded call\n", direct, guarded, (guarded - direct) / 5);
    PSE_CHECK(guarded < direct + 5 * 200.0);   // generous: a __try is a few instructions
    DeleteTestKey();
}
