// OPTIONS > DESERTSTORMFIX (front-end state 0x47): the mod's optional features as ON / OFF rows. The core
// (widescreen, HUD scaling, controllers, split screen / co-op join, timing / display / graphics) is not listed - it
// is always on.
//
// Also adds OPTIONS > VIDEO (videomenu.cpp, state 0x4C) above it.
//
// Options screen (state 4, class vtable 0x5D91B0): create 0x471870 (list capacity 9, pool: CHEATS 0x7F0 only while
// cheats are unlocked, SFX 0x7F1, MUSIC 0x7F2, CONTROLS 0x7F7), set-up 0x4719D0 (adds the pool's items in order, panel
// layout, cursor back to [screen+0x24]), update 0x471C40 (list input at 0x471C47, ids 0x7EF..0x7F7 handled, the rest
// ignored). Create (slot 0x5D91B4) and set-up (slot 0x5D91BC) are wrapped: one more plain item at the end of the pool,
// shown last as DESERTSTORMFIX (id 0x7F8); the update's list input call is hooked to open state 0x47 for it (and keep
// the row for the way back, as the original does for its own items).
//
// Our screen: an Options sub-screen object (like developer.cpp / modsmenu.cpp) with choice rows (0x68-byte items, ctor
// 0x4E5010, like cheats.cpp's) in a window of kVisible rows that scrolls like the MODS menu. The row -> settings field
// mapping is kRows below; whether a change needs a restart comes from settings::Toggles() at run time (the four
// launcher booleans: known here). Restart rows show "ON *" / "OFF *"; the prompt line says so, or asks for a restart
// once such a row differs from what the game started with.
//
// The first row, PLUGINS, opens pluginsmenu.cpp's list of PSE plugins (state 0x4D) with a settings page per plugin built
// from pse.settings. ApplyFixMenu also registers pse.ui (pseui.cpp) - it runs inside DesertStormFix's Load, before any
// external plugin can ask for it.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/log.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/overlay.h"
#include "loader.h"

