// pse_build.exe - command line front end of the first-run builder (docs/sdk/DESIGN.md §12, docs/sdk/asset-store.md).
//
//   pse_build [--game <dir>] [--out <dir>] [--force] [--no-names]      build / confirm the asset store
//   pse_build --verify [--game <dir>] [--out <dir>]                    every archive entry read back byte-identical
//   pse_build --list <archive> [--out <dir>]                           a manifest: hash, size, content, name
//   pse_build --find <name> [--out <dir>]                              which archives carry a file, and which bytes
//
// --game defaults to the Steam / GOG install found in the registry (or the current folder when it holds archives).
// Exit code: 0 ok, 1 usage, 2 build / verify failed.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pse_build.h"

using namespace pse::build;

namespace {

bool HasArchives(const std::wstring& dir) {
    return GetFileAttributesW((dir + L"\\chardata.dat").c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value, REGSAM view) {
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof buf;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | (view == KEY_WOW64_64KEY ? RRF_SUBKEY_WOW6464KEY : RRF_SUBKEY_WOW6432KEY),
                     nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

// The install: the folder of the game's ExePath (the game's own key), then Steam's libraries, then GOG.
std::wstring FindGame() {
    wchar_t cwd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);
    if (HasArchives(cwd)) return cwd;
    std::vector<std::wstring> cands;
    std::wstring exe = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Pivotal Games\\Conflict Desert Storm", L"ExePath",
                                 KEY_WOW64_32KEY);
    if (!exe.empty()) cands.push_back(exe.substr(0, exe.find_last_of(L"\\/")));
    std::wstring steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", KEY_WOW64_64KEY);
    if (!steam.empty()) {
        cands.push_back(steam + L"\\steamapps\\common\\conflict_desert_storm");
        HANDLE f = CreateFileW((steam + L"\\steamapps\\libraryfolders.vdf").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            std::string text(GetFileSize(f, nullptr), '\0');
            DWORD got = 0;
            ReadFile(f, text.data(), static_cast<DWORD>(text.size()), &got, nullptr);
            CloseHandle(f);
            for (size_t p = text.find("\"path\""); p != std::string::npos; p = text.find("\"path\"", p + 6)) {
                const size_t a = text.find('"', p + 6), b = a == std::string::npos ? a : text.find('"', a + 1);
                if (b == std::string::npos) break;
                std::string path = text.substr(a + 1, b - a - 1);
                std::string clean;
                for (size_t i = 0; i < path.size(); ++i)
                    if (!(path[i] == '\\' && i + 1 < path.size() && path[i + 1] == '\\')) clean += path[i];
                cands.push_back(Widen(clean) + L"\\steamapps\\common\\conflict_desert_storm");
            }
        }
    }
    for (const wchar_t* gog : {L"H:\\GOG\\Games\\Conflict - Desert Storm", L"C:\\GOG Games\\Conflict - Desert Storm"})
        cands.push_back(gog);
    for (const std::wstring& c : cands)
        if (HasArchives(c)) return c;
    return {};
}

int Usage() {
    std::fputs("usage: pse_build [--game <dir>] [--out <dir>] [--force] [--no-names] [--quiet]\n"
               "       pse_build --verify [--game <dir>] [--out <dir>]\n"
               "       pse_build --list <archive> [--game <dir>] [--out <dir>]\n"
               "       pse_build --find <file name> [--game <dir>] [--out <dir>]\n",
               stderr);
    return 1;
}

// Every entry of every archive read back from the store must be byte-identical.
int Verify(const std::wstring& game, const std::wstring& out) {
    Store store;
    std::string err;
    if (!store.Open(out, &err)) {
        std::printf("verify: can't open the store: %s\n", err.c_str());
        return 2;
    }
    const ArchiveFormat& fmt = Ds1Archives();
    HANDLE pack = CreateFileW(store.PackPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pack == INVALID_HANDLE_VALUE) return 2;
    uint32_t checked = 0, wrong = 0, archives = 0;
    for (const Store::Manifest& m : store.Manifests()) {
        HANDLE f = CreateFileW((game + L"\\" + Widen(m.archive)).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            std::printf("verify: %s is gone\n", m.archive.c_str());
            ++wrong;
            continue;
        }
        std::vector<uint8_t> data(GetFileSize(f, nullptr));
        DWORD got = 0;
        ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr);
        CloseHandle(f);
        std::vector<ArchiveEntry> entries;
        uint32_t bad = 0;
        fmt.parse(data.data(), data.size(), &entries, &bad);
        ++archives;
        if (entries.size() != m.entries.size()) {
            std::printf("verify: %s has %zu entries, its manifest %zu\n", m.archive.c_str(), entries.size(), m.entries.size());
            ++wrong;
            continue;
        }
        std::vector<uint8_t> blob;
        for (size_t i = 0; i < entries.size(); ++i) {
            const ArchiveEntry& e = entries[i];
            const Blob& b = store.Blobs()[m.entries[i].offset];
            blob.resize(b.size);
            LARGE_INTEGER pos;
            pos.QuadPart = static_cast<LONGLONG>(b.offset);
            SetFilePointerEx(pack, pos, nullptr, FILE_BEGIN);
            DWORD n = 0;
            const bool ok = m.entries[i].nameHash == e.nameHash && b.size == e.size &&
                            (b.size == 0 || (ReadFile(pack, blob.data(), b.size, &n, nullptr) && n == b.size)) &&
                            std::memcmp(blob.data(), data.data() + e.offset, e.size) == 0;
            ++checked;
            if (!ok && wrong++ < 20) std::printf("verify: %s entry %zu (%08X) differs\n", m.archive.c_str(), i, e.nameHash);
        }
    }
    CloseHandle(pack);
    std::printf("verify: %u archives, %u entries checked, %u wrong\n", archives, checked, wrong);
    // sound caches: each rebuilt from the store's chunks = the file
    uint32_t caches = 0, cachesWrong = 0;
    for (const Store::SoundManifest& m : store.SoundCaches()) {
        std::vector<uint8_t> want;
        std::string rebuilt;
        HANDLE f = CreateFileW((game + L"\\" + Widen(m.cache)).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            want.resize(GetFileSize(f, nullptr));
            DWORD got = 0;
            ReadFile(f, want.data(), static_cast<DWORD>(want.size()), &got, nullptr);
            CloseHandle(f);
        }
        ++caches;
        if (f == INVALID_HANDLE_VALUE || !store.RebuildSound(m.cache, &rebuilt) || rebuilt.size() != want.size() ||
            std::memcmp(rebuilt.data(), want.data(), want.size()) != 0) {
            std::printf("verify: sound cache %s differs\n", m.cache.c_str());
            ++cachesWrong;
        }
    }
    std::printf("verify: %u sound caches rebuilt, %u wrong\n", caches, cachesWrong);
    wrong += cachesWrong;
    return wrong ? 2 : 0;
}

}  // namespace

