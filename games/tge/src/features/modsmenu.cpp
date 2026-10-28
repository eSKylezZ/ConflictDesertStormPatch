// Main menu MODS (tgefix.modsmenu): every folder in <game>\Mods with ON / OFF, like DesertStormFix / DesertStorm2Fix's
// MODS menu. Choosing a row switches that mod for the next APPLY; APPLY renames the folders ("_" in front = off, the
// rule mods.cpp indexes by) and rebuilds the mod index, so files the game opens from then on come from the new set.
// Files read at start-up (menus, fonts, start-up tables) only change after a restart - the APPLY row says so.
//
// The game's menus are GUI files (GE_FrontEnd.gui in catalog.dat, CGUI::Load 0x4BA870): a binary list of screens, each a
// tree of controls, then a navigation table. Layout (version 0x1D, 2026-10-24):
//   header (version, design size, safe area), .loc / shader / font / sprite / bitmap lists, then
//   screens: u32 count, per screen u32 hash (the factory key, below), u32 len + name, ... its controls, ... u32 hash of
//     the control focused first. Controls: u32 type (4 = button), u32 hash, u32 len + name, u16, 4 floats x y w h
//     (640 x 480 design), state data, children. Every name's hash = the archive LFSR (lower-cased name) - FE_MAIN =
//     0xABD02060. Text states: u32 len + font name, u32 key hash, u32 len + key (a catalog key: the text shown is the
//     catalog's, looked up every frame by FUN_0047EA90 thiscall([0x7DC26C], hash) -> wchar_t*; a key it doesn't know
//     shows "*NO STRING*").
//   navigation (FUN_004C23E0): u32 screen count, per screen a table (Esc: {5, 7, 0, screen}) + u32 control count + one
//     table per control in control order; table = u32 n + n x {u32 direction 0 left / 1 right / 2 up / 3 down, u32
//     type (1 = go to control), u32 target hash, u32 source hash}. Hidden targets are skipped (OPTIONS down ->
//     GREATEST ESCAPE (hidden) -> SELECT LEVEL (hidden) -> QUIT).
// The file is read whole by FUN_004C5240(name, &buffer, &size, heap 0 = game allocator, tag), freed by FUN_004CF950;
// that read is hooked for GE_FrontEnd.gui and the buffer replaced (game allocator FUN_004CF8C0(size, tag)) by a copy
// with:
//   - FE_MAIN: a MODS button (copy of OPTIONS, y 329) between OPTIONS and the hidden GREATEST ESCAPE / SELECT LEVEL
//     (shown once the game is finished), those and QUIT one row lower; FE_MAIN's control count + navigation updated;
//   - a MODS screen: a copy of the hidden CHEATS screen (title, 6 label + value rows, a plain row = APPLY, Select / Back
//     prompts) after it, every name / key renamed so its hashes are its own;
//   - texts: every key in our copies -> our hash, answered by the hooked text lookup.
// Screens get their behaviour from the GUI's screen factory ([0x7AB31C] = FUN_005890B0(hash): the game's screen classes
// by hash, FE_MAIN = vtable 0x60F090, CHEATS = 0x610930): our screen hash gets an object of the CHEATS class (base ctor
// FUN_004C1800) with a copied vtable whose event method (slot 14, thiscall(msg, control) ret 8) is ours. Events: 0x1D
// set-up (once), 0x15 shown, 4 = a control chosen (control +0xC = its hash); controls pass events up to their screen
// (FUN_004BC890). FE_MAIN's event method (0x58E400) is wrapped: MODS -> FUN_004BA3D0(gui [0x7D8EC4], screen hash), the
// call OPTIONS uses. Show / hide a control: +0x80 |= 2 + FUN_004BC890(0x19) / &= ~2 + 0x1A (FE_MAIN's own way).
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

