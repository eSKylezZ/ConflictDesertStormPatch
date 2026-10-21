// mission2lua, the C++ mission translator (pse/build/mission2lua, docs/internal/mission-translation.md "C++ port").
// Tier 1 = the decoder on single instructions (capstone's view through Zydis) and the whole pipeline on a small
// hand-made DLL; tier 2 = Mission1.dll of the real install (PSE_GAME_DIR, else the Steam / GOG path): the output is
// compared with the Python generator when Python + the ModKit are there (PSE_MODKIT_DIR, else ../ConflictDesertStormModKit),
// else with the SHA-256 the gate recorded (scripts/m2l_gate.py) - never with stored Lua (it is game code).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "../../third_party/lua/lauxlib.h"
#include "../../third_party/lua/lua.h"
}

#include "m2l_program.h"
#include "mission2lua.h"
#include "pse_test.h"

using namespace pse::m2l;

namespace {

std::vector<uint8_t> Hex(const char* h) {
    std::vector<uint8_t> out;
    for (size_t k = 0; h[k] && h[k + 1]; k += 2) {
        unsigned v = 0;
        std::sscanf(h + k, "%2x", &v);
        out.push_back(static_cast<uint8_t>(v));
    }
    return out;
}

Insn Dec(const char* hex, uint32_t va = 0x10001000) {
    const std::vector<uint8_t> b = Hex(hex);
    Insn i;
    if (!Decode(b.data(), b.size(), va, &i)) i.mnemonic = "<invalid>";
    return i;
}
std::string Text(const char* hex, uint32_t va = 0x10001000) {
    const Insn i = Dec(hex, va);
    return i.mnemonic + " " + i.op_str;
}

std::string LuaCompiles(const std::string& src) {
    lua_State* L = luaL_newstate();
    std::string err;
    if (luaL_loadbuffer(L, src.data(), src.size(), "=test") != LUA_OK) err = lua_tostring(L, -1);
    lua_close(L);
    return err;
}

void Put32(std::vector<uint8_t>& f, size_t o, uint32_t v) { std::memcpy(&f[o], &v, 4); }
void Put16(std::vector<uint8_t>& f, size_t o, uint16_t v) { std::memcpy(&f[o], &v, 2); }

// A tiny mission-like DLL: DllMain, LevelSetup with a 3-way switch (jump table in .text), a relocated callback push,
// a call to a helper; .data with the export directory; .reloc.
std::vector<uint8_t> TinyDll() {
    std::vector<uint8_t> f(0xA00, 0);
    f[0] = 'M', f[1] = 'Z';
    Put32(f, 0x3C, 0x40);
    Put32(f, 0x40, 0x4550);
    Put16(f, 0x44, 0x14C);   // i386
    Put16(f, 0x46, 3);       // sections
    Put16(f, 0x54, 0xE0);    // optional header size
    Put16(f, 0x56, 0x2102);
    const size_t o = 0x58;
    Put16(f, o, 0x10B);
    Put32(f, o + 16, 0x1000);       // entry
    Put32(f, o + 28, 0x10000000);   // image base
    Put32(f, o + 32, 0x1000);
    Put32(f, o + 36, 0x200);
    Put32(f, o + 56, 0x4000);       // size of image
    Put32(f, o + 60, 0x400);
    Put32(f, o + 92, 16);
    Put32(f, o + 96, 0x2100), Put32(f, o + 100, 0x50);        // export
    Put32(f, o + 96 + 5 * 8, 0x3000), Put32(f, o + 100 + 5 * 8, 20);   // relocations
    auto section = [&](int k, const char* name, uint32_t va, uint32_t vsize, uint32_t raw) {
        const size_t s = 0x138 + k * 40;
        std::memcpy(&f[s], name, std::strlen(name));
        Put32(f, s + 8, vsize), Put32(f, s + 12, va), Put32(f, s + 16, 0x200), Put32(f, s + 20, raw);
    };
    section(0, ".text", 0x1000, 0x96, 0x400);
    section(1, ".data", 0x2000, 0x150, 0x600);
    section(2, ".reloc", 0x3000, 20, 0x800);
    const char* code =
        "b801000000c20c00cccccccccccccccc"   // 00 DllMain: mov eax, 1; ret 0xc
        "8b44240483f802771dff24856010001068801000"   // 10 LevelSetup: mov; cmp eax, 2; ja; jmp [eax*4+table]; push cb
        "10e86600000083c404c333c0c3cccccccccc"       //    call helper; add esp, 4; ret | 2e: xor eax, eax; ret
        "b800200010c3";                              // 36 mov eax, 0x10002000; ret
    std::vector<uint8_t> c = Hex(code);
    std::memcpy(&f[0x400], c.data(), c.size());
    for (size_t k = 0x400 + c.size(); k < 0x400 + 0x60; ++k) f[k] = 0xCC;
    Put32(f, 0x460, 0x10001020), Put32(f, 0x464, 0x1000102E), Put32(f, 0x468, 0x10001036);   // jump table
    for (size_t k = 0x46C; k < 0x480; ++k) f[k] = 0xCC;
    std::vector<uint8_t> cb = Hex("b807000000c3cccccccccccccccccccc8b442404c3");   // 80 callback, 90 helper
    std::memcpy(&f[0x480], cb.data(), cb.size());
    std::memcpy(&f[0x600], "hello", 5);
    // export directory at RVA 0x2100
    Put32(f, 0x700 + 12, 0x2140), Put32(f, 0x700 + 16, 1), Put32(f, 0x700 + 20, 1), Put32(f, 0x700 + 24, 1);
    Put32(f, 0x700 + 28, 0x2128), Put32(f, 0x700 + 32, 0x212C), Put32(f, 0x700 + 36, 0x2130);
    Put32(f, 0x728, 0x1010), Put32(f, 0x72C, 0x2134), Put16(f, 0x730, 0);
    std::memcpy(&f[0x734], "LevelSetup", 10);
    std::memcpy(&f[0x740], "tiny.dll", 8);
    // relocations: page 0x1000, HIGHLOW at 0x1C (table disp), 0x21 (push cb), 0x37 (mov imm), 0x60 / 0x64 / 0x68
    Put32(f, 0x800, 0x1000), Put32(f, 0x804, 20);
    const uint16_t rel[] = {0x1C, 0x21, 0x37, 0x60, 0x64, 0x68};
    for (int k = 0; k < 6; ++k) Put16(f, 0x808 + k * 2, static_cast<uint16_t>(0x3000 | rel[k]));
    return f;
}

}  // namespace

