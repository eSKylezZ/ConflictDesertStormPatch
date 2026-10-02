// In-game button icons. Textures are created once per device from the RCDATA resource game_icons.bin (A8R8G8B8,
// managed pool, so they survive a device Reset) and drawn as pre-transformed quads (FVF 0x144, like the game's own 2D) right
// before EndScene. Every state we touch is read back first and restored afterwards, including the viewport (split
// screen leaves the last player's view set) - the game sets its own states per draw, but nothing is left changed.
// Menu transitions: the game zooms and fades its 2D quads through FUN_005470a0 while renderer +0x40a70 is set
// (flags +0x40a74: 1 scale +0x40a78 about the centre +0x4066c/+0x40670, 2 rotation, 4..0x20 colour factors
// +0x40a80..+0x40a8c). QueueIcon applies the same transform, captured when the menu is drawn, to the icon quad.
//
// Icons in text: both fonts (small [0x60EDB4], large [0x60EDB8]) use one image sheet (font +0; records +4, 24 bytes:
// w +0xe, h +0x10, draw offset +0x12/+0x14; UVs +8, 16 bytes; count = header [+0] +6) and a char -> glyph table
// (font +0x20, 256 x u16; glyph range font +4..+8, checked by the char width FUN_0053a390). The sheet's arrays are
// copied into larger ones (game allocator, so the game can still free them) with 27 extra glyphs per font, blank
// copies of the space glyph sized to the icon and centred on the capitals; control characters 1-31 (not tab,
// newline, return or 25, which the fonts use) map to them. Every text routine then measures and wraps them like
// letters, and the image draw FUN_0053c9e0 (all glyphs go through it) is hooked: our glyphs queue the icon in the
// text's colour alpha instead of drawing. Characters are handed out per icon on demand (IconChar).
#include "features/overlay.h"

#include <windows.h>

#include <wincodec.h>
#undef small  // rpcndr.h (via wincodec.h) defines it as char

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
// Direct3D 8 vtable indices / values (no d3d8.h in the SDK).
enum Device : int {
    kCreateTexture = 20, kEndScene = 35, kSetViewport = 40, kGetViewport = 41, kSetRenderState = 50,
    kGetRenderState = 51, kGetTexture = 60, kSetTexture = 61, kGetTss = 62, kSetTss = 63, kDrawPrimitiveUP = 72,
    kSetVertexShader = 76, kGetVertexShader = 77, kSetPixelShader = 88, kGetPixelShader = 89,
};
enum Texture : int { kRelease = 2, kLockRect = 16, kUnlockRect = 17 };
constexpr uint32_t kFvf = 0x144;  // XYZRHW | DIFFUSE | TEX1
constexpr uint32_t kFormatArgb = 21, kPoolManaged = 1, kTriangleStrip = 5;
constexpr uint32_t kRenderer = 0x63C924;  // W +0x40688, H +0x4068c

struct Vertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};
struct Queued {
    int icon;       // overlay::Icon, or -1: `texture` (nullptr = solid colour)
    void* texture;
    Vertex quad[4];  // triangle strip, already transformed
};
std::vector<Queued> g_queue;
void* g_device = nullptr;
void* g_textures[static_cast<int>(overlay::Icon::Count)] = {};

void** Vt(void* obj) { return *static_cast<void***>(obj); }
template <class Fn>
Fn M(void* obj, int i) {
    return reinterpret_cast<Fn>(Vt(obj)[i]);
}

// Pixels of the icons: the RCDATA resource in this DLL.
const uint32_t* IconPixels() {
    static const uint32_t* pixels = nullptr;
    if (pixels) return pixels;
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&IconPixels), &self);
    HRSRC res = FindResourceA(self, MAKEINTRESOURCEA(gen::kGameIconsResource), MAKEINTRESOURCEA(10) /*RT_RCDATA*/);
    HGLOBAL data = res ? LoadResource(self, res) : nullptr;
    constexpr size_t need = size_t(gen::kGameIconSize) * gen::kGameIconSize * 4 * size_t(overlay::Icon::Count);
    if (!data || SizeofResource(self, res) < need) {
        dslog::Write("[fail] Overlay: icon resource missing");
        return nullptr;
    }
    pixels = static_cast<const uint32_t*>(LockResource(data));
    return pixels;
}

