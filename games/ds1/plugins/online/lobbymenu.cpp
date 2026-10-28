// State 0x4F ONLINE LOBBY (user, 2026-10-22: one lobby for every PC's players, Xbox-style joining, colours, names,
// split / own view per player, START held until everyone is ready and has the same files). The table and its rules:
// lobby.h / lobbynet.cpp; this file is the screen.
//
// The host comes here from HOST GAME, a joiner once the host's table has arrived (JOIN GAME > PASTE JOIN CODE). Each
// PC starts with one player on "keyboard + first pad". Every local device then works its own player (coop.cpp
// CoopDeviceActions: pads 0..7, the keyboard):
//   Cross / A / Space     join (another pad: that PC's first player keeps the keyboard or pad 0 - the one it used
//                         last here); a joined player: READY on / off
//   Left / Right          colour (the next one nobody wears)
//   R1 / RB / Tab         SPLIT SCREEN <-> OWN VIEW (this player's view full screen on their PC; used from stage 5)
//   Circle / B / Backspace  leave (not a PC's last player)
//   N                     type the keyboard player's name (Enter keeps it - remembered as PlayerName<k> - Esc drops it)
//   Options / Start / Enter  host: START (every other PC's players READY, plugin / mod check passed); joiner: READY
//   C / Ctrl+V            host: copy the join code / paste a reply code;   Esc: leave the lobby (ends the session)
// Built like the join screen (coop.cpp): a main-menu-class object with a copied vtable, its list only eats the menu
// input, the panels are drawn in a late draw (shapes, then game-font text over them).
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/net.h"
#include "features/features.h"
#include "features/lobbynet.h"
#include "features/overlay.h"