namespace {
constexpr uint32_t kSetState = 0x47EF20, kBackPressed = 0x478380, kListInput = 0x4E5D80;
constexpr uint32_t kAlloc = 0x4B83D0, kListCtor = 0x4E5B10, kListReserve = 0x4E56C0, kListAddToPool = 0x4E58C0;
constexpr uint32_t kItemCtor = 0x4E4F90, kChoiceCtor = 0x4E5010, kItemSet = 0x4E4FB0, kChoices = 0x4E5090;
constexpr uint32_t kListCallbacks = 0x4E59E0, kChoiceDraw = 0x402E20, kRelayout = 0x4E8130;
constexpr uint32_t kDrawPanel = 0x456EB0, kDrawTitle = 0x456F40;
constexpr uint32_t kLargeFont = 0x60EDB8, kHudSheet = 0x60EE18, kMainMenuScreen = 0x617A00, kRenderer = 0x63C924;
constexpr uint32_t kMainMenuEnter = 0x4153B0, kOptCtor = 0x409B90, kOptVtable = 0x5D8510, kOptSetup = 0x409C90;
constexpr uint32_t kStateOptions = 4, kStateFixMenu = 0x47;
constexpr int kVtableEntries = 21;

// the game's Options screen
constexpr uint32_t kOptionsCreateSlot = 0x5D91B4, kOptionsCreate = 0x471870;
constexpr uint32_t kOptionsSetupSlot = 0x5D91BC, kOptionsSetup = 0x4719D0;
constexpr uint32_t kOptionsListInputCall = 0x471C47, kOptionsItemDraw = 0x471D90, kOptionsTitle = 0x29257EAE;
constexpr uint16_t kOptionsItem = 0x7F8, kVideoItem = 0x7F9;
constexpr uint32_t kStateVideo = 0x4C;  // videomenu.cpp
constexpr uint32_t kStatePlugins = 0x4D;  // pluginsmenu.cpp

enum : uint32_t {
    kTextVideoItem = 0xC0120001,  // videomenu.cpp's title
    kTextItem = 0xC00B0001, kTextOff, kTextOn, kTextOffRestart, kTextOnRestart, kTextRow0 = 0xC00B0010,
    kTextFov0 = 0xC00B0020, kTextGameColour = 0xC00B0030, kTextCustomColour, kTextColourTitle, kTextHudColourTitle,
    kTextChoice0 = 0xC00B0040,  // + visible row * 8 + choice (Kind::Choice rows)
    kTextLink = 0xC00B0090      // the PLUGINS row's value
};

// FIELD OF VIEW rows: the choice item holds at most 8 texts; vertical degrees (the game's own is 70).
constexpr uint32_t kFovValues[] = {55, 60, 65, 70, 75, 80, 85, 90};
constexpr int kFovCount = 8, kFovDefault = 3;
const char* const kFovTexts[] = {"55", "60", "65", "70 (GAME)", "75", "80", "85", "90"};

// Kind: Toggle = one of settings::Toggles() (restart flag from there); Live / Restart = the launcher's booleans;
// Fov = settings fov[player] (splitscreen.cpp's projection reads it every frame); Colour = menu bar (player 0) / HUD
// (player 1) colour: GAME / CUSTOM, Enter opens the colour wheel - barcolour.cpp; Choice = a number setting with up to 8
// named values (text(i)), read live by its feature; Plugin = a live bool setting of another PSE plugin (pse.settings
// `key` of `plugin`; OFF and not changeable when that plugin isn't loaded); Link = opens the PLUGINS list.
enum class Kind { Toggle, Live, Restart, Fov, Colour, Choice, Plugin, Link };
struct Row {
    const char* label;
    bool settings::Values::*field;
    Kind kind;
    int player = 0;
    uint32_t settings::Values::*number = nullptr;
    int choices = 0;
    const char* (*text)(int) = nullptr;
    const char* plugin = nullptr;
    const char* key = nullptr;
};
const Row kRows[] = {
    // pluginsmenu.cpp: every PSE plugin, its load state and its pse.settings
    {"PLUGINS", nullptr, Kind::Link},
    {"FIELD OF VIEW - PLAYER 1", nullptr, Kind::Fov, 0},
    {"FIELD OF VIEW - PLAYER 2", nullptr, Kind::Fov, 1},
    {"FIELD OF VIEW - PLAYER 3", nullptr, Kind::Fov, 2},
    {"FIELD OF VIEW - PLAYER 4", nullptr, Kind::Fov, 3},
    // barcolour.cpp reads it at every bind of the menu art
    {"MENU BAR COLOUR", nullptr, Kind::Colour, 0},
    {"HUD COLOUR", nullptr, Kind::Colour, 1},
    {"BRIEFING & LOADOUT", &settings::Values::featLoadout, Kind::Toggle},
    {"UNIFORM SELECT SCREEN", &settings::Values::featUniformSelect, Kind::Toggle},
    {"TIME OF DAY OPTIONS", &settings::Values::featTimeOfDay, Kind::Toggle},
    // weather.cpp reads it at each level start (the WEATHER row needs BRIEFING & LOADOUT too)
    {"WEATHER OPTIONS", &settings::Values::featWeather, Kind::Toggle},
    // footprints.cpp reads them at every footstep / frame
    {"FOOTPRINTS", nullptr, Kind::Choice, 0, &settings::Values::footprints, features::kFootprintModes, &features::FootprintLabel},
    {"FOOTPRINTS: EVERYONE", &settings::Values::featFootprintsAll, Kind::Toggle},
    {"WEATHER WIPES FOOTPRINTS", &settings::Values::featFootprintsWeather, Kind::Toggle},
    // customise.cpp reads it every frame (POUCHES row or one row per pouch spot)
    {"FREE POUCH CHOICE", &settings::Values::featFreeGear, Kind::Toggle},
    // customise.cpp reads it every frame (R / R3 on the screen switches it too)
    {"LOADOUT CAMERA SPIN", &settings::Values::featLoadoutSpin, Kind::Toggle},
    // customise.cpp reads it while the screen settles
    {"LOADOUT: LOOK AHEAD", &settings::Values::featLoadoutLookAhead, Kind::Toggle},
    {"SQUAD VOICE ACCENTS", &settings::Values::featVoices, Kind::Toggle},
    {"YOUR SOLDIER TALKS", &settings::Values::featPlayerVoice, Kind::Toggle},
    // nameplates.cpp reads it every frame (co-op: the other players' names over their soldiers)
    {"NAMEPLATES", &settings::Values::featNameplates, Kind::Toggle},
    {"STREAMER MODE", &settings::Values::featStreamerMode, Kind::Toggle},
    {"PLAYABLE FOLLOWERS", nullptr, Kind::Plugin, 0, nullptr, 0, nullptr, "dsfix.followers", "FeatFollowers"},
    // multikbm.cpp: read when the co-op join screen opens and at each session start
    {"SEVERAL KEYBOARDS & MICE", &settings::Values::featMultiKbm, Kind::Toggle},
    // coopscripts.cpp reads it each time a mission asks for the player count
    {"CO-OP MISSION SCRIPTS", &settings::Values::featCoopScripts, Kind::Toggle},
    // own plugins (PSE\Plugins); VERY EASY & VERY HARD needs a restart (declared without PSE_SETTING_LIVE)
    {"VERY EASY & VERY HARD", nullptr, Kind::Plugin, 0, nullptr, 0, nullptr, "dsfix.difficulty", "FeatVeryHard"},
    {"JAPANESE RELEASE FIXES", nullptr, Kind::Plugin, 0, nullptr, 0, nullptr, "dsfix.jpfixes", "FeatJpFixes"},
    {"ENHANCED RUMBLE", nullptr, Kind::Plugin, 0, nullptr, 0, nullptr, "dsfix.rumble", "FeatRumble"},
    {"INVENTORY HINTS", nullptr, Kind::Plugin, 0, nullptr, 0, nullptr, "dsfix.tooltips", "FeatTooltips"},
    // the plugins dsfix.audio / dsfix.shadows (PSE\Plugins) follow their settings at once (OnChange)
    {"AIMING AUDIO FIX", nullptr, Kind::Plugin, 0, nullptr, 0, nullptr, "dsfix.audio", "FeatAimAudio"},
    {"SHADOW FIX", nullptr, Kind::Plugin, 0, nullptr, 0, nullptr, "dsfix.shadows", "FeatShadowFix"},
    {"SHARPER TEXT", nullptr, Kind::Plugin, 0, nullptr, 0, nullptr, "dsfix.fontsharp", "FeatSharpText"},
    {"MODS", &settings::Values::featMods, Kind::Toggle},
    // mods.cpp reads PSE\Library's .weapon / .skin files at start-up when it is on (restart)
    {"OTHER GAMES' GEAR", &settings::Values::featLibraryGear, Kind::Toggle},
    {"CUSTOM CAMPAIGNS", &settings::Values::featCustomMissions, Kind::Toggle},
    {"VERSUS MENU", &settings::Values::featVersus, Kind::Toggle},
    {"EXTENDED CHEATS", &settings::Values::featCheats, Kind::Toggle},
    // lockstep.cpp applies it every frame
    {"RUN IN BACKGROUND", &settings::Values::featRunInBackground, Kind::Toggle},
    // console.cpp reads it every frame
    {"DEVELOPER CONSOLE", &settings::Values::featDevConsole, Kind::Toggle},
    // the external plugin dsfix.discord (PSE\Plugins) follows its setting at once (OnChange)
    {"DISCORD STATUS", nullptr, Kind::Plugin, 0, nullptr, 0, nullptr, "dsfix.discord", "DiscordPresence"},
    // read once by the first front-end load (re-applying it now would queue the intro film for the next menu load)
    {"SKIP INTRO", &settings::Values::skipIntro, Kind::Restart},
    // read by the movie-cancel hook every time (cinematics.cpp)
    {"SKIP CUTSCENES", &settings::Values::skipCutscenes, Kind::Live},
    // controller.cpp's poll hook and pad fix-ups are installed at start-up (the launcher's checkbox)
    {"CONTROLLER SUPPORT", &settings::Values::controller, Kind::Restart},
    // ApplyDiagnostics re-reads it (its hooks are installed once)
    {"PERFORMANCE OVERLAY", &settings::Values::perfOverlay, Kind::Live},
};
constexpr int kRowCount = static_cast<int>(sizeof kRows / sizeof kRows[0]);
constexpr int kVisible = 8;
constexpr uint16_t kFirstId = 0x890;  // visible row k = kFirstId + k

int g_atStart[kRowCount];  // values the game started with (restart rows: changed = restart needed)
int g_offset = 0;           // first row shown (scrolling window)
int g_lastRow = 0;          // list row selected last frame
void* g_optionsItem = nullptr;
void* g_videoItem = nullptr;
uint8_t g_screen[0x20];
void* g_vtable[kVtableEntries];
bool g_built = false;

template <class T>
T& At(void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}
template <class T>
T& Global(uint32_t va) {
    return *reinterpret_cast<T*>(va);
}
void SetState(uint32_t s) { reinterpret_cast<void(__cdecl*)(uint32_t)>(kSetState)(s); }
void* ItemAt(void* list, int i) { return At<void**>(list, 0x2C)[i]; }
void AddToList(void* list, void* item) {
    reinterpret_cast<void(__thiscall*)(void*, void*)>((*reinterpret_cast<void***>(list))[1])(list, item);
}
using ItemSetFn = void(__thiscall*)(void*, int, uint32_t, int, int, uint32_t, int, uint32_t, int);

// ON / OFF rows: 0 / 1; FIELD OF VIEW rows: the choice index (a value saved off the list counts as the nearest).
int Value(int r) {
    if (kRows[r].kind == Kind::Link) return 0;
    if (kRows[r].kind == Kind::Plugin) {
        const PSEPluginHandle h = pse_host::Find(kRows[r].plugin);
        return h && pse::SettingsInterface()->GetInt(h, kRows[r].key) ? 1 : 0;
    }
    if (kRows[r].kind == Kind::Choice)
        return static_cast<int>(std::min<uint32_t>(settings::Get().*kRows[r].number, kRows[r].choices - 1));
    if (kRows[r].kind == Kind::Colour)
        return (kRows[r].player ? settings::Get().hudColourCustom : settings::Get().menuBarCustom) ? 1 : 0;
    if (kRows[r].kind != Kind::Fov) return settings::Get().*kRows[r].field ? 1 : 0;
    const uint32_t fov = settings::Get().fov[kRows[r].player];
    int best = kFovDefault;
    for (int i = 0; i < kFovCount; ++i)
        if (std::abs(static_cast<int>(kFovValues[i]) - static_cast<int>(fov)) <
            std::abs(static_cast<int>(kFovValues[best]) - static_cast<int>(fov)))
            best = i;
    return best;
}

bool NeedsRestart(int r) {
    if (kRows[r].kind == Kind::Plugin) return features::PluginSettingNeedsRestart(kRows[r].plugin, kRows[r].key);
    if (kRows[r].kind == Kind::Colour || kRows[r].kind == Kind::Choice || kRows[r].kind == Kind::Link) return false;
    if (kRows[r].kind != Kind::Toggle) return kRows[r].kind == Kind::Restart;
    int n = 0;
    const settings::Toggle* t = settings::Toggles(n);
    for (int i = 0; i < n; ++i)
        if (t[i].field == kRows[r].field) return t[i].restart;
    return true;
}

bool RestartPending() {
    for (int r = 0; r < kRowCount; ++r) {
        // plugin rows: against the values at PSE_MSG_POST_LOAD (g_atStart is taken before the plugins load)
        if (kRows[r].kind == Kind::Plugin) {
            if (features::PluginSettingRestartPending(kRows[r].plugin, kRows[r].key)) return true;
        } else if (NeedsRestart(r) && Value(r) != g_atStart[r]) {
            return true;
        }
    }
    return false;
}

void Set(int r, int value) {
    if (Value(r) == value) return;
    if (kRows[r].kind == Kind::Fov) {
        settings::Values v = settings::Get();
        v.fov[kRows[r].player] = kFovValues[std::clamp(value, 0, kFovCount - 1)];
        const bool ok = settings::Save(v);
        dslog::Write(ok ? "DesertStormFix menu: %s %u" : "[fail] DesertStormFix menu: %s %u (not saved)",
                     kRows[r].label, v.fov[kRows[r].player]);
        return;
    }
    if (kRows[r].kind == Kind::Choice) {
        settings::Values v = settings::Get();
        v.*kRows[r].number = static_cast<uint32_t>(std::clamp(value, 0, kRows[r].choices - 1));
        const bool ok = settings::Save(v);
        dslog::Write(ok ? "DesertStormFix menu: %s %s" : "[fail] DesertStormFix menu: %s %s (not saved)", kRows[r].label,
                     kRows[r].text(value));
        return;
    }
    if (kRows[r].kind == Kind::Colour) {
        settings::Values v = settings::Get();
        (kRows[r].player ? v.hudColourCustom : v.menuBarCustom) = value != 0;
        const bool ok = settings::Save(v);
        dslog::Write(ok ? "DesertStormFix menu: %s %s" : "[fail] DesertStormFix menu: %s %s (not saved)",
                     kRows[r].label, value ? "custom" : "game");
        return;
    }
    const bool on = value != 0;
    bool ok;
    if (kRows[r].kind == Kind::Plugin) {
        const PSEPluginHandle h = pse_host::Find(kRows[r].plugin);
        ok = h && pse::SettingsInterface()->SetInt(h, kRows[r].key, on ? 1 : 0) == PSE_OK;
        if (!h) dslog::Write("DesertStormFix menu: %s - plugin %s is not installed", kRows[r].label, kRows[r].plugin);
    } else if (kRows[r].kind == Kind::Toggle) {
        ok = settings::SetToggle(kRows[r].field, on);
    } else {
        settings::Values v = settings::Get();
        v.*kRows[r].field = on;
        ok = settings::Save(v);
        if (ok && kRows[r].field == &settings::Values::perfOverlay)
            pse_host::RunAs("dsfix.diagnostics", &features::ApplyDiagnostics);
    }
    dslog::Write(ok ? "DesertStormFix menu: %s %s%s" : "[fail] DesertStormFix menu: %s %s%s (not saved)",
                 kRows[r].label, on ? "on" : "off", NeedsRestart(r) ? " (after a restart)" : "");
}

// ---- Options: one more item ----
using CreateFn = bool(__thiscall*)(void*);
using SetupFn = void(__thiscall*)(void*);

// One more plain item in the pool (count +0x24 at most the reserved 9, push 9 at 0x4718BD), or null.
void* AddPoolItem(void* list) {
    if (At<int16_t>(list, 0x24) >= 9) return nullptr;
    void* item = reinterpret_cast<void*(__cdecl*)(uint32_t, uint32_t)>(kAlloc)(0x24, 0x1E);
    if (!item) return nullptr;
    reinterpret_cast<void*(__thiscall*)(void*)>(kItemCtor)(item);
    reinterpret_cast<void(__thiscall*)(void*, void*)>(kListAddToPool)(list, item);
    return item;
}

void* g_optionsCreateNext = reinterpret_cast<void*>(kOptionsCreate);  // pointer hook chains (patch::HookSlot)
void* g_optionsSetupNext = reinterpret_cast<void*>(kOptionsSetup);

bool __fastcall OptionsCreate(void* screen, void*) {
    g_optionsItem = g_videoItem = nullptr;
    if (!reinterpret_cast<CreateFn>(g_optionsCreateNext)(screen)) return false;
    void* list = At<void*>(screen, 8);
    g_videoItem = AddPoolItem(list);
    g_optionsItem = AddPoolItem(list);
    return true;
}

// After the original (its items added in pool order, panel laid out, cursor restored): ours as the last rows, VIDEO
// then DESERTSTORMFIX.
void __fastcall OptionsSetup(void* screen, void*) {
    reinterpret_cast<SetupFn>(g_optionsSetupNext)(screen);
    void* list = At<void*>(screen, 8);
    if (!list || (!g_optionsItem && !g_videoItem)) return;
    const int16_t cursor = At<int16_t>(list, 0x28);
    struct Ours {
        void* item;
        uint16_t id;
        uint32_t text;
    };
    for (const Ours& o : {Ours{g_videoItem, kVideoItem, kTextVideoItem}, Ours{g_optionsItem, kOptionsItem, kTextItem}}) {
        // VIDEO only with its page: the plugin dsfix.graphics (null without it - state 0x4C would have no screen)
        if (!o.item || (o.id == kVideoItem && !features::VideoMenuScreen())) continue;
        reinterpret_cast<ItemSetFn>(kItemSet)(o.item, o.id, o.text, 0, 0x24, o.text, 0, kOptionsItemDraw, 0);
        AddToList(list, o.item);
        At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + At<int16_t>(list, 0x38));
    }
    if (At<int16_t>(list, 0x42) < At<int16_t>(list, 0x26)) At<int16_t>(list, 0x42) = At<int16_t>(list, 0x26);
    using Layout = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, kOptionsTitle, 0, 0, 0x7FFF);
    reinterpret_cast<void(__thiscall*)(void*)>(kRelayout)(list);
    At<int16_t>(list, 0x28) = cursor;
}

