// Per-player controller layouts, see padlayout.h.
//
// The game's pad bindings come from default.key next to the exe ("FIRE,1=JOY_BUTTON_7", "FORWARD,1=JOY_AXIS_0") with
// the action names of the game's table 0x5EC2B8 ({char* name, u32 id} x 80). Presets are transforms of it:
//   Classic    as default.key (fire R1, orders L1, targeting L2 / R2);
//   Default    L1 <-> L2 and R1 <-> R2 in every context: fire on R2, orders on L2, targeting on the bumpers (modern);
//   Southpaw   Default with the sticks swapped (axis codes 0-5 <-> 6-11).
// Saved per player in the registry (Enhancements\PadPreset<n> DWORD, PadLayout<n> binary for Custom).
// The binding table [0x60687C] = {sets, slots, actions, int32* data}, entry (set, slot, action) at
// data[(set * slots + slot) * actions + action]; set n = player n+1 (4 sets, splitscreen.cpp).
#include "features/padlayout.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/log.h"

namespace {
constexpr uint32_t kBindingTable = 0x60687C, kActionNames = 0x5EC2B8;
constexpr const char* kKey = "SOFTWARE\\Pivotal Games\\Conflict Desert Storm\\Enhancements";
constexpr int kButton = 2, kAxis = 1;

int Type(int32_t e) { return e < 0 ? -1 : e >> 16; }
bool IsPad(int32_t e) { return Type(e) == kButton || Type(e) == kAxis; }

padlayout::Layout Empty() {
    padlayout::Layout l;
    memset(l.entry, 0xFF, sizeof l.entry);
    return l;
}

int ActionId(const char* name) {
    struct Name {
        const char* name;
        uint32_t id;
    };
    const auto* names = reinterpret_cast<const Name*>(kActionNames);
    for (int i = 0; i < padlayout::kActions; ++i)
        if (_stricmp(names[i].name, name) == 0) return static_cast<int>(names[i].id);
    return -1;
}

// Pad entries of default.key.
const padlayout::Layout& Defaults() {
    static padlayout::Layout l = [] {
        padlayout::Layout d = Empty();
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (char* slash = strrchr(path, '\\')) strcpy_s(slash + 1, path + MAX_PATH - slash - 1, "default.key");
        FILE* f = nullptr;
        if (fopen_s(&f, path, "r") || !f) {
            dslog::Write("[fail] Pad layouts: default.key not readable");
            return d;
        }
        char line[128];
        int count = 0;
        while (fgets(line, sizeof line, f)) {
            char name[64], value[64];
            int slot = 0;
            if (sscanf_s(line, "%63[^,],%d=%63s", name, static_cast<unsigned>(sizeof name), &slot, value,
                         static_cast<unsigned>(sizeof value)) != 3)
                continue;
            int code = 0, type = 0;
            if (sscanf_s(value, "JOY_BUTTON_%d", &code) == 1) type = kButton;
            else if (sscanf_s(value, "JOY_AXIS_%d", &code) == 1) type = kAxis;
            else continue;
            const int action = ActionId(name);
            if (action < 0 || action >= padlayout::kActions) continue;
            for (int32_t& e : d.entry[action])
                if (e < 0) {
                    e = type << 16 | code;
                    ++count;
                    break;
                }
        }
        fclose(f);
        dslog::Write("Pad layouts: %d pad bindings in default.key", count);
        return d;
    }();
    return l;
}

padlayout::Layout g_layout[padlayout::kPlayers];
padlayout::Preset g_preset[padlayout::kPlayers];
bool g_vibration[padlayout::kPlayers] = {true, true, true, true};
bool g_triggers[padlayout::kPlayers] = {true, true, true, true};
int g_look[padlayout::kPlayers] = {100, 100, 100, 100};
int g_gyro[padlayout::kPlayers] = {};

DWORD ReadValue(const char* base, int player, DWORD fallback) {
    char name[32];
    snprintf(name, sizeof name, "%s%d", base, player + 1);
    DWORD v = fallback, size = sizeof v;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, kKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) v = fallback;
    return v;
}
bool ReadFlag(const char* base, int player) { return ReadValue(base, player, 1) != 0; }

