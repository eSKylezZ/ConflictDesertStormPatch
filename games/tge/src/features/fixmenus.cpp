// GreatEscapeFix's own menus (tgefix.fixmenus), made the way modsmenu.cpp makes MODS: the game's GUI files are patched
// as they are read (FUN_004C5240, hooked outside modsmenu's so its MODS button is already in) with the gui_file.h
// model, and our screens get their behaviour from the screen factory (FUN_005890B0) - objects of the CHEATS class with
// a copied vtable whose event method (slot 14) is ours.
//   GE_FrontEnd.gui: four copies of the hidden CHEATS screen (title, 6 label + value rows, one plain row):
//     ~fxpad  CONTROLLER - controller support, fire on the trigger (modern layout; off = the game's), deadzone,
//             vibration, adaptive triggers; SHOW BUTTON LAYOUT -> ~fxpic
//     ~fxpic  BUTTON LAYOUT - rows hidden, the panel widened; controllerpic.cpp's picture drawn over it (late draw,
//             while the screen's title is being drawn)
//     ~fxfix  GREATESCAPEFIX - HUD scaling, HUD size, frame cap, frame limit, films fill, widescreen modes
//     ~fxmis  CUSTOM MISSIONS - campaigns + .lua levels of the switched-on mods (scripthost.cpp), paged
//   FE_MAIN gets CUSTOM MISSIONS (a copy of OPTIONS) after OPTIONS / MODS when there is anything to list; the main menu
//   is laid out again along its down links (y 210 + 24 n, the logo higher).
//   GE_PC.gui: OPTIONS_PC gets GREATESCAPEFIX under CONTROLS, CONTROLLER_SCREEN_PC (OPTIONS > CONTROLS) gets
//   CONTROLLER under INPUT SELECTION; their panels grow by one row. Those buttons open our screens through a
//   navigation link {accept, open screen, hash} like the screen's own buttons; FE_MAIN's buttons go through its event
//   method (wrapped, as modsmenu does).
// Rows are CHEATS' toggles (type 8): Left / Right step them (events 0x17 / 0x18 reach the screen), Enter steps forward;
// the toggle's own index (+0xBC of max +0xBD, no wrap) is put back to the middle after every step, so the values are
// ours alone. Texts: every text face of our copies points at a key "~<tag>_<role>", answered by the hooked text
// lookup FUN_0047EA90 (lower-case, like the game's).
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "features/gui_file.h"
#include "features/overlay.h"

