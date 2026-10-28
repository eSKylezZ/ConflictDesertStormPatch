// `pse_build missions` - the mission translator's command line (docs/internal/mission-translation.md "C++ port").
//
//   pse_build missions [--game <dir>] [--out <dir>] [--only <Mission1,Training2,...>] [--check] [--force] [--quiet]
//       every Mission*.dll / Training*.dll of the install -> <out>\<stem>.lua (+ .stamp); default out = the missions
//       cache (<game>\PSE\Cache\missions or %LOCALAPPDATA%\PSE\ds1-<hash>\missions). Up-to-date files are kept unless
//       --force. --check: compile every result with Lua 5.4 and print the coverage table (like ds_mission2lua.py).
//   pse_build missions --readable [--game <dir> | --dll <file>] [--out <dir>] [--only <names>] [--check]
//       stage 2 (readable Lua, the C++ port of ModKit mission2lua_readable.py): <out>\<stem>\{mission.lua, refs.lua,
//       scripts\*.lua} + <out>\<stem>.lua, like ds_mission2lua_readable.py (scene names read from the archives next to
//       each DLL). Default out = .\generated_missions\readable. --check: compile every bundle with Lua 5.4.
//       With --dll, --patch <file.patch.lua> (repeatable) applies mission patches and writes <stem>.patched.lua.
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

bool WriteText(const std::wstring& path, const std::string& text) {
    const size_t cut = path.find_last_of(L"\\/");
    if (cut != std::wstring::npos) {
        std::wstring dir = path.substr(0, cut);
        for (size_t at = 0; (at = dir.find_first_of(L"\\/", at + 1)) != std::wstring::npos;)
            CreateDirectoryW(dir.substr(0, at).c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    return ok;
}

// --patch: the mission patches applied to the readable mission (each must apply and compile), the result in
// <out>\<stem>.patched.lua with the patches' Lua inline before start.lua (an approximation of what the game runs: there
// each patch is its own chunk in the mission's environment) - for checking a patch without the game.
int CheckPatches(const ReadableParts& parts, const std::string& dllName, const std::vector<std::wstring>& files,
                 const std::wstring& outPath) {
    std::string text = parts.bundle;
    std::string inline_;
    int failed = 0;
    for (const std::wstring& f : files) {
        MissionPatch p;
        p.path = N(f);
        p.label = N(f.substr(f.find_last_of(L"\\/") + 1));
        std::vector<uint8_t> raw;
        if (!ReadFileBytes(f, &raw)) {
            std::printf("  [fail] %s: can't read\n", p.label.c_str());
            ++failed;
            continue;
        }
        p.text = NormalizePatchText(raw);
        std::string why = LuaCompile(p.text, "@" + p.label);
        if (why.empty() && ParseMissionPatch(dllName, &p, &why) && ApplyMissionPatchEdits(&text, p, &why)) {
            std::printf("  patch %s: %zu edit(s) applied\n", p.label.c_str(), p.edits.size());
            inline_ += "do -- " + p.label + "\n" + p.text + "\nend\n";
        } else {
            if (why.empty()) why = "doesn't apply";
            std::printf("  [fail] %s: %s\n", p.label.c_str(), why.c_str());
            ++failed;
        }
    }
    if (!HandOutMissionEnv(&text)) {
        std::printf("  [fail] the bundle has no start section\n");
        return failed + 1;
    }
    const std::string marker = "_G.__pse_mission_env = _ENV";
    const size_t at = text.find(marker);
    text.insert(text.find('\n', at) + 1, std::string(MissionPatchPrelude()) + "SCENE = SCENE or {objects = {}, waypoints = {}, "
                                             "regions = {}, cameras = {}}\n" + inline_);
    const std::string err = LuaCompile(text, "=patched");
    if (!err.empty()) {
        std::printf("  [fail] the patched mission doesn't compile: %s\n", err.c_str());
        ++failed;
    }
    WriteText(outPath, text);
    std::printf("  -> %s%s\n", N(outPath).c_str(), failed ? "" : " (compiles)");
    return failed;
}

int Readable(const std::vector<std::wstring>& dlls, const std::wstring& out, bool check,
             const std::vector<std::wstring>& patches = {}) {
    int failed = 0;
    std::printf("mission2lua_readable %d (C++) -> %s\n", ReadableVersion(), N(out).c_str());
    for (const std::wstring& p : dlls) {
        const size_t cut = p.find_last_of(L"\\/");
        const std::wstring file = cut == std::wstring::npos ? p : p.substr(cut + 1);
        const std::wstring dir = cut == std::wstring::npos ? L"." : p.substr(0, cut);
        const std::wstring stem = file.substr(0, file.find_last_of(L'.'));
        std::vector<uint8_t> raw;
        if (!ReadFileBytes(p, &raw)) {
            std::printf("[fail] %s: can't read\n", N(file).c_str());
            ++failed;
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        ReadableParts parts;
        std::string err;
        if (!TranslateReadable(raw, N(file), "", SceneRefsFromGame(dir), &parts, &err)) {
            std::printf("%-12s %s\n", N(stem).c_str(), err.c_str());
            ++failed;
            continue;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const std::wstring root = out + L"\\" + stem;
        bool ok = WriteText(root + L"\\mission.lua", parts.mission) && WriteText(root + L"\\refs.lua", parts.refs) &&
                  WriteText(root + L"\\scripts\\events.lua", parts.events) &&
                  WriteText(root + L"\\scripts\\runtime.lua", parts.runtime) &&
                  WriteText(root + L"\\scripts\\start.lua", parts.start) && WriteText(root + L".lua", parts.bundle);
        std::string comp;
        if (check) comp = LuaCompile(parts.bundle, "=" + N(stem));
        if (!ok) ++failed;
        std::printf("%-12s %6.0f ms%s%s\n", N(stem).c_str(), ms, ok ? "" : "  WRITE FAILED",
                    comp.empty() ? "" : ("  LUA ERROR: " + comp).c_str());
        if (!patches.empty() && CheckPatches(parts, N(file), patches, out + L"\\" + stem + L".patched.lua")) ++failed;
    }
    std::printf("%zu DLL(s), %d failed (game code: keep it local)\n", dlls.size(), failed);
    return failed ? 2 : 0;
}

}  // namespace

int MissionsCommand(int argc, wchar_t** argv, std::wstring (*findGame)()) {
    std::wstring game, out, only, oneDll;
    std::vector<std::wstring> patches;
    bool check = false, force = false, quiet = false, readable = false;
    for (int i = 0; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--game") game = next();
        else if (a == L"--out") out = next();
        else if (a == L"--only") only = next();
        else if (a == L"--check") check = true;
        else if (a == L"--force") force = true;
        else if (a == L"--quiet") quiet = true;
        else if (a == L"--readable") readable = true;
        else if (a == L"--dll") oneDll = next();
        else if (a == L"--patch") patches.push_back(next());
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
    if (readable && !oneDll.empty())
        return Readable({oneDll}, out.empty() ? L"generated_missions\\readable" : out, check, patches);
    if (game.empty()) game = findGame();
    if (game.empty()) {
        std::fputs("pse_build missions: no Conflict: Desert Storm install found (use --game <dir>)\n", stderr);
        return 1;
    }
    if (out.empty()) out = readable ? L"generated_missions\\readable" : DefaultMissionsDir(game);
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