namespace {
constexpr uint32_t kReadWhole = 0x4C5240;
constexpr uint8_t kReadWholePrologue[] = {0x55, 0x8B, 0x6C, 0x24, 0x08};        // push ebp; mov ebp, [esp+8]
constexpr uint32_t kAlloc = 0x4CF8C0, kFree = 0x4CF950;
constexpr uint32_t kTextLookup = 0x47EA90;
constexpr uint8_t kTextLookupPrologue[] = {0x8B, 0x41, 0x38, 0x85, 0xC0};        // mov eax, [ecx+0x38]; test eax, eax
constexpr uint32_t kFactory = 0x5890B0;
constexpr uint8_t kFactoryPrologue[] = {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C};     // push esi; push edi; mov edi, [esp+0xC]
constexpr uint32_t kMainEventSlot = 0x60F090 + 14 * 4, kMainEvent = 0x58E400;
constexpr uint32_t kCheatsVtbl = 0x610930, kScreenCtor = 0x4C1800, kNew = 0x4CF870, kBaseEvent = 0x4BC880;
constexpr uint32_t kFindControl = 0x4C1CB0, kControlEvent = 0x4BC890, kShowScreen = 0x4BA3D0, kGui = 0x7D8EC4;
constexpr int kVtblSlots = 64, kRows = 6;
constexpr int kMsgChosen = 4, kMsgShown = 0x15, kMsgSetUp = 0x1D;

// ---- the archive / GUI name hash (FUN_004B9A10 over the lower-cased name) ------------------------------------------
uint32_t NameHash(const char* s, size_t n) {
    uint32_t h = 1;
    for (size_t k = 0; k < n; ++k) {
        const uint8_t c = static_cast<uint8_t>(tolower(static_cast<uint8_t>(s[k])));
        for (int i = 0; i < 8; ++i) {
            const uint32_t in = ((c >> i) & 1) ^ (h & 1) ^ ((h >> 1) & 1) ^ ((h >> 21) & 1) ^ ((h >> 31) & 1);
            h = (h << 1) | in;
        }
    }
    return h;
}

// ---- text roles of our keys ------------------------------------------------------------------------------------------
enum Role : int { kRoleButton = 1, kRoleTitle, kRoleApply, kRoleLabel0 = 10, kRoleValue0 = 20 };
std::unordered_map<uint32_t, int> g_roles;   // key hash -> role
uint32_t g_buttonHash = 0, g_screenHash = 0, g_rowHash[kRows] = {}, g_applyHash = 0;
bool g_patched = false;

// ---- GUI file surgery ----------------------------------------------------------------------------------------------
uint32_t U32(const std::vector<uint8_t>& b, size_t o) { uint32_t v; memcpy(&v, &b[o], 4); return v; }
void Put32(std::vector<uint8_t>& b, size_t o, uint32_t v) { memcpy(&b[o], &v, 4); }
void PutF(std::vector<uint8_t>& b, size_t o, float v) { memcpy(&b[o], &v, 4); }

bool Printable(const uint8_t* p, size_t n, bool spaces) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] < (spaces ? 32 : 33) || p[i] > 126) return false;
    return true;
}

// Text states inside a control copy: [len][font name][key hash][len][key]. Fonts of GE_FrontEnd.gui.
bool IsFontAt(const std::vector<uint8_t>& b, size_t o, size_t& after) {
    static const char* const kFonts[] = {"GE_FLARGE_N", "GE_FLARGE_NR", "GE_NEW_GE_FONT", "GE_NEW_GE_FONT2"};
    if (o + 4 > b.size()) return false;
    const uint32_t n = U32(b, o);
    for (const char* f : kFonts)
        if (n == strlen(f) && o + 4 + n + 8 <= b.size() && memcmp(&b[o + 4], f, n) == 0) {
            after = o + 4 + n;
            return true;
        }
    return false;
}

// A key of `len` characters for a role (the hash is what counts; the characters only have to be unique per role).
std::string RoleKey(int role, size_t len) {
    char k[16];
    snprintf(k, sizeof k, "~%02d", role);
    std::string s = k;
    if (s.size() > len) s = s.substr(s.size() - len);
    while (s.size() < len) s += '_';
    return s;
}

