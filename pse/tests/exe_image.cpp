// DesertStorm.exe (or, for the DS2 tests, CDS II.exe) mapped at 0x400000 inside pse_tests.exe (see exe_image.h). The
// exes have no relocations, so they can only live at their image base - one at a time (MapGame switches); pse_tests.exe is linked at 0x10000000 to keep that range free, and the range
// is reserved by a static initializer (or, when Windows already put something there, in a restarted copy).
#include "exe_image.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
// Statics of this file (the 0x400000 reservation first) are built before the other test files' objects.
#pragma warning(push)
#pragma warning(disable : 4073)  // initializers put in library initialization area
#pragma init_seg(lib)
#pragma warning(pop)

namespace exe {
namespace {
// The exes the image tests can map (MapGame). Each: game id, known SHA-256 (4 GB flag cleared), env override,
// install paths tried in order.
struct GameExe {
    const char* id;
    const char* sha256;
    const char* env;
    const char* paths[3];
};
constexpr GameExe kGames[] = {
    {"ds1", "D712F5EC82C14DA66B098332B3CC9C99689091F46317112EA35DFD393DC3E164", "PSE_GAME_EXE",
     {"C:\\Program Files (x86)\\Steam\\steamapps\\common\\conflict_desert_storm\\DesertStorm.exe",
      "C:\\GOG Games\\Conflict - Desert Storm\\DesertStorm.EXE", nullptr}},
    {"ds2", "E9B46420DC00B13C3C48A9E9B7B3FDA29458D5EA83AC0D203F5B68FDD111542A", "PSE_GAME_EXE_DS2",
     {nullptr, nullptr, nullptr}},
};
constexpr size_t kGameCount = sizeof kGames / sizeof kGames[0];

// Range 0x400000 .. 0x9A6000 (the larger SizeOfImage: DS2 0x5A6000, DS1 0x403000), reserved at start-up; Map commits it.
constexpr uint32_t kReserveSize = 0x5A6000;
void* g_reserved = nullptr;
DWORD g_reserveError = 0;

int g_relaunchTries = 0;
constexpr int kMaxRelaunchTries = 8;
constexpr const wchar_t* kChildEnv = L"PSE_IMAGE_CHILD";

// By the time any of our code runs, Windows has put the stack, heaps and NLS mappings somewhere in low memory -
// often inside 0x400000..0x803000 (about half of all starts). Then pse_tests starts itself again suspended,
// reserves the range in the child before it runs a single instruction, and passes the child's exit code on.
bool RelaunchWithReservation() {
    if (GetEnvironmentVariableW(kChildEnv, nullptr, 0) || IsDebuggerPresent()) return false;
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return false;
    SetEnvironmentVariableW(kChildEnv, L"1");
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    HANDLE std3[3] = {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)};
    for (HANDLE h : std3)
        if (h && h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    si.hStdInput = std3[0];
    si.hStdOutput = std3[1];
    si.hStdError = std3[2];
    for (g_relaunchTries = 1; g_relaunchTries <= kMaxRelaunchTries; ++g_relaunchTries) {
        std::wstring cmd = GetCommandLineW();
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(exePath, cmd.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
            break;
        if (VirtualAllocEx(pi.hProcess, Ptr(kImageBase), kReserveSize, MEM_RESERVE, PAGE_NOACCESS)) {
            ResumeThread(pi.hThread);
            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            ExitProcess(code);
        }
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    SetEnvironmentVariableW(kChildEnv, nullptr);
    return false;
}

struct EarlyReserve {
    EarlyReserve() {
        g_reserved = VirtualAlloc(Ptr(kImageBase), kReserveSize, MEM_RESERVE, PAGE_NOACCESS);
        if (g_reserved) return;
        g_reserveError = GetLastError();
        MEMORY_BASIC_INFORMATION mbi;
        if (GetEnvironmentVariableW(kChildEnv, nullptr, 0) && VirtualQuery(Ptr(kImageBase), &mbi, sizeof mbi) &&
            mbi.State == MEM_RESERVE && mbi.AllocationBase == Ptr(kImageBase) && mbi.RegionSize >= kReserveSize) {
            g_reserved = mbi.AllocationBase;  // reserved for us by the parent
            return;
        }
        RelaunchWithReservation();  // only returns when that failed too
    }
} g_earlyReserve;

// What occupies the image range (for the skip message when the reservation failed).
std::string RangeUsers() {
    std::string out;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = kImageBase; a < kImageBase + kReserveSize && out.size() < 300;
         a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_FREE) continue;
        char b[96];
        std::snprintf(b, sizeof b, " [%p +0x%zX %s]", mbi.BaseAddress, mbi.RegionSize,
                      mbi.Type == MEM_IMAGE ? "image" : mbi.Type == MEM_MAPPED ? "mapped" : "private");
        out += b;
    }
    return out;
}

struct Section {
    std::string name;
    uint32_t va, vsize, rawOffset, rawSize, characteristics;
};

struct Import {
    uint32_t slot;
    std::string name;
};

struct GameState {
    bool tried = false, ok = false;   // tried and mapped once (it may have been swapped out for another game since)
    std::string why;
};
GameState g_games[kGameCount];
const GameExe* g_current = nullptr;   // the game whose exe is in the range now
bool g_mapped = false;
std::string g_why, g_path;
std::vector<uint8_t> g_file;
std::vector<Section> g_sections;
std::vector<Import> g_imports;
std::deque<std::string> g_importNames;  // stable c_str() for the stubs
uint8_t* g_stubs = nullptr;
uint32_t g_sizeOfHeaders = 0, g_sizeOfImage = 0;

std::string Sha256Hex(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    uint8_t digest[32] = {};
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return out;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        if (BCryptHashData(h, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) == 0 &&
            BCryptFinishHash(h, digest, sizeof digest, 0) == 0) {
            char b[3];
            for (uint8_t d : digest) {
                std::snprintf(b, sizeof b, "%02X", d);
                out += b;
            }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>* out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out->resize(n > 0 ? static_cast<size_t>(n) : 0);
    bool ok = n > 0 && std::fread(out->data(), 1, out->size(), f) == out->size();
    std::fclose(f);
    return ok;
}

bool FileExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ---- IAT traps ----
extern "C" void __stdcall PseImportTrapRaise(const char* name, uint32_t caller) {
    ULONG_PTR args[2] = {reinterpret_cast<ULONG_PTR>(name), caller};
    RaiseException(kImportTrapCode, EXCEPTION_NONCONTINUABLE, 2, args);
}
// Stub i: mov eax, <name>; jmp TrapCommon. TrapCommon: caller = [esp] (the game's call return address).
__declspec(naked) void TrapCommon() {
    __asm {
        push dword ptr [esp]
        push eax
        call PseImportTrapRaise
        int 3
    }
}

void FillImports() {
    for (size_t i = 0; i < g_imports.size(); ++i) *static_cast<uint32_t*>(Ptr(g_imports[i].slot)) =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_stubs + i * 10));
}

bool BuildImports(const IMAGE_NT_HEADERS32* nt, std::string* why) {
    auto rvaToFile = [&](uint32_t rva) -> const uint8_t* {
        for (const auto& s : g_sections) {
            uint32_t off = rva - (s.va - kImageBase);
            if (rva >= s.va - kImageBase && off < s.rawSize && s.rawOffset + off < g_file.size())
                return g_file.data() + s.rawOffset + off;
        }
        return nullptr;
    };
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return true;
    for (uint32_t d = dir.VirtualAddress;; d += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(rvaToFile(d));
        if (!desc) { *why = "import directory outside the file"; return false; }
        if (!desc->Name && !desc->FirstThunk) break;
        auto* dll = reinterpret_cast<const char*>(rvaToFile(desc->Name));
        std::string dllName = dll ? dll : "?";
        uint32_t names = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        for (uint32_t k = 0;; ++k) {
            auto* t = reinterpret_cast<const uint32_t*>(rvaToFile(names + k * 4));
            if (!t || !*t) break;
            std::string fn;
            if (*t & IMAGE_ORDINAL_FLAG32) {
                fn = "#" + std::to_string(*t & 0xFFFF);
            } else {
                auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(rvaToFile(*t));
                fn = ibn ? reinterpret_cast<const char*>(ibn->Name) : "?";
            }
            g_imports.push_back({kImageBase + desc->FirstThunk + k * 4, dllName + "!" + fn});
        }
    }
    g_stubs = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, g_imports.size() * 10 + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_stubs) { *why = "can't allocate the import trap stubs"; return false; }
    for (size_t i = 0; i < g_imports.size(); ++i) {
        g_importNames.push_back(g_imports[i].name);
        uint8_t* p = g_stubs + i * 10;
        uint32_t name = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_importNames.back().c_str()));
        p[0] = 0xB8;
        std::memcpy(p + 1, &name, 4);
        int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(&TrapCommon) - reinterpret_cast<intptr_t>(p + 10));
        p[5] = 0xE9;
        std::memcpy(p + 6, &rel, 4);
    }
    FlushInstructionCache(GetCurrentProcess(), g_stubs, g_imports.size() * 10);
    return true;
}

