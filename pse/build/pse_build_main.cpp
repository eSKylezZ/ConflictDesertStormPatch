// pse_build.exe - command line front end of the first-run builder (docs/internal/DESIGN.md §12, docs/internal/asset-store.md).
//
//   pse_build [--game <dir>] [--out <dir>] [--force] [--no-names]      build / confirm the asset store
//   pse_build --verify [--game <dir>] [--out <dir>]                    every archive entry read back byte-identical
//   pse_build --list <archive> [--out <dir>]                           a manifest: hash, size, content, name
//   pse_build --find <name> [--out <dir>]                              which archives carry a file, and which bytes
//
// --game defaults to the Steam / GOG install found in the registry (or the current folder when it holds archives).
// Exit code: 0 ok, 1 usage, 2 build / verify failed.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <io.h>

#include "data.h"
#include "forms.h"
#include "placed.h"
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
    // GOG Galaxy / the GOG installer: one key per game under GOG.com\Games with its folder in "path"
    HKEY games = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GOG.com\\Games", 0, KEY_READ | KEY_WOW64_32KEY, &games) ==
        ERROR_SUCCESS) {
        wchar_t id[64];
        for (DWORD i = 0, n = 64; RegEnumKeyExW(games, i, id, &n, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
             ++i, n = 64) {
            std::wstring path = RegString(HKEY_LOCAL_MACHINE, (L"SOFTWARE\\GOG.com\\Games\\" + std::wstring(id)).c_str(),
                                          L"path", KEY_WOW64_32KEY);
            if (!path.empty()) cands.push_back(path);
        }
        RegCloseKey(games);
    }
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

// `pse_build data canon <file> [<out>]` / `pse_build data check <file>`: PSE data files (pse/records/data.h) -
// canon writes the canonical text (stdout without <out>), check exits 0 when the file already is canonical, 3 when
// not, 2 when it isn't a PSE data file.
int DataCommand(int argc, wchar_t** argv) {
    if (argc < 2 || (std::wcscmp(argv[0], L"canon") && std::wcscmp(argv[0], L"check"))) {
        std::fputs("usage: pse_build data canon <file> [<out>]\n       pse_build data check <file>\n", stderr);
        return 1;
    }
    FILE* f = _wfopen(argv[1], L"rb");
    if (!f) {
        std::fwprintf(stderr, L"can't read %ls\n", argv[1]);
        return 2;
    }
    std::string text;
    char buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    std::fclose(f);
    pse::data::Document doc;
    std::string out, error;
    if (!pse::data::Parse(text, &doc, &error) || !pse::data::Write(doc, &out, &error)) {
        std::fwprintf(stderr, L"%ls:", argv[1]);
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    if (!std::wcscmp(argv[0], L"check")) return out == text ? 0 : 3;
    if (argc < 3) {
        _setmode(_fileno(stdout), _O_BINARY);
        std::fwrite(out.data(), 1, out.size(), stdout);
        return 0;
    }
    FILE* o = _wfopen(argv[2], L"wb");
    if (!o || std::fwrite(out.data(), 1, out.size(), o) != out.size()) {
        if (o) std::fclose(o);
        std::fwprintf(stderr, L"can't write %ls\n", argv[2]);
        return 2;
    }
    std::fclose(o);
    return 0;
}

// `pse_build records generate --schema <file> [--game <dir>] --out <dir>`: the game's tables as records
// (<out>\<type>.lua); `pse_build records merged --schema <file> [--game <dir>] [--mods <dir>] --out <dir>`: the
// tables with every switched-on mod's record edits (<out>\<TABLE>.TXT + report.txt). pse/records/forms.h.
std::string ReadAllW(const std::wstring& path, bool* ok) {
    std::string s;
    *ok = false;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return s;
    char buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) s.append(buf, n);
    std::fclose(f);
    *ok = true;
    return s;
}
bool WriteAllW(const std::wstring& path, const std::string& s) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = std::fwrite(s.data(), 1, s.size(), f) == s.size();
    std::fclose(f);
    return ok;
}