namespace {
constexpr uint32_t kSetState = 0x47EF20, kListSelected = 0x4E5D80;
constexpr uint32_t kMainMenuScreen = 0x617A00, kMainMenuCtor = 0x415020, kMainMenuVtable = 0x5D86B8;
constexpr uint32_t kMainMenuSetup = 0x415120, kMpVtable = 0x5D90C0, kItemDrawCb = 0x4153E0;
constexpr uint32_t kAlloc = 0x4B83D0, kListCtor = 0x4E5B10, kListReserve = 0x4E56C0, kListAddToPool = 0x4E58C0;
constexpr uint32_t kItemCtor = 0x4E4F90, kItemSet = 0x4E4FB0;
constexpr uint32_t kMenuFont = 0x60EDB4, kLargeFont = 0x60EDB8, kHudSheet = 0x60EE18, kRenderer = 0x63C924;
constexpr uint32_t kFrontEndState = 0x617C18;
constexpr uint32_t kStateLobby = 0x4F, kStateOnline = 0x49;
constexpr uint32_t kHashSlot0 = 0xC0070010, kHashTitle = 0xC0070002;   // coop.cpp's texts (the list isn't drawn)
constexpr int kVtableEntries = 21, kItems = 4, kDevices = 9, kKeyboard = 8;

using features::PadType;

uint8_t g_object[0x20];
void* g_vtable[kVtableEntries];
bool g_built = false, g_due = false;
float g_alpha = 1.0f;
uint32_t g_prev[kDevices];
PadType g_type[kDevices];
uint32_t g_devPress[16] = {};   // dev.lobby_press: held for a few polls, then released
int g_devFrames[16] = {};
int g_anyLast = -1;             // the device that works this PC's "keyboard + first pad" player here (-1: not yet)
int g_editing = -1;             // player whose name is being typed, -1 none
std::string g_edit;
bool g_fresh = false;          // the old name is shown: the first letter replaces it, Backspace edits it
bool g_keyN = true, g_keyC = true, g_keyV = true;   // held when the screen opened = not a press

template <class T>
T& At(void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}
void SetState(uint32_t s) { reinterpret_cast<void(__cdecl*)(uint32_t)>(kSetState)(s); }
void* ItemAt(void* list, int i) { return At<void**>(list, 0x2C)[i]; }

const lobby::Table& T() { return lobbynet::Current(); }
bool Mine(int player) { return T().slots[player].used && T().slots[player].pc == lobbynet::MyPc(); }
int MineCount() {
    int n = 0;
    for (int k = 0; k < lobby::kMaxPlayers; ++k) n += Mine(k);
    return n;
}

// The player a device works: its own, or this PC's "keyboard + first pad" player for the keyboard / pad 0 - but only
// the one of the two that player has used here (the first pressed). The other one is a new person: its Join adds a
// player (user 2026-10-22: a pad plugged in next to the keyboard player "just took over instead of choosing another
// slot").
int PlayerOf(int device) {
    for (int k = 0; k < lobby::kMaxPlayers; ++k) {
        if (!Mine(k)) continue;
        const int d = T().slots[k].device;
        if (d == device) return k;
        if (d == lobby::kDeviceAny && (device == kKeyboard || device == 0) && (g_anyLast < 0 || g_anyLast == device))
            return k;
    }
    return -1;
}

bool OthersReady(std::string* who) {   // every player of the other PCs ready (the host's own START is theirs)
    for (int k = 0; k < lobby::kMaxPlayers; ++k) {
        const lobby::Slot& s = T().slots[k];
        if (s.used && s.pc != 1 && !s.ready) {
            if (who) *who = s.name;
            return false;
        }
    }
    return true;
}

void TryStart() {
    std::string why;
    if (features::OnlineStartBlocked(&why)) return features::OnlineNotice(why.c_str());
    if (!OthersReady(&why)) return features::OnlineNotice(("Waiting for " + why + " to be READY").c_str());
    dslog::Write("Lobby: START with %d player(s) on %d PC(s)", T().Count(), T().Pcs());
    features::OnlineStartSession();
}

void Leave() {
    if (g_editing >= 0) features::UiCaptureKeys(false);
    g_editing = -1;
    net::Stop();
    SetState(kStateOnline);
}

}  // namespace
void features::LobbyLeave() { Leave(); }
namespace {

void Request(const char* fmt, int a, int b) {
    char text[48];
    snprintf(text, sizeof text, fmt, a, b);
    lobbynet::Request(text);
}

void NextColour(int player, int step) {
    for (int i = 1; i < lobby::kColours; ++i) {
        const int c = (T().slots[player].colour + step * i + lobby::kColours * 2) % lobby::kColours;
        bool used = false;
        for (const lobby::Slot& s : T().slots) used |= s.used && s.colour == c;
        if (!used) return Request("LB C %d %d", player, c);
    }
}

void Join(int device, const std::string& name) {
    if (T().Count() >= lobby::kMaxPlayers) return features::OnlineNotice("The lobby is full (4 players)");
    for (int k = 0; k < lobby::kMaxPlayers; ++k)   // the "keyboard + first pad" player keeps the device it used
        if (Mine(k) && T().slots[k].device == lobby::kDeviceAny) {
            const int keep = g_anyLast >= 0 && g_anyLast != device ? g_anyLast : (device == kKeyboard ? 0 : kKeyboard);
            Request("LB D %d %d", k, keep);
        }
    lobbynet::Request(lobby::JoinRequest(device, lobbynet::DefaultName(MineCount())));
    dslog::Write("Lobby: %s joins", name.c_str());
}

void StartEditing(int player) {
    if (player < 0 || !features::UiCaptureKeys(true)) return;
    g_editing = player;
    g_edit = T().slots[player].name;
    g_fresh = true;
}

void Typing() {
    bool isChar;
    uint32_t code;
    while (g_editing >= 0 && features::UiNextKey(isChar, code)) {
        if (!isChar) continue;
        if (code == 8) {
            if (!g_edit.empty()) g_edit.pop_back();
            g_fresh = false;
        } else if (code == 13) {
            const int k = lobbynet::LocalIndex(g_editing);
            if (!lobby::CleanName(g_edit).empty()) {
                lobbynet::Request("LB N " + std::to_string(g_editing) + " " + lobby::Hex(lobby::CleanName(g_edit)));
                lobbynet::RememberName(k, g_edit);
            }
            g_editing = -1;
        } else if (code == 27) {
            g_editing = -1;
        } else if (code >= 0x20 && code < 0x7F) {
            if (g_fresh) g_edit.clear();
            g_fresh = false;
            if (g_edit.size() < lobby::kNameMax) g_edit += static_cast<char>(code);
        }
    }
    if (g_editing >= 0 && !Mine(g_editing)) g_editing = -1;   // the player went (table changed)
    if (g_editing < 0) {
        features::UiCaptureKeys(false);
        for (uint32_t& p : g_prev) p = 0xFFFFFFFF;   // Enter / Esc that ended it isn't a menu press
    }
}

bool KeyPress(int vk, bool& was) {
    const bool down = features::KeyHeld(vk);
    const bool press = down && !was;
    was = down;
    return press;
}

void Poll() {
    if (g_editing >= 0) return Typing();
    if (features::ModSyncActive()) {   // the host's mods first (modsync.cpp): yes / leave only
        for (int d = 0; d < kDevices; ++d) {
            std::string name;
            const uint32_t actions = features::CoopDeviceActions(d, g_type[d], name);
            const uint32_t pressed = actions & ~g_prev[d];
            g_prev[d] = actions;
            if (pressed & features::kCoopBack) {
                features::ModSyncCancel();
                return Leave();
            }
            if ((pressed & (features::kCoopJoin | features::kCoopStart)) && features::ModSyncWaitingForYes())
                features::ModSyncAccept();
        }
        return;
    }
    for (int d = 0; d < kDevices; ++d) {
        std::string name;
        uint32_t actions = features::CoopDeviceActions(d, g_type[d], name);
        if (g_devFrames[d] > 0 && --g_devFrames[d] > 2) actions |= g_devPress[d];   // dev: 3 polls down, then up
        const uint32_t pressed = actions & ~g_prev[d];
        g_prev[d] = actions;
        if (!pressed) continue;
        const int p = PlayerOf(d);
        if ((pressed & features::kCoopBack) && (p >= 0 || d == kKeyboard)) return Leave();   // not a pad nobody joined with
        if (p >= 0 && T().slots[p].device == lobby::kDeviceAny) g_anyLast = d;
        if (p < 0) {
            if (pressed & features::kCoopJoin) Join(d, name);
            continue;
        }
        const lobby::Slot& s = T().slots[p];
        if (pressed & features::kCoopStart) {
            if (lobbynet::IsHost()) TryStart();
            else Request("LB R %d %d", p, s.ready ? 0 : 1);
        }
        if (pressed & features::kCoopJoin) Request("LB R %d %d", p, s.ready ? 0 : 1);
        if ((pressed & features::kCoopLeave) && MineCount() > 1) Request("LB L %d %d", p, 0);
        if (pressed & features::kCoopLeft) NextColour(p, -1);
        if (pressed & features::kCoopRight) NextColour(p, 1);
        if (pressed & features::kCoopCycle) Request("LB V %d %d", p, s.view == lobby::View::Own ? 0 : 1);
    }
    const bool ctrl = features::KeyHeld(VK_CONTROL);
    if (KeyPress('N', g_keyN) && !ctrl) {
        if (g_anyLast < 0) g_anyLast = kKeyboard;   // typing a name = the keyboard is this player's
        StartEditing(PlayerOf(kKeyboard) >= 0 ? PlayerOf(kKeyboard) : lobbynet::Current().FirstOf(lobbynet::MyPc()));
    }
    if (KeyPress('C', g_keyC) && lobbynet::IsHost())
        features::OnlineNotice(features::OnlineCopyJoinCode() ? "Join code copied - send it to your friend" : "No join code yet");
    if (KeyPress('V', g_keyV) && ctrl && lobbynet::IsHost())
        features::OnlineNotice(features::OnlinePasteReplyCode() ? "Reply code accepted - connecting ..." : "The clipboard doesn't hold a reply code");
}

}  // namespace
void features::LobbyDevPress(int device, uint32_t actions) {
    if (device < 0 || device >= kDevices) return;
    g_devPress[device] = actions;
    g_devFrames[device] = 6;
    if (g_prev[device] == 0xFFFFFFFF) g_prev[device] = 0;
}
namespace {
// ---- screen class ----
bool __fastcall Create(void* screen, void*) {
    using Alloc = void*(__cdecl*)(uint32_t, uint32_t);
    void* list = reinterpret_cast<Alloc>(kAlloc)(0xA4, 0x1E);
    if (!list) return false;
    reinterpret_cast<void*(__thiscall*)(void*, void*, void*, int)>(kListCtor)(
        list, *reinterpret_cast<void**>(kMenuFont), *reinterpret_cast<void**>(kHudSheet), 1);
    At<void*>(screen, 8) = list;
    if (!reinterpret_cast<int(__thiscall*)(void*, int)>(kListReserve)(list, kItems)) return false;
    for (int i = 0; i < kItems; ++i) {
        void* item = reinterpret_cast<Alloc>(kAlloc)(0x24, 0x1E);
        if (item) reinterpret_cast<void*(__thiscall*)(void*)>(kItemCtor)(item);
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kListAddToPool)(list, item);
    }
    return true;
}

