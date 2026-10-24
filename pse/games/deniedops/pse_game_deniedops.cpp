// pse.game.deniedops v1 (pse/include/pse/pse_game_deniedops.h, docs/sdk/reference/game-deniedops.md): read-only
// wrappers over Conflict: Denied Ops's identity, served by the built-in plugin "pse.game.deniedops". Game memory,
// the loader and Windows only. Part of the game layer PSE\Games\pse_deniedops.dll.
#include "pse_game_deniedops.h"

#include <windows.h>

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "loader.h"

namespace {
// ConflictDeniedOps.exe, Steam no-CD, 7,061,504 bytes, PE timestamp 0x478652D0 (Thu Jan 10 17:16:00 2008), SteamStub
// entry at RVA 0xB4D3DB. Confirmed live 2026-10-24 against G:\SteamLibrary\...\ConflictDeniedOps.exe.
constexpr char kKnownSha256[] = "104AA5D5A4245F02ED96AA2C370E053A7736EDC49F5F6FA67846D73BECB64838";

const std::string& ExeHash() {
    static const std::string hash = [] {
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        return n && n < MAX_PATH ? pse_game_deniedops::ExeFileSha256(path) : std::string();
    }();
    return hash;
}
const char* PSE_CALL ExeSha256() { return ExeHash().c_str(); }
int PSE_CALL ExeKnown() { return ExeHash() == kKnownSha256 ? 1 : 0; }

const PSEGameDeniedOpsV1 kTable = {sizeof(PSEGameDeniedOpsV1), 1, &ExeSha256, &ExeKnown};
}  // namespace

const PSEPluginInfo pse_game_deniedops::kPluginInfo = {
    sizeof(PSEPluginInfo), 1, PSE_GAME_DENIEDOPS_PLUGIN, "Conflict: Denied Ops game interface", "PSE",
    PSE_MAKE_VERSION(1, 0, 0), PSE_API_VERSION, nullptr, PSE_PLUGIN_CLIENT_ONLY};

const PSEGameDeniedOpsV1* pse_game_deniedops::Table() { return &kTable; }
const char* pse_game_deniedops::KnownSha256() { return kKnownSha256; }

PSEPluginHandle pse_game_deniedops::Register() {
    PSEPluginHandle h = pse::FindPlugin(PSE_GAME_DENIEDOPS_PLUGIN);
    if (!h) h = pse::RegisterInternalPlugin(&kPluginInfo);
    if (h) pse::MessagingInterface()->RegisterInterface(h, PSE_GAME_DENIEDOPS_INTERFACE, 1, &kTable);
    return h;
}

std::string pse_game_deniedops::ExeFileSha256(const wchar_t* path) {
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