bool CreateTextures(void* dev) {
    const uint32_t* pixels = IconPixels();
    if (!pixels) return false;
    using Create = HRESULT(__stdcall*)(void*, UINT, UINT, UINT, DWORD, uint32_t, uint32_t, void**);
    struct Locked {
        INT pitch;
        void* bits;
    };
    using Lock = HRESULT(__stdcall*)(void*, UINT, Locked*, const RECT*, DWORD);
    using Unlock = HRESULT(__stdcall*)(void*, UINT);
    constexpr int n = gen::kGameIconSize;
    for (int i = 0; i < static_cast<int>(overlay::Icon::Count); ++i) {
        void* tex = nullptr;
        if (FAILED(M<Create>(dev, kCreateTexture)(dev, n, n, 1, 0, kFormatArgb, kPoolManaged, &tex)) || !tex) {
            dslog::Write("[fail] Overlay: icon texture %d not created", i);
            return false;
        }
        Locked lr{};
        if (SUCCEEDED(M<Lock>(tex, kLockRect)(tex, 0, &lr, nullptr, 0))) {
            for (int y = 0; y < n; ++y)
                memcpy(static_cast<uint8_t*>(lr.bits) + y * lr.pitch, pixels + (size_t(i) * n + y) * n, n * 4);
            M<Unlock>(tex, kUnlockRect)(tex, 0);
        }
        g_textures[i] = tex;
    }
    dslog::Write("Overlay: %d icon textures created", static_cast<int>(overlay::Icon::Count));
    return true;
}

// ---- icon glyphs ----
constexpr uint32_t kSmallFont = 0x60EDB4, kLargeFont = 0x60EDB8;
constexpr uint32_t kImageDraw = 0x53C9E0, kImageDrawCont = 0x53C9E6;  // entry: sub esp, 0x94 (6 bytes)
constexpr uint32_t kAllocArray = 0x4B8450;  // cdecl (size, tag) - what the sheet loader uses for these arrays
constexpr uint8_t kCodes[] = {1, 2, 3, 4, 5, 6, 7, 8, 11, 12, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 26, 27, 28,
                              29, 30, 31};
constexpr int kSlots = static_cast<int>(sizeof kCodes);
// Icon glyph per font, in font-sheet pixels (scaled with the sheet): side, gap after, top offset from the baseline.
struct GlyphSize {
    int16_t side, advance, top;
};
constexpr GlyphSize kSmallGlyph{16, 18, -13}, kLargeGlyph{28, 31, -23};  // capitals: small -9..0, large -18..0

overlay::Icon g_slotIcon[kSlots];
bool g_slotUsed[kSlots];
int g_nextSlot = 0;
uint8_t* g_sheet = nullptr;       // the fonts' sheet the glyphs were added to
uint8_t* g_records = nullptr;     // our (larger) record array, to notice the game replacing it
uint32_t g_glyphBase = 0xFFFFFFFF;  // first icon glyph: small font base, large font base + kSlots
static_assert(sizeof kCodes == 27, "ImageDrawStub compares against 2 * 27");

uint8_t* Font(uint32_t global) { return *reinterpret_cast<uint8_t**>(global); }

