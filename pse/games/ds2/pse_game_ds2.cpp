// pse.game.ds2 v1 (pse/include/pse/pse_game_ds2.h, docs/sdk/reference/game-ds2.md): read-only wrappers over DS2's
// state, served by the built-in plugin "pse.game.ds2". Game memory, the loader and Windows only. Part of the game layer
// PSE\Games\pse_ds2.dll; compiled into pse_tests as well (pse/tests/test_game_ds2.cpp).
// Addresses: the ds2 ID database (pse/ids/ds2.ids.txt) - FrontEnd_State, Main_DispatchMode, World, Split_Object,
// Window_Handle.
#include "pse_game_ds2.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "loader.h"

namespace {
constexpr uint32_t kFrontEndState = 0x63433C;  // u32: front-end state machine
constexpr uint32_t kDispatchMode = 0x627158;   // u8: main loop dispatch
constexpr uint32_t kWorld = 0x68008C;          // world object*: +0x1B8C bit 0 cinematic, +0x1BBC scene name char*
constexpr uint32_t kCinematicOffset = 0x1B8C, kSceneNameOffset = 0x1BBC;
constexpr uint32_t kLevelNameMax = 64;
constexpr uint32_t kSplit = 0x626C18;          // +0 local MP flag, +4 player count
constexpr uint32_t kGameWindow = 0x62F41C;     // HWND
constexpr char kKnownSha256[] = "E9B46420DC00B13C3C48A9E9B7B3FDA29458D5EA83AC0D203F5B68FDD111542A";
constexpr char kSecuromSha256[] = "44EF1E16170BAF3F981267C170F0671D8015CD9F8BD347B48C63B819978295EF";

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

std::string CurrentLevel() {
    const uint32_t world = Peek32(kWorld);
    const uint32_t name = world ? Peek32(world + kSceneNameOffset) : 0;
    if (!name) return "";
    char text[kLevelNameMax + 1] = {};
    // Byte by byte: the name may sit at the end of a readable region.
    uint32_t n = 0;
    while (n < kLevelNameMax && Peek(name + n, &text[n], 1) && text[n]) ++n;
    text[n] = 0;
    return text;
}

uint32_t PSE_CALL FrontEndState() { return Peek32(kFrontEndState); }
uint32_t PSE_CALL DispatchMode() {
    uint8_t m = 0;
    return Peek(kDispatchMode, &m, 1) ? m : 0;
}
uint32_t PSE_CALL LevelName(char* buf, uint32_t bufLen) { return CopyOut(CurrentLevel(), buf, bufLen); }
uint32_t PSE_CALL LevelKind() { return pse_game_ds2::KindOf(CurrentLevel().c_str()); }
int PSE_CALL InCutscene() {
    const uint32_t world = Peek32(kWorld);
    uint8_t flags = 0;
    return world && Peek(world + kCinematicOffset, &flags, 1) && (flags & 1) ? 1 : 0;
}
uint32_t PSE_CALL SplitScreenPlayers() {
    uint32_t split[2] = {};
    if (!Peek(kSplit, split, sizeof split) || !split[0]) return 1;
    return split[1] >= 2 && split[1] <= 4 ? split[1] : 1;
}
void* PSE_CALL GameWindow() { return reinterpret_cast<void*>(static_cast<uintptr_t>(Peek32(kGameWindow))); }
const std::string& ExeHash() {
    static const std::string hash = [] {
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        return n && n < MAX_PATH ? pse_game_ds2::ExeFileSha256(path) : std::string();
    }();
    return hash;
}
const char* PSE_CALL ExeSha256() { return ExeHash().c_str(); }
int PSE_CALL ExeKnown() { return ExeHash() == kKnownSha256 ? 1 : 0; }

const PSEGameDs2V1 kTable = {
    sizeof(PSEGameDs2V1), 1,           &FrontEndState, &DispatchMode, &LevelName, &LevelKind, &InCutscene,
    &SplitScreenPlayers,  &GameWindow, &ExeSha256,     &ExeKnown,
};
}  // namespace

const PSEPluginInfo pse_game_ds2::kPluginInfo = {sizeof(PSEPluginInfo), 1, PSE_GAME_DS2_PLUGIN,
                                                 "Desert Storm II game interface", "PSE", PSE_MAKE_VERSION(1, 0, 0),
                                                 PSE_API_VERSION, nullptr, PSE_PLUGIN_CLIENT_ONLY};

const PSEGameDs2V1* pse_game_ds2::Table() { return &kTable; }
const char* pse_game_ds2::KnownSha256() { return kKnownSha256; }
const char* pse_game_ds2::SecuromSha256() { return kSecuromSha256; }

PSEPluginHandle pse_game_ds2::Register() {
    PSEPluginHandle h = pse::FindPlugin(PSE_GAME_DS2_PLUGIN);
    if (!h) h = pse::RegisterInternalPlugin(&kPluginInfo);
    if (h) pse::MessagingInterface()->RegisterInterface(h, PSE_GAME_DS2_INTERFACE, 1, &kTable);
    return h;
}

uint32_t pse_game_ds2::KindOf(const char* level) {
    if (!level || !*level) return PSE_DS2_LEVEL_NONE;
    if (_stricmp(level, "frontend") == 0) return PSE_DS2_LEVEL_FRONTEND;
    return PSE_DS2_LEVEL_MISSION;
}

std::string pse_game_ds2::ExeFileSha256(const wchar_t* path) {
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