void __fastcall Setup(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*)>(kMainMenuSetup)(screen);
    void* list = At<void*>(screen, 8);
    using Fn = void(__thiscall*)(void*, int, uint32_t, int, int, int, int, uint32_t, int);
    for (int i = 0; i < kItems; ++i)
        reinterpret_cast<Fn>(kItemSet)(ItemAt(list, i), 0x510 + i, kHashSlot0 + i, 0, 0x24, 0, 0, kItemDrawCb, 0);
    using SetTitle = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<SetTitle>((*reinterpret_cast<void***>(screen))[20])(screen, kHashTitle, 0, 0, 0x7FFF);
    for (uint32_t& p : g_prev) p = 0xFFFFFFFF;   // buttons held on the way in aren't presses
    g_keyN = g_keyC = g_keyV = true;
    g_editing = -1;
    g_anyLast = -1;
    dslog::Write("Lobby: screen opened (%s)", lobbynet::IsHost() ? "host" : "joiner");
}

int __fastcall Update(void* screen, void*) {
    reinterpret_cast<short(__thiscall*)(void*)>(kListSelected)(At<void*>(screen, 8));   // the rows only show players
    if (net::Snap().role == net::Snapshot::Role::None) {   // the session ended (host gone / refused)
        SetState(kStateOnline);
        return 0;
    }
    Poll();
    return 0;
}

