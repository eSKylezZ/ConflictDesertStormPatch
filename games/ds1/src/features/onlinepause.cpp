// The pause menu of an online mission (user 2026-10-22: "there is no pause menu ... to exit the game / close the session
// and return to the lobby or restart the mission"). The game's own pause menu freezes the world on one PC, so online
// PAUSE never reaches the game (lockstep.cpp). This menu doesn't pause: Esc / a local pad's Start opens it while the
// mission plays (not in cutscenes or on the loadout screen), the game runs on and this PC's players stand still
// (lockstep samples neutral input). Rows: RESUME, RESTART MISSION (host: the level from its start for everyone - the
// host's level loads are mirrored), BACK TO LOBBY (host: everyone back to the online lobby; joiner: leaves the session),
// QUIT GAME. Keyboard: Up / Down, Enter, Esc; pads: D-pad, Cross / A, Circle / B, Start.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "core/log.h"
#include "core/net.h"
#include "features/features.h"
#include "features/lobbynet.h"
#include "features/overlay.h"

namespace {
constexpr uint32_t kFrontEndState = 0x617C18, kDispatchMode = 0x60EC8C, kGameState = 0x63C948, kRenderer = 0x63C924;
constexpr uint32_t kGameWindow = 0x606A60;
// game joystick buttons (default.key numbering, after the pad remap): 1 Circle / B, 2 Cross / A, 11 Start, 12 / 14 D-pad
constexpr uint32_t kPadBack = 1u << 1, kPadSelect = 1u << 2, kPadStart = 1u << 11, kPadUp = 1u << 12, kPadDown = 1u << 14;
enum Row { kResume, kRestart, kLobby, kQuit, kRows };

bool g_open = false;
int g_row = 0;
uint32_t g_prevPad = 0;
bool g_prevKeys[5] = {};   // Esc, Up, Down, Enter, Backspace
DWORD g_openedAt = 0;

bool InCutscene() {
    const uint8_t* state = *reinterpret_cast<const uint8_t* const*>(kGameState);
    return state && (state[0x1B8C] & 1);
}
bool Playing() {
    return features::LockstepActive() && *reinterpret_cast<const uint32_t*>(kFrontEndState) == 0xE &&
           *reinterpret_cast<const int8_t*>(kDispatchMode) == 0;
}
bool Host() { return lobbynet::IsHost(); }
const char* RowText(int r) {
    switch (r) {
        case kResume: return "RESUME";
        case kRestart: return Host() ? "RESTART MISSION" : "RESTART MISSION (HOST ONLY)";
        case kLobby: return Host() ? "BACK TO THE LOBBY (EVERYONE)" : "LEAVE THE SESSION";
        default: return "QUIT GAME";
    }
}
bool RowEnabled(int r) { return r != kRestart || Host(); }

void Close() {
    g_open = false;
    dslog::Write("Online menu: closed");
}

void Choose(int r) {
    if (!RowEnabled(r)) return;
    Close();
    switch (r) {
        case kRestart:
            features::OnlineLogLine("Online: the host restarts the mission (online menu)");
            if (!features::RestartLevel()) features::OnlineNotice("The mission can't be restarted right now");
            break;
        case kLobby:
            if (Host()) {
                features::OnlineLogLine("Online: the host goes back to the lobby (online menu) - everyone follows");
                features::ConsoleStartLevel("FrontEnd.dll");
            } else {
                features::OnlineLogLine("Online: this PC leaves the session (online menu)");
                net::Stop();
                features::ConsoleStartLevel("FrontEnd.dll");
            }
            break;
        case kQuit:
            features::OnlineLogLine("Online: quit (online menu)");
            if (HWND w = *reinterpret_cast<HWND*>(kGameWindow)) PostMessageA(w, WM_CLOSE, 0, 0);
            break;
        default: break;
    }
}

// This PC's local pads (the lobby devices of its players; "keyboard + pad" = pad 0).
uint32_t LocalPads() {
    uint32_t bits = 0;
    for (int p = 0; p < 4; ++p) {
        if (!features::LobbyLocal(p)) continue;
        const int d = features::LobbyDevice(p);
        if (d == 8) continue;
        bits |= features::ReadJoystick(d < 0 ? 0 : d) & 0xFFFF;
    }
    return bits;
}

void LateDraw() {
    if (!g_open || !Playing()) return;
    const uint8_t* r = *reinterpret_cast<uint8_t* const*>(kRenderer);
    void* font = overlay::MenuFont();
    if (!r || !font) return;
    const float w = static_cast<float>(*reinterpret_cast<const uint32_t*>(r + 0x40688));
    const float h = static_cast<float>(*reinterpret_cast<const uint32_t*>(r + 0x4068C));
    const float line = static_cast<float>(std::max(8, overlay::TextWidth(font, "M")));
    const char* title = "ONLINE - THE MISSION RUNS ON";
    float pw = static_cast<float>(overlay::TextWidth(font, title));
    for (int i = 0; i < kRows; ++i) pw = std::max(pw, static_cast<float>(overlay::TextWidth(font, RowText(i))));
    pw += line * 4;
    const float rowH = line * 2.4f, ph = rowH * (static_cast<float>(kRows) + 1.6f);
    const float x = (w - pw) / 2, y = (h - ph) / 2;
    overlay::QueueScreenRect(x, y, pw, ph, 0xC0101410);
    overlay::QueueScreenRect(x, y, pw, std::max(2.0f, line / 4), 0xFF5A7A3A);
    overlay::QueueScreenRect(x, y + rowH * 1.9f + g_row * rowH - line * 1.5f, pw, rowH * 0.9f, 0x80507040);
    overlay::Flush();
    overlay::DrawLabel(font, title, static_cast<int>(x + line * 2), static_cast<int>(y + rowH * 0.8f), 0xFFE0E0E0);
    for (int i = 0; i < kRows; ++i)
        overlay::DrawLabel(font, RowText(i), static_cast<int>(x + line * 2), static_cast<int>(y + rowH * 1.9f + i * rowH),
                           RowEnabled(i) ? (i == g_row ? 0xFFFFFFFF : 0xFFC8C8C8) : 0xFF707070);
}
}  // namespace

