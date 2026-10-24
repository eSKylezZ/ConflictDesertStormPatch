// pse.game.cv v1 (pse/include/pse/pse_game_cv.h, docs/sdk/reference/game-cv.md): read-only exe identification for
// Conflict: Vietnam, served by the built-in plugin "pse.game.cv". The loader and Windows only. Part of the game
// layer PSE\Games\pse_cv.dll.
#include "pse_game_cv.h"

#include <windows.h>

#include <cctype>
#include <cstring>
#include <vector>

#include "loader.h"

namespace {
constexpr char kKnownSha256[] = "636576CA842066A56660C1DAE615FBA4DB9726AEA1EF851EE5FD4C6D8CE3710C";

const std::string& ExeHash() {
    static const std::string hash = [] {
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        return n && n < MAX_PATH ? pse_game_cv::ExeFileSha256(path) : std::string();
    }();
    return hash;
}
const char* PSE_CALL ExeSha256() { return ExeHash().c_str(); }
int PSE_CALL ExeKnown() { return ExeHash() == kKnownSha256 ? 1 : 0; }

const PSEGameCvV1 kTable = {sizeof(PSEGameCvV1), 1, &ExeSha256, &ExeKnown};
}  // namespace

const PSEPluginInfo pse_game_cv::kPluginInfo = {sizeof(PSEPluginInfo), 1, PSE_GAME_CV_PLUGIN,
                                                "Conflict: Vietnam game interface", "PSE", PSE_MAKE_VERSION(1, 0, 0),
                                                PSE_API_VERSION, nullptr, PSE_PLUGIN_CLIENT_ONLY};

const PSEGameCvV1* pse_game_cv::Table() { return &kTable; }
const char* pse_game_cv::KnownSha256() { return kKnownSha256; }

PSEPluginHandle pse_game_cv::Register() {
    PSEPluginHandle h = pse::FindPlugin(PSE_GAME_CV_PLUGIN);
    if (!h) h = pse::RegisterInternalPlugin(&kPluginInfo);
    if (h) pse::MessagingInterface()->RegisterInterface(h, PSE_GAME_CV_INTERFACE, 1, &kTable);
    return h;
}

std::string pse_game_cv::ExeFileSha256(const wchar_t* path) {
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