void CopySections() {
    std::memcpy(Ptr(kImageBase), g_file.data(), g_sizeOfHeaders);
    for (const auto& s : g_sections) {
        uint8_t* dst = static_cast<uint8_t*>(Ptr(s.va));
        uint32_t n = s.rawSize < s.vsize ? s.rawSize : s.vsize;
        if (s.rawOffset + n > g_file.size()) n = static_cast<uint32_t>(g_file.size() - s.rawOffset);
        std::memcpy(dst, g_file.data() + s.rawOffset, n);
        std::memset(dst + n, 0, s.vsize - n);  // plugin code caves (0x5D7B00, 0x5E5CB0) live here
    }
}

// Takes the current game's exe out of the range (decommit, forget its sections / imports), so another can go in.
void Unmap() {
    if (!g_current) return;
    VirtualFree(Ptr(kImageBase), kReserveSize, MEM_DECOMMIT);
    if (g_stubs) VirtualFree(g_stubs, 0, MEM_RELEASE);
    g_stubs = nullptr;
    g_sections.clear();
    g_imports.clear();
    g_importNames.clear();
    g_file.clear();
    g_current = nullptr;
    g_mapped = false;
}

bool DoMap(const GameExe& game, std::string* why) {
    std::string path;
    if (const char* env = std::getenv(game.env); env && *env) {
        path = env;
        if (!FileExists(path)) { *why = std::string(game.env) + "=" + path + " does not exist"; return false; }
    } else {
        for (const char* p : game.paths)
            if (p && FileExists(p)) {
                path = p;
                break;
            }
        if (path.empty()) {
            *why = std::string(game.id) + " exe not found (set " + game.env + ")";
            return false;
        }
    }
    std::vector<uint8_t> file;
    if (!ReadWholeFile(path, &file)) { *why = "can't read " + path; return false; }
    if (file.size() > 0x40) {   // the 4 GB patcher's bit (Characteristics 0x20): hash + map the original
        uint32_t lfanew = 0;
        std::memcpy(&lfanew, &file[0x3C], 4);
        if (static_cast<size_t>(lfanew) + 4 + 20 <= file.size()) file[lfanew + 4 + 18] &= ~0x20;
    }
    std::string sha = Sha256Hex(file);
    if (sha != game.sha256) { *why = path + " is not the known build (SHA-256 " + sha + ")"; return false; }
    Unmap();   // another game's exe out first
    g_path = path;
    g_file = std::move(file);
    g_current = &game;

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_file.data());
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(g_file.data() + dos->e_lfanew);
    if (nt->OptionalHeader.ImageBase != kImageBase) { *why = "unexpected image base"; return false; }
    g_sizeOfImage = nt->OptionalHeader.SizeOfImage;
    g_sizeOfHeaders = nt->OptionalHeader.SizeOfHeaders;
    if (g_sizeOfImage > kReserveSize) { *why = "SizeOfImage larger than the reservation"; return false; }
    auto* sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char n[9] = {};
        std::memcpy(n, sh[i].Name, 8);
        uint32_t vsize = sh[i].Misc.VirtualSize ? sh[i].Misc.VirtualSize : sh[i].SizeOfRawData;
        vsize = (vsize + 0xFFF) & ~0xFFFu;
        g_sections.push_back({n, kImageBase + sh[i].VirtualAddress, vsize, sh[i].PointerToRawData, sh[i].SizeOfRawData,
                              sh[i].Characteristics});
    }

    if (!g_reserved) {
        char b[96];
        std::snprintf(b, sizeof b, "0x400000 could not be reserved (error %lu, %d relaunch tries); in use:",
                      g_reserveError, g_relaunchTries > kMaxRelaunchTries ? kMaxRelaunchTries : g_relaunchTries);
        *why = b + RangeUsers();
        return false;
    }
    if (!VirtualAlloc(Ptr(kImageBase), g_sizeOfImage, MEM_COMMIT, PAGE_READWRITE)) {
        *why = "can't commit the image range (error " + std::to_string(GetLastError()) + ")";
        g_current = nullptr;
        return false;
    }
    CopySections();
    if (!BuildImports(nt, why)) return false;
    FillImports();
    // Protections: headers read-only; every section read/write/execute. The exe's own section flags are not
    // usable as-is (.text is 0xC0000040: no execute bit - the original loader tolerated it, DEP doesn't), and tests
    // write game globals / the plugin's caves.
    DWORD old = 0;
    VirtualProtect(Ptr(kImageBase), g_sizeOfHeaders, PAGE_READONLY, &old);
    for (const auto& s : g_sections) VirtualProtect(Ptr(s.va), s.vsize, PAGE_EXECUTE_READWRITE, &old);
    FlushInstructionCache(GetCurrentProcess(), Ptr(kImageBase), g_sizeOfImage);
    return true;
}