// Adds the icon glyphs to the fonts' sheet and maps the control characters to them. Idempotent.
void EnsureGlyphs() {
    uint8_t* small = Font(kSmallFont);
    uint8_t* large = Font(kLargeFont);
    if (!small || !large) return;
    uint8_t* sheet = *reinterpret_cast<uint8_t**>(small);
    if (!sheet || *reinterpret_cast<uint8_t**>(large) != sheet) return;
    if (sheet == g_sheet && *reinterpret_cast<uint8_t**>(sheet + 4) == g_records) return;  // still in place

    const uint8_t* header = *reinterpret_cast<uint8_t**>(sheet);
    const uint32_t count = *reinterpret_cast<const uint16_t*>(header + 6);
    const uint8_t* oldRecords = *reinterpret_cast<uint8_t**>(sheet + 4);
    const uint8_t* oldUvs = *reinterpret_cast<uint8_t**>(sheet + 8);
    const uint16_t* smallTable = *reinterpret_cast<uint16_t**>(small + 0x20);
    const uint16_t space = smallTable[' '];
    if (!header || !oldRecords || !oldUvs || !count || space >= count) return;

    using Alloc = void*(__cdecl*)(uint32_t, uint32_t);
    const uint32_t total = count + 2 * kSlots;
    auto* records = static_cast<uint8_t*>(reinterpret_cast<Alloc>(kAllocArray)(total * 24, 0x17));
    auto* uvs = static_cast<uint8_t*>(reinterpret_cast<Alloc>(kAllocArray)(total * 16, 0x17));
    if (!records || !uvs) return;
    memcpy(records, oldRecords, count * 24);
    memcpy(uvs, oldUvs, count * 16);
    for (int f = 0; f < 2; ++f) {
        const GlyphSize& g = f ? kLargeGlyph : kSmallGlyph;
        for (int k = 0; k < kSlots; ++k) {
            const uint32_t i = count + f * kSlots + k;
            memcpy(records + i * 24, oldRecords + space * 24, 24);  // blank space glyph ...
            memcpy(uvs + i * 16, oldUvs + space * 16, 16);
            *reinterpret_cast<uint16_t*>(records + i * 24 + 0xE) = static_cast<uint16_t>(g.advance);  // ... icon-sized
            *reinterpret_cast<uint16_t*>(records + i * 24 + 0x10) = static_cast<uint16_t>(g.side);
            *reinterpret_cast<int16_t*>(records + i * 24 + 0x12) = 0;
            *reinterpret_cast<int16_t*>(records + i * 24 + 0x14) = g.top;
        }
    }
    *reinterpret_cast<uint8_t**>(sheet + 4) = records;  // the old arrays stay allocated (a few KB, once)
    *reinterpret_cast<uint8_t**>(sheet + 8) = uvs;
    for (int f = 0; f < 2; ++f) {
        uint8_t* font = f ? large : small;
        auto* table = *reinterpret_cast<uint16_t**>(font + 0x20);
        const uint16_t size = *reinterpret_cast<uint16_t*>(font + 0x24);
        for (int k = 0; k < kSlots; ++k)
            if (kCodes[k] < size) table[kCodes[k]] = static_cast<uint16_t>(count + f * kSlots + k);
        auto& max = *reinterpret_cast<uint32_t*>(font + 8);
        if (max < total - 1) max = total - 1;
    }
    g_sheet = sheet;
    g_records = records;
    g_glyphBase = count;
    dslog::Write("Overlay: %d icon glyphs per font added to the font sheet (glyphs %u..%u)", kSlots, count, total - 1);
}

// Image draw of one of our glyphs (ecx = sheet): the icon instead, where the glyph would be. x, y as passed to
// FUN_0053c9e0 (glyph position; the record offsets are added scaled, like the game does).
void __cdecl DrawIconGlyph(const uint8_t* sheet, uint32_t image, int x, int y) {
    const uint32_t k = image - g_glyphBase;
    const bool largeFont = k >= static_cast<uint32_t>(kSlots);
    const int slot = static_cast<int>(largeFont ? k - kSlots : k);
    if (slot >= kSlots || !g_slotUsed[slot]) return;
    const GlyphSize& g = largeFont ? kLargeGlyph : kSmallGlyph;
    const bool scaled = *reinterpret_cast<const uint32_t*>(sheet + 0x20) & 1;
    const float sx = scaled ? *reinterpret_cast<const float*>(sheet + 0x24) : 1.0f;
    const float sy = scaled ? *reinterpret_cast<const float*>(sheet + 0x28) : 1.0f;
    const uint32_t color = *reinterpret_cast<const uint32_t*>(sheet + 0x3C);  // text colour set by FUN_0053bd40
    const float alpha = (color >> 24) / 255.0f * *reinterpret_cast<const float*>(sheet + 0x38);
    overlay::QueueIcon(g_slotIcon[slot], x + (g.advance - g.side) * 0.5f * sx, y + g.top * sy, g.side * sy, alpha);
}

const void* g_drawSheet = nullptr;  // the image being drawn (for the page texture bind inside the draw)
int g_drawImage = -1;
void* g_imageProbe = nullptr;  // dev: stdcall(caller, sheet, image, x, y) for every image draw (splitscreen.cpp probe)

__declspec(naked) void ImageDrawStub() {
    __asm {
        mov eax, [esp + 4]          // image
        sub eax, g_glyphBase
        cmp eax, 54                 // 2 * kSlots; unsigned: below the base wraps around -> original
        jae original
        cmp ecx, g_sheet
        jne original
        push dword ptr [esp + 12]   // y
        push dword ptr [esp + 12]   // x
        push dword ptr [esp + 12]   // image
        push ecx
        call DrawIconGlyph
        add esp, 16
        ret 12
    original:
        cmp g_imageProbe, 0
        je no_probe
        pushad
        push dword ptr [esp + 44]   // y      (args start at esp+36 after pushad + return address)
        push dword ptr [esp + 44]   // x
        push dword ptr [esp + 44]   // image
        push ecx                    // sheet
        push dword ptr [esp + 48]   // caller
        call g_imageProbe
        popad
    no_probe:
        mov eax, [esp + 4]
        mov g_drawImage, eax
        mov g_drawSheet, ecx
        sub esp, 0x94
        push kImageDrawCont
        ret
    }
}
}  // namespace