void __fastcall Draw(void*, void*) {
    g_due = true;
    g_alpha = overlay::MenuAlpha();
}

uint32_t __fastcall NoPrompt(void*, void*) { return 0; }

// ---- drawing ----
uint32_t A(uint32_t rgb, float alpha) {
    const int a = std::max(0, std::min(255, static_cast<int>(alpha * g_alpha * 255.0f)));
    return (static_cast<uint32_t>(a) << 24) | (rgb & 0xFFFFFF);
}

overlay::Icon ConfirmIcon(int device) {
    if (device == kKeyboard) return overlay::Icon::KbSpace;
    return g_type[device] == PadType::PlayStation ? overlay::Icon::PsCross : overlay::Icon::XbCross;
}

bool CanJoin() {   // a local device without a player (another pad, or the keyboard once the first player kept pad 0)
    if (T().Count() >= lobby::kMaxPlayers) return false;
    const int pads = std::min(*reinterpret_cast<int*>(0x754BE8), 8);
    for (int d = 0; d < pads; ++d)
        if (PlayerOf(d) < 0) return true;
    return PlayerOf(kKeyboard) < 0;
}

std::string DeviceText(int device) {
    if (device == lobby::kDeviceAny) return "KEYBOARD + CONTROLLER";
    if (device == kKeyboard) return "KEYBOARD & MOUSE";
    char t[24];
    snprintf(t, sizeof t, "CONTROLLER %d", device + 1);
    return t;
}