// Rewrites every text state in b[from, to) to the role `roleAt(offset)` gives (0 = leave), records the key ranges.
// A text control = ... u32 n, n states of 18 bytes {u16 flags, u32, f32, f32, u32 colour}, u32 n, n texts {u32 len +
// font, u32 key hash (0x10 in the state's flags) or buffer size (literal text: no 0x10), u32 len + key / text, u16}:
// a literal made one of our keys gets flag 0x10 in its state.
template <class RoleAt>
bool RekeyTexts(std::vector<uint8_t>& b, size_t from, size_t to, RoleAt roleAt, std::vector<std::pair<size_t, size_t>>& keys) {
    size_t states = 0, next = 0;   // the current control's states / where its next text would start
    uint32_t count = 0, k = 0;
    for (size_t o = from; o + 12 < to; ++o) {
        size_t after;
        if (!IsFontAt(b, o, after)) continue;
        const uint32_t n = U32(b, after + 4);
        if (n == 0 || n > 64 || after + 8 + n > to || !Printable(&b[after + 8], n, true)) continue;
        if (o != next) {   // the first text of a control: preceded by its count, and the states by theirs
            count = o >= 4 ? U32(b, o - 4) : 0;
            states = o - 4 - 18 * count;
            if (count == 0 || count > 8 || o < 8 + 18 * count || U32(b, states - 4) != count) states = 0;
            k = 0;
        }
        keys.push_back({after, after + 8 + n});
        const int role = roleAt(o);
        if (role) {
            const bool literal = U32(b, after) != NameHash(reinterpret_cast<const char*>(&b[after + 8]), n);
            if (literal) {
                if (!states || k >= count) return false;
                b[states + 18 * k] |= 0x10;
            }
            const std::string key = RoleKey(role, n);
            memcpy(&b[after + 8], key.data(), n);
            const uint32_t h = NameHash(key.data(), n);
            Put32(b, after, h);
            g_roles[h] = role;
        }
        ++k;
        next = after + 8 + n + 2;
        o = next - 1;
    }
    return true;
}

// Every [hash][len][name] with hash == NameHash(name) in b[from, to), outside the text keys: a new name of the same
// length ("~<tag><n>___"), its hash; old -> new hash in `map`.
void RenameControls(std::vector<uint8_t>& b, size_t from, size_t to, char tag, const std::vector<std::pair<size_t, size_t>>& keys,
                    std::unordered_map<uint32_t, uint32_t>& map, uint32_t keep = 0) {
    int counter = 0;
    for (size_t o = from; o + 8 < to; ++o) {
        bool inKey = false;
        for (const auto& k : keys)
            if (o + 8 > k.first && o < k.second) inKey = true;
        if (inKey) continue;
        const uint32_t h = U32(b, o), n = U32(b, o + 4);
        if (n < 2 || n > 64 || o + 8 + n > to || !Printable(&b[o + 8], n, false) ||
            NameHash(reinterpret_cast<const char*>(&b[o + 8]), n) != h || h == keep)
            continue;
        char name[80];
        snprintf(name, sizeof name, "~%c%X", tag, ++counter);
        std::string s = name;
        if (s.size() > n) s = s.substr(s.size() - n);
        while (s.size() < n) s += '_';
        memcpy(&b[o + 8], s.data(), n);
        const uint32_t nh = NameHash(s.data(), n);
        Put32(b, o, nh);
        map.emplace(h, nh);
        o += 8 + n - 1;
    }
}

// Offset of the [hash][len][name] of a control / screen with this hash in b[from, to), or 0.
size_t FindNamed(const std::vector<uint8_t>& b, size_t from, size_t to, uint32_t hash) {
    for (size_t o = from; o + 8 < to; ++o)
        if (U32(b, o) == hash) {
            const uint32_t n = U32(b, o + 4);
            if (n >= 2 && n <= 64 && o + 8 + n <= to && NameHash(reinterpret_cast<const char*>(&b[o + 8]), n) == hash) return o;
        }
    return 0;
}
// A screen record: the name is followed by 0x40 (a text key can carry a screen's name too: PAUSE's "CHEATS").
size_t FindScreen(const std::vector<uint8_t>& b, size_t from, size_t to, uint32_t hash) {
    for (size_t o = from; (o = FindNamed(b, o, to, hash)) != 0; ++o) {
        const size_t end = o + 8 + U32(b, o + 4);
        if (end < b.size() && b[end] == 0x40) return o;
    }
    return 0;
}
void SetY(std::vector<uint8_t>& b, size_t named, float y) { PutF(b, named + 8 + U32(b, named + 4) + 2 + 4, y); }