PSE_TEST("mission2lua: Zydis decodes in capstone's terms (mnemonic, operand text, details)") {
    PSE_CHECK_STR(Text("8b442404").c_str(), "mov eax, dword ptr [esp + 4]");
    PSE_CHECK_STR(Text("8b4df0").c_str(), "mov ecx, dword ptr [ebp - 0x10]");
    PSE_CHECK_STR(Text("6afd").c_str(), "push -3");
    PSE_CHECK_STR(Text("6af6").c_str(), "push -0xa");
    PSE_CHECK_STR(Text("83f0ff").c_str(), "xor eax, 0xffffffff");   // and / or / xor: unsigned
    PSE_CHECK_STR(Text("83c6fc").c_str(), "add esi, -4");
    PSE_CHECK_STR(Text("c20400").c_str(), "ret 4");
    PSE_CHECK_STR(Text("f3a5").c_str(), "rep movsd dword ptr es:[edi], dword ptr [esi]");
    PSE_CHECK_STR(Text("f2ae").c_str(), "repne scasb al, byte ptr es:[edi]");
    PSE_CHECK_STR(Text("f3a6").c_str(), "repe cmpsb byte ptr [esi], byte ptr es:[edi]");
    PSE_CHECK_STR(Text("dfe0").c_str(), "fnstsw ax");
    PSE_CHECK_STR(Text("dcea").c_str(), "fsub st(2), st(0)");
    PSE_CHECK_STR(Text("d9c9").c_str(), "fxch st(1)");
    PSE_CHECK_STR(Text("9b").c_str(), "wait ");
    PSE_CHECK_STR(Text("dd45f8").c_str(), "fld qword ptr [ebp - 8]");
    PSE_CHECK_STR(Text("8d4c242c").c_str(), "lea ecx, [esp + 0x2c]");
    PSE_CHECK_STR(Text("64a100000000").c_str(), "mov eax, dword ptr fs:[0]");
    PSE_CHECK_STR(Text("7414", 0x1000F571).c_str(), "je 0x1000f587");
    PSE_CHECK_STR(Text("e8d3ecffff", 0x1000F5A8).c_str(), "call 0x1000e280");
    PSE_CHECK_STR(Text("0fff").c_str(), "ud0 ");                    // capstone: no ModRM
    PSE_CHECK_STR(Text("f00300").c_str(), "lock add eax, dword ptr [eax]");
    PSE_CHECK_STR(Text("8ecb").c_str(), "mov cs, ebx");
    PSE_CHECK_STR(Text("dcd1").c_str(), "<invalid> ");              // capstone refuses the fcom alias

    const Insn shl = Dec("d1e1");
    PSE_CHECK_STR(shl.op_str.c_str(), "ecx, 1");
    PSE_CHECK_EQ(shl.nops, 2);
    PSE_CHECK_EQ(shl.ops[1].size, 4);
    PSE_CHECK_EQ(shl.imm_offset, 0);
    const Insn push = Dec("6afd");
    PSE_CHECK_EQ(push.ops[0].imm, -3);
    PSE_CHECK_EQ(push.ops[0].size, 4);
    PSE_CHECK_EQ(push.imm_offset, 1);
    const Insn m = Dec("c745fc01000000");
    PSE_CHECK_EQ(m.disp_offset, 2);
    PSE_CHECK_EQ(m.imm_offset, 3);
    PSE_CHECK_EQ(m.ops[0].base, R_EBP);
    PSE_CHECK_EQ(m.ops[0].disp, -4);
    const Insn fs = Dec("64a100000000");
    PSE_CHECK_EQ(fs.ops[1].seg, R_FS);
    const Insn fx = Dec("d9c9");
    PSE_CHECK_EQ(fx.nops, 2);   // st(0), st(1) - text "st(1)"
    const Insn moffs = Dec("a080011088");
    PSE_CHECK_EQ(moffs.ops[1].disp, 0x88100180);   // moffs: unsigned (a ModRM absolute address is signed)
}