void LateDraw() {
    if (!g_due) return;
    g_due = false;
    if (*reinterpret_cast<uint32_t*>(kFrontEndState) != kStateLobby || g_alpha <= 0.01f) return;
    auto* r = *reinterpret_cast<uint8_t**>(kRenderer);
    const float w = static_cast<float>(*reinterpret_cast<int*>(r + 0x40688));
    const float h = static_cast<float>(*reinterpret_cast<int*>(r + 0x4068C));
    void* small = overlay::MenuFont();
    void* large = *reinterpret_cast<void**>(kLargeFont);
    const float gap = w * 0.012f, x0 = w * 0.08f, x1 = w * 0.92f, y0 = h * 0.15f, y1 = h * 0.64f;
    const float pw = (x1 - x0 - gap) / 2, ph = (y1 - y0 - gap) / 2;
    const float line = static_cast<float>(std::max(8, overlay::TextWidth(small, "M")));
    const float border = std::max(2.0f, h / 360.0f), band = ph * 0.22f;
    const float icon = std::min(ph * 0.18f, pw * 0.12f);
    struct Text { void* font; std::string text; float x, y; uint32_t argb; bool centre; };
    std::vector<Text> texts;
    struct IconAt { overlay::Icon icon; float x, y, size; };
    std::vector<IconAt> icons;
    auto centred = [&](void* f, const std::string& t, float cx, float y, uint32_t argb) { texts.push_back({f, t, cx, y, argb, true}); };
    auto left = [&](void* f, const std::string& t, float x, float y, uint32_t argb) { texts.push_back({f, t, x, y, argb, false}); };
    centred(large, "ONLINE LOBBY", w / 2, h * 0.10f, A(0xFFFFFF, 1.0f));
    const int me = lobbynet::MyPc();
    if (features::ModSyncActive()) {   // the host's mods (modsync.cpp) instead of the players' panels
        std::string title, footer;
        const std::vector<features::ModSyncRow> rows = features::ModSyncRows(title, footer);
        overlay::QueueScreenRect(x0, y0, x1 - x0, y1 - y0 + h * 0.2f, A(0x0C120C, 0.85f));
        overlay::QueueScreenRect(x0, y0, x1 - x0, std::max(2.0f, h / 360.0f), A(0xE0D878, 1.0f));
        centred(large, title, w / 2, y0 + line * 3.0f, A(0xE0D878, 1.0f));
        float y = y0 + line * 6.0f;
        const float barW = (x1 - x0) * 0.22f;
        for (const features::ModSyncRow& row : rows) {
            if (y > y1 + h * 0.12f) break;
            std::string text = row.text;
            while (overlay::TextWidth(small, text.c_str()) > (x1 - x0) * 0.72f && text.size() > 8) text.pop_back();
            left(small, text, x0 + line * 2, y, A(0xFFFFFF, 1.0f));
            if (row.progress >= 0) {   // a bar on the right
                const float bx = x1 - barW - line * 2, by = y - line * 1.1f;
                overlay::QueueScreenRect(bx, by, barW, line * 1.3f, A(0x303030, 1.0f));
                overlay::QueueScreenRect(bx, by, barW * std::clamp(row.progress, 0.0f, 1.0f), line * 1.3f, A(0x2EA844, 1.0f));
            }
            y += line * 2.4f;
        }
        std::string f = footer;
        while (overlay::TextWidth(small, f.c_str()) > (x1 - x0) * 0.96f && f.size() > 8) f.pop_back();
        centred(small, f, w / 2, y1 + h * 0.16f, A(0xFFFFFF, 0.95f));
        using overlay::Icon;
        const float ps = std::max(12.0f, icon * 0.75f);
        float px = w / 2 - ps * 9;
        const float py = h * 0.925f;
        if (features::ModSyncWaitingForYes()) {
            for (Icon k : {Icon::PsCross, Icon::XbCross, Icon::KbEnter}) icons.push_back({k, px, py - line / 2 - ps / 2, ps}), px += ps;
            left(small, "Get them", px + ps / 4, py, A(0xFFFFFF, 1.0f));
            px += ps / 4 + overlay::TextWidth(small, "Get them") + ps;
        }
        icons.push_back({Icon::KbEscape, px, py - line / 2 - ps / 2, ps});
        left(small, "Leave the session", px + ps * 1.25f, py, A(0xFFFFFF, 1.0f));
        overlay::Flush();
        for (const Text& t : texts) {
            const int tx = t.centre ? static_cast<int>(t.x - overlay::TextWidth(t.font, t.text.c_str()) / 2.0f) : static_cast<int>(t.x);
            overlay::DrawLabel(t.font, t.text.c_str(), tx, static_cast<int>(t.y), t.argb);
        }
        for (const IconAt& k : icons) overlay::QueueIcon(k.icon, k.x, k.y, k.size, g_alpha);
        return;
    }
    for (int i = 0; i < lobby::kMaxPlayers; ++i) {
        const lobby::Slot& s = T().slots[i];
        const float px = x0 + (i % 2) * (pw + gap), py = y0 + (i / 2) * (ph + gap), cx = px + pw / 2;
        const uint32_t colour = s.used ? lobby::kPalette[s.colour] : 0x404040;
        const bool mine = s.used && s.pc == me;
        overlay::QueueScreenRect(px, py, pw, ph, A(0x0C120C, s.used ? 0.82f : 0.6f));
        overlay::QueueScreenRect(px, py, pw, band, A(colour, s.used ? 0.9f : 0.55f));
        const uint32_t edge = A(s.used ? colour : 0xFFFFFF, s.used ? 1.0f : 0.25f);
        overlay::QueueScreenRect(px, py, pw, border, edge);
        overlay::QueueScreenRect(px, py + ph - border, pw, border, edge);
        overlay::QueueScreenRect(px, py, border, ph, edge);
        overlay::QueueScreenRect(px + pw - border, py, border, ph, edge);
        char header[48];
        snprintf(header, sizeof header, "PLAYER %d", i + 1);
        centred(large, header, cx, py + band / 2 + line / 2, A(0xFFFFFF, s.used ? 1.0f : 0.6f));
        if (!s.used) {
            if (CanJoin()) {   // "PRESS [Cross] [A] [Space] TO JOIN"
                const std::string a = "PRESS", b = "TO JOIN";
                const float sz = icon * 0.8f, sp = line;
                const float wa = static_cast<float>(overlay::TextWidth(small, a.c_str()));
                const float wb = static_cast<float>(overlay::TextWidth(small, b.c_str()));
                float x = cx - (wa + 3 * sz + wb + 2 * sp) / 2;
                const float y = py + ph * 0.6f;
                left(small, a, x, y + line / 2, A(0xD8D8D8, 1.0f));
                x += wa + sp;
                for (overlay::Icon k : {overlay::Icon::PsCross, overlay::Icon::XbCross, overlay::Icon::KbSpace}) {
                    icons.push_back({k, x, y - sz / 2, sz});
                    x += sz;
                }
                left(small, b, x + sp, y + line / 2, A(0xD8D8D8, 1.0f));
            } else {
                centred(small, "OPEN", cx, py + ph * 0.6f + line / 2, A(0xA0A0A0, 1.0f));
            }
            continue;
        }
        const float step = std::max(0.0f, ph - band - (4 * line + icon)) / 6;
        float y = py + band + step + line * 1.2f;
        const bool editing = g_editing == i;
        centred(large, editing ? g_edit + "_" : s.name, cx, y, A(editing ? 0xFFE070 : 0xFFFFFF, 1.0f));
        y += step + line;
        std::string where = s.pc == 1 ? "HOST" : "PC " + std::to_string(s.pc);
        if (mine) where += " - THIS PC";
        centred(small, where + " - " + lobby::kColourNames[s.colour], cx, y, A(0xD0D0D0, 1.0f));
        y += step * 0.6f;
        if (mine) {   // the device(s) this player is worked with
            if (s.device == lobby::kDeviceAny || s.device == kKeyboard) {
                const float n = s.device == lobby::kDeviceAny ? 3.0f : 2.0f;
                float x = cx - icon * n / 2;
                icons.push_back({overlay::Icon::Keyboard, x, y, icon});
                icons.push_back({overlay::Icon::Mouse, x + icon, y, icon});
                if (s.device == lobby::kDeviceAny) icons.push_back({ConfirmIcon(0), x + 2 * icon, y, icon});
            } else {
                icons.push_back({ConfirmIcon(s.device), cx - icon / 2, y, icon});
            }
        } else {
            centred(small, DeviceText(s.device == lobby::kDeviceAny ? lobby::kDeviceAny : s.device), cx, y + icon / 2 + line / 2,
                    A(0x909090, 1.0f));
        }
        y += icon + step + line;
        const char* view = s.view == lobby::View::Own ? "OWN VIEW" : "SPLIT SCREEN";
        centred(small, mine ? std::string("< ") + view + " >" : std::string(view), cx, y, A(0xE8E8E8, 1.0f));
        y += step + line;
        const char* status = s.pc == 1 ? (s.ready ? "READY" : "HOST") : s.ready ? "READY" : "NOT READY";
        centred(small, status, cx, y, s.ready || s.pc == 1 ? A(colour | 0x404040, 1.0f) : A(0x909090, 1.0f));
    }
    // status: join code / connection / plugin check, then what the lobby waits for
    float y = y1 + line * 1.8f;
    const float maxY = h * 0.85f;
    for (const std::string& l : features::OnlineStatusLines(lobbynet::IsHost())) {
        if (y > maxY) break;
        const bool gold = l.rfind("JOIN CODE", 0) == 0 || l.find("REFUSED") != std::string::npos;
        std::string text = l;
        while (overlay::TextWidth(small, text.c_str()) > x1 - x0 && text.size() > 8) text.pop_back();
        left(small, text, x0, y, gold ? A(0xE0D878, 1.0f) : A(0xC8C8C8, 1.0f));
        y += line * 1.6f;
    }
    std::string who;
    const char* footer = !lobbynet::IsHost()           ? "WAITING FOR THE HOST TO START"
                         : T().Pcs() < 2               ? "WAITING FOR PLAYERS TO JOIN"
                         : !OthersReady(&who)          ? "WAITING FOR EVERY PLAYER TO BE READY"
                                                       : "PRESS START WHEN ALL PLAYERS ARE READY";
    centred(small, footer, w / 2, h * 0.875f, A(0xFFFFFF, 0.9f));
    // prompts: two rows
    using overlay::Icon;
    const float ps = std::max(12.0f, icon * 0.75f);
    struct Prompt { Icon a, b, c; const char* text; };
    std::vector<Prompt> row1 = {{Icon::PsCross, Icon::XbCross, Icon::KbSpace, "Join / Ready"},
                                {Icon::PsDpadHorizontal, Icon::XbDpadHorizontal, Icon::KbArrowsHorizontal, "Colour"},
                                {Icon::PsR1, Icon::XbR1, Icon::KbTab, "View"},
                                {Icon::PsCircle, Icon::XbCircle, Icon::KbBackspace, "Leave"}};
    std::vector<Prompt> row2;
    if (lobbynet::IsHost()) row2.push_back({Icon::PsStart, Icon::XbStart, Icon::KbEnter, "Start"});
    row2.push_back({Icon::KbN, Icon::KbN, Icon::KbN, "Name"});
    if (lobbynet::IsHost()) row2.push_back({Icon::KbC, Icon::KbC, Icon::KbC, "Copy join code"});
    row2.push_back({Icon::KbEscape, Icon::KbEscape, Icon::KbEscape, "Leave lobby"});
    auto drawRow = [&](const std::vector<Prompt>& row, float by) {
        float total = 0;
        for (const Prompt& p : row) {
            const int n = p.a == p.c ? 1 : 3;
            total += n * ps + ps / 4 + overlay::TextWidth(small, p.text) + ps;
        }
        float x = w / 2 - total / 2;
        for (const Prompt& p : row) {
            const int n = p.a == p.c ? 1 : 3;
            const Icon list[3] = {p.a, p.b, p.c};
            for (int k = 0; k < n; ++k) icons.push_back({list[k], x + k * ps, by - line / 2 - ps / 2, ps});
            x += n * ps + ps / 4;
            left(small, p.text, x, by, A(0xFFFFFF, 1.0f));
            x += overlay::TextWidth(small, p.text) + ps;
        }
    };
    drawRow(row1, h * 0.925f);
    drawRow(row2, h * 0.965f);
    overlay::Flush();   // the shapes, then the text over them
    for (const Text& t : texts) {
        const int x = t.centre ? static_cast<int>(t.x - overlay::TextWidth(t.font, t.text.c_str()) / 2.0f) : static_cast<int>(t.x);
        overlay::DrawLabel(t.font, t.text.c_str(), x, static_cast<int>(t.y), t.argb);
    }
    for (const IconAt& k : icons) overlay::QueueIcon(k.icon, k.x, k.y, k.size, g_alpha);
}

