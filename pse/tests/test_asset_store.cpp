// pse_build: the asset store (pse/build, docs/internal/asset-store.md). Tier 1 = synthetic archives in a temp folder;
// tier 2 = the real install (PSE_GAME_DIR, else the Steam / GOG path) built into a temp folder and read back.
#include <windows.h>

#include <shellapi.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "pse_build.h"
#include "pse_test.h"

using namespace pse::build;

namespace {

std::wstring TempDir(const wchar_t* name) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring d = std::wstring(tmp) + L"pse-store-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + name;
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

void RemoveTree(const std::wstring& dir) {
    std::wstring from = dir + L'\0';
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

void WriteBytes(const std::wstring& path, const std::string& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0;
    WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &w, nullptr);
    CloseHandle(f);
}

// A DS1 archive: index {hash, offset, size} x n from 0, data from `dataStart`.
std::string MakeArchive(const std::vector<std::pair<std::string, std::string>>& files, uint32_t dataStart = 0x100) {
    std::string out(dataStart, '\0');
    uint32_t i = 0;
    for (const auto& [name, data] : files) {
        const uint32_t e[3] = {Ds1NameHash(name.c_str()), static_cast<uint32_t>(out.size()), static_cast<uint32_t>(data.size())};
        std::memcpy(&out[i * 12], e, 12);
        ++i;
        out += data;
    }
    return out;
}

StoreStats Build(const std::wstring& game, const std::wstring& out, bool force = false, bool names = false) {
    StoreOptions o;
    o.gameDir = game;
    o.outDir = out;
    o.force = force;
    o.names = names;
    StoreStats st;
    std::string err;
    PSE_CHECK(BuildStore(o, &st, &err));
    return st;
}

std::string BlobText(const Store& s, int32_t blob) {
    std::vector<uint8_t> b;
    if (blob < 0 || !s.Read(static_cast<uint32_t>(blob), &b)) return "(none)";
    return std::string(b.begin(), b.end());
}

// The usual synthetic install: catalog (A), mission1 (A B C), mission2 (A B' D), plus the uninstaller's .dat.
void MakeGame(const std::wstring& game) {
    WriteBytes(game + L"\\catalog.dat", MakeArchive({{"Weaps.txt", "AAAA shared"}}));
    WriteBytes(game + L"\\mission1.dat",
               MakeArchive({{"Weaps.txt", "AAAA shared"}, {"anim.prb", "BBBB level one"}, {"tank.evo", "CCCC tank"}}));
    WriteBytes(game + L"\\mission2.dat",
               MakeArchive({{"Weaps.txt", "AAAA shared"}, {"anim.prb", "BBBB level two"}, {"heli.evo", "DDDD heli"}}));
    WriteBytes(game + L"\\unins000.dat", "Inno Setup uninstall log, not an archive");
}

}  // namespace

PSE_TEST("asset store: DS1 name hash = the game's (upper-case, bare name)") {
    PSE_CHECK_EQ(Ds1NameHash("WEAPONS.RFX"), 0x4475E8D7u);
    PSE_CHECK_EQ(Ds1NameHash("weapons.rfx"), 0x4475E8D7u);
    PSE_CHECK_EQ(Ds1NameHash("Data\\Some/Where\\Weapons.rfx"), 0x4475E8D7u);
    PSE_CHECK(Ds1NameHash("WEAPONS.RFX") != Ds1NameHash("WEAPON.RFX"));
}