// The shipped GE_FrontEnd.gui's landmarks (hashes of FE_MAIN's / CHEATS' controls).
constexpr uint32_t kFeMain = 0xABD02060, kCheats = 0xEE1A0D13, kLevelFailed = 0xB2FD3116;
constexpr uint32_t kOptions = 0x1B3E1801, kQuit = 0x194A20FF, kGreatest = 0xBA80F1C0, kSelectLevel = 0x70A19841;
constexpr uint32_t kCheatRows[kRows] = {0x349E1125, 0xB0ED7E02, 0xDCCBC2E0, 0xF2B6B0DB, 0xD0C069C2, 0x24F3A3F8};
constexpr uint32_t kCheatValues[kRows] = {0x2B583FDE, 0x94AC2347, 0x888666DB, 0xEAF437D7, 0xBA6B1EEA, 0xE93381DC};
constexpr uint32_t kCheatComplete = 0xFF175834, kCheatTitle = 0x4BBCE8CA, kCheatPic198 = 0x2B68C9DC;
constexpr uint32_t kCheatBack = 0x3AF87D85, kCheatSelect = 0x44E40C01, kCheatSpot = 0xFAB35095;

const char* g_why = nullptr;
#define CHECK(c, why) if (!(c)) { g_why = why; return false; }

