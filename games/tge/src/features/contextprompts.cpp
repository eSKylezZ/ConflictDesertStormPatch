// Context action prompts (part of tgefix.ingameinput, setting ActionPrompts): "[✕] Pick up fire extinguisher",
// "Hold [E] Take down" near the bottom of the screen while the player can do something. The game itself only pulses an
// icon (HUD widget 0x587E00, slot 72 of vtable 0x60E150) - it never says which button or what the action is.
//
// What the player can do (RE 2026-10-25): FUN_005BC860(block) runs every frame and stores the available action in
// block +0x29C (player 1: [0x7DCD4C]; blocks 0x7DCAB0 + i*0x3FC); ACTION (control id 1, "ACTION" in the table
// 0x6602B8) pressed = block +0x5C bit 2. Player character = [block +0x238] (player 1 [0x7DCCE8]). Targets: pick-up
// item = character +0x1924 (item +0x20 = its definition: +0 category, +0x5C u16 picture = an IMAGE_INV_* image), door
// = character +0x1700 (door +0x5C: bit 2 open, bit 0x20 locked). Timed holds (unlock without a key, takedown):
// block +0x394 active, +0x3B8 remaining ms, +0x3BC total ms (float). The picture's name comes from the game's own
// name -> image chain (0x594000..: `mov edi, "IMAGE_INV_X"; ... mov ax, n`), read once.
// Action ids (from the press switch / checks; some guessed): 2 use, 3 cancel planting, 4 / 0xD get out, 5 climb,
// 6 ladder, 7 / 0xC get in, 8 reload, 0xE unlock, 0xF lock, 0x10 locked, 0x12 pick up, 0x14 door, 0x15 show papers,
// 0x17 shake off, 0x18 takedown (hold), 0x24 release, 9 search a body; every other id = "Use" (logged once).
// The button: ACTION's binding from the key file (features::KeyFilePath) - the pad's when a pad drives (icon in the
// pad's style, modern swap applied), else the keyboard / mouse one.
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "core/log.h"
#include "features/features.h"
#include "features/overlay.h"