namespace {
constexpr uint32_t kReadWhole = 0x4C5240;
constexpr uint8_t kReadWholePrologue[] = {0x55, 0x8B, 0x6C, 0x24, 0x08};
constexpr uint32_t kAlloc = 0x4CF8C0, kFree = 0x4CF950;
constexpr uint32_t kTextLookup = 0x47EA90;
constexpr uint8_t kTextLookupPrologue[] = {0x8B, 0x41, 0x38, 0x85, 0xC0};
constexpr uint32_t kFactory = 0x5890B0;
constexpr uint8_t kFactoryPrologue[] = {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C};
constexpr uint32_t kMainEventSlot = 0x60F090 + 14 * 4, kMainEvent = 0x58E400;
constexpr uint32_t kCheatsVtbl = 0x610930, kScreenCtor = 0x4C1800, kNew = 0x4CF870, kBaseEvent = 0x4BC880;
constexpr uint32_t kFindControl = 0x4C1CB0, kControlEvent = 0x4BC890, kShowScreen = 0x4BA3D0, kGui = 0x7D8EC4;
constexpr int kVtblSlots = 64, kRows = 6;
constexpr int kMsgChosen = 4, kMsgShown = 0x15, kMsgNext = 0x17, kMsgPrev = 0x18, kMsgSetUp = 0x1D;
constexpr int32_t kReadWholePriority = 10;   // outside modsmenu's (0): its MODS button is in when we see FE_MAIN

// GE_FrontEnd.gui
constexpr uint32_t kFeMain = 0xABD02060, kCheats = 0xEE1A0D13;
constexpr uint32_t kNewGame = 0x2FE288CC, kOptions = 0x1B3E1801, kGreatest = 0xBA80F1C0, kLogo = 0xBF4DF0F3;
constexpr uint32_t kCheatRows[kRows] = {0x349E1125, 0xB0ED7E02, 0xDCCBC2E0, 0xF2B6B0DB, 0xD0C069C2, 0x24F3A3F8};
constexpr uint32_t kCheatButton = 0xFF175834, kCheatTitle = 0x4BBCE8CA, kCheatSpot = 0xFAB35095;
constexpr uint32_t kCheatBack = 0x5A6EB84A, kCheatFrame = 0xC0918794, kCheatPlaque = 0xE43A67B9;
constexpr uint32_t kCheatScrewL = 0x2B68C9DC, kCheatScrewR = 0x2B68C907;
// GE_PC.gui
constexpr uint32_t kOptionsPc = 0xAEB3C1B5, kOptControl = 0xF1D98384, kOptFx = 0x2CF5712E;
constexpr uint32_t kOptGrow[] = {0x6EB8CABD, 0x918779E3, 0xB3507822}, kOptBarbs = 0xB7334081;
constexpr uint32_t kControllerPc = 0x2136C458, kCtlInput = 0x698F5530, kCtlKeys = 0xF1A6AE67;
constexpr uint32_t kCtlGrow[] = {0x5A6EB8FC, 0x8714B8C1, 0xB350A3A2}, kCtlBarbs = 0x11B733C0;

enum Id { kPad, kPic, kFix, kMis, kScreens };
const char* const kTags[kScreens] = {"fxpad", "fxpic", "fxfix", "fxmis"};
// Roles: 0 title, 1 the plain row, 10 + r row r's label, 20 + r its value; buttons in the game's screens below.
enum Role { kTitle = 0, kButton = 1, kLabel0 = 10, kValue0 = 20 };
constexpr int kMainButton = 900, kOptionsButton = 910, kControlsButton = 920;

struct Ours {
    uint32_t hash = 0, rows[kRows] = {}, button = 0;
    void* rowCtl[kRows] = {};
    void* buttonCtl = nullptr;
    bool patched = false;
};
Ours g_s[kScreens];
std::vector<std::pair<void*, int>> g_objects;   // screen object -> Id
std::unordered_map<uint32_t, int> g_roles;      // text key hash -> Id * 100 + role (or a button's role)
uint32_t g_mainButton = 0;
bool g_mainPatched = false;
std::vector<features::CustomMission> g_missions;
int g_page = 0;
bool g_pictureSeen = false;

std::string Key(int code) { return "~fx_" + std::to_string(code); }
void KeyFaces(gui::Control& c, int code) {
    for (size_t f = 0; f < c.texts.size(); ++f) g_roles[gui::SetKey(c, f, Key(code))] = code;
}
// The type-3 children of a control (row label, row value / a button's text).
std::vector<gui::Control*> Texts(gui::Control& c) {
    std::vector<gui::Control*> out;
    for (auto& ch : c.children)
        if (ch->type == 3) out.push_back(ch.get());
    return out;
}
gui::Link* LinkOf(std::vector<gui::Link>* table, uint32_t key) {
    if (table)
        for (gui::Link& l : *table)
            if (l.key == key) return &l;
    return nullptr;
}
size_t TopIndex(gui::Screen& s, uint32_t hash) {
    for (size_t i = 0; i < s.controls.size(); ++i)
        if (s.controls[i]->hash == hash) return i;
    return s.controls.size();
}

const char* g_why = nullptr;
#define CHECK(c, why) if (!(c)) { g_why = why; return false; }

// ---- GE_FrontEnd.gui -----------------------------------------------------------------------------------------------
bool AddScreens(gui::File& f) {
    gui::Screen* cheats = f.FindScreen(kCheats);
    CHECK(cheats, "CHEATS screen not found");
    for (int id = 0; id < kScreens; ++id) {
        if (f.FindScreen(gui::NameHash(std::string("~") + kTags[id]))) continue;   // already ours
        auto s = cheats->Clone();
        const auto map = gui::Rename(*s, kTags[id]);
        auto mapped = [&](uint32_t h) {
            for (const auto& [from, to] : map)
                if (from == h) return to;
            return 0u;
        };
        Ours& o = g_s[id];
        o.hash = s->hash;
        if (gui::Control* spot = gui::Find(*s, mapped(kCheatSpot))) {   // the spotlight is made by its hash: keep it
            spot->hash = kCheatSpot;
            spot->name = "CTRL_CUSTOM_SPOTLIGHT7";
        }
        gui::Control* title = gui::Find(*s, mapped(kCheatTitle));
        CHECK(title, "CHEATS title missing");
        KeyFaces(*title, id * 100 + kTitle);
        for (int r = 0; r < kRows; ++r) {
            o.rows[r] = mapped(kCheatRows[r]);
            gui::Control* row = gui::Find(*s, o.rows[r]);
            CHECK(row, "a CHEATS row is missing");
            const auto texts = Texts(*row);
            CHECK(texts.size() == 2, "a CHEATS row isn't label + value");
            KeyFaces(*texts[0], id * 100 + kLabel0 + r);
            KeyFaces(*texts[1], id * 100 + kValue0 + r);
        }
        if (id == kMis)   // names need the room: the label bar across the row, no value box
            for (int r = 0; r < kRows; ++r) {
                gui::Control* row = gui::Find(*s, o.rows[r]);
                std::vector<std::unique_ptr<gui::Control>> kept;
                int borders = 0, texts = 0;
                for (auto& ch : row->children) {
                    if (ch->type == 5 && borders++ == 0) ch->rect[2] = 339;
                    else if (ch->type == 3 && texts++ == 0) ch->rect[2] = 335;
                    else continue;
                    kept.push_back(std::move(ch));
                }
                row->children = std::move(kept);
            }
        // a wider title plaque (our titles are longer than CHEATS)
        auto place = [&](uint32_t h, float x, float y) {
            if (gui::Control* c = gui::Find(*s, mapped(h))) c->rect[0] = x, c->rect[1] = y;
        };
        if (gui::Control* plaque = gui::Find(*s, mapped(kCheatPlaque))) plaque->rect[0] = 206, plaque->rect[2] = 222;
        place(kCheatScrewL, 210, 114);
        place(kCheatScrewR, 412, 113);
        o.button = mapped(kCheatButton);
        gui::Control* button = gui::Find(*s, o.button);
        CHECK(button && !Texts(*button).empty(), "CHEATS' COMPLETE MISSION row missing");
        KeyFaces(*Texts(*button)[0], id * 100 + kButton);
        if (id == kPic) {   // a wider panel for the picture
            auto set = [&](uint32_t h, float x, float y, float w, float hgt) {
                if (gui::Control* c = gui::Find(*s, mapped(h))) {
                    c->rect[0] = x, c->rect[1] = y, c->rect[2] = w, c->rect[3] = hgt;
                }
            };
            set(kCheatBack, 22, 68, 596, 344);
            set(kCheatFrame, 12, 62, 616, 360);
            if (gui::Control* spot = gui::Find(*s, kCheatSpot)) spot->rect[0] = 16, spot->rect[1] = 62, spot->rect[2] = 608, spot->rect[3] = 352;
            set(kCheatPlaque, 206, 76, 222, 32);
            set(kCheatScrewL, 210, 84, 12, 12);
            set(kCheatScrewR, 412, 83, 12, 12);
            title->rect[1] = 72;
        }
        f.AddScreen(std::move(s));
        o.patched = true;
    }
    return true;
}

// FE_MAIN: CUSTOM MISSIONS after the control whose down link goes to GREATEST ESCAPE (OPTIONS, or MODS).
bool AddMainButton(gui::File& f) {
    gui::Screen* fe = f.FindScreen(kFeMain);
    CHECK(fe, "FE_MAIN not found");
    size_t above = fe->controls.size();
    for (size_t i = 0; i < fe->controls.size(); ++i) {
        const gui::Link* down = LinkOf(&fe->controlNav[i], 3);
        if (down && down->type == 1 && down->target == kGreatest) above = i;
    }
    const size_t opt = TopIndex(*fe, kOptions), greatest = TopIndex(*fe, kGreatest);
    CHECK(above < fe->controls.size() && opt < fe->controls.size() && greatest < fe->controls.size(),
          "FE_MAIN's OPTIONS / GREATEST ESCAPE not found");
    auto button = fe->controls[opt]->Clone();
    int n = 0;
    gui::RenameTree(*button, "fxmain", n);
    g_mainButton = button->hash;
    const auto texts = Texts(*button);
    CHECK(!texts.empty(), "OPTIONS has no text");
    KeyFaces(*texts[0], kMainButton);
    const uint32_t aboveHash = fe->controls[above]->hash;
    LinkOf(&fe->controlNav[above], 3)->target = g_mainButton;
    if (gui::Link* up = LinkOf(&fe->controlNav[greatest], 2)) up->target = g_mainButton;
    gui::Insert(*fe, above + 1, std::move(button), {{2, 1, aboveHash, g_mainButton}, {3, 1, kGreatest, g_mainButton}});
    // lay the column out again: NEW GAME and every control down from it
    uint32_t h = kNewGame;
    for (int row = 0; row < 12; ++row) {
        const size_t i = TopIndex(*fe, h);
        if (i >= fe->controls.size()) break;
        fe->controls[i]->rect[1] = 210.0f + 24.0f * row;
        const gui::Link* down = LinkOf(&fe->controlNav[i], 3);
        if (!down || down->type != 1 || down->target == kNewGame) break;
        h = down->target;
    }
    if (gui::Control* logo = gui::Find(*fe, kLogo)) logo->rect[1] = 70.0f;
    g_mainPatched = true;
    return true;
}

// ---- GE_PC.gui -------------------------------------------------------------------------------------------------------
// A copy of `like` under it (one row lower), between `like` and `below` in the down / up links, opening `screen`; the
// panels `grow` and the barbed-wire group's lower pieces move down by the row; everything above the prompts moves up
// by half a row so the panel stays centred.
bool AddPcButton(gui::Screen& s, uint32_t like, uint32_t below, float pitch, const char* tag, int role, uint32_t screen,
                 const uint32_t* grow, size_t growCount, uint32_t barbs) {
    const size_t at = TopIndex(s, like);
    CHECK(at < s.controls.size() && TopIndex(s, below) < s.controls.size(), "a button of the screen is missing");
    auto button = s.controls[at]->Clone();
    int n = 0;
    gui::RenameTree(*button, tag, n);
    const uint32_t h = button->hash;
    button->rect[1] += pitch;
    const auto texts = Texts(*button);
    CHECK(!texts.empty(), "the button has no text");
    KeyFaces(*texts[0], role);
    gui::Link* down = LinkOf(&s.controlNav[at], 3);
    CHECK(down && down->target == below, "unexpected navigation");
    down->target = h;
    if (gui::Link* up = LinkOf(gui::NavOf(s, below), 2)) up->target = h;
    gui::Insert(s, at + 1, std::move(button), {{2, 1, like, h}, {3, 1, below, h}, {4, 0, screen, h}});
    for (size_t g = 0; g < growCount; ++g)
        if (gui::Control* c = gui::Find(s, grow[g])) c->rect[3] += pitch;
    if (gui::Control* group = gui::Find(s, barbs))
        for (auto& ch : group->children)
            if (ch->rect[1] > 100.0f) ch->rect[1] += pitch;
    for (auto& c : s.controls)
        if (c->rect[1] < 420.0f) c->rect[1] -= pitch / 2;
    return true;
}

bool PatchPc(gui::File& f) {
    gui::Screen* options = f.FindScreen(kOptionsPc);
    gui::Screen* controller = f.FindScreen(kControllerPc);
    CHECK(options && controller, "OPTIONS_PC / CONTROLLER_SCREEN_PC not found");
    return AddPcButton(*options, kOptControl, kOptFx, 36.0f, "fxopt", kOptionsButton, gui::NameHash("~fxfix"), kOptGrow,
                       3, kOptBarbs) &&
           AddPcButton(*controller, kCtlInput, kCtlKeys, 38.0f, "fxctl", kControlsButton, gui::NameHash("~fxpad"),
                       kCtlGrow, 3, kCtlBarbs);
}

// ---- the file read ---------------------------------------------------------------------------------------------------
using ReadWholeFn = int(__cdecl*)(const char*, uint8_t**, uint32_t*, void*, int);
ReadWholeFn g_readWhole = nullptr;

const char* BaseName(const char* name) {
    const char* base = name;
    for (const char* p = name; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    return base;
}

int __cdecl ReadWhole(const char* name, uint8_t** buffer, uint32_t* size, void* heap, int tag) {
    const int ok = g_readWhole(name, buffer, size, heap, tag);
    if (!ok || heap || !name || !buffer || !*buffer || !size) return ok;
    const char* base = BaseName(name);
    const bool frontEnd = _stricmp(base, "GE_FrontEnd.gui") == 0, pc = _stricmp(base, "GE_PC.gui") == 0;
    if (!frontEnd && !pc) return ok;
    gui::File f;
    std::string error;
    g_why = nullptr;
    if (!f.Parse(*buffer, *size, &error)) {
        features::ModLog("[fail] GreatEscapeFix menus: %s not recognised (%s)", base, error.c_str());
        return ok;
    }
    bool done;
    if (frontEnd) {
        done = AddScreens(f);
        g_missions = features::Get().mods ? features::CustomMissions() : std::vector<features::CustomMission>{};
        if (done && !g_missions.empty()) done = AddMainButton(f);
    } else {
        done = PatchPc(f);
    }
    if (!done) {
        features::ModLog("[fail] GreatEscapeFix menus: %s - %s", base, g_why ? g_why : "?");
        return ok;
    }
    const std::vector<uint8_t> out = f.Write();
    auto* fresh = reinterpret_cast<uint8_t*(__cdecl*)(uint32_t, int)>(kAlloc)(static_cast<uint32_t>(out.size() + 1), tag);
    if (!fresh) return ok;
    memcpy(fresh, out.data(), out.size());
    fresh[out.size()] = 0;
    reinterpret_cast<void(__cdecl*)(void*)>(kFree)(*buffer);
    const uint32_t before = *size;
    *buffer = fresh;
    *size = static_cast<uint32_t>(out.size());
    dslog::Write("GreatEscapeFix menus: %s patched (%u -> %u bytes%s)", base, before, *size,
                 frontEnd ? (g_mainPatched ? ", CUSTOM MISSIONS on the main menu" : "") : ", GREATESCAPEFIX + CONTROLLER");
    return ok;
}

// ---- values ------------------------------------------------------------------------------------------------------------
wchar_t g_text[48][96];
int g_slot = 0;
const wchar_t* Text(const char* fmt, ...) {
    char a[96];
    va_list args;
    va_start(args, fmt);
    vsnprintf(a, sizeof a, fmt, args);
    va_end(args);
    wchar_t* out = g_text[g_slot++ % 48];
    MultiByteToWideChar(CP_ACP, 0, a, -1, out, 96);
    return out;
}
const wchar_t* OnOff(bool on) { return on ? L"on" : L"off"; }

// The values the game started with (rows marked * take effect after a restart).
struct StartValues {
    bool controller, hudScaling, frameCap, filmsFill, widescreen;
} g_start;
bool g_startTaken = false;
void TakeStart() {
    if (g_startTaken) return;
    const features::Settings& s = features::Get();
    g_start = {s.controller, s.hudScaling, s.frameCap, s.filmsFill, s.widescreen};
    g_startTaken = true;
}
bool RestartPending() {
    const features::Settings& s = features::Get();
    return s.controller != g_start.controller || s.hudScaling != g_start.hudScaling || s.frameCap != g_start.frameCap ||
           s.filmsFill != g_start.filmsFill || s.widescreen != g_start.widescreen;
}
const wchar_t* Starred(bool on, bool started) { return on == started ? OnOff(on) : (on ? L"on *" : L"off *"); }

int RowsOf(int id);
bool Paged() { return g_missions.size() > kRows; }
int PerPage() { return Paged() ? kRows - 1 : kRows; }
int Pages() { return Paged() ? static_cast<int>((g_missions.size() + PerPage() - 1) / PerPage()) : 1; }
int MissionOnRow(int r) {
    if (Paged() && r == kRows - 1) return -1;
    const int i = g_page * PerPage() + r;
    return i < static_cast<int>(g_missions.size()) ? i : -1;
}

const wchar_t* Label(int id, int r) {
    switch (id) {
    case kPad: {
        static const wchar_t* const k[] = {L"controller support", L"fire on the trigger", L"stick deadzone", L"vibration",
                                           L"adaptive triggers", L"action prompts"};
        return k[r];
    }
    case kFix: {
        static const wchar_t* const k[] = {L"menu & hud scaling", L"hud size", L"frame cap", L"frame limit",
                                           L"films fill the screen", L"widescreen modes"};
        return k[r];
    }
    case kMis: {
        const int i = MissionOnRow(r);
        if (i >= 0) return Text(g_missions[i].campaign >= 0 ? "%s (campaign)" : "%s", g_missions[i].name.c_str());
        if (Paged() && r == kRows - 1) return Text("more (%d / %d)", g_page + 1, Pages());
        return r == 0 && g_missions.empty() ? L"no custom missions" : L"";
    }
    }
    return L"";
}

const wchar_t* Value(int id, int r) {
    const features::Settings& s = features::Get();
    if (id == kPad) {
        switch (r) {
        case 0: return Starred(s.controller, g_start.controller);
        case 1: return OnOff(s.padModernLayout);
        case 2: return Text("%u%%", s.padDeadzone);
        case 3: return OnOff(s.padVibration);
        case 4: return OnOff(s.padTriggers);
        case 5: return OnOff(s.actionPrompts);
        }
    } else if (id == kFix) {
        switch (r) {
        case 0: return Starred(s.hudScaling, g_start.hudScaling);
        case 1: return Text("%u%%", s.hudScalePercent);
        case 2: return Starred(s.frameCap, g_start.frameCap);
        case 3: return s.fpsCap ? Text("%u", s.fpsCap) : L"auto";
        case 4: return Starred(s.filmsFill, g_start.filmsFill);
        case 5: return Starred(s.widescreen, g_start.widescreen);
        }
    }
    return L"";
}

const wchar_t* RoleText(int code) {
    if (code == kMainButton) return L"custom missions";
    if (code == kOptionsButton) return L"greatescapefix";
    if (code == kControlsButton) return L"controller";
    const int id = code / 100, role = code % 100;
    if (role == kTitle) {
        static const wchar_t* const k[] = {L"controller", L"button layout", L"greatescapefix", L"custom missions"};
        if (id == kPic) g_pictureSeen = true;
        return k[id];
    }
    if (role == kButton) {
        if (id == kPad) return L"show button layout";
        if (id == kFix) return L"* = after a restart";
        return L"";
    }
    if (role >= kLabel0 && role < kLabel0 + kRows) return Label(id, role - kLabel0);
    if (role >= kValue0 && role < kValue0 + kRows) return Value(id, role - kValue0);
    return nullptr;
}

using TextLookupFn = const wchar_t*(__thiscall*)(void*, uint32_t);
TextLookupFn g_textLookup = nullptr;
const wchar_t* __fastcall TextLookup(void* catalog, void*, uint32_t hash) {
    auto it = g_roles.find(hash);
    if (it != g_roles.end())
        if (const wchar_t* t = RoleText(it->second)) return t;
    return g_textLookup(catalog, hash);
}

// ---- changes -----------------------------------------------------------------------------------------------------------
void Step(int id, int r, int dir) {
    const features::Settings& s = features::Get();
    auto flip = [](const char* key, bool on) { features::SetSetting(key, on ? 0 : 1); };
    auto number = [](const char* key, int v, int step, int lo, int hi, int dir) {
        features::SetSetting(key, std::clamp(v + step * dir, lo, hi));
    };
    if (id == kPad) {
        switch (r) {
        case 0: flip("Controller", s.controller); break;
        case 1: flip("PadModernLayout", s.padModernLayout); break;
        case 2:
            number("PadDeadzone", static_cast<int>(s.padDeadzone), 5, 0, 90, dir);
            features::ApplyPadDeadzone();
            break;
        case 3: flip("PadVibration", s.padVibration); break;
        case 4: flip("PadTriggers", s.padTriggers); break;
        case 5: flip("ActionPrompts", s.actionPrompts); break;
        }
    } else if (id == kFix) {
        switch (r) {
        case 0: flip("HudScaling", s.hudScaling); break;
        case 1: {   // the next ten up / down, within the setting's 25..400
            const int v = static_cast<int>(s.hudScalePercent);
            features::SetSetting("HudScalePercent", std::clamp(dir > 0 ? (v / 10 + 1) * 10 : ((v + 9) / 10 - 1) * 10, 25, 400));
            break;
        }
        case 2: flip("FrameCap", s.frameCap); break;
        case 3: {   // monitor (0), 30 .. 240 in tens
            int v = static_cast<int>(s.fpsCap) + 10 * dir;
            if (s.fpsCap == 0) v = dir > 0 ? 30 : 240;
            else if (v < 30 || v > 240) v = 0;
            features::SetSetting("FpsCap", v);
            break;
        }
        case 4: flip("FilmsFill", s.filmsFill); break;
        case 5: flip("FeatWidescreen", s.widescreen); break;
        }
    }
}

// ---- the screens -----------------------------------------------------------------------------------------------------
using EventFn = int(__thiscall*)(void*, int, void*);
uint32_t g_vtbl[kVtblSlots];

void* FindControl(void* screen, uint32_t hash) {
    return reinterpret_cast<void*(__thiscall*)(void*, uint32_t)>(kFindControl)(screen, hash);
}
void ShowControl(void* control, bool show) {
    if (!control) return;
    auto& flags = *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(control) + 0x80);
    if (((flags & 2) != 0) == show) return;
    flags = show ? (flags | 2) : (flags & ~2);
    reinterpret_cast<int(__thiscall*)(void*, int)>(kControlEvent)(control, show ? 0x19 : 0x1A);
}
void Recentre(void* toggle) {   // the toggle's own index back in the middle (it doesn't wrap)
    if (!toggle) return;
    auto* t = static_cast<uint8_t*>(toggle);
    t[0xBD] = 2;
    t[0xBC] = 1;
}
void ShowScreen(uint32_t hash) {
    if (void* gui = *reinterpret_cast<void**>(kGui)) reinterpret_cast<int(__thiscall*)(void*, uint32_t)>(kShowScreen)(gui, hash);
}

int RowsOf(int id) { return id == kPad ? 6 : id == kFix ? 6 : id == kPic ? 0 : kRows; }
void Layout(int id) {
    Ours& o = g_s[id];
    for (int r = 0; r < kRows; ++r) {
        bool show = r < RowsOf(id);
        if (id == kMis) show = MissionOnRow(r) >= 0 || (Paged() && r == kRows - 1) || (r == 0 && g_missions.empty());
        ShowControl(o.rowCtl[r], show);
        Recentre(o.rowCtl[r]);
    }
    ShowControl(o.buttonCtl, id == kPad || (id == kFix && RestartPending()));
}

int IdOf(void* self) {
    for (const auto& [obj, id] : g_objects)
        if (obj == self) return id;
    return -1;
}
uint32_t HashOf(void* control) { return *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(control) + 0xC); }