// One file by name from the game's archives (catalog.dat first, then the other .dat files of the game folder and
// PSE\Originals by name - as DesertStormFix's ReadGameFile): only each archive's index is read.
bool ReadGameArchiveFile(const std::wstring& game, const std::string& name, std::string* out) {
    std::vector<std::wstring> dats;
    for (const std::wstring& dir : {game, pse::build::OriginalsDir(game)}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*.dat").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::vector<std::wstring> names;
        do names.push_back(fd.cFileName);
        while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
            const bool ca = !_wcsicmp(a.c_str(), L"catalog.dat"), cb = !_wcsicmp(b.c_str(), L"catalog.dat");
            return ca != cb ? ca : _wcsicmp(a.c_str(), b.c_str()) < 0;
        });
        for (const std::wstring& n : names) dats.push_back(dir + L"\\" + n);
    }
    const uint32_t hash = pse::build::Ds1NameHash(name.c_str());
    for (const std::wstring& path : dats) {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) continue;
        std::vector<uint32_t> index(0x8000 / 4);
        const size_t got = std::fread(index.data(), 1, 0x8000, f) / 4;
        for (size_t i = 0; i + 2 < got; i += 3) {
            if (!index[i] && !index[i + 1] && !index[i + 2]) break;
            if (index[i] != hash) continue;
            out->assign(index[i + 2], '\0');
            const bool ok = !_fseeki64(f, index[i + 1], SEEK_SET) && std::fread(out->data(), 1, out->size(), f) == out->size();
            std::fclose(f);
            return ok;
        }
        std::fclose(f);
    }
    return false;
}

