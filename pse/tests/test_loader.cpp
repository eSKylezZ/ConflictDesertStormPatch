// Plugin registry, PSELoadInterface / Query, pse.log.
#include <cstdio>
#include <thread>

#include "loader_fixture.h"

using namespace pse_fixture;

PSE_TEST("loader: the loader is plugin 'pse', handle 0 is invalid") {
    Fresh();
    PSEPluginHandle pse = pse::FindPlugin("pse");
    PSE_CHECK(pse != 0);
    PSE_CHECK_EQ(pse::PluginCount(), 1u);
    PSE_CHECK_STR(pse::PluginName(pse), "pse");
    PSE_CHECK_STR(pse::PluginName(0), "");
    PSE_CHECK_STR(pse::PluginName(99), "");
    PSE_CHECK(pse::PluginInfo(0) == nullptr);
    PSE_CHECK(pse::LoadInterfaceFor(0) == nullptr);
    PSE_CHECK(pse::FindPlugin(nullptr) == 0);
    PSE_CHECK(pse::FindPlugin("nobody") == 0);
    PSE_CHECK(pse::PluginInfo(pse)->flags & PSE_PLUGIN_INTERNAL);
}

PSE_TEST("loader: Init needs a game id - no default game") {
    pse::ResetForTests();
    pse::Config c;
    c.registryRoot = TestRegistryRoot();
    c.logSink = &Sink;
    PSE_CHECK(!pse::Init(c));   // none
    PSE_CHECK(pse::FindPlugin("pse") == 0);
    c.gameId = "DS1";
    PSE_CHECK(!pse::Init(c));   // not lower case
    c.gameId = "ds2";
    c.registryRoot.clear();
    PSE_REQUIRE(pse::Init(c));
    PSE_CHECK(pse::FindPlugin("pse") != 0);
    PSE_CHECK_EQ(pse::PluginSettingsKey(pse::FindPlugin("pse")), std::string("Software\\PSE\\ds2\\Plugins\\pse"));
    Fresh();
}

PSE_TEST("loader: register plugins, names checked, duplicates refused") {
    Fresh();
    PSEPluginHandle a = AddPlugin("dsfix.alpha");
    PSEPluginHandle b = AddPlugin("myname.beta-2_x");
    PSE_CHECK(a != 0 && b != 0 && a != b);
    PSE_CHECK_EQ(pse::FindPlugin("dsfix.alpha"), a);
    PSE_CHECK_EQ(pse::PluginCount(), 3u);
    PSE_CHECK_EQ(AddPlugin("dsfix.alpha"), 0u);   // duplicate
    PSE_CHECK_EQ(AddPlugin("Upper"), 0u);         // [a-z0-9_.-] only
    PSE_CHECK_EQ(AddPlugin(""), 0u);
    PSE_CHECK_EQ(AddPlugin("with space"), 0u);
    PSE_CHECK_EQ(pse::RegisterInternalPlugin(nullptr), 0u);
    PSEPluginInfo small = {8, 1, "small"};
    PSE_CHECK_EQ(pse::RegisterInternalPlugin(&small), 0u);   // size too small
    PSE_CHECK_EQ(pse::PluginCount(), 3u);
    const PSEPluginInfo* info = pse::PluginInfo(a);
    PSE_REQUIRE(info);
    PSE_CHECK(info->flags & PSE_PLUGIN_INTERNAL);
    PSE_CHECK(info->flags & PSE_PLUGIN_CLIENT_ONLY);
    PSE_CHECK_EQ(info->version, PSE_MAKE_VERSION(1, 0, 0));
}

PSE_TEST("loader: register before Init is refused; reset forgets plugins") {
    pse::ResetForTests();
    PSE_CHECK_EQ(AddPlugin("early"), 0u);
    Fresh();
    AddPlugin("one");
    Fresh();
    PSE_CHECK_EQ(pse::FindPlugin("one"), 0u);
    PSE_CHECK_EQ(pse::PluginCount(), 1u);
}

