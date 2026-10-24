// pse.game.cgs v1 (pse/include/pse/pse_game_cgs.h, docs/sdk/reference/game-cgs.md): read-only wrappers over
// Conflict: Global Storm's identity, served by the built-in plugin "pse.game.cgs". Game memory, the loader and
// Windows only. Part of the game layer PSE\Games\pse_cgs.dll.
#include "pse_game_cgs.h"

#include <windows.h>

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "loader.h"

namespace {
constexpr char kKnownSha256[] = "D1EE014F6888B100C8806EAACB8FFC22D069B815CF48C3A60FFDA0EC538539A1";  // certutil, 2026-10-24

HWND g_window = nullptr;   // found once via EnumWindows (no RE'd static HWND global for this exe yet)

BOOL CALLBACK FindOurWindow(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && GetWindow(hwnd, GW_OWNER) == nullptr) {
        g_window = hwnd;
        return FALSE;
    }
    return TRUE;
}

void* PSE_CALL GameWindow() {
    if (!g_window || !IsWindow(g_window)) {
        g_window = nullptr;
        EnumWindows(FindOurWindow, 0);
    }
    return g_window;
}

const std::string& ExeHash() {
    static const std::string hash = [] {
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        return n && n < MAX_PATH ? pse_game_cgs::ExeFileSha256(path) : std::string();
    }();
    return hash;
}
const char* PSE_CALL ExeSha256() { return ExeHash().c_str(); }
int PSE_CALL ExeKnown() { return ExeHash() == kKnownSha256 ? 1 : 0; }

const PSEGameCgsV1 kTable = {sizeof(PSEGameCgsV1), 1, &GameWindow, &ExeSha256, &ExeKnown};
}  // namespace

const PSEPluginInfo pse_game_cgs::kPluginInfo = {sizeof(PSEPluginInfo), 1, PSE_GAME_CGS_PLUGIN,
                                                 "Conflict: Global Storm game interface", "PSE", PSE_MAKE_VERSION(1, 0, 0),
                                                 PSE_API_VERSION, nullptr, PSE_PLUGIN_CLIENT_ONLY};

const PSEGameCgsV1* pse_game_cgs::Table() { return &kTable; }
const char* pse_game_cgs::KnownSha256() { return kKnownSha256; }

PSEPluginHandle pse_game_cgs::Register() {
    PSEPluginHandle h = pse::FindPlugin(PSE_GAME_CGS_PLUGIN);
    if (!h) h = pse::RegisterInternalPlugin(&kPluginInfo);
    if (h) pse::MessagingInterface()->RegisterInterface(h, PSE_GAME_CGS_INTERFACE, 1, &kTable);
    return h;
}

std::string pse_game_cgs::ExeFileSha256(const wchar_t* path) {
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
    std::string hex = pse::compat::Sha256Hex(data.data(), data.size());
    for (char& c : hex) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return hex;
}