// ---- guarded calls ----
// GuardAsm saves the callee-saved registers, esp and the SEH chain head; on an exception the vectored handler
// resumes at Landing, which puts them back and returns 0 from GuardAsm.
uint32_t g_savedEsp = 0, g_savedSeh = 0;
volatile bool g_inGuard = false;
Fault* g_fault = nullptr;
PVOID g_veh = nullptr;

__declspec(naked) int __cdecl GuardAsm(void(__cdecl*)(void*), void*) {
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        mov eax, fs:[0]
        mov g_savedSeh, eax
        mov g_savedEsp, esp
        push dword ptr [esp + 24]   ; ctx
        call dword ptr [esp + 24]   ; fn (shifted by the push)
        add esp, 4
        mov eax, 1
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}
// Puts back the SEH chain head saved by GuardAsm (not a new handler).
#pragma warning(push)
#pragma warning(disable : 4733)
__declspec(naked) void Landing() {
    __asm {
        mov esp, g_savedEsp
        mov eax, g_savedSeh
        mov fs:[0], eax
        cld
        fninit
        xor eax, eax
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}
#pragma warning(pop)

LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    if (!g_inGuard) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == DBG_PRINTEXCEPTION_C || code == 0x4001000A /* DBG_PRINTEXCEPTION_WIDE_C */ || code == 0x406D1388)
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_fault) {
        Fault& f = *g_fault;
        f.code = code;
        f.address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress));
        if (code == kImportTrapCode && ep->ExceptionRecord->NumberParameters >= 2) {
            f.import = reinterpret_cast<const char*>(ep->ExceptionRecord->ExceptionInformation[0]);
            f.caller = static_cast<uint32_t>(ep->ExceptionRecord->ExceptionInformation[1]);
        } else if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            f.dataAddress = static_cast<uint32_t>(ep->ExceptionRecord->ExceptionInformation[1]);
        }
    }
    g_inGuard = false;
    ep->ContextRecord->Eip = static_cast<DWORD>(reinterpret_cast<uintptr_t>(&Landing));
    return EXCEPTION_CONTINUE_EXECUTION;
}
}  // namespace

