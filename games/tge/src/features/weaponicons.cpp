// Weapon pictures for mod weapons (tgefix.weaponicons): port of DesertStorm2Fix's weaponicons.cpp (games/ds2/src/
// features/weaponicons.cpp; DS2 addresses in brackets). A .weapon's "hud icon = MyGun" (MyGun.png / .dds / .tga in the
// mod folder) is drawn wherever the game draws that weapon's picture (inventory, held-item HUD).
//
// How GreatEscape.exe pictures a weapon (mapped 2026-10-25):
//  - Weaps.txt column 3 ("IMAGE_INV_MG42") -> the picture lookup FUN_00594150 [FUN_00452170]: a chain of string
//    compares, name in eax, plain ret, number in ax (29 of them jump to shared tails); an unknown name prints a warning
//    (stubbed) and answers 0xFFFF. The number is a GE_Items.img image index (STENGUN 0x5A, MP40 0x41, MG42 0x3E) and is
//    stored at the weapon definition +0x5C; the item picture getter 0x585E30 hands it to the GUI.
//  - There is no global sheet: GE_Items.img (catalog.dat) is loaded by every .gui that shows items, through CGUIImage
//    (0x4C39D0) into a CSpriteRender (0x50 bytes, DS2's layout: +0 header 0x11C bytes (+6 image count u16, +0xA page
//    count u16), +4 records 0x1C bytes (+0 page u16, +4 owned pointer, +0xA x, +0xC y, +0xE w, +0x10 h), +8 UVs 16
//    bytes, +0xC page objects (+0xC palette, -1 = none; +0x10 IDirect3DTexture8*)) read by FUN_0044a430 thiscall(name,
//    4 more) ret 0x14 [FUN_0052ad20]; arrays from the game allocator FUN_004cf8c0(size, 0x18); FUN_0044a340 frees the
//    first <count> records' owned pointers, the arrays and never the page objects. The 13 image draws bind
//    pages[record.page] +0x10 with no bound on the image number.
//
// Here: the lookup's entry is detoured: "TGEFIX_ICON_<n>" (written into the Weaps row by mods_tables.cpp) answers
// kIconBase + n. The reader is detoured: after it read a GE_Items.img, the sprite's record / UV / page arrays are
// copied into larger ones (the game's allocator, so its free takes them): our images are the base weapon's record sized
// to the picture with UVs over it, on a page of their own - a small page object whose texture is the picture. The header
// counts stay the game's.
#include <windows.h>
#include <wincodec.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kLookup = 0x594150;
constexpr uint8_t kLookupEntry[] = {0x56, 0x57, 0xBF, 0x18, 0x21, 0x61, 0x00};  // push esi; push edi; mov edi, 0x612118
constexpr uint32_t kReader = 0x44A430;
constexpr uint8_t kReaderEntry[] = {0x81, 0xEC, 0x04, 0x05, 0x00, 0x00};        // sub esp, 0x504
constexpr uint32_t kAlloc = 0x4CF8C0;     // cdecl (size, tag)
constexpr uint32_t kRenderer = 0x79ED20;  // +0x40530 IDirect3DDevice8*
constexpr uint32_t kIconBase = 1000;      // our image numbers (GE_Items has 108 images)
constexpr uint32_t kRecord = 0x1C, kUv = 0x10;
constexpr char kPrefix[] = "TGEFIX_ICON_";

struct Icon {
    std::string path, base, file;   // picture file; the base weapon's picture name (Weaps col 3); the .weapon (report)
    void* texture = nullptr;        // IDirect3DTexture8*
    bool tried = false;
    UINT w = 0, h = 0, texW = 0, texH = 0;
    uint8_t page[0x40] = {};         // our page object: +0xC palette -1, +0x10 the texture
};
std::vector<Icon*> g_icons;          // never freed: sprites' page arrays point into them
bool g_hooked = false;
void* g_lookupNext = nullptr;        // the lookup's trampoline
using ReaderFn = int(__thiscall*)(void*, const char*, int, int, int, int);
ReaderFn g_readerNext = nullptr;

