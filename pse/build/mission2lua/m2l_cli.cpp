// `pse_build missions` - the mission translator's command line (docs/internal/mission-translation.md "C++ port").
//
//   pse_build missions [--game <dir>] [--out <dir>] [--only <Mission1,Training2,...>] [--check] [--force] [--quiet]
//       every Mission*.dll / Training*.dll of the install -> <out>\<stem>.lua (+ .stamp); default out = the missions
//       cache (<game>\PSE\Cache\missions or %LOCALAPPDATA%\PSE\ds1-<hash>\missions). Up-to-date files are kept unless
//       --force. --check: compile every result with Lua 5.4 and print the coverage table (like ds_mission2lua.py).
//   pse_build missions --dump <dll> <out.txt>
//       the decoder's view of every byte offset of the DLL's .text (capstone's format; compared with the Python
//       reference dump by tools/m2l_gate.py --decode).
// Output is game code generated from the player's files: never commit or distribute it.
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "../../../third_party/lua/lauxlib.h"
#include "../../../third_party/lua/lua.h"
}

#include "m2l_program.h"
#include "mission2lua.h"

namespace pse::m2l {

namespace {

std::string N(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
    return s;
}

std::string LuaCompile(const std::string& src, const std::string& chunk) {
    lua_State* L = luaL_newstate();
    if (!L) return "no Lua state";
    std::string err;
    if (luaL_loadbuffer(L, src.data(), src.size(), chunk.c_str()) != LUA_OK) err = lua_tostring(L, -1);
    lua_close(L);
    return err;
}

int Dump(const std::wstring& dllPath, const std::wstring& outPath) {
    std::vector<uint8_t> raw;
    if (!ReadFileBytes(dllPath, &raw)) {
        std::fprintf(stderr, "can't read %s\n", N(dllPath).c_str());
        return 2;
    }
    Dll d;
    std::string err;
    if (!d.Parse(raw, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    FILE* f = _wfopen(outPath.c_str(), L"wb");
    if (!f) return 2;
    for (uint32_t va = d.textLo; va < d.textHi; ++va) {
        const Insn* i = d.At(va);
        if (!i) std::fprintf(f, "%08X -\n", va);
        else std::fprintf(f, "%08X %s\n", va, DumpLine(*i).c_str());
    }
    std::fclose(f);
    return 0;
}

}  // namespace

int MissionsCommand(int argc, wchar_t** argv, std::wstring (*findGame)()) {
    std::wstring game, out, only;
    bool check = false, force = false, quiet = false;
    for (int i = 0; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--game") game = next();
        else if (a == L"--out") out = next();
        else if (a == L"--only") only = next();
        else if (a == L"--check") check = true;
        else if (a == L"--force") force = true;
        else if (a == L"--quiet") quiet = true;
        else if (a == L"--dump") {
            const std::wstring dll = next(), to = next();
            return Dump(dll, to);
        } else {
            std::fputs("usage: pse_build missions [--game <dir>] [--out <dir>] [--only <names>] [--check] [--force] [--quiet]\n"
                       "       pse_build missions --dump <dll> <out.txt>\n",
                       stderr);
            return 1;
        }
    }
    if (game.empty()) game = findGame();
    if (game.empty()) {
        std::fputs("pse_build missions: no Conflict: Desert Storm install found (use --game <dir>)\n", stderr);
        return 1;
    }
    if (out.empty()) out = DefaultMissionsDir(game);
    if (check) g_luaCheck = LuaCompile;
    std::vector<std::wstring> dlls = MissionDlls(game);
    if (!only.empty()) {
        std::vector<std::wstring> keep;
        for (const std::wstring& p : dlls) {
            const std::wstring file = p.substr(p.find_last_of(L"\\/") + 1);
            const std::wstring stem = file.substr(0, file.find_last_of(L'.'));
            size_t at = 0;
            bool hit = false;
            while (at <= only.size()) {
                size_t end = only.find(L',', at);
                if (end == std::wstring::npos) end = only.size();
                const std::wstring want = only.substr(at, end - at);
                if (!want.empty() && (_wcsicmp(want.c_str(), stem.c_str()) == 0 || _wcsicmp(want.c_str(), file.c_str()) == 0))
                    hit = true;
                at = end + 1;
            }
            if (hit) keep.push_back(p);
        }
        dlls = keep;
    }
    if (dlls.empty()) {
        std::fprintf(stderr, "pse_build missions: no mission DLLs in %s\n", N(game).c_str());
        return 2;
    }
    if (!quiet) std::printf("mission2lua %s -> %s\n", TranslatorBuild(), N(out).c_str());
    const auto t0 = std::chrono::steady_clock::now();
    int failed = 0, made = 0;
    if (!quiet && check) std::printf("%-12s %9s %12s %7s %11s %10s %8s\n", "DLL", "functions", "instructions", "thunks",
                                     "unsupported", "Lua", "time");
    for (const std::wstring& p : dlls) {
        const CacheResult r = EnsureCached(p, out, force);
        const std::string name = N(p.substr(p.find_last_of(L"\\/") + 1));
        if (!r.ok) {
            ++failed;
            std::printf("[fail] %s: %s\n", name.c_str(), r.error.c_str());
            continue;
        }
        if (r.generated) ++made;
        if (quiet) continue;
        if (check && r.generated) {
            int unsup = 0;
            for (auto& kv : r.stats.unsupported) unsup += kv.second;
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            GetFileAttributesExW(r.luaPath.c_str(), GetFileExInfoStandard, &fa);
            std::printf("%-12s %9u %12u %7u %11d %8.1f MB %6.0f ms\n", name.substr(0, name.find('.')).c_str(),
                        r.stats.functions, r.stats.instructions, r.stats.thunks, unsup, fa.nFileSizeLow / 1048576.0,
                        r.stats.ms);
            for (const std::string& w : r.stats.warnings) std::printf("    warning: %s\n", w.c_str());
        } else {
            std::printf("%s %s (%s)\n", r.generated ? "translated" : "up to date", name.c_str(), r.sha256.substr(0, 16).c_str());
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!quiet)
        std::printf("%zu DLL(s), %d translated, %d failed, %.0f ms%s\n", dlls.size(), made, failed, ms,
                    check ? ", every result compiled with Lua 5.4" : "");
    return failed ? 2 : 0;
}

}  // namespace pse::m2l
