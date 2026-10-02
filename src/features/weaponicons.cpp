// Weapon pictures for mod weapons: the HUD weapon panel, the inventory rows and the blinking pick-up icon.
//
// All three draw image def +0x84 of the GWInt HUD sheet [0x60EE18] (GWInt.img: pages GWint0-4.png, 272-byte entries
// {u16 page, name[256], pad, x, y, w, h, offset x/y}), handed to them by FUN_0047faa0(item) (weapon panel FUN_0048ed30,
// inventory rows 0x452B10, pick-up icon 0x44BDC3). def +0x84 comes from Weaps.txt column 5 ("IMAGE_WEAPON_SAW") via
// FUN_004966c0(name): a chain of string compares that returns the image number.
//
// A .weapon's "hud icon = MyIcon" (a .png / .dds in the mod folder, mods.cpp) becomes column 5 "DSFIX_ICON_<n>": the
// lookup (entry jump) answers kIconBase + n for those. The sheet's record / UV arrays are copied into larger ones (game
// allocator, like the font glyphs in overlay.cpp) where our images are the base weapon's picture record (page, draw
// offset) sized to the icon file, with UVs over the whole icon; the page texture bind inside the image draw (0x53CC9B,
// fontsharp.cpp's hook) swaps in the icon's texture. The game draws it like its own: tint, fades, HUD scale, split views.
#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "features/overlay.h"

namespace {
constexpr uint32_t kIconLookup = 0x4966C0, kIconLookupCont = 0x4966C6;
constexpr uint8_t kIconLookupEntry[] = {0x8B, 0x44, 0x24, 0x04, 0x56, 0x57};  // mov eax, [esp+4]; push esi; push edi
constexpr uint32_t kHudSheet = 0x60EE18;   // GWInt.img
constexpr uint32_t kAllocArray = 0x4B8450;  // cdecl (size, tag): the sheet loader's allocator
constexpr uint32_t kIconBase = 1000;        // our image numbers (GWInt has 115 images)
constexpr char kPrefix[] = "DSFIX_ICON_";

struct Icon {
    std::string path, base;  // icon file; the base weapon's picture name (Weaps.txt column 5)
    void* texture = nullptr;
    bool tried = false;
    int w = 0, h = 0, texW = 0, texH = 0;
};
std::vector<Icon> g_icons;

using LookupFn = uint32_t(__cdecl*)(const char*);
LookupFn g_lookup = nullptr;  // the game's lookup (trampoline)

uint8_t* g_sheet = nullptr;    // the sheet our images were added to
uint8_t* g_records = nullptr;  // our record array, to notice the sheet being reloaded

uint32_t __cdecl IconLookup(const char* name) {
    if (name && _strnicmp(name, kPrefix, sizeof kPrefix - 1) == 0) {
        const int n = atoi(name + sizeof kPrefix - 1);
        if (n >= 0 && n < static_cast<int>(g_icons.size())) return kIconBase + n;
    }
    return g_lookup(name);
}

void* Trampoline(uint32_t entry, size_t len, uint32_t cont) {
    auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return nullptr;
    memcpy(t, reinterpret_cast<const void*>(entry), len);
    t[len] = 0xE9;
    const int32_t rel = static_cast<int32_t>(cont - (reinterpret_cast<uint32_t>(t) + len + 5));
    memcpy(t + len + 1, &rel, 4);
    return t;
}

// Adds our images to the HUD sheet (again if the game reloaded it).
void Extend() {
    auto* sheet = *reinterpret_cast<uint8_t**>(kHudSheet);
    if (!sheet || g_icons.empty()) return;
    if (sheet == g_sheet && *reinterpret_cast<uint8_t**>(sheet + 4) == g_records) return;
    const uint8_t* header = *reinterpret_cast<uint8_t**>(sheet);
    const uint8_t* oldRecords = *reinterpret_cast<uint8_t**>(sheet + 4);
    const uint8_t* oldUvs = *reinterpret_cast<uint8_t**>(sheet + 8);
    if (!header || !oldRecords || !oldUvs) return;
    const uint32_t count = *reinterpret_cast<const uint16_t*>(header + 6);
    if (!count || count > kIconBase) {
        static bool logged = false;
        if (!logged) dslog::Write("[fail] Weapon icons: HUD sheet has %u images - no room", count), logged = true;
        return;
    }
    using Alloc = void*(__cdecl*)(uint32_t, uint32_t);
    const uint32_t total = kIconBase + static_cast<uint32_t>(g_icons.size());
    auto* records = static_cast<uint8_t*>(reinterpret_cast<Alloc>(kAllocArray)(total * 24, 0x17));
    auto* uvs = static_cast<uint8_t*>(reinterpret_cast<Alloc>(kAllocArray)(total * 16, 0x17));
    if (!records || !uvs) return;
    memcpy(records, oldRecords, count * 24);
    memcpy(uvs, oldUvs, count * 16);
    for (uint32_t i = count; i < kIconBase; ++i) {  // unused numbers: copies of image 0
        memcpy(records + i * 24, oldRecords, 24);
        memcpy(uvs + i * 16, oldUvs, 16);
    }
    int loaded = 0;
    for (size_t n = 0; n < g_icons.size(); ++n) {
        Icon& ic = g_icons[n];
        uint32_t base = g_lookup(ic.base.c_str()) & 0xFFFF;
        if (base >= count) base = 0;
        uint8_t* rec = records + (kIconBase + n) * 24;
        auto* uv = reinterpret_cast<float*>(uvs + (kIconBase + n) * 16);
        memcpy(rec, oldRecords + base * 24, 24);  // the base weapon's picture: same page, draw offset
        memcpy(uv, oldUvs + base * 16, 16);       // (and its picture, should the icon file fail)
        if (!ic.tried) {
            ic.tried = true;
            ic.texture = overlay::LoadTextureFile(ic.path.c_str(), ic.w, ic.h, ic.texW, ic.texH);
            if (!ic.texture) features::ModLog("[fail] Mods: picture %s not loaded - the %s picture is used", ic.path.c_str(),
                                          ic.base.c_str());
        }
        if (!ic.texture) continue;
        *reinterpret_cast<uint16_t*>(rec + 0xE) = static_cast<uint16_t>(ic.w);  // 1 icon pixel = 1 sheet pixel
        *reinterpret_cast<uint16_t*>(rec + 0x10) = static_cast<uint16_t>(ic.h);
        uv[0] = 0.0f, uv[1] = 0.0f;
        uv[2] = static_cast<float>(ic.w) / ic.texW, uv[3] = static_cast<float>(ic.h) / ic.texH;
        ++loaded;
    }
    *reinterpret_cast<uint8_t**>(sheet + 4) = records;  // the old arrays stay allocated (a few KB, once per load)
    *reinterpret_cast<uint8_t**>(sheet + 8) = uvs;
    g_sheet = sheet;
    g_records = records;
    dslog::Write("Weapon icons: %d of %u added to the HUD sheet (images %u..%u)", loaded,
                 static_cast<unsigned>(g_icons.size()), kIconBase, total - 1);
}
}  // namespace

