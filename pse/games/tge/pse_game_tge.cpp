// pse.game.tge v1 (pse/include/pse/pse_game_tge.h, docs/sdk/reference/game-tge.md): read-only wrappers over The Great
// Escape's state, served by the built-in plugin "pse.game.tge". Game memory, the loader and Windows only. Part of the
// game layer PSE\Games\pse_tge.dll.
// Addresses: the tge ID database (pse/ids/tge.ids.txt) - Main_DispatchMode, Level_TableIndex, Window_Handle.
#include "pse_game_tge.h"

#include <windows.h>

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "loader.h"

namespace {
constexpr uint32_t kDispatchMode = 0x66A69C;   // u8: main loop dispatch
constexpr uint32_t kLevelTableIndex = 0x7DDAEC; // i32
constexpr uint32_t kGameWindow = 0x7DC238;     // HWND
constexpr char kKnownSha256[] = "50F4A87EA7F370BDB0F41E029351B07B752E4042B26553D1CECE0EB577702CDF";

std::string g_level;   // set by the events (tge_events.cpp), game thread

bool Peek(uint32_t va, void* out, size_t n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(va)), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uint32_t Peek32(uint32_t va) {
    uint32_t v = 0;
    return Peek(va, &v, 4) ? v : 0;
}

uint32_t CopyOut(const std::string& s, char* buf, uint32_t bufLen) {
    if (buf && bufLen) {
        const size_t n = s.size() < bufLen - 1 ? s.size() : bufLen - 1;
        std::memcpy(buf, s.data(), n);
        buf[n] = 0;
    }
    return static_cast<uint32_t>(s.size());
}

uint32_t PSE_CALL DispatchMode() {
    uint8_t m = 0;
    return Peek(kDispatchMode, &m, 1) ? m : 0;
}
int32_t PSE_CALL LevelTableIndex() {
    int32_t i = -1;
    return Peek(kLevelTableIndex, &i, 4) ? i : -1;
}
uint32_t PSE_CALL LevelName(char* buf, uint32_t bufLen) { return CopyOut(g_level, buf, bufLen); }
uint32_t PSE_CALL LevelKind() { return pse_game_tge::KindOf(g_level.c_str()); }
void* PSE_CALL GameWindow() { return reinterpret_cast<void*>(static_cast<uintptr_t>(Peek32(kGameWindow))); }
const std::string& ExeHash() {
    static const std::string hash = [] {
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        return n && n < MAX_PATH ? pse_game_tge::ExeFileSha256(path) : std::string();
    }();
    return hash;
}
const char* PSE_CALL ExeSha256() { return ExeHash().c_str(); }
int PSE_CALL ExeKnown() { return ExeHash() == kKnownSha256 ? 1 : 0; }

const PSEGameTgeV1 kTable = {
    sizeof(PSEGameTgeV1), 1, &DispatchMode, &LevelTableIndex, &LevelName, &LevelKind, &GameWindow, &ExeSha256, &ExeKnown,
};
}  // namespace

const PSEPluginInfo pse_game_tge::kPluginInfo = {sizeof(PSEPluginInfo), 1, PSE_GAME_TGE_PLUGIN,
                                                 "The Great Escape game interface", "PSE", PSE_MAKE_VERSION(1, 0, 0),
                                                 PSE_API_VERSION, nullptr, PSE_PLUGIN_CLIENT_ONLY};

const PSEGameTgeV1* pse_game_tge::Table() { return &kTable; }
const char* pse_game_tge::KnownSha256() { return kKnownSha256; }

PSEPluginHandle pse_game_tge::Register() {
    PSEPluginHandle h = pse::FindPlugin(PSE_GAME_TGE_PLUGIN);
    if (!h) h = pse::RegisterInternalPlugin(&kPluginInfo);
    if (h) pse::MessagingInterface()->RegisterInterface(h, PSE_GAME_TGE_INTERFACE, 1, &kTable);
    return h;
}

void pse_game_tge::SetLevel(const char* name) { g_level = name ? name : ""; }

uint32_t pse_game_tge::KindOf(const char* level) {
    if (!level || !*level) return PSE_TGE_LEVEL_NONE;
    if (_stricmp(level, "FE") == 0) return PSE_TGE_LEVEL_FRONTEND;
    const size_t n = std::strlen(level);
    if (n > 4 && _stricmp(level + n - 4, ".dll") == 0) return PSE_TGE_LEVEL_MISSION;
    return PSE_TGE_LEVEL_SCREEN;
}

std::string pse_game_tge::ExeFileSha256(const wchar_t* path) {
    const HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    LARGE_INTEGER size{};
    std::vector<uint8_t> data;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0x40 && size.QuadPart < (64 << 20)) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr) || got != data.size()) data.clear();
    }
    CloseHandle(f);
    if (data.size() < 0x40) return "";
    uint32_t lfanew = 0;   // IMAGE_FILE_HEADER.Characteristics = e_lfanew + 4 + 18: the LAA bit doesn't count
    std::memcpy(&lfanew, &data[0x3C], 4);
    if (static_cast<uint64_t>(lfanew) + 4 + 20 <= data.size()) data[lfanew + 4 + 18] &= static_cast<uint8_t>(~0x20);
    std::string hex = pse::compat::Sha256Hex(data.data(), data.size());
    for (char& c : hex) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return hex;
}