PSE_TEST("loader: load interface + Query for loader interfaces") {
    Fresh();
    PSEPluginHandle a = AddPlugin("q.plugin");
    const PSELoadInterface* li = pse::LoadInterfaceFor(a);
    PSE_REQUIRE(li);
    PSE_CHECK_EQ(li->size, (uint32_t)sizeof(PSELoadInterface));
    PSE_CHECK_EQ(li->plugin, a);
    PSE_CHECK_EQ(li->apiVersion, PSE_API_VERSION);
    PSE_CHECK_STR(li->gameId, "ds1");
    PSE_CHECK(li->Query(a, PSE_LOG_INTERFACE, 1) == pse::LogInterface());
    PSE_CHECK(li->Query(a, PSE_HOOKS_INTERFACE, 1) == pse::HooksInterface());
    PSE_CHECK(li->Query(a, PSE_MESSAGING_INTERFACE, 0) == pse::MessagingInterface());
    PSE_CHECK(li->Query(a, PSE_TASKS_INTERFACE, 1) == pse::TasksInterface());
    PSE_CHECK(li->Query(a, PSE_SETTINGS_INTERFACE, 1) == pse::SettingsInterface());
    PSE_CHECK(li->Query(a, PSE_SERIALIZATION_INTERFACE, 1) == pse::SerializationInterface());
    PSE_CHECK(li->Query(a, PSE_LOG_INTERFACE, 2) == nullptr);   // newer than we have
    PSE_CHECK(li->Query(a, "pse.nothing", 0) == nullptr);
    PSE_CHECK(li->Query(a, nullptr, 0) == nullptr);
    PSE_CHECK(li->Query(0, PSE_LOG_INTERFACE, 1) == nullptr);   // bad handle
}

PSE_TEST("loader: Query finds interfaces registered through messaging, by version") {
    Fresh();
    PSEPluginHandle a = AddPlugin("provider");
    PSEPluginHandle b = AddPlugin("consumer");
    static const int v1 = 1, v3 = 3;
    const PSEMessagingInterface* m = pse::MessagingInterface();
    PSE_CHECK_EQ(m->RegisterInterface(a, "provider.api", 1, &v1), PSE_OK);
    PSE_CHECK_EQ(m->RegisterInterface(a, "provider.api", 3, &v3), PSE_OK);
    const PSELoadInterface* li = pse::LoadInterfaceFor(b);
    PSE_CHECK(li->Query(b, "provider.api", 0) == &v3);
    PSE_CHECK(li->Query(b, "provider.api", 2) == &v3);
    PSE_CHECK(li->Query(b, "provider.api", 4) == nullptr);
}

PSE_TEST("log: line format, levels, unknown handle") {
    Fresh();
    PSEPluginHandle a = AddPlugin("logger");
    const PSELogInterface* log = pse::LogInterface();
    log->Write(a, PSE_LOG_INFO, "hello %d %s", 42, "world");
    log->Write(a, PSE_LOG_WARN, "w");
    log->Write(a, PSE_LOG_ERROR, "e");
    log->Write(a, PSE_LOG_DEBUG, "d");
    log->Write(0, PSE_LOG_INFO, "nobody");
    auto lines = Lines();
    PSE_REQUIRE(lines.size() == 5);
    // [mm:ss.mmm] [plugin] LEVEL text
    unsigned mm, ss, ms;
    char rest[256];
    PSE_CHECK_EQ(std::sscanf(lines[0].c_str(), "[%2u:%2u.%3u] %255[^\n]", &mm, &ss, &ms, rest), 4);
    PSE_CHECK_STR(rest, "[logger] INFO hello 42 world");
    PSE_CHECK(lines[0][3] == ':' && lines[0][6] == '.' && lines[0][10] == ']');
    PSE_CHECK(lines[1].find("[logger] WARN w") != std::string::npos);
    PSE_CHECK(lines[2].find("[logger] ERROR e") != std::string::npos);
    PSE_CHECK(lines[3].find("[logger] DEBUG d") != std::string::npos);
    PSE_CHECK(lines[4].find("[?] INFO nobody") != std::string::npos);
}

