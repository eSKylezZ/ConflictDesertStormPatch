// Weapon pictures for mod weapons (port of DS1's weaponicons.cpp): a .weapon's "hud icon = MyGun" (MyGun.png / .dds /
// .tga in the mod folder) is drawn wherever the game draws a weapon's picture - the HUD weapon panel, the inventory rows,
// the pick-up icon - like the game's own (tint, fades, HUD scale, split views).
//
// How CDS II.exe pictures a weapon (2026-10-23; DS1 addresses in brackets):
//  - Weaps.txt column 5 ("IMAGE_WEAPON_SAW") -> the picture lookup FUN_00452170 [FUN_004966c0]: a chain of string
//    compares returning the image number in ax; own convention: name in eax, no stack arguments, plain ret; an unknown
//    name = "Warning - unable to find interface ID %s" (FUN_004cd600) + 0xFFFF. Callers: Weaps parser 0x453170 (-> the
//    weapon definition +0x84, as DS1), Units 0x454AED, Inven 0x455546.
//  - the images are those of the interface sheet ds_int.img [0x62F434] (CSpriteRender, 0x50 bytes, ctor FUN_0052b290,
//    read by FUN_0052ad20 at start-up 0x41448F; freed by FUN_0052ac30 at exit): +0 header (0x11C bytes: +4 version u16,
//    +6 image count u16 (178), +0xA page count u16 (6), +0xC page names char[16]), +4 records (0x1C bytes: +0 page u16,
//    +4 owned pointer (freed at exit when non-zero), +0xA x, +0xC y, +0xE w, +0x10 h, +0x12 / +0x14 draw offset (s16),
//    +0x18 name hash), +8 UVs (16 bytes: u0 v0 u1 v1), +0xC pages (texture objects from FUN_005432b0: +0xC palette
//    (-1 = none), +0x10 IDirect3DTexture9*). File ds_int.img (catalog.dat): 284-byte header, 178 x 272-byte entries
//    {u16 page, char[256] name, u16 x 7} - IMAGE_WEAPON_M16M203 = 166.
//  - every sheet image draw (FUN_0052b7b0 and the 13 other variants in 0x52BE70..0x52E2xx) binds
//    `pages[record.page]->+0x10` through the renderer's SetTexture wrapper 0x5364E0 (call sites 0x52BA5E, 0x52BE23,
//    0x52C191, 0x52C3A6, 0x52C3D9, 0x52C70F, 0x52CA51, 0x52CA86, 0x52CE9A, 0x52D0B2, 0x52D376, 0x52D69A, 0x52D9A7,
//    0x52DA4C, 0x52E284) after FUN_00536af0(+0xC) when +0xC >= 0.
//
// Here: the lookup's entry is detoured (pse.hooks HookFunction): "DS2FIX_ICON_<n>" (written into the Weaps row by
// mods_tables.cpp) answers kIconBase + n. Once the sheet and the device exist, the sheet's record / UV / page arrays are
// copied into larger ones (the game's allocator FUN_004be320, so the exit clean-up frees them as its own): our images
// are the base weapon's record (draw offset, hash) sized to the picture with UVs over it, on a page of their own - a
// small page object whose texture is the picture. So every draw variant binds the picture itself: no texture-bind hook,
// nothing shared with ds2fix.widescreen's sharp-text hook at 0x52BA5E. The header counts stay the game's.
#include <windows.h>
#include <d3d9.h>
#include <wincodec.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "features/graphics.h"
#ifdef DS2_PSE_HOST
#include "core/pse_host.h"
#include "loader.h"
#endif