PSE_TEST("mission2lua: a small DLL - functions, blocks, switch table, thunks, Lua that compiles") {
    const std::vector<uint8_t> dll = TinyDll();
    Program p;
    std::string err;
    PSE_REQUIRE(p.Load(dll, "tiny.dll", &err));
    PSE_CHECK_EQ(p.dll.exports.count("LevelSetup"), 1u);
    PSE_CHECK_EQ(p.dll.relocs.size(), 6u);
    PSE_CHECK_EQ(p.functions.size(), 4u);   // DllMain, LevelSetup, the callback (relocated push), the helper (call)
    PSE_CHECK_EQ(p.taken.count(0x10001080), 1u);
    PSE_CHECK_EQ(p.taken.count(0x10001090), 0u);
    PSE_REQUIRE(p.functions.count(0x10001010));
    const Function& f = *p.functions[0x10001010];
    PSE_CHECK_EQ(f.blocks.size(), 5u);
    PSE_REQUIRE(f.blocks.count(0x10001019));
    const Block& sw = f.blocks.at(0x10001019);
    PSE_CHECK_EQ(static_cast<int>(sw.kind), static_cast<int>(Block::Switch));
    PSE_REQUIRE(sw.table);
    PSE_CHECK_EQ(sw.table->values.size(), 3u);
    PSE_CHECK_EQ(sw.table->values.at(1), 0x1000102Eu);
    PSE_CHECK_EQ(static_cast<int>(f.blocks.at(0x10001010).kind), static_cast<int>(Block::Cond));

    std::string lua;
    Stats st;
    PSE_REQUIRE(Translate(dll, "Tiny.dll", &lua, &st, &err));
    PSE_CHECK_EQ(st.functions, 4u);
    PSE_CHECK_EQ(st.instructions, 18u);
    PSE_CHECK_EQ(st.thunks, 3u);
    PSE_CHECK(st.unsupported.empty());
    PSE_CHECK_STR(LuaCompiles(lua).c_str(), "");
    PSE_CHECK(lua.rfind("--@archive tiny\n--@standard_anims off\n", 0) == 0);
    PSE_CHECK(lua.find("-- 4 functions, 18 instructions, 3 thunks.") != std::string::npos);
    PSE_CHECK(lua.find("F[0x10001010] = function()") != std::string::npos);
    PSE_CHECK(lua.find("if FA > FB then goto L10001036 end") != std::string::npos);    // cmp + ja, fast path
    PSE_CHECK(lua.find("if t == 0x1000102E then goto L1000102E") != std::string::npos);  // the switch
    PSE_CHECK(lua.find("x = (0x10001080+D)") != std::string::npos);                     // relocated immediate
    PSE_CHECK(lua.find("t = (r32(((EAX*4 + (0x10001060+D)) & 0xFFFFFFFF)) - D) & 0xFFFFFFFF") != std::string::npos);
    PSE_CHECK(lua.find("F[0x10001090]()") != std::string::npos);                         // the call
    PSE_CHECK(lua.find("thunks = {0x1000, 0x1010, 0x1080}") != std::string::npos);
    PSE_CHECK(lua.find("-- 1000102E xor eax, eax") != std::string::npos);
    PSE_CHECK(lua.find("{0x2000, \"68656c6c6f00") != std::string::npos);                  // .data
    PSE_CHECK(lua.find("local ENTRY, LEVELSETUP = 0x10001000, 0x10001010") != std::string::npos);
    PSE_CHECK(lua.find("Tiny.dll: DLL_PROCESS_ATTACH failed") != std::string::npos);

    // not a DLL / no LevelSetup -> refused
    std::vector<uint8_t> broken = dll;
    broken[0x734] = 'X';
    PSE_CHECK(!Translate(broken, "x.dll", &lua, &st, &err));
    PSE_CHECK(!Translate(std::vector<uint8_t>(64, 0), "x.dll", &lua, &st, &err));
}