bool Patch(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    std::vector<uint8_t> d = in;
    CHECK(d.size() > 0x200 && (U32(d, 0) & 0xFFFF) == 0x1D, "not a version 0x1D GUI file");
    // screens: count before FE_MAIN, then contiguous records
    const size_t feMain = FindScreen(d, 0x100, 0x400, kFeMain);
    CHECK(feMain && U32(d, feMain - 4) >= 2, "FE_MAIN not found");
    const uint32_t screenCount = U32(d, feMain - 4);
    const size_t feMainEnd = d.size();
    const size_t cheats = FindScreen(d, feMain, feMainEnd, kCheats);
    const size_t cheatsEnd = cheats ? FindScreen(d, cheats + 8, feMainEnd, kLevelFailed) : 0;
    CHECK(cheats && cheatsEnd, "CHEATS screen not found");
    // FE_MAIN: [hash][len][name][u8][len][background][u32 controls]
    const size_t feName = feMain + 8 + U32(d, feMain + 4) + 1;
    const size_t feCount = feName + 4 + U32(d, feName);
    CHECK(U32(d, feCount) == 8, "FE_MAIN doesn't have 8 controls");
    const size_t opt = FindNamed(d, feMain, cheats, kOptions), quit = FindNamed(d, feMain, cheats, kQuit);
    const size_t greatest = FindNamed(d, feMain, cheats, kGreatest), select = FindNamed(d, feMain, cheats, kSelectLevel);
    CHECK(opt && quit && greatest && select && opt < quit && U32(d, opt - 4) == 4 && U32(d, quit - 4) == 4,
          "FE_MAIN's OPTIONS / QUIT buttons not found");
    // navigation table: the last section, parsed to the end of the file
    std::vector<size_t> groups;
    size_t links = 0;
    for (size_t o = d.size() - 4; o > cheatsEnd && !links; --o) {
        if (U32(d, o) != screenCount) continue;
        size_t p = o + 4;
        std::vector<size_t> g;
        bool ok = true;
        for (uint32_t k = 0; k < screenCount && ok; ++k) {
            g.push_back(p);
            auto table = [&](size_t& q) {
                if (q + 4 > d.size()) return false;
                const uint32_t n = U32(d, q);
                if (n > 64 || q + 4 + 16ull * n > d.size()) return false;
                q += 4 + 16 * n;
                return true;
            };
            if (!table(p) || p + 4 > d.size()) { ok = false; break; }
            const uint32_t m = U32(d, p);
            p += 4;
            if (m > 512) { ok = false; break; }
            for (uint32_t j = 0; j < m && ok; ++j) ok = table(p);
        }
        if (ok && p == d.size()) {
            links = o;
            groups = g;
            groups.push_back(p);
        }
    }
    CHECK(links && groups.size() > 6, "navigation table not found");
    size_t cheatsGroup = 0;   // the group whose Esc entry names CHEATS
    for (size_t k = 0; k + 1 < groups.size(); ++k)
        if (U32(d, groups[k]) == 1 && U32(d, groups[k] + 16) == kCheats) cheatsGroup = k;
    CHECK(cheatsGroup, "CHEATS navigation not found");

    // ---- the MODS button: OPTIONS' record [type .. QUIT's type) ----
    std::vector<uint8_t> btn(d.begin() + (opt - 4), d.begin() + (quit - 4));
    std::vector<std::pair<size_t, size_t>> keys;
    CHECK(RekeyTexts(btn, 0, btn.size(), [](size_t) { return static_cast<int>(kRoleButton); }, keys), "MODS button texts");
    std::unordered_map<uint32_t, uint32_t> btnMap;
    RenameControls(btn, 0, btn.size(), 'b', keys, btnMap);
    CHECK(btnMap.count(kOptions), "MODS button not renamed");
    g_buttonHash = btnMap[kOptions];
    SetY(btn, 4, 329.0f);
    SetY(d, greatest, 353.0f);
    SetY(d, select, 377.0f);
    SetY(d, quit, 402.0f);

    // ---- the MODS screen: CHEATS' record ----
    std::vector<uint8_t> scr(d.begin() + cheats, d.begin() + cheatsEnd);
    const size_t at = cheats;   // offsets below are in the copy
    auto local = [&](uint32_t h) { const size_t o = FindNamed(d, cheats, cheatsEnd, h); return o ? o - at : 0; };
    // top-level controls in order -> extents
    const uint32_t tops[] = {kCheatTitle, kCheatPic198, kCheatRows[0], kCheatRows[1], kCheatBack, kCheatSelect, kCheatRows[2],
                             kCheatRows[3], kCheatComplete, kCheatRows[4], kCheatRows[5], kCheatSpot};
    std::vector<size_t> starts;
    for (uint32_t h : tops) {
        const size_t o = local(h);
        CHECK(o, "a CHEATS control is missing");
        starts.push_back(o);
    }
    auto extentEnd = [&](size_t start) {
        size_t e = scr.size();
        for (size_t s : starts)
            if (s > start && s < e) e = s;
        return e;
    };
    size_t valueAt[kRows];
    for (int r = 0; r < kRows; ++r) {
        valueAt[r] = local(kCheatValues[r]);
        CHECK(valueAt[r], "a CHEATS value text is missing");
    }
    keys.clear();
    const bool rekeyed = RekeyTexts(scr, 0, scr.size(), [&](size_t o) -> int {
        const size_t title = local(kCheatTitle);
        if (o > title && o < extentEnd(title)) return kRoleTitle;
        for (int r = 0; r < kRows; ++r) {
            const size_t row = local(kCheatRows[r]);
            if (o > row && o < extentEnd(row)) return o < valueAt[r] ? kRoleLabel0 + r : kRoleValue0 + r;
        }
        const size_t done = local(kCheatComplete);
        if (o > done && o < extentEnd(done)) return kRoleApply;
        return 0;   // Select / Back prompts keep the game's texts
    }, keys);
    CHECK(rekeyed, "MODS screen texts");
    std::unordered_map<uint32_t, uint32_t> scrMap;
    // the spotlight is a custom control the game makes by its hash (renamed = an empty placeholder box)
    RenameControls(scr, 0, scr.size(), 's', keys, scrMap, kCheatSpot);
    CHECK(scrMap.count(kCheats), "MODS screen not renamed");
    g_screenHash = scrMap[kCheats];
    for (int r = 0; r < kRows; ++r) g_rowHash[r] = scrMap[kCheatRows[r]];
    g_applyHash = scrMap[kCheatComplete];
    const uint32_t focus = U32(scr, scr.size() - 4);   // the control focused first
    if (scrMap.count(focus)) Put32(scr, scr.size() - 4, scrMap[focus]);

    // ---- navigation ----
    // FE_MAIN = group 0: one more control table (MODS, before QUIT's = after OPTIONS'), OPTIONS down / GREATEST up -> MODS
    std::vector<uint8_t> g0(d.begin() + groups[0], d.begin() + groups[1]);
    size_t q = 4 + 16 * U32(g0, 0);
    CHECK(U32(g0, q) == 8, "FE_MAIN navigation doesn't have 8 tables");
    Put32(g0, q, 9);
    q += 4;
    size_t quitTable = 0;
    for (int j = 0; j < 8; ++j) {
        const uint32_t n = U32(g0, q);
        if (n && U32(g0, q + 4 + 12) == kQuit) quitTable = q;
        for (uint32_t e = 0; e < n; ++e) {
            const size_t en = q + 4 + 16 * e;
            if (U32(g0, en) == 3 && U32(g0, en + 12) == kOptions) Put32(g0, en + 8, g_buttonHash);
            if (U32(g0, en) == 2 && U32(g0, en + 12) == kGreatest) Put32(g0, en + 8, g_buttonHash);
        }
        q += 4 + 16 * n;
    }
    CHECK(quitTable, "QUIT's navigation not found");
    const uint32_t modsTable[9] = {2, 2, 1, kOptions, g_buttonHash, 3, 1, kGreatest, g_buttonHash};
    g0.insert(g0.begin() + quitTable, reinterpret_cast<const uint8_t*>(modsTable),
              reinterpret_cast<const uint8_t*>(modsTable) + sizeof modsTable);
    // CHEATS' group copied for the MODS screen
    std::vector<uint8_t> gm(d.begin() + groups[cheatsGroup], d.begin() + groups[cheatsGroup + 1]);
    for (size_t o = 0; o + 4 <= gm.size(); o += 4)
        if (scrMap.count(U32(gm, o))) Put32(gm, o, scrMap[U32(gm, o)]);

    // ---- assemble: ... OPTIONS, MODS button, QUIT ... CHEATS, MODS screen, LEVEL_FAILED ... navigation (+ ours) ----
    out.clear();
    out.insert(out.end(), d.begin(), d.begin() + (quit - 4));
    out.insert(out.end(), btn.begin(), btn.end());
    out.insert(out.end(), d.begin() + (quit - 4), d.begin() + cheatsEnd);
    out.insert(out.end(), scr.begin(), scr.end());
    out.insert(out.end(), d.begin() + cheatsEnd, d.begin() + groups[0]);
    out.insert(out.end(), g0.begin(), g0.end());
    out.insert(out.end(), d.begin() + groups[1], d.begin() + groups[cheatsGroup + 1]);
    out.insert(out.end(), gm.begin(), gm.end());
    out.insert(out.end(), d.begin() + groups[cheatsGroup + 1], d.end());
    Put32(out, feCount, 9);
    Put32(out, feMain - 4, screenCount + 1);
    const size_t newLinks = links + btn.size() + scr.size();
    CHECK(U32(out, newLinks) == screenCount, "navigation count moved");
    Put32(out, newLinks, screenCount + 1);
    return true;
}

