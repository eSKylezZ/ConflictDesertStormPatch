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

#include <cstring>
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
    overlay::Icon icon;
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
        sub esp, 0x94
        push kImageDrawCont
        ret
    }
}
}  // namespace

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

void overlay::QueueIcon(Icon icon, float x, float y, float size, float alpha) {
    if (alpha < 0.0f) alpha = MenuAlpha();
    float scale = 1.0f, cx = 0.0f, cy = 0.0f, mult[4] = {1, 1, 1, 1};
    // The game's 2D transform of this moment (menu transitions), see FUN_005470a0.
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
    float vx, vy;
    if (features::SplitHudOffset(vx, vy)) x += vx, y += vy;
    auto channel = [](float v) { return static_cast<DWORD>((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0f + 0.5f); };
    const DWORD color = channel(alpha) << 24 | channel(mult[0]) << 16 | channel(mult[1]) << 8 | channel(mult[2]);
    auto px = [&](float v) { return (v - cx) * scale + cx - 0.5f; };
    auto py = [&](float v) { return (v - cy) * scale + cy - 0.5f; };
    const float x0 = px(x), y0 = py(y), x1 = px(x + size), y1 = py(y + size);
    g_queue.push_back({icon,
                       {{x0, y0, 0, 1, color, 0, 0},
                        {x1, y0, 0, 1, color, 1, 0},
                        {x0, y1, 0, 1, color, 0, 1},
                        {x1, y1, 0, 1, color, 1, 1}}});
}

void overlay::Render(void* dev) {
    if (g_queue.empty()) return;
    if (dev != g_device) {  // first use, or a new device
        g_device = dev;
        if (!CreateTextures(dev)) g_device = nullptr;
    }
    if (!g_device) {
        g_queue.clear();
        return;
    }
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

    // Save.
    static const DWORD kStates[] = {7 /*ZENABLE*/, 14 /*ZWRITEENABLE*/, 15 /*ALPHATEST*/, 19 /*SRCBLEND*/,
                                    20 /*DESTBLEND*/, 22 /*CULLMODE*/, 27 /*ALPHABLEND*/, 28 /*FOG*/,
                                    52 /*STENCIL*/, 137 /*LIGHTING*/};
    static const DWORD kStage0[] = {1, 2, 3, 4, 5, 6, 13, 14, 16, 17, 18};  // COLOROP..ALPHAARG2, ADDRESSU/V, filters
    DWORD rs[std::size(kStates)], tss0[std::size(kStage0)], tss1ColorOp = 0, vs = 0, ps = 0, vp[6];
    for (size_t i = 0; i < std::size(kStates); ++i) M<GetRs>(dev, kGetRenderState)(dev, kStates[i], &rs[i]);
    for (size_t i = 0; i < std::size(kStage0); ++i) M<GetTss>(dev, kGetTss)(dev, 0, kStage0[i], &tss0[i]);
    M<GetTss>(dev, kGetTss)(dev, 1, 1, &tss1ColorOp);
    void* tex0 = nullptr;
    M<GetTex>(dev, kGetTexture)(dev, 0, &tex0);
    M<GetDw>(dev, kGetVertexShader)(dev, &vs);
    M<GetDw>(dev, kGetPixelShader)(dev, &ps);
    M<Viewport>(dev, kGetViewport)(dev, vp);

    // Set: plain alpha-blended textured quads over the whole back buffer.
    auto* r = *reinterpret_cast<uint8_t**>(kRenderer);
    if (r) {
        DWORD full[6] = {0, 0, *reinterpret_cast<DWORD*>(r + 0x40688), *reinterpret_cast<DWORD*>(r + 0x4068C), 0, 0x3F800000};
        M<Viewport>(dev, kSetViewport)(dev, full);
    }
    const DWORD set[][2] = {{7, 0}, {14, 0}, {15, 0}, {19, 5}, {20, 6}, {22, 1}, {27, 1}, {28, 0}, {52, 0}, {137, 0}};
    for (auto& s : set) M<SetRs>(dev, kSetRenderState)(dev, s[0], s[1]);
    const DWORD stage0[][2] = {{1, 4}, {2, 2}, {3, 0}, {4, 4}, {5, 2}, {6, 0}, {13, 3}, {14, 3}, {16, 2}, {17, 2}, {18, 0}};
    for (auto& s : stage0) M<SetTss>(dev, kSetTss)(dev, 0, s[0], s[1]);
    M<SetTss>(dev, kSetTss)(dev, 1, 1, 1 /*DISABLE*/);
    M<SetDw>(dev, kSetPixelShader)(dev, 0);
    M<SetDw>(dev, kSetVertexShader)(dev, kFvf);

    for (const Queued& q : g_queue) {
        M<SetTex>(dev, kSetTexture)(dev, 0, g_textures[static_cast<int>(q.icon)]);
        M<Draw>(dev, kDrawPrimitiveUP)(dev, kTriangleStrip, 2, q.quad, sizeof(Vertex));
    }
    g_queue.clear();

    // Restore.
    M<Viewport>(dev, kSetViewport)(dev, vp);
    M<SetTex>(dev, kSetTexture)(dev, 0, tex0);
    if (tex0) M<ULONG(__stdcall*)(void*)>(tex0, kRelease)(tex0);  // GetTexture added a reference
    M<SetDw>(dev, kSetVertexShader)(dev, vs);
    M<SetDw>(dev, kSetPixelShader)(dev, ps);
    M<SetTss>(dev, kSetTss)(dev, 1, 1, tss1ColorOp);
    for (size_t i = 0; i < std::size(kStage0); ++i) M<SetTss>(dev, kSetTss)(dev, 0, kStage0[i], tss0[i]);
    for (size_t i = 0; i < std::size(kStates); ++i) M<SetRs>(dev, kSetRenderState)(dev, kStates[i], rs[i]);
}

void* overlay::MenuFont() { return *reinterpret_cast<void**>(0x60EDB4); }

int overlay::TextWidth(void* font, const char* text) {
    return reinterpret_cast<int16_t(__thiscall*)(void*, const char*)>(0x53A0F0)(font, text);
}

void overlay::DrawLabel(void* font, const char* text, int x, int y, uint32_t argb) {
    reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(0x539F80)(font, argb);
    reinterpret_cast<void(__thiscall*)(void*, const char*, int, int, int)>(0x53A7B0)(font, text, x, y, 0);
}