bool features::OnlinePauseOpen() { return g_open; }

void features::OnFrameOnlinePause() {
    static bool added = false;
    if (!added) added = true, overlay::AddLateDraw(&LateDraw);
    if (!Playing() || InCutscene() || features::CustomiseOpen()) {
        if (g_open) Close();
        g_prevPad = LocalPads();
        return;
    }
    const uint32_t pad = LocalPads(), pressed = pad & ~g_prevPad;
    g_prevPad = pad;
    bool keys[5] = {};
    bool kp[5] = {};
    const int vks[5] = {VK_ESCAPE, VK_UP, VK_DOWN, VK_RETURN, VK_BACK};
    const bool focused = features::GameFocused();
    for (int i = 0; i < 5; ++i) {
        keys[i] = focused && features::KeyHeld(vks[i]);
        kp[i] = keys[i] && !g_prevKeys[i];
        g_prevKeys[i] = keys[i];
    }
    if (!g_open) {
        if (kp[0] || (pressed & kPadStart)) {
            g_open = true;
            g_row = 0;
            g_openedAt = GetTickCount();
            dslog::Write("Online menu: opened");
        }
        return;
    }
    if (GetTickCount() - g_openedAt < 150) return;
    if (kp[1] || (pressed & kPadUp)) g_row = (g_row + kRows - 1) % kRows;
    if (kp[2] || (pressed & kPadDown)) g_row = (g_row + 1) % kRows;
    if (kp[0] || kp[4] || (pressed & (kPadBack | kPadStart))) return Close();
    if (kp[3] || (pressed & kPadSelect)) Choose(g_row);
}
