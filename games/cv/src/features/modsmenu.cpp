// MODS menu: a MODS item in the game's own main menu and a front-end state of its own listing every Mods\ folder
// (ON / OFF, toggled with the game's list input). The screen is an object of the game's OPTIONS screen class with a
// copied vtable (create / set-up / update replaced), so the game's list widget lays out, draws and navigates the rows
// and its transitions / prompts / Back work as on any other front-end screen. RE notes: games/cv/CLAUDE.md "Mods menu".
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {

// ---- game addresses (Vietnam.exe Sep 2004) ----
constexpr uint32_t kMainVtable = 0x6AB760;      // main menu screen class (state 2)
constexpr uint32_t kOptionsVtable = 0x6AC170;   // OPTIONS screen class (state 6) - our screen's class
constexpr int kVtableSlots = 22;
constexpr uint32_t kMainSetupSlot = kMainVtable + 3 * 4;
constexpr uint32_t kMainSetup = 0x460CFF;
constexpr uint32_t kMainCapacityImm = 0x460A35;  // push 4 (main menu list capacity + item count) in create 0x4609E1
constexpr uint32_t kMainInputCall = 0x460AF9;    // main menu update: call list input 0x582B40
constexpr uint32_t kListInput = 0x582B40;        // thiscall(list) -> id of the item picked this frame (0 none)
constexpr uint32_t kScreenForState = 0x443316;   // cdecl(state) -> screen object
constexpr uint32_t kScreenForStateCont = 0x44331D;
constexpr uint32_t kLookup = 0x56BE90;           // thiscall(catalog [0x70D694], hash) -> wchar_t* (catalog text)
constexpr uint32_t kLookupCont = 0x56BE95;
constexpr uint32_t kRequestState = 0x443012;     // cdecl(state)
constexpr uint32_t kSetMenuMode = 0x443042;      // cdecl(mode) -> [0x7126B4]
constexpr uint32_t kBackState = 0x7126A0;        // state Back returns to (the main menu sets it before OPTIONS)
constexpr uint32_t kOptionsCtor = 0x46A33A;      // thiscall(obj) - 0x104-byte OPTIONS screen object
constexpr uint32_t kMenuMgr = 0x712688;          // menu manager (+0x128 = transition running)
constexpr uint32_t kRenderer = 0x8842D0;         // +0x41A34 W, +0x41A38 H (u16)

constexpr int kModsState = 0x39;                 // first state id ScreenForState doesn't know
constexpr uint16_t kMainItemId = 0x401;          // next after the main menu's 0x3FD..0x400
constexpr uint16_t kRowId = 0x8A0;               // our rows: 0x8A0 + i
constexpr int kMaxRows = 128;
constexpr int kVisibleRows = 10;                 // more rows than this scroll (the list's own window, +0x5C / +0x5E)
constexpr uint32_t kHashModsItem = 0xC0A10001;   // our catalog texts (Lookup hook)
constexpr uint32_t kHashModsTitle = 0xC0A10002;

// Screen object (OPTIONS class, 0x104 bytes): +0 vtable, +0xDC list, +0x100 remembered cursor (-1 none).
// List (0xD8 bytes, vtable 0x6BD6F4, +4 = add item): +0x18 font, +0x1C sheet, +0x38 pool count, +0x3A items shown,
// +0x3C cursor, +0x40 item pool (shown in pool order), +0x52 row pitch, +0x54/+0x56/+0x58/+0x5A area x/y/w/h (set by
// 0x580460; layout 4 = rows that fit = h / pitch -> +0x5C), +0x5E first row drawn (the cursor moves keep it in view; the
// draw 0x583490 shows +0x5C rows from it). Item (0xBC bytes): +4 id, +8 text hash, +0x10 flags (0x80 on,
// 0x81 greyed), +0x28 wchar_t text buffer (used instead of the hash's text when not empty; 0x94 bytes).
constexpr size_t kItemTextOffset = 0x28;
constexpr size_t kItemTextChars = 0x94 / 2;

using Fn0 = int(__fastcall*)(void* self, void*);
using Fn1 = int(__fastcall*)(void* self, void*, int);
using Fn2 = int(__fastcall*)(void* self, void*, int, int);
using Fn3 = int(__fastcall*)(void* self, void*, int, int, int);
using Fn4 = int(__fastcall*)(void* self, void*, int, int, int, int);
using Fn10 = int(__fastcall*)(void* self, void*, int, int, int, int, int, int, int, int, int, int);
template <class F>
F At(uint32_t va) { return reinterpret_cast<F>(va); }
template <class T>
T& Ref(uintptr_t a) { return *reinterpret_cast<T*>(a); }

void* (__cdecl* g_screenForState)(int) = nullptr;
const wchar_t*(__fastcall* g_lookup)(void*, void*, uint32_t) = nullptr;
alignas(16) uint8_t g_object[0x200];
void* g_vtable[kVtableSlots];
bool g_built = false;
void* g_mainSeen = nullptr;                      // main menu object our screen was built alongside
std::vector<std::pair<std::string, bool>> g_rows;

uint8_t* List(void* screen) { return Ref<uint8_t*>(reinterpret_cast<uintptr_t>(screen) + 0xDC); }
void** Pool(uint8_t* list) { return Ref<void**>(reinterpret_cast<uintptr_t>(list) + 0x40); }

void AllocSite() {   // the allocator's bookkeeping call, as OPTIONS' create makes it before each allocation
    reinterpret_cast<void(__cdecl*)(uint32_t, int, uint32_t)>(0x52B855)(0x6AC1C8, 0x78, 0x6AC1E0);
}
void* Alloc(int size) {
    AllocSite();
    return reinterpret_cast<void*(__cdecl*)(int, int)>(0x52BCB6)(size, 0x37);
}

// One row: init the pool item, put our text in its buffer, add it (the list shows pool order).
void AddItem(uint8_t* list, int index, uint16_t id, uint32_t hash, int flags, const wchar_t* text) {
    void* item = Pool(list)[index];
    const float zero = 0.0f;
    int zbits;
    memcpy(&zbits, &zero, 4);
    At<Fn10>(0x580040)(item, nullptr, id, static_cast<int>(hash), 0, flags, 0, 0, 0x46A513, zbits, 0, 0x60);
    wchar_t* buf = reinterpret_cast<wchar_t*>(static_cast<uint8_t*>(item) + kItemTextOffset);
    if (text) {
        wcsncpy(buf, text, kItemTextChars - 1);
        buf[kItemTextChars - 1] = 0;
    } else {
        buf[0] = 0;
    }
    auto vt = Ref<void**>(reinterpret_cast<uintptr_t>(list));
    reinterpret_cast<Fn1>(vt[1])(list, nullptr, reinterpret_cast<int>(item));
}

std::wstring RowText(const std::pair<std::string, bool>& m) {
    std::wstring w;
    for (char c : m.first) w += static_cast<wchar_t>(static_cast<unsigned char>(toupper(static_cast<unsigned char>(c))));
    if (w.size() > 40) w.resize(40);
    return w + (m.second ? L"   ON" : L"   OFF");
}

// ---- our screen's vtable slots ----

// slot 1: create (once) - list with room for kMaxRows rows (OPTIONS' own create makes 4).
char __fastcall ModsCreate(void* self, void*) {
    void* list = Alloc(0xD8);
    if (list)
        list = reinterpret_cast<void*>(At<Fn3>(0x581750)(list, nullptr, Ref<int>(0x70D674), Ref<int>(0x70D660), 1));
    Ref<void*>(reinterpret_cast<uintptr_t>(self) + 0xDC) = list;
    if (!list || !At<Fn1>(0x580330)(list, nullptr, kMaxRows)) return 0;
    for (int i = 0; i < kMaxRows; ++i) {
        void* item = Alloc(0xBC);
        if (item) item = reinterpret_cast<void*>(At<Fn0>(0x5811D0)(item, nullptr));
        At<Fn1>(0x5803E0)(list, nullptr, reinterpret_cast<int>(item));
    }
    return 1;
}

// slot 3: set-up (every time the screen is entered) - OPTIONS' set-up with our title and rows.
char __fastcall ModsSetup(void* self, void*) {
    // Fonts / sheets are re-assigned to every screen at each front-end load (vtable +0x54 = 0x4326FF); ours isn't in
    // the game's screen list, so take the current ones here.
    At<Fn3>(0x4326FF)(self, nullptr, Ref<int>(0x70D674), Ref<int>(0x70D660), Ref<int>(0x7126BC));
    reinterpret_cast<void(__cdecl*)(int)>(0x429A96)(1);  // what the state change does for OPTIONS (ours is unknown)
    uint8_t* list = List(self);
    At<Fn0>(0x581710)(list, nullptr);
    At<Fn0>(0x585F40)(list, nullptr);
    At<Fn2>(0x5803C0)(list, nullptr, 4, 1);
    At<Fn2>(0x5815C0)(list, nullptr, -1, -1);
    const uintptr_t r = Ref<uintptr_t>(kRenderer);
    At<Fn4>(0x580460)(list, nullptr, 0, 0, Ref<uint16_t>(r + 0x41A34), Ref<uint16_t>(r + 0x41A38));
    At<Fn1>(0x580870)(list, nullptr, 4);
    At<Fn1>(0x580880)(list, nullptr, 1);
    At<Fn10>(0x5804F0)(list, nullptr, 0x4420B3, 0x44221D, 0x442282, 0x4422E7, 0x44234C, 0, 0, 0, 0, 0);
    const int unit = (reinterpret_cast<int(__cdecl*)()>(0x40EBD3)() << 9) / 0x280;
    At<Fn1>(0x580370)(list, nullptr, static_cast<short>(unit) / 2);
    At<Fn1>(0x5803B0)(list, nullptr, (reinterpret_cast<int(__cdecl*)()>(0x40EBD3)() << 9) / 0x280);
    At<Fn4>(0x432E31)(self, nullptr, static_cast<int>(kHashModsTitle), 1, 1, 0);

    g_rows = features::ModsList();
    if (g_rows.size() > static_cast<size_t>(kMaxRows)) g_rows.resize(kMaxRows);
    if (g_rows.empty()) {
        AddItem(list, 0, kRowId, 0, 0x81, L"NO MODS INSTALLED");
    } else {
        for (size_t i = 0; i < g_rows.size(); ++i)
            AddItem(list, static_cast<int>(i), static_cast<uint16_t>(kRowId + i), 0, 0x80, RowText(g_rows[i]).c_str());
    }
    // Many mods: give the list a window of kVisibleRows centred on the screen, so the game's own list scrolls it.
    const uintptr_t l = reinterpret_cast<uintptr_t>(list);
    const int count = static_cast<int>(g_rows.size());
    const int pitch = Ref<int16_t>(l + 0x52);
    int visible = count;
    if (count > kVisibleRows && pitch > 0) {
        const int w = Ref<uint16_t>(r + 0x41A34), h = Ref<uint16_t>(r + 0x41A38);
        const int areaH = kVisibleRows * pitch;
        At<Fn4>(0x580460)(list, nullptr, 0, (h - areaH) / 2, w, areaH);
        visible = Ref<uint16_t>(l + 0x5C);
        if (visible <= 0 || visible > count) visible = count;
    }
    const int cursor = Ref<int>(reinterpret_cast<uintptr_t>(self) + 0x100);
    if (cursor != -1 && cursor < count) Ref<int16_t>(l + 0x3C) = static_cast<int16_t>(cursor);
    At<Fn2>(0x5836E0)(list, nullptr, 0x7052C4, 0x7052B4);
    At<Fn0>(0x582A80)(list, nullptr);
    if (visible < count) {   // keep the remembered row in view
        const int cur = Ref<int16_t>(l + 0x3C);
        int first = Ref<int16_t>(l + 0x5E);
        if (first < 0 || first > count - visible) first = 0;
        if (cur < first) first = cur;
        if (cur >= first + visible) first = cur - visible + 1;
        Ref<int16_t>(l + 0x5E) = static_cast<int16_t>(first);
    }
    dslog::Write("Mods menu: %d row(s), %d shown at a time (pitch %d)", count, visible, pitch);
    return 1;
}

// slot 4: update (every frame) - OPTIONS' update with our rows: Enter toggles, Back returns.
int __fastcall ModsUpdate(void* self, void*) {
    uint8_t* list = List(self);
    const int result = At<Fn1>(0x466464)(Ref<void*>(kMenuMgr), nullptr, reinterpret_cast<int>(list));
    int id = 0;
    if (Ref<int>(Ref<uintptr_t>(kMenuMgr) + 0x128) == 0) id = static_cast<uint16_t>(At<Fn0>(kListInput)(list, nullptr));
    if (reinterpret_cast<int(__cdecl*)(int)>(0x442059)(0)) {   // Back
        reinterpret_cast<void(__cdecl*)(int)>(kRequestState)(Ref<int>(kBackState));
        Ref<int>(reinterpret_cast<uintptr_t>(self) + 0x100) = -1;
        return result;
    }
    const int row = id - kRowId;
    if (id && row >= 0 && row < static_cast<int>(g_rows.size())) {
        Ref<int>(reinterpret_cast<uintptr_t>(self) + 0x100) = Ref<int16_t>(reinterpret_cast<uintptr_t>(list) + 0x3C);
        if (features::ModsToggle(g_rows[row].first)) {
            g_rows = features::ModsList();
            // the rename keeps the folder order (names compare without the "_"), so the row stays put
            if (row < static_cast<int>(g_rows.size())) {
                wchar_t* buf = reinterpret_cast<wchar_t*>(static_cast<uint8_t*>(Pool(list)[row]) + kItemTextOffset);
                wcsncpy(buf, RowText(g_rows[row]).c_str(), kItemTextChars - 1);
            }
        }
    }
    return result;
}

void Build(void* mainScreen) {
    memset(g_object, 0, sizeof g_object);
    At<Fn0>(kOptionsCtor)(g_object, nullptr);
    memcpy(g_vtable, reinterpret_cast<void*>(kOptionsVtable), sizeof g_vtable);
    g_vtable[1] = reinterpret_cast<void*>(&ModsCreate);
    g_vtable[3] = reinterpret_cast<void*>(&ModsSetup);
    g_vtable[4] = reinterpret_cast<void*>(&ModsUpdate);
    *reinterpret_cast<void***>(g_object) = g_vtable;
    Ref<int>(reinterpret_cast<uintptr_t>(g_object) + 0x100) = -1;
    g_built = ModsCreate(g_object, nullptr) != 0;
    g_mainSeen = mainScreen;
    dslog::Write(g_built ? "Mods menu: screen built (state 0x%X)" : "[fail] Mods menu: screen not built (state 0x%X)",
                 kModsState);
}

// ---- hooks on the game ----

// Main menu set-up (vtable slot 3): the game's 4 items, then MODS (pool item 4), shown before OPTIONS.
char __fastcall MainSetup(void* self, void*) {
    const char ok = static_cast<char>(At<Fn0>(kMainSetup)(self, nullptr));
    if (!g_built || g_mainSeen != self) Build(self);   // the game made its screens (again): fonts / heaps are new
    uint8_t* list = List(self);
    if (!list || Ref<int16_t>(reinterpret_cast<uintptr_t>(list) + 0x38) < 5) return ok;
    AddItem(list, 4, kMainItemId, kHashModsItem, 0x80, nullptr);
    void** pool = Pool(list);
    void* mods = pool[4];   // NEW GAME, LOAD GAME, MODS, OPTIONS, EXIT TO WINDOWS
    pool[4] = pool[3];
    pool[3] = pool[2];
    pool[2] = mods;
    return ok;
}

// Main menu update's list input: the game handles 0x3FD..0x400; MODS opens our screen.
int __fastcall MainListInput(void* list, void*) {
    const int id = At<Fn0>(kListInput)(list, nullptr);
    if (static_cast<uint16_t>(id) != kMainItemId || !g_built) return id;
    Ref<int>(kBackState) = 2;
    reinterpret_cast<void(__cdecl*)(int)>(kSetMenuMode)(1);
    reinterpret_cast<void(__cdecl*)(int)>(kRequestState)(kModsState);
    return 0;
}

void* __cdecl ScreenForState(int state) {
    if (state == kModsState) return g_built ? g_object : nullptr;
    return g_screenForState(state);
}

const wchar_t* __fastcall Lookup(void* catalog, void* edx, uint32_t hash) {
    if (hash == kHashModsItem || hash == kHashModsTitle) return L"MODS";
    return g_lookup(catalog, edx, hash);
}

void* Trampoline(const uint8_t* entry, size_t len, uint32_t cont) {
    auto stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) return nullptr;
    memcpy(stub, entry, len);
    stub[len] = 0xE9;
    const int32_t rel = static_cast<int32_t>(cont - (reinterpret_cast<uint32_t>(stub) + len + 5));
    memcpy(stub + len + 1, &rel, 4);
    return stub;
}

}  // namespace