// The update's list input: ours opens state 0x47 (row kept in screen +0x24 like the original's own items).
int16_t __fastcall OptionsListInput(void* list, void*) {
    const int16_t id = reinterpret_cast<int16_t(__thiscall*)(void*)>(kListInput)(list);
    if (id == kOptionsItem || id == kVideoItem) {
        if (void* screen = At<void*>(list, 0x90)) At<int>(screen, 0x24) = At<int16_t>(list, 0x28);
        SetState(id == kVideoItem ? kStateVideo : kStateFixMenu);
    }
    return id;
}

// ---- our screen ----
int VisibleRows() { return std::min(kVisible, kRowCount); }
int MaxOffset() { return std::max(0, kRowCount - kVisible); }

bool __fastcall Create(void* screen, void*) {
    using Alloc = void*(__cdecl*)(uint32_t, uint32_t);
    void* list = reinterpret_cast<Alloc>(kAlloc)(0xA4, 0x1E);
    if (!list) return false;
    reinterpret_cast<void*(__thiscall*)(void*, void*, void*, int)>(kListCtor)(
        list, Global<void*>(kLargeFont), Global<void*>(kHudSheet), 1);
    At<void*>(screen, 8) = list;
    if (!reinterpret_cast<int(__thiscall*)(void*, int)>(kListReserve)(list, VisibleRows())) return false;
    for (int i = 0; i < VisibleRows(); ++i) {
        void* item = reinterpret_cast<Alloc>(kAlloc)(0x68, 0x1E);
        if (item) reinterpret_cast<void*(__thiscall*)(void*)>(kChoiceCtor)(item);
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kListAddToPool)(list, item);
    }
    return true;
}