void overlay::SetImageProbe(void* probe) { g_imageProbe = probe; }

bool overlay::Install() {
    static const uint8_t entry[] = {0x81, 0xEC, 0x94, 0x00, 0x00, 0x00};
    static bool done = false;
    if (done) return true;
    if (!patch::Matches(kImageDraw, entry, sizeof entry)) {
        dslog::Write("[fail] Overlay: unexpected image draw code - icons in text disabled");
        return false;
    }
    const uint8_t nop = 0x90;
    done = patch::WriteJump(kImageDraw, reinterpret_cast<const void*>(&ImageDrawStub)) &&
           patch::Write(kImageDraw + 5, &nop, 1);
    return done;
}

void overlay::OnFrame() {
    if (g_sheet || Font(kSmallFont)) EnsureGlyphs();
}

bool overlay::GlyphsReady() { return g_sheet && g_glyphBase != 0xFFFFFFFF; }

const void* overlay::DrawingSheet() { return g_drawSheet; }
int overlay::DrawingImage() { return g_drawImage; }

// Least recently requested character gets reused (prompts ask every frame, so what is on screen stays fresh).
const char* overlay::IconChar(Icon icon) {
    static char text[kSlots][2];
    static uint32_t lastUse[kSlots];
    static uint32_t clock = 0;
    ++clock;
    int k = -1;
    for (int i = 0; i < kSlots; ++i)
        if (g_slotUsed[i] && g_slotIcon[i] == icon) k = i;
    if (k < 0) {
        k = 0;
        for (int i = 1; i < kSlots; ++i)
            if (lastUse[i] < lastUse[k]) k = i;
        g_slotIcon[k] = icon;
        g_slotUsed[k] = true;
        text[k][0] = static_cast<char>(kCodes[k]);
        text[k][1] = 0;
    }
    lastUse[k] = clock;
    return text[k];
}

float overlay::MenuAlpha() {
    const auto* sheet = *reinterpret_cast<const uint8_t* const*>(0x60EE18);
    if (!sheet) return 1.0f;
    const float a = *reinterpret_cast<const float*>(sheet + 0x38);  // tint multipliers r g b a at +0x2c..+0x38
    return a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
}