void features::ApplyModsMenu() {
    static const uint8_t kSfsEntry[] = {0x8B, 0x44, 0x24, 0x04, 0x83, 0xF8, 0x12};   // mov eax,[esp+4]; cmp eax,0x12
    static const uint8_t kLookupEntry[] = {0x8B, 0x41, 0x38, 0x85, 0xC0};           // mov eax,[ecx+0x38]; test eax,eax
    static const uint8_t kCapOld[] = {0x04};
    static const uint8_t kCapNew[] = {0x05};
    const uint32_t mainSetup = kMainSetup;
    if (!patch::Matches(kScreenForState, kSfsEntry, sizeof kSfsEntry) ||
        !patch::Matches(kLookup, kLookupEntry, sizeof kLookupEntry) ||
        !patch::Matches(kMainCapacityImm, kCapOld, 1) || !patch::Matches(kMainSetupSlot, &mainSetup, 4)) {
        dslog::Write("[fail] Mods menu: unexpected bytes at the front-end sites - different exe version? No MODS menu.");
        return;
    }
    g_screenForState = reinterpret_cast<void*(__cdecl*)(int)>(Trampoline(kSfsEntry, sizeof kSfsEntry, kScreenForStateCont));
    g_lookup = reinterpret_cast<const wchar_t*(__fastcall*)(void*, void*, uint32_t)>(
        Trampoline(kLookupEntry, sizeof kLookupEntry, kLookupCont));
    if (!g_screenForState || !g_lookup || !patch::HookCall(kMainInputCall, reinterpret_cast<void*>(&MainListInput), kListInput)) {
        dslog::Write("[fail] Mods menu: hooks not installed");
        return;
    }
    patch::WriteJump(kScreenForState, reinterpret_cast<void*>(&ScreenForState));
    patch::WriteJump(kLookup, reinterpret_cast<void*>(&Lookup));
    patch::Write(kMainCapacityImm, kCapNew, 1);
    patch::WriteValue(kMainSetupSlot, reinterpret_cast<uint32_t>(&MainSetup));
    dslog::Write("Mods menu: MODS item on the main menu (state 0x%X)", kModsState);
}