void WriteValue(const char* base, int player, DWORD v) {
    HKEY key = nullptr;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    char name[32];
    snprintf(name, sizeof name, "%s%d", base, player + 1);
    RegSetValueExA(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v);
    RegCloseKey(key);
}
void WriteFlag(const char* base, int player, bool on) { WriteValue(base, player, on ? 1 : 0); }
bool g_loaded = false;

void Load() {
    if (g_loaded) return;
    g_loaded = true;
    for (int p = 0; p < padlayout::kPlayers; ++p) {
        char name[32];
        DWORD v = 0, size = sizeof v;
        snprintf(name, sizeof name, "PadPreset%d", p + 1);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, kKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS ||
            v >= static_cast<DWORD>(padlayout::Preset::Count))
            v = 0;
        g_preset[p] = static_cast<padlayout::Preset>(v);
        g_vibration[p] = ReadFlag("PadVibration", p);
        g_triggers[p] = ReadFlag("PadTriggers", p);
        g_gyro[p] = std::clamp(static_cast<int>(ReadValue("PadGyro", p, 0)), 0, 2);
        g_look[p] = std::clamp(static_cast<int>(ReadValue("PadLook", p, 100)), padlayout::kLookMin, padlayout::kLookMax);
        g_layout[p] = padlayout::Make(g_preset[p]);
        if (g_preset[p] == padlayout::Preset::Custom) {
            snprintf(name, sizeof name, "PadLayout%d", p + 1);
            size = sizeof g_layout[p];
            padlayout::Layout custom;
            if (RegGetValueA(HKEY_LOCAL_MACHINE, kKey, name, RRF_RT_REG_BINARY, nullptr, &custom, &size) ==
                    ERROR_SUCCESS && size == sizeof custom)
                g_layout[p] = custom;
        }
    }
}
}  // namespace

padlayout::Context padlayout::ContextOf(int a) {
    if (a == 26 || a == 43) return Context::OnFoot;      // ORDER / INVENTORY open their menus
    if (a >= 27 && a <= 42) return Context::Orders;      // ORDER_UP .. ORDER_ADVANCE_ROTATE_RIGHT
    if (a >= 44 && a <= 51) return Context::Inventory;   // INVENTORY_UP .. GIVE_TAKE_AMMO
    if (a >= 54) return Context::None;                   // cutscenes, objectives screen, menus, PC-only keys
    return Context::OnFoot;
}

padlayout::Layout padlayout::Make(Preset preset) {
    Layout l = Defaults();
    if (preset == Preset::Default || preset == Preset::Southpaw) {
        for (int a = 0; a < kActions; ++a)
            for (int32_t& e : l.entry[a]) {
                if (Type(e) != kButton || ContextOf(a) == Context::None) continue;
                static const int swap[16] = {0, 1, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15};  // L2<->L1, R2<->R1
                e = kButton << 16 | swap[e & 15];
            }
    }
    if (preset == Preset::Southpaw) {
        for (int a = 0; a < kActions; ++a)
            for (int32_t& e : l.entry[a])
                if (Type(e) == kAxis && ContextOf(a) != Context::None) {
                    const int c = e & 0xFFFF;
                    if (c < 12) e = kAxis << 16 | (c < 6 ? c + 6 : c - 6);
                }
    }
    return l;
}

padlayout::Preset padlayout::PresetOf(int player) {
    Load();
    return g_preset[player];
}

const padlayout::Layout& padlayout::Get(int player) {
    Load();
    return g_layout[player];
}

void padlayout::Set(int player, Preset preset, const Layout& layout) {
    Load();
    g_preset[player] = preset;
    g_layout[player] = layout;
    HKEY key = nullptr;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        char name[32];
        const DWORD v = static_cast<DWORD>(preset);
        snprintf(name, sizeof name, "PadPreset%d", player + 1);
        RegSetValueExA(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v);
        snprintf(name, sizeof name, "PadLayout%d", player + 1);
        if (preset == Preset::Custom)
            RegSetValueExA(key, name, 0, REG_BINARY, reinterpret_cast<const BYTE*>(&layout), sizeof layout);
        else
            RegDeleteValueA(key, name);
        RegCloseKey(key);
    }
    ApplyToGame();
}