PSE_TEST("mission2lua: missions cache - stamp keeps an up-to-date file, a changed DLL or --force redoes it") {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"pse-m2l-test-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring dllPath = dir + L"\\Tiny.dll";
    auto write = [&](const std::vector<uint8_t>& d) {
        HANDLE h = CreateFileW(dllPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD w = 0;
        WriteFile(h, d.data(), static_cast<DWORD>(d.size()), &w, nullptr);
        CloseHandle(h);
    };
    std::vector<uint8_t> dll = TinyDll();
    write(dll);
    const std::wstring out = dir + L"\\missions";
    CacheResult r = EnsureCached(dllPath, out, false);
    PSE_CHECK(r.ok && r.generated);
    r = EnsureCached(dllPath, out, false);
    PSE_CHECK(r.ok && !r.generated);
    dll[0x480 + 1] = 8;   // mov eax, 8 in the callback
    write(dll);
    r = EnsureCached(dllPath, out, false);
    PSE_CHECK(r.ok && r.generated);
    r = EnsureCached(dllPath, out, true);
    PSE_CHECK(r.ok && r.generated);
    std::vector<uint8_t> stamp;
    PSE_CHECK(ReadFileBytes(out + L"\\Tiny.stamp", &stamp));
    const std::string s(stamp.begin(), stamp.end());
    PSE_CHECK(s.find("dll " + Sha256Hex(dll)) == 0);
    PSE_CHECK(s.find(std::string("translator ") + TranslatorBuild()) != std::string::npos);
    for (const wchar_t* n : {L"\\missions\\Tiny.lua", L"\\missions\\Tiny.stamp", L"\\Tiny.dll"}) DeleteFileW((dir + n).c_str());
    RemoveDirectoryW(out.c_str());
    RemoveDirectoryW(dir.c_str());
}

// ---- tier 2: the real install ----
namespace {
std::wstring RealGame() {
    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"PSE_GAME_DIR", env, MAX_PATH)) return env;
    for (const wchar_t* d : {L"H:\\SteamLibrary\\steamapps\\common\\conflict_desert_storm",
                             L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\conflict_desert_storm",
                             L"H:\\GOG\\Games\\Conflict - Desert Storm"})
        if (GetFileAttributesW((std::wstring(d) + L"\\Mission1.dll").c_str()) != INVALID_FILE_ATTRIBUTES) return d;
    return {};
}
// The shipped Mission1.dll (Steam = GOG) and its translation by mission2lua 2 / port cpp.2, from scripts/m2l_gate.py
// (byte-identical with ModKit mission2lua.py). Update both when the translator's output changes (gate first).
constexpr const char* kMission1Dll = "258e6d8a2e7e2a0079120fa8f5f770b759835985adce34bcb60f2934ce915f67";
constexpr const char* kMission1Lua = "41fd7455020c17909322979f6f3fb9a5ef9b6f32a66de05aa05c41e81d9cd7c3";