PSE_TEST("log: long lines are not cut") {
    Fresh();
    PSEPluginHandle a = AddPlugin("long");
    std::string big(5000, 'x');
    pse::LogInterface()->Write(a, PSE_LOG_INFO, "%s|end", big.c_str());
    auto lines = Lines();
    PSE_REQUIRE(lines.size() == 1);
    PSE_CHECK(lines[0].find(big + "|end") != std::string::npos);
}

PSE_TEST("log: Report goes to the log and the report sink") {
    Fresh();
    PSEPluginHandle a = AddPlugin("reporter");
    pse::LogInterface()->Report(a, 1, "bad file %s", "x.txt");
    pse::LogInterface()->Report(a, 0, "note");
    PSE_CHECK(AnyLineContains("[reporter] ERROR bad file x.txt"));
    PSE_CHECK(AnyLineContains("[reporter] INFO note"));
    std::lock_guard lock(Cap().m);
    PSE_REQUIRE(Cap().reports.size() == 2);
    PSE_CHECK_STR(Cap().reports[0].first.c_str(), "[reporter] bad file x.txt");
    PSE_CHECK_EQ(Cap().reports[0].second, 1);
    PSE_CHECK_EQ(Cap().reports[1].second, 0);
}

PSE_TEST("log: LogLine falls back to the loader name") {
    Fresh();
    pse::LogLine(0, PSE_LOG_WARN, "from the broker");
    pse::LogLine(pse::FindPlugin("pse"), PSE_LOG_INFO, nullptr);
    PSE_CHECK(AnyLineContains("[pse] WARN from the broker"));
    PSE_CHECK_EQ(Lines().size(), (size_t)2);
}

PSE_TEST("log: many threads, every line whole") {
    Fresh();
    PSEPluginHandle a = AddPlugin("threads");
    constexpr int kThreads = 8, kLines = 200;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t)
        ts.emplace_back([=] {
            for (int i = 0; i < kLines; ++i) pse::LogInterface()->Write(a, PSE_LOG_INFO, "t%d-l%d-abcdefghij", t, i);
        });
    for (auto& t : ts) t.join();
    auto lines = Lines();
    PSE_CHECK_EQ(lines.size(), (size_t)(kThreads * kLines));
    int whole = 0;
    for (const std::string& l : lines)
        if (l.find("[threads] INFO t") != std::string::npos && l.size() > 10 && l.substr(l.size() - 10) == "abcdefghij")
            ++whole;
    PSE_CHECK_EQ(whole, kThreads * kLines);
}

PSE_TEST("log: per-plugin files, rewritten each start") {
    std::string dir = TempPath("logs");
    CreateDirectoryA(dir.c_str(), nullptr);
    std::string file = dir + "\\filelog.log";
    auto readAll = [&] {
        std::string s;
        if (FILE* f = std::fopen(file.c_str(), "rb")) {
            char b[4096];
            size_t n;
            while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
            std::fclose(f);
        }
        return s;
    };
    Fresh(dir);
    PSEPluginHandle a = AddPlugin("filelog");
    pse::LogInterface()->Write(a, PSE_LOG_INFO, "first run");
    pse::LogInterface()->Write(a, PSE_LOG_INFO, "second line");
    std::string s = readAll();
    PSE_CHECK(s.find("[filelog] INFO first run\r\n") != std::string::npos);
    PSE_CHECK(s.find("second line") != std::string::npos);
    Fresh(dir);   // a new "start"
    a = AddPlugin("filelog");
    pse::LogInterface()->Write(a, PSE_LOG_INFO, "after restart");
    s = readAll();
    PSE_CHECK(s.find("first run") == std::string::npos);
    PSE_CHECK(s.find("after restart") != std::string::npos);
    pse::ResetForTests();   // closes the files
    DeleteFileA(file.c_str());
    DeleteFileA((dir + "\\pse.log").c_str());
    RemoveDirectoryA(dir.c_str());
}