// Each visible row k shows kRows[g_offset + k]: its label (text looked up every frame), OFF / ON texts with the
// restart marker, and the current value.
int g_choicesFor[kVisible];  // row (<< 1 | restart marker) whose choices each item holds, -1 = none

void RefreshRows(void* list) {
    using ChoicesFn = void(__thiscall*)(void*, uint32_t, uint32_t, int, int, int, int, int, int);
    for (int k = 0; k < VisibleRows(); ++k) {
        void* item = ItemAt(list, k);
        if (!item) continue;
        const int r = g_offset + k;
        const bool restart = NeedsRestart(r);
        // only when the row shown changes: setting the choices every frame reset the item's state and swallowed the
        // Up / Down presses after a Left / Right change
        const int key = r << 1 | (restart ? 1 : 0);
        if (g_choicesFor[k] != key) {
            g_choicesFor[k] = key;
            if (kRows[r].kind == Kind::Fov)
                reinterpret_cast<ChoicesFn>(kChoices)(item, kTextFov0, kTextFov0 + 1, kTextFov0 + 2, kTextFov0 + 3,
                                                      kTextFov0 + 4, kTextFov0 + 5, kTextFov0 + 6, kTextFov0 + 7);
            else if (kRows[r].kind == Kind::Colour)
                reinterpret_cast<ChoicesFn>(kChoices)(item, kTextGameColour, kTextCustomColour, -1, -1, -1, -1, -1, -1);
            else if (kRows[r].kind == Kind::Link)
                reinterpret_cast<ChoicesFn>(kChoices)(item, kTextLink, static_cast<uint32_t>(-1), -1, -1, -1, -1, -1, -1);
            else if (kRows[r].kind == Kind::Choice) {
                int t[8];
                for (int i = 0; i < 8; ++i) t[i] = i < kRows[r].choices ? static_cast<int>(kTextChoice0 + k * 8 + i) : -1;
                reinterpret_cast<ChoicesFn>(kChoices)(item, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]);
            }
            else
                reinterpret_cast<ChoicesFn>(kChoices)(item, restart ? kTextOffRestart : kTextOff,
                                                      restart ? kTextOnRestart : kTextOn, -1, -1, -1, -1, -1, -1);
        }
        At<int8_t>(item, 0x24) = static_cast<int8_t>(Value(r));
    }
}