namespace {
constexpr uint32_t kLookup = 0x452170, kLookupEnd = 0x4527EC;
constexpr uint8_t kLookupEntry[] = {0x56, 0x57, 0xBF, 0x64, 0x55, 0x5E, 0x00};  // push esi; push edi; mov edi, 0x5E5564
constexpr uint32_t kSheet = 0x62F434;   // ds_int.img
constexpr uint32_t kAlloc = 0x4BE320;   // cdecl (size, tag): the sheet reader's allocator (freed by FUN_004be3b0)
constexpr uint32_t kIconBase = 1000;    // our image numbers (ds_int has 178 images)
constexpr uint32_t kRecord = 0x1C, kUv = 0x10;
constexpr char kPrefix[] = "DS2FIX_ICON_";

struct Icon {
    std::string path, base, file;   // picture file; the base weapon's picture name (Weaps col 5); the .weapon (report)
    IDirect3DTexture9* texture = nullptr;
    bool tried = false;
    UINT w = 0, h = 0, texW = 0, texH = 0;
    uint8_t page[0x40] = {};         // our page object: +0xC palette -1, +0x10 the texture
};
std::vector<Icon*> g_icons;          // never freed: the sheet's page array points into them
bool g_hooked = false;
void* g_next = nullptr;              // the lookup's trampoline
uint8_t* g_sheet = nullptr;          // the sheet our images were added to
uint8_t* g_records = nullptr;        // our record array (the sheet reloaded = a new one there)

// Our number for a DS2FIX_ICON_<n> name, else ~0 (the game's lookup answers).
uint32_t __cdecl IconNumber(const char* name) {
    if (name && _strnicmp(name, kPrefix, sizeof kPrefix - 1) == 0) {
        const int n = atoi(name + sizeof kPrefix - 1);
        if (n >= 0 && n < static_cast<int>(g_icons.size())) return kIconBase + n;
    }
    return ~0u;
}

__declspec(naked) void LookupStub() {
    __asm {
        push eax              // the name (eax) for the game's lookup
        push eax
        call IconNumber
        add esp, 4
        cmp eax, 0xFFFFFFFF
        je game
        add esp, 4
        ret
    game:
        pop eax
        jmp dword ptr [g_next]
    }
}

// The game's number for a picture name, read from the lookup's compare chain (mov edi, <string>; ... mov ax, <n>)
// without calling it (an unknown name would show a warning box): 0xFFFF = unknown.
uint32_t GameImage(const char* name) {
    if (!name || !*name) return 0xFFFF;
    if (_stricmp(name, "IMAGE_MK19") == 0) return 0x43;   // the first compare (its bytes move into the hook's trampoline)
    for (uint32_t a = kLookup + sizeof kLookupEntry; a + 5 < kLookupEnd; ++a) {
        const auto* p = reinterpret_cast<const uint8_t*>(a);
        if (p[0] != 0xBF) continue;
        const auto* s = *reinterpret_cast<const char* const*>(p + 1);
        if (reinterpret_cast<uintptr_t>(s) < 0x5D0000 || reinterpret_cast<uintptr_t>(s) > 0x620000) continue;
        if (_stricmp(s, name) != 0) continue;
        for (uint32_t b = a + 5; b < a + 40 && b + 4 < kLookupEnd; ++b)   // the match's "mov ax, imm16"
            if (reinterpret_cast<const uint8_t*>(b)[0] == 0x66 && reinterpret_cast<const uint8_t*>(b)[1] == 0xB8)
                return *reinterpret_cast<const uint16_t*>(b + 2);
    }
    return 0xFFFF;
}

// An encoded picture (PNG, DDS DXT1-5, TGA ... what WIC reads) as a managed A8R8G8B8 texture, sizes rounded up to
// powers of two, the picture at the top-left.
IDirect3DTexture9* LoadPicture(const char* path, UINT& w, UINT& h, UINT& texW, UINT& texH) {
    w = h = texW = texH = 0;
    IDirect3DDevice9* dev = graphics::Device();
    if (!dev) return nullptr;
    std::vector<uint8_t> pixels;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    wchar_t wide[MAX_PATH];
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH) &&
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(wide, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                        WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(converter->GetSize(&w, &h)) && w && h && w <= 1024 && h <= 1024) {
        pixels.resize(size_t(w) * h * 4);
        if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data()))) pixels.clear();
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{converter, frame, decoder, factory})
        if (u) u->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    if (pixels.empty()) return nullptr;
    for (texW = 1; texW < w; texW *= 2) {}
    for (texH = 1; texH < h; texH *= 2) {}
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(dev->CreateTexture(texW, texH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) || !tex) return nullptr;
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
        for (UINT y = 0; y < texH; ++y) {
            auto* row = static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch;
            memset(row, 0, size_t(texW) * 4);
            if (y < h) memcpy(row, &pixels[size_t(y) * w * 4], size_t(w) * 4);
        }
        tex->UnlockRect(0);
    }
    return tex;
}