int padlayout::ButtonOf(const Layout& l, int action) {
    for (int32_t e : l.entry[action])
        if (Type(e) == kButton) return e & 0xFFFF;
    return -1;
}

void padlayout::Assign(Layout& l, int action, int button) {
    const int old = ButtonOf(l, action);
    if (old == button) return;
    const Context ctx = ContextOf(action);
    for (int a = 0; a < kActions; ++a) {
        if (a == action || ContextOf(a) != ctx) continue;
        for (int32_t& e : l.entry[a])
            if (Type(e) == kButton && (e & 0xFFFF) == button) e = old >= 0 ? (kButton << 16 | old) : -1;
    }
    bool placed = false;
    for (int32_t& e : l.entry[action])
        if (Type(e) == kButton) {
            e = placed ? -1 : (kButton << 16 | button);  // the first pad button becomes the new one
            placed = true;
        }
    if (!placed)
        for (int32_t& e : l.entry[action])
            if (e < 0) {
                e = kButton << 16 | button;
                break;
            }
}

void padlayout::ApplyToGame() {
    Load();
    auto* table = *reinterpret_cast<uint32_t**>(kBindingTable);
    if (!table || !table[3] || table[1] != kSlots || table[2] != kActions) return;
    auto* data = reinterpret_cast<int32_t*>(table[3]);
    const int sets = static_cast<int>(table[0]) < kPlayers ? static_cast<int>(table[0]) : kPlayers;
    for (int s = 0; s < sets; ++s) {
        for (int a = 0; a < kActions; ++a) {
            int32_t* slot[kSlots];
            int32_t keep[kSlots];
            int n = 0;
            for (int k = 0; k < kSlots; ++k) {
                slot[k] = &data[(s * kSlots + k) * kActions + a];
                if (*slot[k] >= 0 && !IsPad(*slot[k])) keep[n++] = *slot[k];  // keyboard / mouse
            }
            for (int32_t e : g_layout[s].entry[a])
                if (e >= 0 && n < kSlots) keep[n++] = e;
            for (int k = 0; k < kSlots; ++k) *slot[k] = k < n ? keep[k] : -1;
        }
    }
}

bool padlayout::Vibration(int player) {
    Load();
    return player < 0 || player >= kPlayers || g_vibration[player];
}

void padlayout::SetVibration(int player, bool on) {
    Load();
    if (player < 0 || player >= kPlayers) return;
    g_vibration[player] = on;
    WriteFlag("PadVibration", player, on);
}

bool padlayout::AdaptiveTriggers(int player) {
    Load();
    return player < 0 || player >= kPlayers || g_triggers[player];
}

void padlayout::SetAdaptiveTriggers(int player, bool on) {
    Load();
    if (player < 0 || player >= kPlayers) return;
    g_triggers[player] = on;
    WriteFlag("PadTriggers", player, on);
}

int padlayout::LookSensitivity(int player) {
    Load();
    return player < 0 || player >= kPlayers ? 100 : g_look[player];
}

void padlayout::SetLookSensitivity(int player, int percent) {
    Load();
    if (player < 0 || player >= kPlayers) return;
    g_look[player] = std::clamp(percent, kLookMin, kLookMax);
    WriteValue("PadLook", player, static_cast<DWORD>(g_look[player]));
}

int padlayout::GyroMode(int player) {
    Load();
    return player < 0 || player >= kPlayers ? 0 : g_gyro[player];
}

void padlayout::SetGyroMode(int player, int mode) {
    Load();
    if (player < 0 || player >= kPlayers) return;
    g_gyro[player] = std::clamp(mode, 0, 2);
    WriteValue("PadGyro", player, static_cast<DWORD>(g_gyro[player]));
}