// Every time the screen is entered: from the top.
void __fastcall Setup(void* screen, void*) {
    g_offset = 0;
    g_lastRow = 0;
    for (int& c : g_choicesFor) c = -1;
    reinterpret_cast<SetupFn>(kOptSetup)(screen);  // list layout for its two rows + two items
    void* list = At<void*>(screen, 8);
    using Callbacks = void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, int, int, int, int, int);
    reinterpret_cast<Callbacks>(kListCallbacks)(list, 0x478440, 0x478480, 0x478530, 0x4785E0, 0x478690, 0, 0, 0, 0, 0);
    At<void*>(list, 0x90) = screen;
    const int rows = VisibleRows();
    for (int k = 0; k < rows; ++k)
        reinterpret_cast<ItemSetFn>(kItemSet)(ItemAt(list, k), kFirstId + k, 0xFFFFFFFF, 0, 0x24, kTextRow0 + k, 0,
                                              kChoiceDraw, 0);
    RefreshRows(list);
    for (int k = 2; k < rows; ++k) AddToList(list, ItemAt(list, k));
    At<int16_t>(list, 0x28) = 0;  // cursor on the first row (not the OPTIONS row it was opened from)
    At<uint16_t>(list, 0x30) = static_cast<uint16_t>(kFirstId);  // the class set-up added the first two
    At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + (rows - 2) * At<int16_t>(list, 0x38));
    At<int16_t>(list, 0x42) = static_cast<int16_t>(rows);
    using Layout = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, kTextItem, 0, 0, 0x7FFF);
}