std::wstring ModKit() {
    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"PSE_MODKIT_DIR", env, MAX_PATH)) return env;
#ifdef PSE_REPO_DIR
    std::wstring repo = L"" PSE_REPO_DIR;
    for (wchar_t& c : repo)
        if (c == L'/') c = L'\\';
    const std::wstring mk = repo.substr(0, repo.find_last_of(L'\\')) + L"\\ConflictDesertStormModKit";
    if (GetFileAttributesW((mk + L"\\conflict_ds_tools\\mission2lua.py").c_str()) != INVALID_FILE_ATTRIBUTES) return mk;
#endif
    return {};
}

// Runs the Python generator on `dll` (python on PATH, capstone + pefile installed) -> its output, or "" when it can't.
std::string PythonTranslate(const std::wstring& modkit, const std::wstring& dll, const std::wstring& outFile) {
    std::wstring cmd = L"python -I -c \"import sys; sys.path.insert(0, sys.argv[1]); "
                       L"from conflict_ds_tools import mission2lua as m; s, _ = m.translate(sys.argv[2]); "
                       L"open(sys.argv[3], 'w', encoding='utf-8', newline='\\n').write(s)\" \"" +
                       modkit + L"\" \"" + dll + L"\" \"" + outFile + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return {};
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::vector<uint8_t> out;
    if (code != 0 || !ReadFileBytes(outFile, &out)) return {};
    DeleteFileW(outFile.c_str());
    return std::string(out.begin(), out.end());
}
}  // namespace

PSE_TEST("mission2lua (real data): Mission1.dll = the Python generator's output (or its recorded hash)") {
    const std::wstring game = RealGame();
    if (game.empty()) PSE_SKIP("no install (set PSE_GAME_DIR)");
    const std::wstring path = game + L"\\Mission1.dll";
    std::vector<uint8_t> dll;
    PSE_REQUIRE(ReadFileBytes(path, &dll));
    std::string lua, err;
    Stats st;
    PSE_REQUIRE(Translate(dll, "Mission1.dll", &lua, &st, &err));
    PSE_CHECK_EQ(st.functions, 321u);
    PSE_CHECK_EQ(st.instructions, 18031u);
    PSE_CHECK_EQ(st.thunks, 92u);
    PSE_CHECK(st.unsupported.empty());
    PSE_CHECK_STR(LuaCompiles(lua).c_str(), "");
    std::printf("    Mission1.dll -> %zu bytes of Lua in %.0f ms\n", lua.size(), st.ms);
    const std::wstring modkit = ModKit();
    if (!modkit.empty()) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring outFile = std::wstring(tmp) + L"pse-m2l-py-" + std::to_wstring(GetCurrentProcessId()) + L".lua";
        const std::string py = PythonTranslate(modkit, path, outFile);
        if (!py.empty()) {
            PSE_CHECK(py == lua);
            if (py != lua) {
                size_t k = 0;
                while (k < py.size() && k < lua.size() && py[k] == lua[k]) ++k;
                std::printf("    first difference at byte %zu: py '%s' / cpp '%s'\n", k, py.substr(k, 60).c_str(),
                            lua.substr(k, 60).c_str());
            }
            std::printf("    compared with ModKit mission2lua.py: %s\n", py == lua ? "byte-identical" : "DIFFERENT");
            return;
        }
        std::printf("    (Python generator not runnable here - hash check instead)\n");
    }
    if (Sha256Hex(dll) != kMission1Dll) PSE_SKIP("Mission1.dll isn't the shipped version the hash was recorded for");
    PSE_CHECK_STR(Sha256Hex(std::vector<uint8_t>(lua.begin(), lua.end())).c_str(), kMission1Lua);
}