namespace pse::m2l {
int MissionsCommand(int argc, wchar_t** argv, std::wstring (*findGame)());   // mission2lua/m2l_cli.cpp
}

int wmain(int argc, wchar_t** argv) {
    // `pse_build missions ...`: the mission translator (docs/sdk/mission-translation.md "C++ port")
    if (argc > 1 && std::wcscmp(argv[1], L"missions") == 0) return pse::m2l::MissionsCommand(argc - 2, argv + 2, &FindGame);
    StoreOptions o;
    std::wstring list, find;
    bool verify = false, quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--game") o.gameDir = next();
        else if (a == L"--out") o.outDir = next();
        else if (a == L"--force") o.force = true;
        else if (a == L"--no-names") o.names = false;
        else if (a == L"--verify") verify = true;
        else if (a == L"--quiet") quiet = true;
        else if (a == L"--list") list = next();
        else if (a == L"--find") find = next();
        else return Usage();
    }
    if (o.gameDir.empty()) o.gameDir = FindGame();
    if (o.gameDir.empty() || !HasArchives(o.gameDir)) {
        std::fputs("pse_build: no Conflict: Desert Storm install found (use --game <dir>)\n", stderr);
        return 1;
    }
    if (o.outDir.empty()) o.outDir = DefaultStoreDir(o.gameDir, "ds1");
    if (!quiet) o.log = [](const std::string& s) { std::printf("%s\n", s.c_str()); };

    if (verify) return Verify(o.gameDir, o.outDir);
    if (!list.empty() || !find.empty()) {
        Store store;
        std::string err;
        if (!store.Open(o.outDir, &err)) {
            std::printf("can't open the store in %s: %s (build it first)\n", Narrow(o.outDir).c_str(), err.c_str());
            return 2;
        }
        if (!list.empty()) {
            const Store::Manifest* m = store.Find(Narrow(list));
            if (!m) {
                std::printf("no manifest for %s\n", Narrow(list).c_str());
                return 2;
            }
            for (const ArchiveEntry& e : m->entries) {
                const Blob& b = store.Blobs()[e.offset];
                auto n = m->names.find(e.nameHash);
                std::printf("%08X %10u %s %s\n", e.nameHash, b.size, b.hash.Hex().substr(0, 16).c_str(),
                            n == m->names.end() ? "" : n->second.c_str());
            }
            return 0;
        }
        const std::string name = Narrow(find);
        const uint32_t h = Ds1NameHash(name.c_str());
        int hits = 0;
        for (const Store::Manifest& m : store.Manifests()) {
            auto it = m.byHash.find(h);
            if (it == m.byHash.end()) continue;
            const Blob& b = store.Blobs()[it->second];
            std::printf("%-16s %10u bytes  %s\n", m.archive.c_str(), b.size, b.hash.Hex().substr(0, 16).c_str());
            ++hits;
        }
        std::printf("%s (%08X): in %d archive(s)\n", name.c_str(), h, hits);
        return hits ? 0 : 2;
    }

    StoreStats st;
    std::string err;
    if (!BuildStore(o, &st, &err)) return 2;
    if (!st.upToDate && !quiet) {
        std::printf("\nsummary\n  archives          %u (+%u other .dat skipped)\n  files             %u\n"
                    "  unique blobs      %u\n  input             %.1f MB\n  store.pack        %.1f MB\n"
                    "  saved             %.1f MB (%.1f %%)\n  names w/ variants %u\n  named entries     %u of %u\n"
                    "  time              %u ms (read %u, hash %u, write %u, names %u)\n",
                    st.archives, st.skippedFiles, st.files, st.uniqueBlobs, st.inputBytes / 1048576.0,
                    st.storeBytes / 1048576.0, (double(st.inputBytes) - double(st.storeBytes - st.soundStoreBytes)) / 1048576.0,
                    st.inputBytes ? 100.0 * (double(st.inputBytes) - double(st.storeBytes - st.soundStoreBytes)) / double(st.inputBytes) : 0.0,
                    st.namesWithVariants, st.namesKnown, st.files, st.msTotal, st.msRead, st.msHash, st.msWrite,
                    st.msNames);
        std::printf("  sound caches      %u (%u chunks, %.1f MB -> %u unique chunks, %.1f MB in store.pack)\n",
                    st.soundCaches, st.soundPieces, st.soundInputBytes / 1048576.0, st.soundBlobs,
                    st.soundStoreBytes / 1048576.0);
        if (!st.variantExamples.empty()) {
            std::printf("  variants (most first; full list in variants.txt):\n");
            for (const std::string& v : st.variantExamples) std::printf("    %s\n", v.c_str());
        }
    }
    return 0;
}