int __fastcall ScreenEvent(void* self, void*, int msg, void* control) {
    const int id = IdOf(self);
#ifndef DS_DIST
    static uint32_t seen[8] = {};
    if (msg >= 0 && msg < 256 && !(seen[msg >> 5] & (1u << (msg & 31)))) {
        seen[msg >> 5] |= 1u << (msg & 31);
        dslog::Write("GreatEscapeFix menus: event 0x%X (screen %d, control %08X)", msg, id, control ? HashOf(control) : 0);
    }
#endif
    if (id >= 0) {
        Ours& o = g_s[id];
        if (msg == kMsgSetUp) {
            for (int r = 0; r < kRows; ++r) o.rowCtl[r] = FindControl(self, o.rows[r]);
            o.buttonCtl = FindControl(self, o.button);
            return 0;
        }
        if (msg == kMsgShown) {
            TakeStart();
            if (id == kMis) {
                g_page = 0;
                if (features::Get().mods) g_missions = features::CustomMissions();
            }
            Layout(id);
            return 0;
        }
        if ((msg == kMsgChosen || msg == kMsgNext || msg == kMsgPrev) && control) {
            const uint32_t h = HashOf(control);
            if (h == o.button) {
                if (msg == kMsgChosen && id == kPad) ShowScreen(g_s[kPic].hash);
                return 1;
            }
            for (int r = 0; r < kRows; ++r) {
                if (h != o.rows[r]) continue;
                if (id == kMis) {
                    if (msg != kMsgChosen) return 1;
                    const int i = MissionOnRow(r);
                    if (i >= 0) features::StartCustomMission(g_missions[i]);
                    else if (Paged() && r == kRows - 1) {
                        g_page = (g_page + 1) % Pages();
                        Layout(id);
                    }
                    return 1;
                }
                Step(id, r, msg == kMsgPrev ? -1 : 1);
                Recentre(control);
                Layout(id);
                return 1;
            }
        }
    }
    return reinterpret_cast<EventFn>(kBaseEvent)(self, msg, control);
}