// ---- the file read ---------------------------------------------------------------------------------------------------
using ReadWholeFn = int(__cdecl*)(const char*, uint8_t**, uint32_t*, void*, int);
ReadWholeFn g_readWhole = nullptr;

bool IsFrontEndGui(const char* name) {
    const char* base = name;
    for (const char* p = name; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    return _stricmp(base, "GE_FrontEnd.gui") == 0;
}

int __cdecl ReadWhole(const char* name, uint8_t** buffer, uint32_t* size, void* heap, int tag) {
    const int ok = g_readWhole(name, buffer, size, heap, tag);
    if (!ok || heap || !name || !buffer || !*buffer || !size || !IsFrontEndGui(name)) return ok;
    g_roles.clear();
    g_why = nullptr;
    std::vector<uint8_t> in(*buffer, *buffer + *size), out;
    if (!Patch(in, out)) {
        g_patched = false;
        features::ModLog("[fail] MODS menu: %s not recognised (%s) - no MODS item", name, g_why ? g_why : "?");
        return ok;
    }
    auto* fresh = reinterpret_cast<uint8_t*(__cdecl*)(uint32_t, int)>(kAlloc)(static_cast<uint32_t>(out.size() + 1), tag);
    if (!fresh) return ok;
    memcpy(fresh, out.data(), out.size());
    fresh[out.size()] = 0;
    reinterpret_cast<void(__cdecl*)(void*)>(kFree)(*buffer);
    *buffer = fresh;
    *size = static_cast<uint32_t>(out.size());
    g_patched = true;
    dslog::Write("MODS menu: %s + MODS button %08X + screen %08X (%u -> %u bytes, %zu text keys)", name, g_buttonHash,
                 g_screenHash, static_cast<unsigned>(in.size()), static_cast<unsigned>(out.size()), g_roles.size());
    return ok;
}

// ---- the mods list ---------------------------------------------------------------------------------------------------
struct Mod {
    std::string name;   // without the "_"
    bool on, want;
};
std::vector<Mod> g_mods;
int g_page = 0;
bool g_applied = false;
std::string g_failed;

std::string ModsDir() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    return dir.substr(0, dir.find_last_of("\\/")) + "\\Mods";
}