// Adds our images to the sheet (again if the game made a new one).
void Extend() {
    auto* sheet = *reinterpret_cast<uint8_t**>(kSheet);
    if (!sheet || g_icons.empty() || !graphics::Device()) return;
    if (sheet == g_sheet && *reinterpret_cast<uint8_t**>(sheet + 4) == g_records) return;
    const uint8_t* header = *reinterpret_cast<uint8_t**>(sheet);
    const uint8_t* oldRecords = *reinterpret_cast<uint8_t**>(sheet + 4);
    const uint8_t* oldUvs = *reinterpret_cast<uint8_t**>(sheet + 8);
    void* const* oldPages = *reinterpret_cast<void* const* const*>(sheet + 0xC);
    if (!header || !oldRecords || !oldUvs || !oldPages) return;
    const uint32_t count = *reinterpret_cast<const uint16_t*>(header + 6);
    const uint32_t pageCount = *reinterpret_cast<const uint16_t*>(header + 0xA);
    if (!count || count > kIconBase || !pageCount) {
        static bool logged = false;
        if (!logged) features::ModLog("[fail] Weapon pictures: interface sheet has %u images - no room", count), logged = true;
        g_sheet = sheet, g_records = const_cast<uint8_t*>(oldRecords);
        return;
    }
    using Alloc = void*(__cdecl*)(uint32_t, uint32_t);
    const uint32_t total = kIconBase + static_cast<uint32_t>(g_icons.size());
    const uint32_t pages = pageCount + static_cast<uint32_t>(g_icons.size());
    auto* records = static_cast<uint8_t*>(reinterpret_cast<Alloc>(kAlloc)(total * kRecord, 0x1F));
    auto* uvs = static_cast<uint8_t*>(reinterpret_cast<Alloc>(kAlloc)(total * kUv, 0x1F));
    auto* pageArray = static_cast<void**>(reinterpret_cast<Alloc>(kAlloc)(pages * 4, 0x1F));
    if (!records || !uvs || !pageArray) return;
    memcpy(records, oldRecords, count * kRecord);
    memcpy(uvs, oldUvs, count * kUv);
    memcpy(pageArray, oldPages, pageCount * 4);
    for (uint32_t i = count; i < kIconBase; ++i) {   // unused numbers: copies of image 0 (owning nothing)
        memcpy(records + i * kRecord, oldRecords, kRecord);
        *reinterpret_cast<uint32_t*>(records + i * kRecord + 4) = 0;
        memcpy(uvs + i * kUv, oldUvs, kUv);
    }
    int loaded = 0;
    for (size_t n = 0; n < g_icons.size(); ++n) {
        Icon& ic = *g_icons[n];
        uint32_t base = GameImage(ic.base.c_str());
        if (base >= count) base = 0;
        uint8_t* rec = records + (kIconBase + n) * kRecord;
        auto* uv = reinterpret_cast<float*>(uvs + (kIconBase + n) * kUv);
        memcpy(rec, oldRecords + base * kRecord, kRecord);   // the base weapon's picture: draw offset, hash
        *reinterpret_cast<uint32_t*>(rec + 4) = 0;
        memcpy(uv, oldUvs + base * kUv, kUv);                 // (and its picture, should the file fail)
        pageArray[pageCount + n] = oldPages[*reinterpret_cast<uint16_t*>(rec) < pageCount ? *reinterpret_cast<uint16_t*>(rec) : 0];
        if (!ic.tried) {
            ic.tried = true;
            ic.texture = LoadPicture(ic.path.c_str(), ic.w, ic.h, ic.texW, ic.texH);
            if (!ic.texture)
                features::ModLog("[fail] Mods: %s - picture %s not loaded (PNG / DDS / TGA up to 1024 x 1024) - the %s "
                                 "picture is used", ic.file.c_str(), ic.path.c_str(), ic.base.c_str());
            else if (ic.w > 256 || ic.h > 96)
                features::ModLog("Mods: note - %s: picture %ux%u is drawn at that size (the game's weapon pictures are "
                                 "about 120 x 35)", ic.file.c_str(), ic.w, ic.h);
        }
        if (!ic.texture) continue;
        *reinterpret_cast<int32_t*>(ic.page + 0xC) = -1;
        *reinterpret_cast<IDirect3DTexture9**>(ic.page + 0x10) = ic.texture;
        pageArray[pageCount + n] = ic.page;
        *reinterpret_cast<uint16_t*>(rec) = static_cast<uint16_t>(pageCount + n);
        *reinterpret_cast<uint16_t*>(rec + 0xA) = 0, *reinterpret_cast<uint16_t*>(rec + 0xC) = 0;
        *reinterpret_cast<uint16_t*>(rec + 0xE) = static_cast<uint16_t>(ic.w);   // 1 picture pixel = 1 sheet pixel
        *reinterpret_cast<uint16_t*>(rec + 0x10) = static_cast<uint16_t>(ic.h);
        uv[0] = 0.0f, uv[1] = 0.0f;
        uv[2] = static_cast<float>(ic.w) / ic.texW, uv[3] = static_cast<float>(ic.h) / ic.texH;
        ++loaded;
    }
    // The old arrays stay allocated (a few KB); the exit clean-up frees ours.
    *reinterpret_cast<uint8_t**>(sheet + 4) = records;
    *reinterpret_cast<uint8_t**>(sheet + 8) = uvs;
    *reinterpret_cast<void***>(sheet + 0xC) = pageArray;
    g_sheet = sheet;
    g_records = records;
    dslog::Write("Weapon pictures: %d of %u added to the interface sheet (images %u..%u, pages %u..%u)", loaded,
                 static_cast<unsigned>(g_icons.size()), kIconBase, total - 1, pageCount, pages - 1);
}