// Scrolling as in the MODS menu: moving down past the last row (the list wraps to the first) while rows remain below,
// or up past the first while rows remain above, moves the window instead of the cursor; at the very end / top the wrap
// goes through and the window jumps with it. Only while a navigation input is held (the list follows the pointer
// every frame, which must not scroll); the mouse wheel scrolls too.
constexpr uint32_t kJoystickCount = 0x754BE8, kMouseWheel = 0x754C24 + 8;  // DIMOUSESTATE lZ

void Scroll(void* list) {
    const int vis = VisibleRows();
    int16_t& row = At<int16_t>(list, 0x28);
    const int prev = g_lastRow, now = row;
    if (prev == vis - 1 && now == 0) {
        if (g_offset < MaxOffset()) {
            ++g_offset;
            row = static_cast<int16_t>(vis - 1);
        } else {
            g_offset = 0;
        }
    } else if (prev == 0 && now == vis - 1) {
        if (g_offset > 0) {
            --g_offset;
            row = 0;
        } else {
            g_offset = MaxOffset();
        }
    }
    g_lastRow = row;
}

// W / S as Up / Down: first press, then repeating after 400 ms every 120 ms (like the arrows).
int WasdStep() {
    static int dir = 0;
    static DWORD since = 0, last = 0;
    const int now = features::KeyHeld('W') ? -1 : features::KeyHeld('S') ? 1 : 0;
    const DWORD t = GetTickCount();
    if (now != dir) {
        dir = now;
        since = last = t;
        return now;
    }
    if (now && t - since >= 400 && t - last >= 120) {
        last = t;
        return now;
    }
    return 0;
}