PSE_TEST("asset store: dedupe across archives, per-archive variants, manifests read back") {
    const std::wstring game = TempDir(L"game1"), out = TempDir(L"out1");
    MakeGame(game);
    const StoreStats st = Build(game, out);
    PSE_CHECK_EQ(st.archives, 3u);
    PSE_CHECK_EQ(st.skippedFiles, 0u);   // unins*.dat never counts as an input
    PSE_CHECK_EQ(st.files, 7u);
    PSE_CHECK_EQ(st.uniqueBlobs, 5u);    // A once, B twice (two contents), C, D
    PSE_CHECK_EQ(st.namesWithVariants, 1u);   // anim.prb
    PSE_CHECK(!st.upToDate);

    Store s;
    std::string err;
    PSE_REQUIRE(s.Open(out, &err));
    PSE_CHECK_EQ(s.Manifests().size(), size_t(3));
    const uint32_t weaps = Ds1NameHash("Weaps.txt"), anim = Ds1NameHash("anim.prb");
    PSE_CHECK_EQ(s.Lookup("catalog.dat", weaps), s.Lookup("mission2.dat", weaps));   // one blob for the shared file
    PSE_CHECK(s.Lookup("mission1.dat", anim) != s.Lookup("mission2.dat", anim));      // the variant
    PSE_CHECK_STR(BlobText(s, s.Lookup("MISSION1.DAT", anim)).c_str(), "BBBB level one");   // any case
    PSE_CHECK_STR(BlobText(s, s.Lookup("x\\mission2.dat", anim)).c_str(), "BBBB level two");
    PSE_CHECK_EQ(s.Lookup("mission1.dat", Ds1NameHash("heli.evo")), -1);
    PSE_CHECK_STR(BlobText(s, s.Lookup("mission2.dat", Ds1NameHash("heli.evo"))).c_str(), "DDDD heli");
    PSE_CHECK(s.Find("unins000.dat") == nullptr);
    // pack layout: 16-byte aligned blobs after the 16-byte header
    for (const Blob& b : s.Blobs()) PSE_CHECK_EQ(b.offset % kPackAlign, 0ull);
    s.Close();
    RemoveTree(game);
    RemoveTree(out);
}

PSE_TEST("asset store: stamp - unchanged inputs = nothing to do, a changed / new / removed archive rebuilds") {
    const std::wstring game = TempDir(L"game2"), out = TempDir(L"out2");
    MakeGame(game);
    Build(game, out);
    std::string why;
    PSE_CHECK(StampValid(out, game, Ds1Archives(), &why));
    PSE_CHECK(Build(game, out).upToDate);
    PSE_CHECK(!Build(game, out, /*force*/ true).upToDate);
    // changed content (size differs)
    WriteBytes(game + L"\\mission2.dat", MakeArchive({{"anim.prb", "BBBB level two, patched"}}));
    PSE_CHECK(!StampValid(out, game, Ds1Archives(), &why));
    PSE_CHECK(why.find("mission2.dat") != std::string::npos);
    StoreStats st = Build(game, out);
    PSE_CHECK(!st.upToDate);
    PSE_CHECK_EQ(st.files, 5u);
    // a new archive
    WriteBytes(game + L"\\mission3.dat", MakeArchive({{"new.evo", "EEEE"}}));
    PSE_CHECK(!StampValid(out, game, Ds1Archives(), &why));
    st = Build(game, out);
    PSE_CHECK_EQ(st.archives, 4u);
    // a removed archive: its manifest goes too
    DeleteFileW((game + L"\\mission3.dat").c_str());
    PSE_CHECK(!StampValid(out, game, Ds1Archives(), &why));
    Build(game, out);
    Store s;
    PSE_REQUIRE(s.Open(out));
    PSE_CHECK(s.Find("mission3.dat") == nullptr);
    s.Close();
    // a missing pack invalidates the stamp
    DeleteFileW((out + L"\\store.pack").c_str());
    PSE_CHECK(!StampValid(out, game, Ds1Archives(), &why));
    PSE_CHECK(!Build(game, out).upToDate);
    RemoveTree(game);
    RemoveTree(out);
}

PSE_TEST("asset store: corrupt input - bad entries left out, non-archives skipped, damaged store refused") {
    const std::wstring game = TempDir(L"game3"), out = TempDir(L"out3");
    std::string a = MakeArchive({{"good.txt", "GOOD"}, {"bad.txt", "BAD!"}, {"also.txt", "ALSO"}});
    const uint32_t past = static_cast<uint32_t>(a.size()) + 100;
    std::memcpy(&a[12 + 4], &past, 4);   // entry 2 points past the end
    WriteBytes(game + L"\\mission1.dat", a);
    WriteBytes(game + L"\\tiny.dat", "short");   // < 12 bytes: not an archive
    std::string notArchive(64, 'x');
    WriteBytes(game + L"\\other.dat", notArchive);   // first "offset" past the file: not an archive
    const StoreStats st = Build(game, out);
    PSE_CHECK_EQ(st.archives, 1u);
    PSE_CHECK_EQ(st.skippedFiles, 2u);
    PSE_CHECK_EQ(st.badEntries, 1u);
    PSE_CHECK_EQ(st.files, 2u);
    Store s;
    PSE_REQUIRE(s.Open(out));
    PSE_CHECK_STR(BlobText(s, s.Lookup("mission1.dat", Ds1NameHash("also.txt"))).c_str(), "ALSO");
    PSE_CHECK_EQ(s.Lookup("mission1.dat", Ds1NameHash("bad.txt")), -1);
    s.Close();
    // a damaged index is refused, not half-read
    WriteBytes(out + L"\\store.idx", "PSEIDX01 truncated");
    std::string err;
    PSE_CHECK(!s.Open(out, &err));
    PSE_CHECK(!err.empty());
    // no archives at all = failed build
    const std::wstring empty = TempDir(L"empty");
    StoreOptions o;
    o.gameDir = empty;
    o.outDir = TempDir(L"out3b");
    StoreStats st2;
    PSE_CHECK(!BuildStore(o, &st2, &err));
    RemoveTree(game);
    RemoveTree(out);
    RemoveTree(empty);
    RemoveTree(o.outDir);
}