namespace {
// The game's 2D transform of this moment (menu transitions, FUN_005470a0) and the split-screen HUD offset, applied to
// a quad given by its four corners (top-left, top-right, bottom-left, bottom-right) and queued.
void Queue(int icon, void* texture, const float (&corner)[4][2], const float (&uv)[4][2], float alpha, DWORD rgb) {
    if (alpha < 0.0f) alpha = overlay::MenuAlpha();
    float scale = 1.0f, cx = 0.0f, cy = 0.0f, mult[4] = {1, 1, 1, 1};
    if (auto* r = *reinterpret_cast<uint8_t**>(kRenderer); r && *reinterpret_cast<uint32_t*>(r + 0x40A70)) {
        const uint32_t flags = *reinterpret_cast<uint32_t*>(r + 0x40A74);
        if (flags & 1) {
            scale = *reinterpret_cast<float*>(r + 0x40A78);
            cx = *reinterpret_cast<float*>(r + 0x4066C);
            cy = *reinterpret_cast<float*>(r + 0x40670);
        }
        if (flags & 0x3C)
            for (int i = 0; i < 4; ++i) mult[i] = *reinterpret_cast<float*>(r + 0x40A80 + i * 4);
    }
    alpha *= mult[3];
    if (alpha <= 0.0f) return;
    // Split screen: a view's HUD is drawn in view-local coordinates and moved to the view (splitscreen.cpp).
    float vx = 0, vy = 0;
    features::SplitHudOffset(vx, vy);
    auto channel = [](float v) { return static_cast<DWORD>((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0f + 0.5f); };
    const float rgbf[3] = {((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f};
    const DWORD color = channel(alpha) << 24 | channel(rgbf[0] * mult[0]) << 16 | channel(rgbf[1] * mult[1]) << 8 |
                        channel(rgbf[2] * mult[2]);
    Queued q{icon, texture, {}};
    for (int i = 0; i < 4; ++i)
        q.quad[i] = {(corner[i][0] + vx - cx) * scale + cx - 0.5f, (corner[i][1] + vy - cy) * scale + cy - 0.5f, 0, 1,
                     color, uv[i][0], uv[i][1]};
    g_queue.push_back(q);
}

void QueueRectangle(int icon, void* texture, float x, float y, float w, float h, float u1, float v1, float alpha,
                    DWORD rgb) {
    const float corner[4][2] = {{x, y}, {x + w, y}, {x, y + h}, {x + w, y + h}};
    const float uv[4][2] = {{0, 0}, {u1, 0}, {0, v1}, {u1, v1}};
    Queue(icon, texture, corner, uv, alpha, rgb);
}
}  // namespace

void overlay::QueueIcon(Icon icon, float x, float y, float size, float alpha) {
    QueueRectangle(static_cast<int>(icon), nullptr, x, y, size, size, 1, 1, alpha, 0xFFFFFF);
}

void overlay::QueueImage(void* texture, float x, float y, float w, float h, float u1, float v1, float alpha) {
    if (texture) QueueRectangle(-1, texture, x, y, w, h, u1, v1, alpha, 0xFFFFFF);
}

void overlay::QueueScreenImage(void* texture, float x, float y, float w, float h, float u1, float v1) {
    if (!texture) return;
    constexpr DWORD white = 0xFFFFFFFF;
    x -= 0.5f, y -= 0.5f;
    g_queue.push_back({-1, texture, {{x, y, 0, 1, white, 0, 0}, {x + w, y, 0, 1, white, u1, 0},
                                     {x, y + h, 0, 1, white, 0, v1}, {x + w, y + h, 0, 1, white, u1, v1}}});
}

void overlay::QueueScreenRect(float x, float y, float w, float h, uint32_t argb) {
    x -= 0.5f, y -= 0.5f;
    g_queue.push_back({-1, nullptr, {{x, y, 0, 1, argb, 0, 0}, {x + w, y, 0, 1, argb, 1, 0},
                                     {x, y + h, 0, 1, argb, 0, 1}, {x + w, y + h, 0, 1, argb, 1, 1}}});
}

void overlay::QueueRect(float x, float y, float w, float h, uint32_t argb) {
    QueueRectangle(-1, nullptr, x, y, w, h, 0, 0, ((argb >> 24) / 255.0f) * MenuAlpha(), argb & 0xFFFFFF);
}

void overlay::QueueLine(float x0, float y0, float x1, float y1, float width, uint32_t argb) {
    const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.01f) return;
    const float nx = -dy / len * width / 2, ny = dx / len * width / 2;
    const float corner[4][2] = {{x0 + nx, y0 + ny}, {x1 + nx, y1 + ny}, {x0 - nx, y0 - ny}, {x1 - nx, y1 - ny}};
    const float uv[4][2] = {};
    Queue(-1, nullptr, corner, uv, ((argb >> 24) / 255.0f) * MenuAlpha(), argb & 0xFFFFFF);
}

namespace {
// An encoded image (PNG, DDS DXT1-5 ... whatever WIC reads) as a managed A8R8G8B8 texture of texW x texH with the
// image at the top-left; texW / texH 0 = the image size rounded up to powers of two. w / h = the image size.
void* TextureFromImage(const void* bytes, size_t size, int texW, int texH, UINT& w, UINT& h, const char* what) {
    void* dev = *reinterpret_cast<void**>(0x755050);
    w = h = 0;
    if (!dev || !bytes || !size) return nullptr;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<uint8_t> pixels;
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(bytes)), static_cast<DWORD>(size))) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                        WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(converter->GetSize(&w, &h))) {
        if (!texW) for (texW = 1; texW < static_cast<int>(w); texW *= 2) {}
        if (!texH) for (texH = 1; texH < static_cast<int>(h); texH *= 2) {}
        if (w <= static_cast<UINT>(texW) && h <= static_cast<UINT>(texH)) {
            pixels.resize(size_t(w) * h * 4);
            if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data())))
                pixels.clear();
        }
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{converter, frame, decoder, stream, factory})
        if (u) u->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    if (pixels.empty()) {
        dslog::Write("[fail] Overlay: image %s not decoded", what);
        return nullptr;
    }
    using Create = HRESULT(__stdcall*)(void*, UINT, UINT, UINT, DWORD, uint32_t, uint32_t, void**);
    struct Locked {
        INT pitch;
        void* bits;
    };
    using Lock = HRESULT(__stdcall*)(void*, UINT, Locked*, const RECT*, DWORD);
    using Unlock = HRESULT(__stdcall*)(void*, UINT);
    void* tex = nullptr;
    if (FAILED(M<Create>(dev, kCreateTexture)(dev, texW, texH, 1, 0, kFormatArgb, kPoolManaged, &tex)) || !tex) return nullptr;
    Locked lr{};
    if (SUCCEEDED(M<Lock>(tex, kLockRect)(tex, 0, &lr, nullptr, 0))) {
        for (int y = 0; y < texH; ++y) {
            auto* row = static_cast<uint8_t*>(lr.bits) + y * lr.pitch;
            memset(row, 0, size_t(texW) * 4);
            if (y < static_cast<int>(h)) memcpy(row, &pixels[size_t(y) * w * 4], size_t(w) * 4);
        }
        M<Unlock>(tex, kUnlockRect)(tex, 0);
    }
    dslog::Write("Overlay: image %s (%ux%u) loaded", what, w, h);
    return tex;
}
}  // namespace