bool Build() {
    reinterpret_cast<void(__thiscall*)(void*)>(kMainMenuCtor)(g_object);
    std::memcpy(g_vtable, reinterpret_cast<void*>(kMainMenuVtable), sizeof g_vtable);
    const auto mp = reinterpret_cast<void* const*>(kMpVtable);
    g_vtable[1] = reinterpret_cast<void*>(&Create);
    g_vtable[3] = reinterpret_cast<void*>(&Setup);
    g_vtable[4] = reinterpret_cast<void*>(&Update);
    g_vtable[5] = reinterpret_cast<void*>(&Draw);
    g_vtable[13] = reinterpret_cast<void*>(&NoPrompt);
    g_vtable[17] = reinterpret_cast<void*>(&NoPrompt);
    g_vtable[9] = mp[9];   // back handling (as the join screen)
    At<void**>(g_object, 0) = g_vtable;
    overlay::AddLateDraw(&LateDraw);
    const bool ok = Create(g_object, nullptr);
    dslog::Write(ok ? "Lobby: screen 0x4F built" : "[fail] Lobby: screen 0x4F not built");
    return ok;
}
}  // namespace

void* features::LobbyScreen() {
    if (!g_built && *reinterpret_cast<void**>(kMainMenuScreen)) g_built = Build();
    return g_built ? g_object : nullptr;
}