#ifdef DS2_PSE_HOST
int PSE_CALL OnFrame(PSEEventId, void*, void*) {
    Extend();
    return 0;
}
#endif
}  // namespace

int features::AddWeaponIcon(const std::string& path, const std::string& basePicture, const std::string& file) {
    if (!g_hooked) return -1;
    for (size_t n = 0; n < g_icons.size(); ++n)   // the MODS menu's Apply builds the tables again: same file, same number
        if (_stricmp(g_icons[n]->path.c_str(), path.c_str()) == 0) return static_cast<int>(n);
    auto* ic = new Icon;
    ic->path = path;
    ic->base = basePicture;
    ic->file = file;
    g_icons.push_back(ic);
    g_sheet = nullptr;   // extend again (a picture added by Apply - drawn once its weapon row is read, i.e. a restart)
    return static_cast<int>(g_icons.size()) - 1;
}

bool features::WeaponPictureKnown(const std::string& name) { return GameImage(name.c_str()) != 0xFFFF; }

void features::ApplyWeaponIcons() {
#ifdef DS2_PSE_HOST
    if (g_hooked) return;   // installed even without pictures: a mod switched on later can bring some
    if (!features::ModsFolderEnabled()) {
        dslog::Write("[off]  Weapon pictures: off (FeatMods)");
        return;
    }
    if (!patch::Matches(kLookup, kLookupEntry, sizeof kLookupEntry)) {
        dslog::Write("[fail] Weapon pictures: picture lookup not recognised - mod weapons keep their base picture");
        return;
    }
    const PSEMessagingInterface* messaging = pse::MessagingInterface();
    const PSEPluginHandle self = pse_host::Find("ds2fix.weaponicons");
    if (!messaging || !messaging->Subscribe(self, PSE_EVENT_FRAME, &OnFrame, nullptr, 0)) {
        dslog::Write("[fail] Weapon pictures: no frame event - mod weapons keep their base picture");
        return;
    }
    if (!patch::HookFunction(kLookup, reinterpret_cast<const void*>(&LookupStub), &g_next, kLookupEntry,
                             sizeof kLookupEntry)) {
        dslog::Write("[fail] Weapon pictures: picture lookup not hooked - mod weapons keep their base picture");
        return;
    }
    g_hooked = true;
    dslog::Write("[ok]   Weapon pictures: lookup 0x%08X hooked", kLookup);
#else
    dslog::Write("[off]  Weapon pictures: PSE build only (ds2fix.weaponicons)");
#endif
}