bool MapGame(const char* gameId, std::string* whyNot) {
    size_t i = 0;
    while (i < kGameCount && std::strcmp(kGames[i].id, gameId) != 0) ++i;
    if (i == kGameCount) {
        if (whyNot) *whyNot = std::string("no exe known for game ") + gameId;
        return false;
    }
    GameState& s = g_games[i];
    if (g_current == &kGames[i] && g_mapped) return true;
    if (s.tried && !s.ok) {   // missing / not the known build: not read again
        if (whyNot) *whyNot = s.why;
        return false;
    }
    s.tried = true;
    std::string why;
    const bool ok = DoMap(kGames[i], &why);
    s.ok = ok;
    g_mapped = ok && g_current == &kGames[i];
    if (!ok) {
        s.why = why;
        g_why = why;
        if (whyNot) *whyNot = why;
    }
    return ok;
}
bool Map(std::string* whyNot) { return MapGame("ds1", whyNot); }
bool Mapped() { return g_mapped; }
const char* MappedGame() { return g_mapped && g_current ? g_current->id : ""; }
const std::string& Path() { return g_path; }

void Restore() {
    if (!g_mapped) return;
    DWORD old = 0;
    VirtualProtect(Ptr(kImageBase), g_sizeOfHeaders, PAGE_READWRITE, &old);
    CopySections();
    FillImports();
    VirtualProtect(Ptr(kImageBase), g_sizeOfHeaders, PAGE_READONLY, &old);
    FlushInstructionCache(GetCurrentProcess(), Ptr(kImageBase), g_sizeOfImage);
}