PSE_TEST("asset store: names from the dictionary (a table naming a texture)") {
    const std::wstring game = TempDir(L"game4"), out = TempDir(L"out4");
    std::string dds = "DDS " + std::string(60, '\x01');
    WriteBytes(game + L"\\catalog.dat",
               MakeArchive({{"Skins.txt", "Bradley,HERO01_UK_01,Portrait\r\n"}, {"HERO01_UK_01.dds", dds},
                            {"NotADds.dds", "EOBJ mesh data that is no texture"}}));
    const StoreStats st = Build(game, out, false, /*names*/ true);
    Store s;
    PSE_REQUIRE(s.Open(out));
    PSE_CHECK_STR(s.NameOf(Ds1NameHash("HERO01_UK_01.DDS")).c_str(), "HERO01_UK_01.DDS");   // stem + known extension
    PSE_CHECK_STR(s.NameOf(Ds1NameHash("NOTADDS.DDS")).c_str(), "");   // never mentioned
    PSE_CHECK(st.namesKnown >= 1u);
    s.Close();
    RemoveTree(game);
    RemoveTree(out);
}

// ---- sound caches (pse/build/sound.cpp) ----

namespace {
std::string U32(uint32_t v) { return std::string(reinterpret_cast<const char*>(&v), 4); }
std::string Chunk(const char* tag, const std::string& body) { return std::string(tag, 4) + U32(static_cast<uint32_t>(body.size())) + body; }
// BANK {key, entries naming sample ids (unaligned on purpose)}; PFSM {lang, id, size, rate|bits, data}.
std::string Bank(const char* name, std::vector<uint32_t> ids) {
    std::string b = U32(Ds1BankKey(name)) + "\x01";
    for (uint32_t id : ids) b += U32(id);
    return Chunk("BANK", b);
}
std::string Sample(int32_t lang, uint32_t id, const std::string& data) {
    return Chunk("PFSM", U32(static_cast<uint32_t>(lang)) + U32(id) + U32(static_cast<uint32_t>(data.size())) + U32(22050 | 4u << 16) + data);
}
std::string Sch(const std::string& chunks, const std::string& tail = "") {
    return std::string("SCH\0", 4) + U32(static_cast<uint32_t>(chunks.size() + tail.size())) + chunks + tail;
}
}  // namespace

PSE_TEST("asset store: sound cache parse - every byte covered, keys read, a broken tail kept") {
    const std::string chunks = Bank("Gun.sbk", {7, 8}) + Sample(0, 7, "seven") + Sample(-1, 8, "eight") +
                               Chunk("IMUS", "music");
    const std::string file = Sch(chunks, "PFSM\xFF\xFF\xFF\x7F trailing");   // a chunk claiming more than is left
    uint8_t header[8];
    std::vector<SoundPiece> p;
    PSE_REQUIRE(Ds1ParseSoundCache(reinterpret_cast<const uint8_t*>(file.data()), file.size(), header, &p));
    PSE_CHECK_EQ(p.size(), size_t(5));
    PSE_CHECK(std::memcmp(header, file.data(), 8) == 0);
    PSE_CHECK_EQ(p[0].tag, kSoundTagBank);
    PSE_CHECK_EQ(p[0].a, Ds1BankKey("Gun.sbk"));
    PSE_CHECK_EQ(p[1].tag, kSoundTagSample);
    PSE_CHECK_EQ(p[1].a, 0u);
    PSE_CHECK_EQ(p[1].b, 7u);
    PSE_CHECK_EQ(p[2].a, 0xFFFFFFFFu);
    PSE_CHECK_EQ(p[4].tag, 0u);   // the tail
    uint32_t covered = 8;
    for (const SoundPiece& s : p) {
        PSE_CHECK_EQ(s.offset, covered);
        covered += s.size;
    }
    PSE_CHECK_EQ(covered, static_cast<uint32_t>(file.size()));
    PSE_CHECK(!Ds1ParseSoundCache(reinterpret_cast<const uint8_t*>("RIFF...."), 8, header, &p));
    PSE_CHECK_EQ(Ds1BankKey("Bradley.sbk"), Ds1BankKey("Bradley.sbk"));
    PSE_CHECK(Ds1BankKey("Bradley.sbk") != Ds1BankKey("bradley.sbk"));   // case-sensitive, like the game's
}