namespace {
constexpr uint32_t kBlock0 = 0x7DCAB0;
constexpr uint32_t kAction = kBlock0 + 0x29C, kCharacter = kBlock0 + 0x238, kDevices = kBlock0 + 0x14;
constexpr uint32_t kHoldActive = kBlock0 + 0x394, kHoldLeft = kBlock0 + 0x3B8, kHoldTotal = kBlock0 + 0x3BC;
constexpr uint32_t kPaused = 0x7DC068;
constexpr uint32_t kImageChainFrom = 0x590000, kImageChainTo = 0x5A0000;

template <class T>
bool Read(uintptr_t a, T& out) {
    __try {
        out = *reinterpret_cast<const T*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- item names ------------------------------------------------------------------------------------------------------
std::unordered_map<int, std::string> g_pictureNames;   // IMAGE_INV_* image -> "fire extinguisher"

// The picture names run words together (MILKBOTTLE, WIRECUTTERS ...): those are spelt out here, the rest lower-cased
// with "_" as spaces.
std::string Readable(const char* raw) {
    static const struct { const char* raw; const char* text; } kNames[] = {
        {"AMMOWELRODMK1", "Welrod ammo"}, {"ARMINIUS_MODEL_10", "Arminius revolver"}, {"BINO", "binoculars"},
        {"BOMBER_GUN_EMPLACEMENT", "bomber gun"}, {"BROKENCROWBAR", "broken crowbar"}, {"CHANGE", "change of clothes"},
        {"CITYPASS", "city pass"}, {"CIVILIANCLOTHES", "civilian clothes"}, {"CODEBOOK", "code book"},
        {"DOCTORSUNIFORM", "doctor's uniform"}, {"DRIVE_HANNOMAG", "Hanomag"}, {"EMPTYJERRYCAN", "empty jerry can"},
        {"FIRSTAIDKIT", "first aid kit"}, {"FOODPARCEL", "food parcel"}, {"FORGEDPASS", "forged pass"},
        {"FUELNOZZLE", "fuel nozzle"}, {"FULLJERRYCAN", "full jerry can"}, {"GATEPASS", "gate pass"},
        {"GEN_BIKE", "motorbike"}, {"GEN_CAR", "car"}, {"GEN_TRUCK", "truck"}, {"GERMANHANDGRENADE", "German grenade"},
        {"GERMANMONEY", "German money"}, {"GERMANUNIFORM", "German uniform"}, {"GERMAN_AA_GUN", "AA gun"},
        {"GROUNDCREWUNIFORM", "ground crew uniform"}, {"GUN_HANNOMAG", "Hanomag gun"}, {"HACKSAWBLADE", "hacksaw blade"},
        {"IDCARD", "ID card"}, {"INKPAPER", "ink and paper"}, {"KEYTRUCK", "truck key"},
        {"LIBERATORPISTOL", "Liberator pistol"}, {"LUGER_PARABELLUM_P08", "Luger"}, {"MAPDEPOT", "depot map"},
        {"MAS_M1E_1936", "MAS rifle"}, {"MAUSERKARABINER98K", "Kar98k"}, {"MEATHOOK", "meat hook"},
        {"MG42", "MG42"}, {"MG42_NEST", "MG42"}, {"MILKBOTTLE", "milk bottle"}, {"MP40", "MP40"},
        {"OFFICEFILES", "office files"}, {"ORIGINALPASS", "original pass"}, {"PANZERFAUST30", "Panzerfaust"},
        {"PANZERFAUST_BOX", "Panzerfaust crate"}, {"PATIENTUNIFORM", "patient's clothes"}, {"PISTOLAMMO", "pistol ammo"},
        {"POCKETKNIFE", "pocket knife"}, {"POWCLOTHES", "POW clothes"}, {"PRIESTSUNIFORM", "priest's robes"},
        {"REDCROSSPARCELS", "Red Cross parcels"}, {"RIFLE_AMMO", "rifle ammo"}, {"SKETCHPADPENCIL", "sketch pad"},
        {"SMG_AMMO", "SMG ammo"}, {"SPECIALPARCEL", "special parcel"}, {"SPECIALPASS", "special pass"},
        {"SSSOLDIERUNIFORM", "SS uniform"}, {"STARTERHANDLE", "starter handle"}, {"STENGUN", "Sten gun"},
        {"STEN_MKII", "Sten gun"}, {"TRAINTICKETS", "train tickets"}, {"TRAINTICKETSSTAMPED", "stamped train tickets"},
        {"UNSTAMPEDWORKPASS", "unstamped work pass"}, {"VALVEHANDLE", "valve handle"},
        {"WEBLEY_MK5_REVOLVER", "Webley revolver"}, {"WELRODMK1", "Welrod pistol"}, {"WIRECOIL", "coil of wire"},
        {"WIRECUTTERS", "wire cutters"}, {"WORKMANS_UNIFORM", "workman's clothes"}, {"WORKPASS", "work pass"},
    };
    for (const auto& n : kNames)
        if (strcmp(raw, n.raw) == 0) return n.text;
    std::string s = raw;
    for (char& c : s) c = c == '_' ? ' ' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}
void LoadPictureNames() {
    static bool done = false;
    if (done) return;
    done = true;
    // BF <name> 8B F0 B9 <len> 00 00 00 33 D2 F3 A6 75 07 5F 66 B8 <u16 image>
    const auto* p = reinterpret_cast<const uint8_t*>(kImageChainFrom);
    for (uint32_t o = 0; o + 24 < kImageChainTo - kImageChainFrom; ++o) {
        if (p[o] != 0xBF || p[o + 5] != 0x8B || p[o + 6] != 0xF0 || p[o + 7] != 0xB9 || p[o + 12] != 0x33 ||
            p[o + 14] != 0xF3 || p[o + 15] != 0xA6 || p[o + 18] != 0x5F || p[o + 19] != 0x66 || p[o + 20] != 0xB8)
            continue;
        const char* name = *reinterpret_cast<const char* const*>(p + o + 1);
        const int image = *reinterpret_cast<const uint16_t*>(p + o + 21);
        if (IsBadReadPtr(name, 10) || strncmp(name, "IMAGE_INV_", 10) != 0) continue;
        g_pictureNames.emplace(image, Readable(name + 10));
    }
    dslog::Write("Action prompts: %zu item names from the game's picture list", g_pictureNames.size());
}

std::string ItemName(uintptr_t character) {
    uintptr_t item = 0, def = 0;
    uint16_t picture = 0;
    if (!Read(character + 0x1924, item) || !item || !Read(item + 0x20, def) || !def || !Read(def + 0x5C, picture))
        return {};
    auto it = g_pictureNames.find(picture);
    return it == g_pictureNames.end() || it->second == "unused" || it->second == "empty" ? std::string() : it->second;
}

// ---- what to say -----------------------------------------------------------------------------------------------------
struct Prompt {
    std::string text;
    bool hold = false;     // the button is held (a timed action)
    bool button = true;    // no button for "Locked"
};
bool Describe(int action, uintptr_t character, Prompt& p) {
    switch (action) {
    case 0x12: {
        const std::string item = ItemName(character);
        p.text = item.empty() ? "Pick up" : "Pick up " + item;
        return true;
    }
    case 0x14: {
        uintptr_t door = 0;
        uint32_t flags = 0;
        // door +0x5C bit 2 set = closed (the player's report: the texts were the wrong way round with "set = open")
        const bool closed = Read(character + 0x1700, door) && door && Read(door + 0x5C, flags) && (flags & 2);
        p.text = closed ? "Open door" : "Close door";
        return true;
    }
    case 0x04: case 0x0D: p.text = "Get out"; return true;
    case 0x07: case 0x0C: p.text = "Get in"; return true;
    case 0x05: p.text = "Climb"; return true;
    case 0x06: p.text = "Use ladder"; return true;
    case 0x08: p.text = "Reload"; return true;
    case 0x03: p.text = "Cancel"; return true;
    case 0x0E: p.text = "Unlock"; return true;
    case 0x0F: p.text = "Lock"; return true;
    case 0x10: p.text = "Locked"; p.button = false; return true;
    case 0x11: p.text = "Talk"; return true;   // seen on the prisoners / guards one talks to (player's log)
    case 0x15: p.text = "Show papers"; return true;
    case 0x17: p.text = "Shake off"; return true;
    case 0x18: p.text = "Take down"; p.hold = true; return true;
    case 0x24: p.text = "Let go"; return true;
    case 0x09: p.text = "Search"; return true;
    case 0x01: case 0x0A: case 0x21: case 0x22: return false;   // none / give (2 players) / no icon in the game either
    }
    // the rest: "Use" (+ the item it points at, if any)
    const std::string item = ItemName(character);
    p.text = item.empty() ? "Use" : "Use " + item;
    return true;
}

// ---- the ACTION button ---------------------------------------------------------------------------------------------
struct Binding {
    int pad = -1;              // game button number
    std::string key;           // key file name: KEY_E, MOUSE_BUTTON_0 ...
    FILETIME stamp{};
    std::string path;
} g_binding;
void LoadBinding() {
    const std::string path = features::KeyFilePath();
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (path.empty() || !GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info)) return;
    if (path == g_binding.path && CompareFileTime(&info.ftLastWriteTime, &g_binding.stamp) == 0) return;
    g_binding = Binding{};
    g_binding.path = path;
    g_binding.stamp = info.ftLastWriteTime;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") || !f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char action[64] = {}, code[64] = {};
        int slot = 0;
        if (sscanf_s(line, "%63[^,],%d=%63s", action, 64, &slot, code, 64) != 3 || strcmp(action, "ACTION") != 0) continue;
        if (strncmp(code, "JOY_BUTTON_", 11) == 0) {
            if (g_binding.pad < 0) g_binding.pad = atoi(code + 11);
        } else if (g_binding.key.empty() && strncmp(code, "JOY_", 4) != 0) {
            g_binding.key = code;
        }
    }
    fclose(f);
}

int Physical(int game) {   // controller.cpp's PadModernLayout swap
    if (!features::Get().padModernLayout) return game;
    switch (game) {
    case 4: return 6;
    case 5: return 7;
    case 6: return 4;
    case 7: return 5;
    }
    return game;
}

using gen::GameIcon;
// The key's icon, or Count with the key's own name in `label`.
GameIcon KeyIcon(const std::string& code, std::string& label) {
    std::string k = code.rfind("KEY_", 0) == 0 ? code.substr(4) : code;
    if (k.size() == 1 && k[0] >= 'A' && k[0] <= 'Z') return static_cast<GameIcon>(static_cast<int>(GameIcon::KbA) + (k[0] - 'A'));
    if (k.size() == 1 && k[0] >= '0' && k[0] <= '9') return static_cast<GameIcon>(static_cast<int>(GameIcon::Kb0) + (k[0] - '0'));
    static const struct { const char* name; GameIcon icon; } kNamed[] = {
        {"SPACE", GameIcon::KbSpace},     {"RETURN", GameIcon::KbEnter},   {"ENTER", GameIcon::KbEnter},
        {"TAB", GameIcon::KbTab},         {"LSHIFT", GameIcon::KbShift},   {"RSHIFT", GameIcon::KbShift},
        {"LCONTROL", GameIcon::KbCtrl},   {"RCONTROL", GameIcon::KbCtrl},  {"LALT", GameIcon::KbAlt},
        {"RALT", GameIcon::KbAlt},        {"LMENU", GameIcon::KbAlt},      {"BACKSPACE", GameIcon::KbBackspace},
        {"BACK", GameIcon::KbBackspace},  {"MOUSE_BUTTON_0", GameIcon::MouseLeft},
        {"MOUSE_BUTTON_1", GameIcon::MouseRight}, {"MOUSE_BUTTON_2", GameIcon::MouseMiddle},
    };
    for (const auto& n : kNamed)
        if (k == n.name) return n.icon;
    label = k;
    return GameIcon::Count;
}

// ---- drawing -------------------------------------------------------------------------------------------------------
DWORD g_shownSince = 0;
int g_lastAction = 0;

// Only while the in-game HUD screen is the top GUI screen (GUI [0x7D8EC4] +0xA4, hash +0xC): not over the notebook,
// the pause menu or any other screen opened in a mission.
bool HudOnTop() {
    constexpr uint32_t kGui = 0x7D8EC4, kIngameHud = 0x82B9E96B;
    uintptr_t gui = 0, top = 0;
    uint32_t hash = 0;
    return Read(kGui, gui) && gui && Read(gui + 0xA4, top) && top && Read(top + 0xC, hash) && hash == kIngameHud;
}

void Draw() {
    const features::Settings& s = features::Get();
    if (!s.actionPrompts || !features::InMission()) return;
    int action = 0, paused = 0;
    uintptr_t character = 0;
    if (!Read(kAction, action) || !Read(kPaused, paused) || paused || !HudOnTop() || !Read(kCharacter, character) ||
        !character) {
        g_lastAction = 0;
        return;
    }
#ifndef DS_DIST
    {   // Dev\ForcePrompt = an action id: drawn as if it were available (scripted checks of the prompt's look)
        static DWORD checked = 0, forced = 0;
        if (GetTickCount() - checked > 1000) {
            checked = GetTickCount();
            DWORD size = sizeof forced;
            if (RegGetValueA(HKEY_CURRENT_USER, "Software\\GreatEscapeFix\\Dev", "ForcePrompt", RRF_RT_REG_DWORD, nullptr,
                             &forced, &size) != ERROR_SUCCESS)
                forced = 0;
        }
        if (forced) action = static_cast<int>(forced);
    }
#endif
    Prompt p;
    if (!action || !Describe(action, character, p)) {
        g_lastAction = 0;
        return;
    }
    LoadPictureNames();
    LoadBinding();
    static uint64_t logged = 0;   // each id once: which action the game uses where (names partly inferred)
    if (action > 0 && action < 64 && !(logged & (1ull << action))) {
        logged |= 1ull << action;
        dslog::Write("Action prompts: action 0x%X -> \"%s\"", action, p.text.c_str());
    }
    const DWORD now = GetTickCount();
    if (action != g_lastAction) g_lastAction = action, g_shownSince = now;
    const float alpha = std::min(1.0f, (now - g_shownSince) / 150.0f);   // fades in
    int holdActive = 0;
    float left = 0, total = 0;
    const bool holding = Read(kHoldActive, holdActive) && holdActive && Read(kHoldLeft, left) && Read(kHoldTotal, total) &&
                         total > 0;
    if (holding) p.hold = true;

    float w, h;
    overlay::ScreenSize(w, h);
    const float size = h / 26, pad = size * 0.35f;
    uint32_t devices = 0;
    Read(kDevices, devices);
    const bool padMode = (devices & 3) == 2 && g_binding.pad >= 0 && g_binding.pad < 16;
    GameIcon icon = GameIcon::Count;
    std::string keyLabel;
    if (p.button) {
        if (padMode) {
            const int first = features::PadInUseIsXbox() ? static_cast<int>(GameIcon::XbTriangle) : static_cast<int>(GameIcon::PsTriangle);
            icon = static_cast<GameIcon>(first + Physical(g_binding.pad));
        } else if (!g_binding.key.empty()) {
            icon = KeyIcon(g_binding.key, keyLabel);
        }
    }
    const std::string lead = p.hold ? "Hold" : "";
    const float textH = size * 0.62f;
    const float leadW = lead.empty() ? 0 : overlay::TextWidth(lead.c_str(), textH) + pad;
    const float iconW = !p.button ? 0 : icon != GameIcon::Count ? size + pad : overlay::TextWidth(("[" + keyLabel + "]").c_str(), textH) + pad;
    const float textW = overlay::TextWidth(p.text.c_str(), textH);
    const float boxW = pad * 2 + leadW + iconW + textW, boxH = size + pad * 2;
    float x = (w - boxW) / 2;
    const float y = h * 0.80f;
    const uint32_t a = static_cast<uint32_t>(alpha * 255.0f);
    overlay::QueueRect(x, y, boxW, boxH, (static_cast<uint32_t>(alpha * 0xA0) << 24));
    x += pad;
    const float ty = y + pad + (size - textH) / 2;
    if (!lead.empty()) {
        overlay::QueueText(lead.c_str(), x, ty, textH, (a << 24) | 0xE8E8E8);
        x += leadW;
    }
    if (p.button) {
        if (icon != GameIcon::Count) overlay::QueueIcon(icon, x, y + pad, size, alpha);
        else overlay::QueueText(("[" + keyLabel + "]").c_str(), x, ty, textH, (a << 24) | 0xFFFFFF);
        x += iconW;
    }
    overlay::QueueText(p.text.c_str(), x, ty, textH, (a << 24) | 0xFFFFFF);
    if (holding) {   // how far the hold has got
        const float done = std::clamp(1.0f - left / total, 0.0f, 1.0f);
        overlay::QueueRect(x - iconW, y + boxH - pad * 0.6f, (boxW - pad * 2 - leadW) * done, pad * 0.35f,
                           (a << 24) | 0xE8C040);
    }
}
}  // namespace

void features::ApplyContextPrompts() {
    overlay::Install();
    overlay::AddLateDraw(&Draw);
    dslog::Write("[ok]   Action prompts: \"Press [button] to ...\" while an action is available (ActionPrompts)");
}