// Our number for a TGEFIX_ICON_<n> name, else ~0 (the game's lookup answers).
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
        jmp dword ptr [g_lookupNext]
    }
}

// The game's number for a picture name (its own lookup, through the trampoline); 0xFFFF = unknown.
uint32_t GameImage(const char* name) {
    if (!name || !*name || !g_lookupNext) return 0xFFFF;
    uint32_t result = 0xFFFF;
    __asm {
        mov eax, name
        call dword ptr [g_lookupNext]
        movzx eax, ax
        mov result, eax
    }
    return result;
}

void** Vt(void* com) { return *reinterpret_cast<void***>(com); }

// An encoded picture (PNG, DDS DXT1-5, TGA ... what WIC reads) as a managed A8R8G8B8 texture, sizes rounded up to
// powers of two, the picture at the top-left.
void* LoadPicture(const char* path, UINT& w, UINT& h, UINT& texW, UINT& texH) {
    w = h = texW = texH = 0;
    auto* renderer = *reinterpret_cast<uint8_t**>(kRenderer);
    void* dev = renderer ? *reinterpret_cast<void**>(renderer + 0x40530) : nullptr;
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
    void* tex = nullptr;   // IDirect3DDevice8::CreateTexture (20), managed A8R8G8B8
    using CreateTex = HRESULT(__stdcall*)(void*, UINT, UINT, UINT, DWORD, UINT, UINT, void**);
    if (FAILED(reinterpret_cast<CreateTex>(Vt(dev)[20])(dev, texW, texH, 1, 0, 21 /* A8R8G8B8 */, 1 /* MANAGED */, &tex)) || !tex)
        return nullptr;
    struct { INT pitch; void* bits; } lr{};
    using LockRect = HRESULT(__stdcall*)(void*, UINT, void*, const RECT*, DWORD);
    using UnlockRect = HRESULT(__stdcall*)(void*, UINT);
    if (SUCCEEDED(reinterpret_cast<LockRect>(Vt(tex)[16])(tex, 0, &lr, nullptr, 0))) {
        for (UINT y = 0; y < texH; ++y) {
            auto* row = static_cast<uint8_t*>(lr.bits) + size_t(y) * lr.pitch;
            memset(row, 0, size_t(texW) * 4);
            if (y < h) memcpy(row, &pixels[size_t(y) * w * 4], size_t(w) * 4);
        }
        reinterpret_cast<UnlockRect>(Vt(tex)[17])(tex, 0);
    }
    return tex;
}