PSE_TEST("asset store: sound caches - deduplicated chunks, byte-identical rebuild, banks from any level") {
    const std::wstring game = TempDir(L"game6"), out = TempDir(L"out6");
    MakeGame(game);
    // mission1: its gun + the hero; mission2: the hero (same chunks) + a tank whose samples are only in mission2;
    // a language variant of sample 7 lives only in mission2.
    const std::string hero = Bank("Bradley.sbk", {100}) + Sample(0, 100, "hello");
    WriteBytes(game + L"\\mission1.sch", Sch(Bank("Gun.sbk", {7, 8}) + Sample(0, 7, "bang") + Sample(0, 8, "tail") + hero));
    WriteBytes(game + L"\\mission2.sch",
               Sch(hero + Bank("Tank.sbk", {20}) + Sample(0, 20, "engine") + Sample(6, 7, "bang (US)") + Chunk("IMUS", "m"),
                   "xx"));
    WriteBytes(game + L"\\notsound.sch", "not a sound cache at all");
    const StoreStats st = Build(game, out);
    PSE_CHECK_EQ(st.soundCaches, 2u);
    PSE_CHECK_EQ(st.skippedFiles, 1u);
    PSE_CHECK_EQ(st.soundPieces, 12u);
    PSE_CHECK_EQ(st.soundBlobs, 10u);   // the hero's two chunks are stored once
    PSE_CHECK_EQ(st.archives, 3u);      // the archives are unaffected
    std::string why;
    PSE_CHECK(StampValid(out, game, Ds1Archives(), &why));

    Store s;
    std::string err;
    PSE_REQUIRE(s.Open(out, &err));
    PSE_CHECK_EQ(s.SoundCaches().size(), size_t(2));
    for (const wchar_t* name : {L"mission1.sch", L"mission2.sch"}) {
        std::string rebuilt, want;
        PSE_REQUIRE(s.RebuildSound(Narrow(name), &rebuilt));
        HANDLE f = CreateFileW((game + L"\\" + name).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        want.resize(GetFileSize(f, nullptr));
        DWORD got = 0;
        ReadFile(f, want.data(), static_cast<DWORD>(want.size()), &got, nullptr);
        CloseHandle(f);
        PSE_CHECK(rebuilt == want);
    }
    PSE_CHECK(s.FindSound("MISSION2.SCH") != nullptr);
    PSE_CHECK(s.FindSound("mission3.sch") == nullptr);
    std::string from, pack;
    PSE_CHECK(s.HasSoundBank(Ds1BankKey("Tank.sbk"), &from));
    PSE_CHECK_STR(from.c_str(), "mission2.sch");
    PSE_CHECK(!s.HasSoundBank(Ds1BankKey("Nothing.sbk")));
    // The gun's bank pack: the bank + samples 7 (both languages, from both caches) and 8 - not the hero's.
    PSE_REQUIRE(s.SoundBankPack(Ds1BankKey("Gun.sbk"), &pack, &from));
    PSE_CHECK_STR(from.c_str(), "mission1.sch");
    uint8_t header[8];
    std::vector<SoundPiece> p;
    PSE_REQUIRE(Ds1ParseSoundCache(reinterpret_cast<const uint8_t*>(pack.data()), pack.size(), header, &p));
    PSE_CHECK_EQ(p.size(), size_t(4));
    PSE_CHECK_EQ(p[0].tag, kSoundTagBank);
    std::map<std::pair<uint32_t, uint32_t>, int> samples;
    for (size_t i = 1; i < p.size(); ++i) ++samples[{p[i].a, p[i].b}];
    PSE_CHECK_EQ(samples[std::make_pair(0u, 7u)], 1);
    PSE_CHECK_EQ(samples[std::make_pair(6u, 7u)], 1);
    PSE_CHECK_EQ(samples[std::make_pair(0u, 8u)], 1);
    uint32_t size;
    std::memcpy(&size, pack.data() + 4, 4);
    PSE_CHECK_EQ(size, static_cast<uint32_t>(pack.size() - 8));
    s.Close();
    // a changed sound cache invalidates the stamp; a damaged sound manifest is refused
    WriteBytes(game + L"\\mission1.sch", Sch(Bank("Gun.sbk", {7}) + Sample(0, 7, "BANG")));
    PSE_CHECK(!StampValid(out, game, Ds1Archives(), &why));
    PSE_CHECK(why.find("mission1.sch") != std::string::npos);
    Build(game, out);
    WriteBytes(out + L"\\sound\\mission2.sch.sman", "PSESND01 truncated");
    PSE_CHECK(!s.Open(out, &err));
    RemoveTree(game);
    RemoveTree(out);
}

PSE_TEST("asset store: a cancelled build leaves no store; progress reaches 1000") {
    const std::wstring game = TempDir(L"game7"), out = TempDir(L"out7");
    MakeGame(game);
    WriteBytes(game + L"\\mission1.sch", Sch(Bank("Gun.sbk", {7}) + Sample(0, 7, "bang")));
    StoreOptions o;
    o.gameDir = game;
    o.outDir = out;
    o.names = false;
    int calls = 0;
    o.progress = [&](uint32_t, const std::string&) { ++calls; };
    o.cancelled = [&] { return calls >= 2; };   // after the first input
    StoreStats st;
    std::string err;
    PSE_CHECK(!BuildStore(o, &st, &err));
    PSE_CHECK(st.cancelled);
    PSE_CHECK(GetFileAttributesW((out + L"\\stamp.txt").c_str()) == INVALID_FILE_ATTRIBUTES);
    PSE_CHECK(GetFileAttributesW((out + L"\\store.pack.tmp").c_str()) == INVALID_FILE_ATTRIBUTES);
    std::string why;
    PSE_CHECK(!StampValid(out, game, Ds1Archives(), &why));
    uint32_t last = 0;
    o.progress = [&](uint32_t permille, const std::string&) { last = permille; };
    o.cancelled = nullptr;
    PSE_CHECK(BuildStore(o, &st, &err));
    PSE_CHECK_EQ(last, 1000u);
    RemoveTree(game);
    RemoveTree(out);
}

PSE_TEST("asset store: default folder = <game>\\PSE\\Cache\\assets when writable") {
    const std::wstring game = TempDir(L"game5");
    const std::wstring dir = DefaultStoreDir(game, "ds1");
    PSE_CHECK(dir == game + L"\\PSE\\Cache\\assets");
    PSE_CHECK(GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES);
    RemoveTree(game);
}

// ---- tier 2: the real install ----

namespace {
std::wstring RealGame() {
    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"PSE_GAME_DIR", env, MAX_PATH)) return env;
    for (const wchar_t* d : {L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\conflict_desert_storm",
                             L"C:\\GOG Games\\Conflict - Desert Storm"})
        if (GetFileAttributesW((std::wstring(d) + L"\\chardata.dat").c_str()) != INVALID_FILE_ATTRIBUTES) return d;
    return {};
}
}  // namespace