void Scan() {
    g_mods.clear();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((ModsDir() + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            const bool off = fd.cFileName[0] == '_';
            g_mods.push_back({off ? fd.cFileName + 1 : fd.cFileName, !off, !off});
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(g_mods.begin(), g_mods.end(), [](const Mod& a, const Mod& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
}

int Pending() {
    int n = 0;
    for (const Mod& m : g_mods) n += m.on != m.want;
    return n;
}
bool Paged() { return g_mods.size() > kRows; }
int PerPage() { return Paged() ? kRows - 1 : kRows; }
int Pages() { return Paged() ? static_cast<int>((g_mods.size() + PerPage() - 1) / PerPage()) : 1; }
// Mod shown on row r, or -1 (r = kRows - 1 on a paged list = MORE).
int ModOnRow(int r) {
    if (Paged() && r == kRows - 1) return -1;
    const int i = g_page * PerPage() + r;
    return i < static_cast<int>(g_mods.size()) ? i : -1;
}

void Apply() {
    g_failed.clear();
    const std::string dir = ModsDir();
    for (Mod& m : g_mods) {
        if (m.on == m.want) continue;
        const std::string from = dir + "\\" + (m.on ? "" : "_") + m.name, to = dir + "\\" + (m.want ? "" : "_") + m.name;
        if (MoveFileA(from.c_str(), to.c_str())) {
            m.on = m.want;
            features::ModLog("Mods: %s switched %s (MODS menu)", m.name.c_str(), m.on ? "on" : "off");
        } else {
            m.want = m.on;
            if (g_failed.empty()) g_failed = m.name;
            features::ModLog("[fail] Mods: couldn't rename %s to %s (error %lu)", from.c_str(), to.c_str(), GetLastError());
        }
    }
    features::ReloadModsIndex();
    g_applied = true;
}

// ---- texts -----------------------------------------------------------------------------------------------------------
wchar_t g_text[40][96];
const wchar_t* Text(int slot, const char* fmt, ...) {
    char a[96];
    va_list args;
    va_start(args, fmt);
    vsnprintf(a, sizeof a, fmt, args);
    va_end(args);
    MultiByteToWideChar(CP_ACP, 0, a, -1, g_text[slot], 96);
    return g_text[slot];
}

const wchar_t* RoleText(int role) {
    if (role == kRoleButton) return L"mods";
    if (role == kRoleTitle) return L"mods";
    if (role == kRoleApply) {
        if (!g_failed.empty()) return Text(0, "couldn't switch %s", g_failed.c_str());
        if (const int n = Pending()) return Text(0, "apply %d change%s", n, n == 1 ? "" : "s");
        return g_applied ? L"restart the game to apply" : L"no changes";
    }
    if (role >= kRoleLabel0 && role < kRoleLabel0 + kRows) {
        const int r = role - kRoleLabel0, i = ModOnRow(r);
        if (i >= 0) return Text(1 + r, "%s", g_mods[i].name.c_str());
        if (Paged() && r == kRows - 1) return Text(1 + r, "more mods (%d / %d)", g_page + 1, Pages());
        return r == 0 && g_mods.empty() ? L"no mods installed" : L"";
    }
    if (role >= kRoleValue0 && role < kRoleValue0 + kRows) {
        const int i = ModOnRow(role - kRoleValue0);
        if (i < 0) return L"";
        const Mod& m = g_mods[i];
        return m.want ? (m.on ? L"on" : L"on *") : (m.on ? L"off *" : L"off");
    }
    return nullptr;
}

using TextLookupFn = const wchar_t*(__thiscall*)(void*, uint32_t);
TextLookupFn g_textLookup = nullptr;
const wchar_t* __fastcall TextLookup(void* catalog, void*, uint32_t hash) {
    if (!g_roles.empty()) {
        auto it = g_roles.find(hash);
        if (it != g_roles.end())
            if (const wchar_t* t = RoleText(it->second)) return t;
    }
    return g_textLookup(catalog, hash);
}

// ---- the screen ------------------------------------------------------------------------------------------------------
using EventFn = int(__thiscall*)(void*, int, void*);
uint32_t g_vtbl[kVtblSlots];
void* g_rowCtl[kRows] = {};

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
void Layout() {
    for (int r = 0; r < kRows; ++r)
        ShowControl(g_rowCtl[r], ModOnRow(r) >= 0 || (Paged() && r == kRows - 1) || (r == 0 && g_mods.empty()));
}

int __fastcall ScreenEvent(void* self, void*, int msg, void* control) {
#ifndef DS_DIST
    static uint32_t seen[8] = {};
    if (msg >= 0 && msg < 256 && !(seen[msg >> 5] & (1u << (msg & 31)))) {
        seen[msg >> 5] |= 1u << (msg & 31);
        dslog::Write("MODS menu: event 0x%X (control %08X)", msg, control ? *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(control) + 0xC) : 0);
    }
#endif
    if (msg == kMsgSetUp) {
        for (int r = 0; r < kRows; ++r) g_rowCtl[r] = FindControl(self, g_rowHash[r]);
        return 0;
    }
    if (msg == kMsgShown) {
        Scan();
        g_page = 0;
        g_applied = false;
        g_failed.clear();
        Layout();
        return 0;
    }
    if (msg == kMsgChosen && control) {
        const uint32_t h = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(control) + 0xC);
        if (h == g_applyHash) {
            if (Pending()) Apply();
            return 1;
        }
        for (int r = 0; r < kRows; ++r) {
            if (h != g_rowHash[r]) continue;
            const int i = ModOnRow(r);
            if (i >= 0) {
                g_mods[i].want = !g_mods[i].want;
                g_applied = false;
                g_failed.clear();
            } else if (Paged() && r == kRows - 1) {
                g_page = (g_page + 1) % Pages();
                Layout();
            }
            return 1;
        }
    }
    return reinterpret_cast<EventFn>(kBaseEvent)(self, msg, control);
}

using FactoryFn = void*(__cdecl*)(uint32_t);
FactoryFn g_factory = nullptr;
void* __cdecl Factory(uint32_t hash) {
    if (!g_patched || hash != g_screenHash) return g_factory(hash);
    void* obj = reinterpret_cast<void*(__cdecl*)(uint32_t, int)>(kNew)(0x70, 0x1F);
    if (!obj) return nullptr;
    reinterpret_cast<void*(__thiscall*)(void*, int)>(kScreenCtor)(obj, 0);
    *static_cast<uint32_t**>(obj) = g_vtbl;
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(obj) + 0x6C) = 0;
    dslog::Write("MODS menu: screen object %p", obj);
    return obj;
}

// ---- FE_MAIN ---------------------------------------------------------------------------------------------------------
EventFn g_mainEvent = nullptr;
int __fastcall MainEvent(void* self, void*, int msg, void* control) {
    if (g_patched && msg == kMsgChosen && control &&
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(control) + 0xC) == g_buttonHash) {
        if (void* gui = *reinterpret_cast<void**>(kGui)) reinterpret_cast<int(__thiscall*)(void*, uint32_t)>(kShowScreen)(gui, g_screenHash);
        return 1;
    }
    return g_mainEvent(self, msg, control);
}