using FactoryFn = void*(__cdecl*)(uint32_t);
FactoryFn g_factory = nullptr;
void* __cdecl Factory(uint32_t hash) {
    int id = -1;
    for (int i = 0; i < kScreens; ++i)
        if (g_s[i].patched && hash == g_s[i].hash) id = i;
    if (id < 0) return g_factory(hash);
    void* obj = reinterpret_cast<void*(__cdecl*)(uint32_t, int)>(kNew)(0x70, 0x1F);
    if (!obj) return nullptr;
    reinterpret_cast<void*(__thiscall*)(void*, int)>(kScreenCtor)(obj, 0);
    *static_cast<uint32_t**>(obj) = g_vtbl;
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(obj) + 0x6C) = 0;
    // a front end loaded again may put a screen where an old one was: one entry per address
    g_objects.erase(std::remove_if(g_objects.begin(), g_objects.end(), [&](const auto& e) { return e.first == obj; }),
                    g_objects.end());
    g_objects.push_back({obj, id});
    dslog::Write("GreatEscapeFix menus: screen %s = %p", kTags[id], obj);
    return obj;
}

EventFn g_mainEvent = nullptr;
int __fastcall MainEvent(void* self, void*, int msg, void* control) {
    if (g_mainPatched && msg == kMsgChosen && control && HashOf(control) == g_mainButton) {
        ShowScreen(g_s[kMis].hash);
        return 1;
    }
    return g_mainEvent(self, msg, control);
}