// A PNG from this DLL's RCDATA as a managed A8R8G8B8 texture of texW x texH (image at the top-left), via WIC.
void* overlay::LoadTexture(int resource, int texW, int texH) {
    struct Cached {
        int resource;
        void* device;
        void* texture;
    };
    static std::vector<Cached> cache;
    void* dev = *reinterpret_cast<void**>(0x755050);
    if (!dev) return nullptr;
    for (const Cached& c : cache)
        if (c.resource == resource && c.device == dev) return c.texture;
    cache.push_back({resource, dev, nullptr});  // failures are not retried

    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&IconPixels), &self);
    HRSRC res = FindResourceA(self, MAKEINTRESOURCEA(resource), MAKEINTRESOURCEA(10) /*RT_RCDATA*/);
    HGLOBAL data = res ? LoadResource(self, res) : nullptr;
    if (!data) return nullptr;
    char what[16];
    snprintf(what, sizeof what, "%d", resource);
    UINT w = 0, h = 0;
    cache.back().texture = TextureFromImage(LockResource(data), SizeofResource(self, res), texW, texH, w, h, what);
    return cache.back().texture;
}

void* overlay::LoadTextureFile(const char* path, int& w, int& h, int& texW, int& texH) {
    w = h = texW = texH = 0;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    std::vector<uint8_t> data(GetFileSize(f, nullptr));
    DWORD got = 0;
    const BOOL ok = ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr);
    CloseHandle(f);
    if (!ok || got != data.size()) return nullptr;
    UINT iw = 0, ih = 0;
    void* tex = TextureFromImage(data.data(), data.size(), 0, 0, iw, ih, path);
    if (!tex) return nullptr;
    w = static_cast<int>(iw), h = static_cast<int>(ih);
    for (texW = 1; texW < w; texW *= 2) {}
    for (texH = 1; texH < h; texH *= 2) {}
    return tex;
}

void* overlay::CurrentDevice() { return *reinterpret_cast<void**>(0x755050); }

void* overlay::CreateTexture(int texW, int texH) {
    using Create = HRESULT(__stdcall*)(void*, UINT, UINT, UINT, DWORD, uint32_t, uint32_t, void**);
    void* dev = CurrentDevice();
    void* tex = nullptr;
    if (!dev || FAILED(M<Create>(dev, kCreateTexture)(dev, texW, texH, 1, 0, kFormatArgb, kPoolManaged, &tex))) return nullptr;
    return tex;
}

bool overlay::UploadTexture(void* texture, const uint32_t* pixels, int w, int h) {
    struct Locked {
        INT pitch;
        void* bits;
    };
    using Lock = HRESULT(__stdcall*)(void*, UINT, Locked*, const RECT*, DWORD);
    using Unlock = HRESULT(__stdcall*)(void*, UINT);
    Locked lr{};
    const RECT r{0, 0, w, h};
    if (!texture || FAILED(M<Lock>(texture, kLockRect)(texture, 0, &lr, &r, 0))) return false;
    for (int y = 0; y < h; ++y) memcpy(static_cast<uint8_t*>(lr.bits) + y * lr.pitch, pixels + size_t(y) * w, size_t(w) * 4);
    M<Unlock>(texture, kUnlockRect)(texture, 0);
    return true;
}