int features::AddWeaponIcon(const std::string& path, const std::string& baseIcon) {
    Icon ic;
    ic.path = path;
    ic.base = baseIcon;
    g_icons.push_back(ic);
    return static_cast<int>(g_icons.size()) - 1;
}

void* features::WeaponIconTexture(const void* sheet, int image) {
    if (!g_sheet || sheet != g_sheet || image < static_cast<int>(kIconBase)) return nullptr;
    const size_t n = static_cast<size_t>(image) - kIconBase;
    return n < g_icons.size() ? g_icons[n].texture : nullptr;
}

// Hot reload: the icons are named again by the .weapon files; our images are added to the sheet again.
void features::ResetWeaponIcons() {
    for (Icon& ic : g_icons)
        if (ic.texture) reinterpret_cast<ULONG(__stdcall*)(void*)>((*static_cast<void***>(ic.texture))[2])(ic.texture);
    g_icons.clear();
    g_sheet = nullptr;  // re-extend the sheet on the next frame
}

void features::OnFrameWeaponIcons() {
    if (g_lookup) Extend();
}

void features::ApplyWeaponIcons() {
    if (g_lookup) return;  // installed even without icons: a mod switched on later can bring some
    if (!patch::Matches(kIconLookup, kIconLookupEntry, sizeof kIconLookupEntry) || !overlay::Install()) {
        dslog::Write("[fail] Weapon icons: picture lookup / image draw not recognised - mod weapons keep their base picture");
        return;
    }
    g_lookup = reinterpret_cast<LookupFn>(Trampoline(kIconLookup, sizeof kIconLookupEntry, kIconLookupCont));
    if (!g_lookup || !patch::WriteJump(kIconLookup, reinterpret_cast<const void*>(&IconLookup))) {
        g_lookup = nullptr;
        return;
    }
    if (!g_icons.empty()) dslog::Write("Weapon icons: %u mod weapon picture(s)", static_cast<unsigned>(g_icons.size()));
}