template <class Fn>
bool Hook(uint32_t entry, const void* fn, Fn& next, const uint8_t* prologue, size_t len) {
    return patch::HookFunction(entry, fn, reinterpret_cast<void**>(&next), prologue, len);
}
}  // namespace

void features::ApplyModsMenu() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!Get().mods) return;   // no Mods folder, no menu
    memcpy(g_vtbl, reinterpret_cast<const void*>(kCheatsVtbl), sizeof g_vtbl);
    g_vtbl[14] = reinterpret_cast<uint32_t>(&ScreenEvent);
    if (!Hook(kReadWhole, reinterpret_cast<const void*>(&ReadWhole), g_readWhole, kReadWholePrologue, sizeof kReadWholePrologue) ||
        !Hook(kTextLookup, reinterpret_cast<const void*>(&TextLookup), g_textLookup, kTextLookupPrologue, sizeof kTextLookupPrologue) ||
        !Hook(kFactory, reinterpret_cast<const void*>(&Factory), g_factory, kFactoryPrologue, sizeof kFactoryPrologue) ||
        !patch::HookSlot(kMainEventSlot, reinterpret_cast<const void*>(&MainEvent), reinterpret_cast<void**>(&g_mainEvent), kMainEvent)) {
        ModLog("[fail] MODS menu: game code not recognised - no MODS item");
        return;
    }
    dslog::Write("[ok]   MODS menu: main menu MODS item");
}