// Adds our images to a sprite that just read GE_Items.img.
void Extend(uint8_t* sprite) {
    if (!sprite || g_icons.empty()) return;
    const uint8_t* header = *reinterpret_cast<uint8_t**>(sprite);
    const uint8_t* oldRecords = *reinterpret_cast<uint8_t**>(sprite + 4);
    const uint8_t* oldUvs = *reinterpret_cast<uint8_t**>(sprite + 8);
    void* const* oldPages = *reinterpret_cast<void* const* const*>(sprite + 0xC);
    if (!header || !oldRecords || !oldUvs || !oldPages) return;
    const uint32_t count = *reinterpret_cast<const uint16_t*>(header + 6);
    const uint32_t pageCount = *reinterpret_cast<const uint16_t*>(header + 0xA);
    if (!count || count > kIconBase || !pageCount) return;
    using Alloc = void*(__cdecl*)(uint32_t, uint32_t);
    const uint32_t total = kIconBase + static_cast<uint32_t>(g_icons.size());
    const uint32_t pages = pageCount + static_cast<uint32_t>(g_icons.size());
    auto* records = static_cast<uint8_t*>(reinterpret_cast<Alloc>(kAlloc)(total * kRecord, 0x18));
    auto* uvs = static_cast<uint8_t*>(reinterpret_cast<Alloc>(kAlloc)(total * kUv, 0x18));
    auto* pageArray = static_cast<void**>(reinterpret_cast<Alloc>(kAlloc)(pages * 4, 0x18));
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
        memcpy(rec, oldRecords + base * kRecord, kRecord);   // the base weapon's picture (should the file fail)
        *reinterpret_cast<uint32_t*>(rec + 4) = 0;
        memcpy(uv, oldUvs + base * kUv, kUv);
        pageArray[pageCount + n] = oldPages[*reinterpret_cast<uint16_t*>(rec) < pageCount ? *reinterpret_cast<uint16_t*>(rec) : 0];
        if (!ic.tried) {
            ic.tried = true;
            ic.texture = LoadPicture(ic.path.c_str(), ic.w, ic.h, ic.texW, ic.texH);
            if (!ic.texture)
                features::ModLog("[fail] Mods: %s - picture %s not loaded (PNG / DDS / TGA up to 1024 x 1024) - the %s "
                                 "picture is used", ic.file.c_str(), ic.path.c_str(), ic.base.c_str());
        }
        if (!ic.texture) continue;
        *reinterpret_cast<int32_t*>(ic.page + 0xC) = -1;
        *reinterpret_cast<void**>(ic.page + 0x10) = ic.texture;
        pageArray[pageCount + n] = ic.page;
        *reinterpret_cast<uint16_t*>(rec) = static_cast<uint16_t>(pageCount + n);
        *reinterpret_cast<uint16_t*>(rec + 0xA) = 0, *reinterpret_cast<uint16_t*>(rec + 0xC) = 0;
        *reinterpret_cast<uint16_t*>(rec + 0xE) = static_cast<uint16_t>(ic.w);   // 1 picture pixel = 1 sheet pixel
        *reinterpret_cast<uint16_t*>(rec + 0x10) = static_cast<uint16_t>(ic.h);
        uv[0] = 0.0f, uv[1] = 0.0f;
        uv[2] = static_cast<float>(ic.w) / ic.texW, uv[3] = static_cast<float>(ic.h) / ic.texH;
        ++loaded;
    }
    *reinterpret_cast<uint8_t**>(sprite + 4) = records;   // the old arrays stay allocated (a few KB)
    *reinterpret_cast<uint8_t**>(sprite + 8) = uvs;
    *reinterpret_cast<void***>(sprite + 0xC) = pageArray;
    dslog::Write("Weapon pictures: %d of %u added to a GE_Items sheet (images %u..%u)", loaded,
                 static_cast<unsigned>(g_icons.size()), kIconBase, total - 1);
}

int __fastcall Reader(uint8_t* sprite, void*, const char* name, int a, int b, int c, int d) {
    const int r = g_readerNext(sprite, name, a, b, c, d);
    if (!name) return r;
    const char* base = strrchr(name, '\\');
    if (_stricmp(base ? base + 1 : name, "GE_Items.img") == 0) Extend(sprite);
    return r;
}
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
    return static_cast<int>(g_icons.size()) - 1;
}

bool features::WeaponPictureKnown(const std::string& name) { return GameImage(name.c_str()) != 0xFFFF; }

void features::ApplyWeaponIcons() {
    if (g_hooked || !Get().mods) return;
    if (!patch::HookFunction(kLookup, reinterpret_cast<const void*>(&LookupStub), &g_lookupNext, kLookupEntry,
                             sizeof kLookupEntry) ||
        !patch::HookFunction(kReader, reinterpret_cast<const void*>(&Reader), reinterpret_cast<void**>(&g_readerNext),
                             kReaderEntry, sizeof kReaderEntry)) {
        dslog::Write("[fail] Weapon pictures: picture lookup / sheet reader not hooked - mod weapons keep their base picture");
        return;
    }
    g_hooked = true;
    dslog::Write("[ok]   Weapon pictures: lookup 0x%08X, sheet reader 0x%08X hooked", kLookup, kReader);
}