bool SectionOf(uint32_t va, std::string* name, uint32_t* start, uint32_t* size) {
    for (const auto& s : g_sections) {
        if (va >= s.va && va - s.va < s.vsize) {
            if (name) *name = s.name;
            if (start) *start = s.va;
            if (size) *size = s.vsize;
            return true;
        }
    }
    return false;
}

bool OriginalBytes(uint32_t va, void* out, size_t len) {
    auto* o = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < len; ++i) {
        uint32_t a = va + static_cast<uint32_t>(i);
        const Section* sec = nullptr;
        for (const auto& s : g_sections)
            if (a >= s.va && a - s.va < s.vsize) sec = &s;
        if (!sec) return false;
        uint32_t off = a - sec->va;
        o[i] = off < sec->rawSize && sec->rawOffset + off < g_file.size() ? g_file[sec->rawOffset + off] : 0;
    }
    return true;
}

std::string ImportName(uint32_t slot) {
    for (const auto& i : g_imports)
        if (i.slot == slot) return i.name;
    return {};
}
size_t ImportCount() { return g_imports.size(); }

std::string Fault::Describe() const {
    char b[160];
    if (code == kImportTrapCode)
        std::snprintf(b, sizeof b, "called import %s (from 0x%08X)", import.c_str(), caller);
    else if (code == EXCEPTION_ACCESS_VIOLATION)
        std::snprintf(b, sizeof b, "access violation at 0x%08X (address 0x%08X)", address, dataAddress);
    else
        std::snprintf(b, sizeof b, "exception 0x%08X at 0x%08X", code, address);
    return b;
}

bool Guard(void(__cdecl* fn)(void*), void* ctx, Fault* fault) {
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Veh);
    if (g_inGuard) {  // not re-entrant
        if (fault) fault->code = 0xFFFFFFFF;
        return false;
    }
    Fault local;
    g_fault = fault ? fault : &local;
    *g_fault = Fault{};
    g_inGuard = true;
    int ok = GuardAsm(fn, ctx);
    g_inGuard = false;
    g_fault = nullptr;
    return ok != 0;
}
}  // namespace exe
