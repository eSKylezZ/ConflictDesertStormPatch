// pse.serialization + pse::cosave (temp files in %TEMP%).
#include <cstdio>

#include "loader_fixture.h"

using namespace pse_fixture;

namespace {
const PSESerializationInterface* Z() { return pse::SerializationInterface(); }

struct State {
    PSEPluginHandle plugin = 0;
    std::vector<std::pair<uint32_t, std::string>> toWrite;   // tag, data
    std::vector<std::pair<uint32_t, std::string>> loaded;
    std::vector<uint32_t> versions;
    int saves = 0, loads = 0, reverts = 0;
    uint32_t readStep = 0;   // >0: read in pieces of this size
};

void PSE_CALL OnSave(PSESaveWriter* w, void* ctx) {
    auto* s = static_cast<State*>(ctx);
    ++s->saves;
    for (auto& [tag, data] : s->toWrite) Z()->WriteChunk(w, tag, 1, data.data(), (uint32_t)data.size());
}
void PSE_CALL OnLoad(PSESaveReader* r, void* ctx) {
    auto* s = static_cast<State*>(ctx);
    ++s->loads;
    uint32_t tag, version, len;
    while (Z()->NextChunk(r, &tag, &version, &len)) {
        std::string data(len, '\0');
        if (s->readStep) {
            uint32_t got = 0, n;
            while (got < len && (n = Z()->Read(r, data.data() + got, s->readStep)) > 0) got += n;
            data.resize(got);
            PSE_CHECK_EQ(Z()->Read(r, data.data(), 1), 0u);   // past the end
        } else {
            data.resize(Z()->Read(r, data.data(), len + 10));   // asking for more than there is is fine
        }
        s->loaded.emplace_back(tag, data);
        s->versions.push_back(version);
    }
}
void PSE_CALL OnRevert(void* ctx) { ++static_cast<State*>(ctx)->reverts; }

State& Plugin(State& s, const char* name) {
    s.plugin = AddPlugin(name);
    Z()->SetCallbacks(s.plugin, &OnSave, &OnLoad, &OnRevert, &s);
    return s;
}

std::string ReadFile(const std::string& path) {
    std::string out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char b[4096];
        size_t n;
        while ((n = std::fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
        std::fclose(f);
    }
    return out;
}
void WriteFile(const std::string& path, const std::string& bytes) {
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
}
struct FileGuard {
    std::string path;
    explicit FileGuard(const char* name) : path(TempPath(name)) { DeleteFileA(path.c_str()); }
    ~FileGuard() {
        DeleteFileA(path.c_str());
        DeleteFileA((path + ".tmp").c_str());
    }
};
}  // namespace

PSE_TEST("cosave: save + load round trip, several chunks, file layout") {
    FileGuard file("roundtrip.pse");
    Fresh();
    State a, b;
    Plugin(a, "co.a").toWrite = {{PSE_TAG('V', 'E', 'H', '1'), "vehicles"}, {PSE_TAG('G', 'E', 'A', 'R'), ""}};
    Plugin(b, "co.b").toWrite = {{7, std::string("bin\0ary", 7)}};
    PSE_CHECK(pse::cosave::Save(file.path.c_str()));
    PSE_CHECK_EQ(a.saves, 1);
    PSE_CHECK_EQ(b.saves, 1);

    std::string raw = ReadFile(file.path);
    PSE_REQUIRE(raw.size() >= 8);
    PSE_CHECK(raw.compare(0, 4, std::string("PSE\x01", 4)) == 0);
    uint32_t count;
    memcpy(&count, raw.data() + 4, 4);
    PSE_CHECK_EQ(count, 3u);
    PSE_CHECK_EQ((int)raw[8], 4);                       // name length of "co.a"
    PSE_CHECK(raw.compare(9, 4, "co.a") == 0);
    // 4+4 header, chunks: (1+4+12+8) + (1+4+12+0) + (1+4+12+7)
    PSE_CHECK_EQ(raw.size(), (size_t)(8 + 25 + 17 + 24));

    pse::cosave::Load(file.path.c_str());
    PSE_CHECK_EQ(a.loads, 1);
    PSE_CHECK_EQ(a.reverts, 0);
    PSE_REQUIRE(a.loaded.size() == 2);
    PSE_CHECK_EQ(a.loaded[0].first, PSE_TAG('V', 'E', 'H', '1'));
    PSE_CHECK_STR(a.loaded[0].second.c_str(), "vehicles");
    PSE_CHECK_EQ(a.loaded[1].second.size(), (size_t)0);
    PSE_CHECK_EQ(a.versions[0], 1u);
    PSE_REQUIRE(b.loaded.size() == 1);
    PSE_CHECK(b.loaded[0].second == std::string("bin\0ary", 7));
}

PSE_TEST("cosave: partial reads") {
    FileGuard file("partial.pse");
    Fresh();
    State a;
    std::string big(1000, '\0');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>((i * 7) & 0x7F);
    Plugin(a, "co.p").toWrite = {{1, big}, {2, "xyz"}};
    a.readStep = 3;
    pse::cosave::Save(file.path.c_str());
    pse::cosave::Load(file.path.c_str());
    PSE_REQUIRE(a.loaded.size() == 2);
    PSE_CHECK(a.loaded[0].second == big);
    PSE_CHECK_STR(a.loaded[1].second.c_str(), "xyz");
}

PSE_TEST("cosave: missing file, no chunks, NewGame -> revert") {
    FileGuard file("missing.pse");
    Fresh();
    State a, b;
    Plugin(a, "co.a").toWrite = {{1, "a"}};
    Plugin(b, "co.b");   // writes nothing
    PSE_CHECK(!pse::cosave::Load(file.path.c_str()));
    PSE_CHECK_EQ(a.reverts, 1);
    PSE_CHECK_EQ(b.reverts, 1);
    PSE_CHECK_EQ(a.loads, 0);
    pse::cosave::Save(file.path.c_str());
    PSE_CHECK(pse::cosave::Load(file.path.c_str()));
    PSE_CHECK_EQ(a.loads, 1);
    PSE_CHECK_EQ(b.loads, 0);
    PSE_CHECK_EQ(b.reverts, 2);   // a save without its chunks
    pse::cosave::NewGame();
    PSE_CHECK_EQ(a.reverts, 2);
    PSE_CHECK_EQ(b.reverts, 3);
    // A plugin with only a revert callback, or none at all, is fine.
    PSEPluginHandle c = AddPlugin("co.c");
    PSE_CHECK_EQ(Z()->SetCallbacks(c, nullptr, nullptr, nullptr, nullptr), PSE_OK);
    pse::cosave::NewGame();
    PSE_CHECK(pse::cosave::Save(file.path.c_str()));
    PSE_CHECK_EQ(Z()->SetCallbacks(0, &OnSave, nullptr, nullptr, nullptr), PSE_E_INVALID_ARG);
}

PSE_TEST("cosave: chunks of plugins not loaded survive a rewrite") {
    FileGuard file("unknown.pse");
    Fresh();
    State a, x;
    Plugin(a, "co.a").toWrite = {{1, "a-v1"}};
    Plugin(x, "co.gone").toWrite = {{5, "gone-data"}, {6, "more"}};
    pse::cosave::Save(file.path.c_str());

    // Next start without co.gone: its chunks are kept on save (from the file being overwritten).
    Fresh();
    State a2;
    Plugin(a2, "co.a").toWrite = {{1, "a-v2"}};
    PSE_CHECK(pse::cosave::Save(file.path.c_str()));

    // co.gone is back: it gets its data; co.a has the new one.
    Fresh();
    State a3, x3;
    Plugin(a3, "co.a");
    Plugin(x3, "co.gone");
    pse::cosave::Load(file.path.c_str());
    PSE_REQUIRE(a3.loaded.size() == 1);
    PSE_CHECK_STR(a3.loaded[0].second.c_str(), "a-v2");
    PSE_REQUIRE(x3.loaded.size() == 2);
    PSE_CHECK_STR(x3.loaded[0].second.c_str(), "gone-data");
    PSE_CHECK_EQ(x3.loaded[1].first, 6u);
}

PSE_TEST("cosave: loaded unknown chunks are kept even if the file was deleted") {
    FileGuard file("deleted.pse");
    FileGuard other("deleted-other.pse");
    Fresh();
    State a, x;
    Plugin(a, "co.a").toWrite = {{1, "a"}};
    Plugin(x, "co.gone").toWrite = {{9, "keep me"}};
    pse::cosave::Save(file.path.c_str());

    Fresh();
    State a2;
    Plugin(a2, "co.a").toWrite = {{1, "a2"}};
    pse::cosave::Load(file.path.c_str());
    DeleteFileA(file.path.c_str());
    PSE_CHECK(pse::cosave::Save(file.path.c_str()));     // same slot, file gone
    PSE_CHECK(pse::cosave::Save(other.path.c_str()));    // another slot

    Fresh();
    State x3;
    Plugin(x3, "co.gone");
    pse::cosave::Load(other.path.c_str());
    PSE_REQUIRE(x3.loaded.size() == 1);
    PSE_CHECK_STR(x3.loaded[0].second.c_str(), "keep me");
    x3.loaded.clear();
    pse::cosave::Load(file.path.c_str());
    PSE_REQUIRE(x3.loaded.size() == 1);

    // NewGame forgets the session's unknown chunks.
    Fresh();
    State a4;
    Plugin(a4, "co.a").toWrite = {{1, "a4"}};
    pse::cosave::Load(file.path.c_str());
    pse::cosave::NewGame();
    DeleteFileA(other.path.c_str());
    pse::cosave::Save(other.path.c_str());
    Fresh();
    State x5;
    Plugin(x5, "co.gone");
    pse::cosave::Load(other.path.c_str());
    PSE_CHECK(x5.loaded.empty());
    PSE_CHECK_EQ(x5.reverts, 1);
}

PSE_TEST("cosave: damaged files are treated as missing, with a warning") {
    FileGuard file("bad.pse");
    Fresh();
    State a;
    Plugin(a, "co.a").toWrite = {{1, "hello world"}};
    pse::cosave::Save(file.path.c_str());
    std::string good = ReadFile(file.path);
    PSE_REQUIRE(good.size() > 12);

    std::vector<std::string> bad = {
        "",                                          // empty
        "PSE",                                       // shorter than the header
        std::string("XYZ\x01\0\0\0\0", 8),           // wrong magic
        good.substr(0, good.size() - 1),             // truncated data
        good.substr(0, 10),                          // truncated chunk header
        good + "x",                                  // trailing garbage
    };
    std::string hugeCount = good;
    const uint32_t manyChunks = 0x7FFFFFFF, hugeChunk = 0xFFFFFFF0;
    memcpy(hugeCount.data() + 4, &manyChunks, 4);
    bad.push_back(hugeCount);
    std::string hugeLen = good;
    size_t lenAt = 8 + 1 + 4 + 8;   // header, name, tag + version
    memcpy(hugeLen.data() + lenAt, &hugeChunk, 4);
    bad.push_back(hugeLen);

    for (size_t i = 0; i < bad.size(); ++i) {
        WriteFile(file.path, bad[i]);
        a.loads = a.reverts = 0;
        a.loaded.clear();
        PSE_CHECK(!pse::cosave::Load(file.path.c_str()));
        PSE_CHECK_EQ(a.loads, 0);
        PSE_CHECK_EQ(a.reverts, 1);
    }
    PSE_CHECK(AnyLineContains("is damaged"));

    // Saving over a damaged file works and repairs it.
    WriteFile(file.path, "garbage");
    PSE_CHECK(pse::cosave::Save(file.path.c_str()));
    PSE_CHECK(pse::cosave::Load(file.path.c_str()));
    PSE_CHECK_EQ(a.loads, 1);
}

PSE_TEST("cosave: writer / reader argument checks; empty and null paths") {
    Fresh();
    PSE_CHECK_EQ(Z()->WriteChunk(nullptr, 1, 1, "x", 1), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(Z()->NextChunk(nullptr, nullptr, nullptr, nullptr), 0);
    PSE_CHECK_EQ(Z()->Read(nullptr, nullptr, 0), 0u);
    PSE_CHECK(!pse::cosave::Save(nullptr));
    PSE_CHECK(!pse::cosave::Save(""));
    State a;
    Plugin(a, "co.a");
    PSE_CHECK(!pse::cosave::Load(nullptr));
    PSE_CHECK_EQ(a.reverts, 1);
}

PSE_TEST("cosave: save into a folder that doesn't exist fails cleanly") {
    Fresh();
    State a;
    Plugin(a, "co.a").toWrite = {{1, "x"}};
    std::string path = TempPath("no-such-dir") + "\\x.pse";
    PSE_CHECK(!pse::cosave::Save(path.c_str()));
    PSE_CHECK(AnyLineContains("couldn't write co-save"));
}