PSE_TEST("asset store (real data): every archive entry of the install round-trips byte-identical") {
    const std::wstring game = RealGame();
    if (game.empty()) PSE_SKIP("no install (set PSE_GAME_DIR)");
    const std::wstring out = TempDir(L"real");
    StoreOptions o;
    o.gameDir = game;
    o.outDir = out;
    o.force = true;
    StoreStats st;
    std::string err;
    const DWORD t0 = GetTickCount();
    PSE_REQUIRE(BuildStore(o, &st, &err));
    std::printf("    built: %u archives, %u files, %u blobs, %.1f -> %.1f MB, %u variant names, %u named, %u ms\n",
                st.archives, st.files, st.uniqueBlobs, st.inputBytes / 1048576.0, st.storeBytes / 1048576.0,
                st.namesWithVariants, st.namesKnown, GetTickCount() - t0);
    PSE_CHECK(st.archives >= 20u);
    PSE_CHECK(st.uniqueBlobs < st.files);
    Store s;
    PSE_REQUIRE(s.Open(out, &err));
    // Compare every entry with the archive's own bytes.
    HANDLE pack = CreateFileW(s.PackPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    PSE_REQUIRE(pack != INVALID_HANDLE_VALUE);
    uint32_t checked = 0, wrong = 0;
    std::vector<uint8_t> blob;
    for (const Store::Manifest& m : s.Manifests()) {
        HANDLE f = CreateFileW((game + L"\\" + Widen(m.archive)).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        PSE_REQUIRE(f != INVALID_HANDLE_VALUE);
        std::vector<uint8_t> data(GetFileSize(f, nullptr));
        DWORD got = 0;
        ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr);
        CloseHandle(f);
        std::vector<ArchiveEntry> entries;
        uint32_t bad = 0;
        PSE_REQUIRE(Ds1ParseArchive(data.data(), data.size(), &entries, &bad));
        PSE_CHECK_EQ(entries.size(), m.entries.size());
        for (size_t i = 0; i < entries.size() && i < m.entries.size(); ++i) {
            const Blob& b = s.Blobs()[m.entries[i].offset];
            blob.resize(b.size);
            LARGE_INTEGER pos;
            pos.QuadPart = static_cast<LONGLONG>(b.offset);
            DWORD n = 0;
            SetFilePointerEx(pack, pos, nullptr, FILE_BEGIN);
            const bool ok = m.entries[i].nameHash == entries[i].nameHash && b.size == entries[i].size &&
                            (b.size == 0 || (ReadFile(pack, blob.data(), b.size, &n, nullptr) && n == b.size)) &&
                            std::memcmp(blob.data(), data.data() + entries[i].offset, b.size) == 0;
            ++checked;
            if (!ok) ++wrong;
        }
    }
    CloseHandle(pack);
    std::printf("    %u entries compared, %u differ\n", checked, wrong);
    PSE_CHECK_EQ(checked, st.files);
    PSE_CHECK_EQ(wrong, 0u);
    // Every level sound cache rebuilt from the store's chunks = the file on disk.
    std::printf("    sound: %u caches, %u chunks, %.1f MB -> %u unique chunks, %.1f MB\n", st.soundCaches, st.soundPieces,
                st.soundInputBytes / 1048576.0, st.soundBlobs, st.soundStoreBytes / 1048576.0);
    PSE_CHECK(st.soundCaches >= 20u);
    uint32_t sameSch = 0;
    const DWORD tr = GetTickCount();
    for (const Store::SoundManifest& m : s.SoundCaches()) {
        std::string rebuilt, want;
        PSE_REQUIRE(s.RebuildSound(m.cache, &rebuilt));
        HANDLE f = CreateFileW((game + L"\\" + Widen(m.cache)).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        PSE_REQUIRE(f != INVALID_HANDLE_VALUE);
        want.resize(GetFileSize(f, nullptr));
        DWORD got = 0;
        ReadFile(f, want.data(), static_cast<DWORD>(want.size()), &got, nullptr);
        CloseHandle(f);
        if (rebuilt == want) ++sameSch;
        else std::printf("    %s differs (%zu vs %zu bytes)\n", m.cache.c_str(), rebuilt.size(), want.size());
    }
    std::printf("    %u of %zu sound caches rebuilt byte-identical (%u ms)\n", sameSch, s.SoundCaches().size(),
                GetTickCount() - tr);
    PSE_CHECK_EQ(sameSch, static_cast<uint32_t>(s.SoundCaches().size()));
    // Every bank is in some cache; a weapon bank from another level brings its samples.
    std::string pack2, from;
    PSE_CHECK(s.SoundBankPack(Ds1BankKey("MP5SILENCEDSUBMG.sbk"), &pack2, &from));
    std::printf("    MP5SILENCEDSUBMG.sbk: %zu bytes from %s\n", pack2.size(), from.c_str());
    PSE_CHECK(pack2.size() > 1000);
    s.Close();
    RemoveTree(out);
}