void overlay::ReleaseTexture(void* texture) {
    if (texture) M<ULONG(__stdcall*)(void*)>(texture, kRelease)(texture);
}

namespace {
using SetRs = HRESULT(__stdcall*)(void*, DWORD, DWORD);
using GetRs = HRESULT(__stdcall*)(void*, DWORD, DWORD*);
using SetTss = HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD);
using GetTss = HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD*);
using GetTex = HRESULT(__stdcall*)(void*, DWORD, void**);
using SetTex = HRESULT(__stdcall*)(void*, DWORD, void*);
using SetDw = HRESULT(__stdcall*)(void*, DWORD);
using GetDw = HRESULT(__stdcall*)(void*, DWORD*);
using Viewport = HRESULT(__stdcall*)(void*, DWORD*);  // D3DVIEWPORT8 = 6 dwords
using Draw = HRESULT(__stdcall*)(void*, uint32_t, UINT, const void*, UINT);

// Sets up plain alpha-blended pre-transformed quads over the whole back buffer; every state it touches is read
// first and put back when it goes out of scope.
class Draw2D {
public:
    explicit Draw2D(void* dev) : dev_(dev) {
        for (size_t i = 0; i < std::size(kStates); ++i) M<GetRs>(dev, kGetRenderState)(dev, kStates[i], &rs_[i]);
        for (size_t i = 0; i < std::size(kStage0); ++i) M<GetTss>(dev, kGetTss)(dev, 0, kStage0[i], &tss0_[i]);
        M<GetTss>(dev, kGetTss)(dev, 1, 1, &tss1ColorOp_);
        M<GetTex>(dev, kGetTexture)(dev, 0, &tex0_);
        M<GetDw>(dev, kGetVertexShader)(dev, &vs_);
        M<GetDw>(dev, kGetPixelShader)(dev, &ps_);
        M<Viewport>(dev, kGetViewport)(dev, vp_);

        if (auto* r = *reinterpret_cast<uint8_t**>(kRenderer)) {
            DWORD full[6] = {0, 0, *reinterpret_cast<DWORD*>(r + 0x40688), *reinterpret_cast<DWORD*>(r + 0x4068C), 0,
                             0x3F800000};
            M<Viewport>(dev, kSetViewport)(dev, full);
        }
        const DWORD set[][2] = {{7, 0}, {14, 0}, {15, 0}, {19, 5}, {20, 6}, {22, 1}, {27, 1}, {28, 0}, {52, 0}, {137, 0}};
        for (auto& s : set) M<SetRs>(dev, kSetRenderState)(dev, s[0], s[1]);
        Textured(true);
        M<SetTss>(dev, kSetTss)(dev, 1, 1, 1 /*DISABLE*/);
        M<SetDw>(dev, kSetPixelShader)(dev, 0);
        M<SetDw>(dev, kSetVertexShader)(dev, kFvf);
    }
    ~Draw2D() {
        M<Viewport>(dev_, kSetViewport)(dev_, vp_);
        M<SetTex>(dev_, kSetTexture)(dev_, 0, tex0_);
        if (tex0_) M<ULONG(__stdcall*)(void*)>(tex0_, kRelease)(tex0_);  // GetTexture added a reference
        M<SetDw>(dev_, kSetVertexShader)(dev_, vs_);
        M<SetDw>(dev_, kSetPixelShader)(dev_, ps_);
        M<SetTss>(dev_, kSetTss)(dev_, 1, 1, tss1ColorOp_);
        for (size_t i = 0; i < std::size(kStage0); ++i) M<SetTss>(dev_, kSetTss)(dev_, 0, kStage0[i], tss0_[i]);
        for (size_t i = 0; i < std::size(kStates); ++i) M<SetRs>(dev_, kSetRenderState)(dev_, kStates[i], rs_[i]);
    }
    Draw2D(const Draw2D&) = delete;
    Draw2D& operator=(const Draw2D&) = delete;