void Navigate(void* list) {
    const int vis = VisibleRows();
    const bool front = features::GameFocused();
    int16_t& row = At<int16_t>(list, 0x28);
    const int step = front ? WasdStep() : 0;
    if (step) row = static_cast<int16_t>((row + step + vis) % vis);
    uint32_t pad = 0;
    for (int j = 0, n = std::min(Global<int>(kJoystickCount), 8); front && j < n; ++j) pad |= features::ReadJoystick(j);
    const bool nav = front && (step || features::KeyHeld(VK_UP) || features::KeyHeld(VK_DOWN) ||
                               (pad & (1u << 12 | 1u << 16 | 1u << 14 | 1u << 18)));
    if (nav) Scroll(list);
    else g_lastRow = row;
    // the real foreground only: background test runs never poll the mouse, so lZ keeps a stale value there
    const int wheel = GetForegroundWindow() == Global<HWND>(0x606A60) ? Global<int>(kMouseWheel) : 0;
    if (wheel < 0 && g_offset < MaxOffset()) ++g_offset;
    else if (wheel > 0 && g_offset > 0) --g_offset;
}

int __fastcall Update(void* screen, void*) {
    void* list = At<void*>(screen, 8);
    if (features::BarColourPickerOpen()) {  // the colour wheel has the input (and the list waits for its keys' release)
        features::UpdateBarColourPicker();
        RefreshRows(list);
        return 0;
    }
    Navigate(list);
    RefreshRows(list);
    const int16_t id = reinterpret_cast<int16_t(__thiscall*)(void*)>(kListInput)(list);
    // As cheats.cpp: the list returns a row's id when Left / Right changed its choice or it was selected. A changed
    // choice is taken as it is, otherwise (Enter / Cross / A / click left it alone) the row flips - one change either way.
    const int k = id - kFirstId;
    if (k >= 0 && k < VisibleRows()) {
        if (void* item = ItemAt(list, k)) {
            const int r = g_offset + k;
            const int choice = At<int8_t>(item, 0x24);
            if (kRows[r].kind == Kind::Link) {
                SetState(kStatePlugins);
                return 0;
            }
            if (kRows[r].kind == Kind::Fov)  // Enter / Cross steps up (wrapping), Left / Right picks
                Set(r, choice != Value(r) ? choice : (Value(r) + 1) % kFovCount);
            else if (kRows[r].kind == Kind::Choice)
                Set(r, choice != Value(r) ? choice : (Value(r) + 1) % kRows[r].choices);
            else if (kRows[r].kind == Kind::Colour)  // Left / Right: GAME / CUSTOM; Enter / Cross: the colour wheel
                choice != Value(r) ? Set(r, choice) : features::OpenBarColourPicker(kRows[r].player);
            else
                Set(r, choice != Value(r) ? choice != 0 : !Value(r));
            At<int8_t>(item, 0x24) = static_cast<int8_t>(Value(r));
        }
    }
    if (reinterpret_cast<int(__cdecl*)(int)>(kBackPressed)(0)) SetState(kStateOptions);
    return 0;
}

// Scrollbar right of the rows (as in the MODS menu).
void DrawScrollbar(void* list) {
    const int vis = VisibleRows();
    if (kRowCount <= vis) return;
    const float right = static_cast<float>(At<int16_t>(list, 0x3A) + At<int16_t>(list, 0x32));
    const float pitch = At<int16_t>(list, 0x38), top = At<int16_t>(list, 0x3C);
    const auto* sheet = Global<const uint8_t*>(kHudSheet);
    const float k = sheet ? *reinterpret_cast<const float*>(sheet + 0x24) : 1.0f;
    const float w = 6.0f * k, x = right + 10.0f * k;
    const float trackH = pitch * vis - 4.0f * k, y = top + 2.0f * k;
    const float thumbH = std::max(trackH * vis / kRowCount, 8.0f * k);
    const float thumbY = y + (trackH - thumbH) * g_offset / std::max(1, MaxOffset());
    overlay::QueueRect(x, y, w, trackH, 0x80202A10);
    overlay::QueueRect(x, thumbY, w, thumbH, 0xFFE0D878);
}

// In the prompt line between the game's "Select" and "Back" (where the MODS menu puts its Apply prompt).
void DrawFooter() {
    const auto* r = Global<const uint8_t*>(kRenderer);
    void* font = overlay::MenuFont();
    if (!r || !font) return;
    const int W = *reinterpret_cast<const int*>(r + 0x40688), H = *reinterpret_cast<const int*>(r + 0x4068C);
    const bool pending = RestartPending();
    const char* text = pending ? "RESTART THE GAME TO APPLY" : "* AFTER RESTARTING THE GAME";
    const int y = static_cast<int>(H * 0.92f);
    overlay::DrawLabel(font, text, (W - overlay::TextWidth(font, text)) / 2, y, pending ? 0xFFE0D878 : 0xFFC8C8C8);
}