// The button picture over BUTTON LAYOUT's widened panel (design 640 x 480, centred by its height).
void PictureDraw() {
    if (!g_pictureSeen) return;
    g_pictureSeen = false;
    float w, h;
    overlay::ScreenSize(w, h);
    const float s = h / 480.0f, x0 = (w - 640.0f * s) / 2;
    features::DrawControllerPicture(x0 + 100 * s, 116 * s, 440 * s, 282 * s, features::PadInUseIsXbox());
}

template <class Fn>
bool Hook(uint32_t entry, const void* fn, Fn& next, const uint8_t* prologue, size_t len, int32_t priority = 0) {
    return patch::HookFunction(entry, fn, reinterpret_cast<void**>(&next), prologue, len, 0, priority);
}
}  // namespace

void features::ApplyFixMenus() {
    static bool done = false;
    if (done) return;
    done = true;
    for (int i = 0; i < kScreens; ++i) g_s[i].hash = gui::NameHash(std::string("~") + kTags[i]);
    memcpy(g_vtbl, reinterpret_cast<const void*>(kCheatsVtbl), sizeof g_vtbl);
    g_vtbl[14] = reinterpret_cast<uint32_t>(&ScreenEvent);
    if (!Hook(kReadWhole, reinterpret_cast<const void*>(&ReadWhole), g_readWhole, kReadWholePrologue,
              sizeof kReadWholePrologue, kReadWholePriority) ||
        !Hook(kTextLookup, reinterpret_cast<const void*>(&TextLookup), g_textLookup, kTextLookupPrologue,
              sizeof kTextLookupPrologue) ||
        !Hook(kFactory, reinterpret_cast<const void*>(&Factory), g_factory, kFactoryPrologue, sizeof kFactoryPrologue) ||
        !patch::HookSlot(kMainEventSlot, reinterpret_cast<const void*>(&MainEvent), reinterpret_cast<void**>(&g_mainEvent),
                         kMainEvent)) {
        ModLog("[fail] GreatEscapeFix menus: game code not recognised - no GREATESCAPEFIX / CONTROLLER menus");
        return;
    }
    overlay::Install();
    overlay::AddLateDraw(&PictureDraw);
    dslog::Write("[ok]   GreatEscapeFix menus: OPTIONS > GREATESCAPEFIX, CONTROLS > CONTROLLER, CUSTOM MISSIONS");
}