int RecordsCommand(int argc, wchar_t** argv) {
    if (argc < 1 || (std::wcscmp(argv[0], L"generate") && std::wcscmp(argv[0], L"merged"))) {
        std::fputs("usage: pse_build records generate --schema <file> [--game <dir>] --out <dir>\n"
                   "       pse_build records merged --schema <file> [--game <dir>] [--mods <dir>] --out <dir>\n",
                   stderr);
        return 1;
    }
    const bool merged = !std::wcscmp(argv[0], L"merged");
    std::wstring schemaPath, game, mods, out;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::wcscmp(argv[i], L"--schema")) schemaPath = argv[i + 1];
        else if (!std::wcscmp(argv[i], L"--game")) game = argv[i + 1];
        else if (!std::wcscmp(argv[i], L"--mods")) mods = argv[i + 1];
        else if (!std::wcscmp(argv[i], L"--out")) out = argv[i + 1];
    }
    if (game.empty()) game = FindGame();
    if (mods.empty()) mods = game + L"\\Mods";
    bool ok = false;
    pse::data::Document schema;
    std::string error;
    const std::string schemaText = ReadAllW(schemaPath, &ok);
    if (!ok || !pse::data::Parse(schemaText, &schema, &error)) {
        std::fprintf(stderr, "schema: %s\n", ok ? error.c_str() : "can't read it");
        return 2;
    }
    std::wstring catalog = game + L"\\catalog.dat";
    if (GetFileAttributesW(catalog.c_str()) == INVALID_FILE_ATTRIBUTES)
        catalog = pse::build::OriginalsDir(game) + L"\\catalog.dat";   // moved by DesertStormFix
    const std::string cat = ReadAllW(catalog, &ok);
    std::vector<pse::build::ArchiveEntry> entries;
    uint32_t bad = 0;
    if (!ok || !pse::build::Ds1ParseArchive(reinterpret_cast<const uint8_t*>(cat.data()), cat.size(), &entries, &bad)) {
        std::fputs("catalog.dat not found / not an archive\n", stderr);
        return 2;
    }
    pse::records::TableFiles tables;
    for (const pse::data::Entry& t : schema.root.Get("tables")->entries) {
        const pse::data::Value* f = t.value->Get("file");
        if (!f || f->kind != pse::data::Kind::String) continue;
        const uint32_t h = pse::build::Ds1NameHash(f->s.c_str());
        for (const pse::build::ArchiveEntry& e : entries)
            if (e.nameHash == h) {
                tables[f->s] = cat.substr(e.offset, e.size);
                break;
            }
    }
    pse::records::Forms forms;
    if (!pse::records::Generate(tables, schema.root, &forms, &error)) {
        std::fprintf(stderr, "generate: %s\n", error.c_str());
        return 2;
    }
    CreateDirectoryW(out.c_str(), nullptr);
    if (!merged) {
        for (const auto& [type, doc] : forms) {
            std::string text;
            if (!pse::data::Write(doc, &text, &error)) return std::fprintf(stderr, "%s\n", error.c_str()), 2;
            WriteAllW(out + L"\\" + std::wstring(type.begin(), type.end()) + L".lua", text);
        }
        std::printf("%zu record files\n", forms.size());
        return 0;
    }
    // the switched-on mods' records\*.lua, mods in folder-name order (case-insensitive), files likewise
    std::vector<pse::records::Edits> edits;
    auto listSorted = [](const std::wstring& pattern, bool dirs) {
        std::vector<std::wstring> names;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return names;
        do {
            const bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (dir == dirs && std::wcscmp(fd.cFileName, L".") && std::wcscmp(fd.cFileName, L".."))
                names.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
            std::wstring x = a, y = b;
            for (auto& c : x) c = static_cast<wchar_t>(towlower(c));
            for (auto& c : y) c = static_cast<wchar_t>(towlower(c));
            return x < y;
        });
        return names;
    };
    auto narrow = [](const std::wstring& w) {
        std::string s(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr)), 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), static_cast<int>(s.size()), nullptr, nullptr);
        s.pop_back();
        return s;
    };
    std::vector<std::string> report;
    for (const std::wstring& mod : listSorted(mods + L"\\*", true)) {
        if (mod[0] == L'_' || mod[0] == L'.') continue;
        const std::wstring folder = mods + L"\\" + mod + L"\\records";
        for (const std::wstring& file : listSorted(folder + L"\\*.lua", false)) {
            pse::records::Edits e;
            e.mod = narrow(mod);
            e.file = narrow(file);
            const std::string text = ReadAllW(folder + L"\\" + file, &ok);
            if (!ok || !pse::data::Parse(text, &e.doc, &error)) {
                report.push_back("[fail] " + e.mod + "/" + e.file + ": " + (ok ? error : "can't read it"));
                continue;
            }
            edits.push_back(std::move(e));
        }
    }
    pse::records::Merge(&forms, edits, schema.root, &report);
    for (const auto& [file, bytes] : pse::records::Tables(forms, schema.root))
        WriteAllW(out + L"\\" + std::wstring(file.begin(), file.end()), bytes);
    // placed references: each level's scene with the edits (<out>\<LEVEL>.ENV, placed.h)
    for (const std::string& level : pse::records::PlacedLevels(edits)) {
        std::string name = level;
        for (char& c : name)
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        name += ".ENV";
        std::string scene;
        if (!ReadGameArchiveFile(game, name, &scene)) {
            report.push_back("[fail] no scene " + name);
            continue;
        }
        const std::string text = pse::records::ApplyPlaced(scene, level, edits, forms, &report);
        WriteAllW(out + L"\\" + std::wstring(name.begin(), name.end()), text);
    }
    std::string rep;
    for (const std::string& l : report) rep += l + "\n";
    WriteAllW(out + L"\\report.txt", rep);
    std::printf("%zu edit file(s), %zu report line(s)\n", edits.size(), report.size());
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    // `pse_build missions ...`: the mission translator (docs/internal/mission-translation.md "C++ port")
    if (argc > 1 && std::wcscmp(argv[1], L"missions") == 0) return pse::m2l::MissionsCommand(argc - 2, argv + 2, &FindGame);
    if (argc > 1 && std::wcscmp(argv[1], L"data") == 0) return DataCommand(argc - 2, argv + 2);
    if (argc > 1 && std::wcscmp(argv[1], L"records") == 0) return RecordsCommand(argc - 2, argv + 2);
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