    // Textured: texture x vertex colour; untextured: the vertex colour alone.
    void Textured(bool on) {
        const DWORD op = on ? 4 /*MODULATE*/ : 3 /*SELECTARG2*/;
        const DWORD stage0[][2] = {{1, op}, {2, 2}, {3, 0}, {4, op}, {5, 2}, {6, 0}, {13, 3}, {14, 3}, {16, 2}, {17, 2}, {18, 0}};
        for (auto& s : stage0) M<SetTss>(dev_, kSetTss)(dev_, 0, s[0], s[1]);
        if (!on) M<SetTex>(dev_, kSetTexture)(dev_, 0, nullptr);
    }
    void Quad(const Vertex (&quad)[4]) { M<Draw>(dev_, kDrawPrimitiveUP)(dev_, kTriangleStrip, 2, quad, sizeof(Vertex)); }

private:
    static constexpr DWORD kStates[] = {7 /*ZENABLE*/, 14 /*ZWRITEENABLE*/, 15 /*ALPHATEST*/, 19 /*SRCBLEND*/,
                                        20 /*DESTBLEND*/, 22 /*CULLMODE*/, 27 /*ALPHABLEND*/, 28 /*FOG*/,
                                        52 /*STENCIL*/, 137 /*LIGHTING*/};
    static constexpr DWORD kStage0[] = {1, 2, 3, 4, 5, 6, 13, 14, 16, 17, 18};  // COLOROP..ALPHAARG2, ADDRESSU/V, filters
    void* dev_;
    DWORD rs_[std::size(kStates)], tss0_[std::size(kStage0)], tss1ColorOp_ = 0, vs_ = 0, ps_ = 0, vp_[6];
    void* tex0_ = nullptr;
};
}  // namespace

namespace {
overlay::LateDraw g_lateDraw = nullptr;
overlay::LateDraw g_lateDraws[4] = {};  // AddLateDraw: drawn every frame after g_lateDraw

void DrawQueue(void* dev) {
    if (g_queue.empty()) return;
    if (dev != g_device) {  // first use, or a new device
        g_device = dev;
        if (!CreateTextures(dev)) g_device = nullptr;
    }
    if (!g_device) {
        g_queue.clear();
        return;
    }
    Draw2D draw(dev);
    bool textured = true;
    for (const Queued& q : g_queue) {
        void* tex = q.icon >= 0 ? g_textures[q.icon] : q.texture;
        if (!tex != !textured) draw.Textured(textured = tex != nullptr);
        if (tex) M<SetTex>(dev, kSetTexture)(dev, 0, tex);
        draw.Quad(q.quad);
    }
    g_queue.clear();
}
}  // namespace

void overlay::SetLateDraw(LateDraw fn) { g_lateDraw = fn; }

void overlay::AddLateDraw(LateDraw fn) {
    for (LateDraw& slot : g_lateDraws)
        if (slot == fn) return;
    for (LateDraw& slot : g_lateDraws)
        if (!slot) {
            slot = fn;
            return;
        }
}

// Queued shapes first; then the late draw (text with the game's own font code, drawn right away on top), then what
// that text queued (icon glyphs inside it).
void overlay::Render(void* dev) {
    DrawQueue(dev);
    if (g_lateDraw) {
        g_lateDraw();
        DrawQueue(dev);
    }
    for (LateDraw fn : g_lateDraws)
        if (fn) {
            fn();
            DrawQueue(dev);
        }
}

void overlay::FillRects(const float (*rects)[4], int count, uint32_t argb) {
    void* dev = *reinterpret_cast<void**>(0x755050);
    if (!dev || count <= 0) return;
    Draw2D draw(dev);
    draw.Textured(false);
    for (int i = 0; i < count; ++i) {
        const float x0 = rects[i][0] - 0.5f, y0 = rects[i][1] - 0.5f;
        const float x1 = x0 + rects[i][2], y1 = y0 + rects[i][3];
        draw.Quad({{x0, y0, 0, 1, argb, 0, 0}, {x1, y0, 0, 1, argb, 1, 0}, {x0, y1, 0, 1, argb, 0, 1}, {x1, y1, 0, 1, argb, 1, 1}});
    }
}

void* overlay::MenuFont() { return *reinterpret_cast<void**>(0x60EDB4); }

int overlay::TextWidth(void* font, const char* text) {
    return reinterpret_cast<int16_t(__thiscall*)(void*, const char*)>(0x53A0F0)(font, text);
}

void overlay::DrawLabel(void* font, const char* text, int x, int y, uint32_t argb) {
    reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(0x539F80)(font, argb);
    reinterpret_cast<void(__thiscall*)(void*, const char*, int, int, int)>(0x53A7B0)(font, text, x, y, 0);
}