void __fastcall Draw(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(kDrawPanel)(screen, 6, 0);
    const int picking = features::BarColourPickerTarget();
    const uint32_t title = picking == 0 ? kTextColourTitle : picking == 1 ? kTextHudColourTitle : kTextItem;
    reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(kDrawTitle)(screen, title, 1);
    if (features::DrawBarColourPicker(screen)) return;
    void* list = At<void*>(screen, 8);
    reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(list))[2])(list);
    DrawScrollbar(list);
    DrawFooter();
}

void Build() {
    reinterpret_cast<void(__thiscall*)(void*)>(kOptCtor)(g_screen);
    std::memcpy(g_vtable, reinterpret_cast<void*>(kOptVtable), sizeof g_vtable);
    At<void**>(g_screen, 0) = g_vtable;
    g_vtable[6] = reinterpret_cast<void*>(kMainMenuEnter);  // enter runs the set-up every time
    g_vtable[1] = reinterpret_cast<void*>(&Create);
    g_vtable[3] = reinterpret_cast<void*>(&Setup);
    g_vtable[4] = reinterpret_cast<void*>(&Update);
    g_vtable[5] = reinterpret_cast<void*>(&Draw);
    g_built = Create(g_screen, nullptr);
    dslog::Write(g_built ? "DesertStormFix menu: screen built" : "[fail] DesertStormFix menu: screen not built");
}
}  // namespace

void* features::FixMenuScreen() {
    if (!g_built && Global<void*>(kMainMenuScreen)) Build();
    return g_built ? g_screen : nullptr;
}

const char* features::FixMenuText(uint32_t hash) {
    switch (hash) {
        case kTextItem: return "DESERTSTORMFIX";
        case kTextOff: return "OFF";
        case kTextOn: return "ON";
        case kTextOffRestart: return "OFF *";
        case kTextOnRestart: return "ON *";
        case kTextGameColour: return "GAME";
        case kTextCustomColour: return "CUSTOM";
        case kTextColourTitle: return "MENU BAR COLOUR";
        case kTextHudColourTitle: return "HUD COLOUR";
        default: break;
    }
    if (hash == kTextLink) {
        static std::string summary;   // the game keeps the pointer for the frame
        summary = features::PluginsMenuSummary();
        return summary.c_str();
    }
    if (hash >= kTextFov0 && hash < kTextFov0 + kFovCount) return kFovTexts[hash - kTextFov0];
    if (hash >= kTextChoice0 && hash < kTextChoice0 + kVisible * 8) {
        const int r = g_offset + static_cast<int>(hash - kTextChoice0) / 8, i = static_cast<int>(hash - kTextChoice0) % 8;
        return r < kRowCount && kRows[r].kind == Kind::Choice && i < kRows[r].choices ? kRows[r].text(i) : "";
    }
    if (hash >= kTextRow0 && hash < kTextRow0 + kVisible) {
        const int r = g_offset + static_cast<int>(hash - kTextRow0);
        return r < kRowCount ? kRows[r].label : "";
    }
    if (const char* own = features::PluginsMenuText(hash)) return own;   // PLUGINS list / pages (0xC013xxxx)
    return features::PseUiText(hash);                                     // texts plugins registered (0xC4xxxxxx)
}

void features::ApplyFixMenu() {
    static bool done = false;
    if (done) return;
    done = true;
    for (int r = 0; r < kRowCount; ++r) g_atStart[r] = Value(r);
    features::RegisterPseUi();   // pse.ui: before the external plugins load (they Query it in their Load)
    const uint32_t create = kOptionsCreate, setup = kOptionsSetup;
    if (!patch::Matches(kOptionsCreateSlot, &create, 4) || !patch::Matches(kOptionsSetupSlot, &setup, 4)) {
        dslog::Write("[fail] DesertStormFix menu: unexpected Options screen - not added");
        return;
    }
    const bool ok = patch::HookCall(kOptionsListInputCall, reinterpret_cast<const void*>(&OptionsListInput), kListInput) &&
                    patch::HookSlot(kOptionsCreateSlot, reinterpret_cast<const void*>(&OptionsCreate),
                                    &g_optionsCreateNext, kOptionsCreate) &&
                    patch::HookSlot(kOptionsSetupSlot, reinterpret_cast<const void*>(&OptionsSetup), &g_optionsSetupNext,
                                    kOptionsSetup);
    dslog::Write(ok ? "[ok]   DesertStormFix menu: OPTIONS item" : "[fail] DesertStormFix menu: OPTIONS item");
}
