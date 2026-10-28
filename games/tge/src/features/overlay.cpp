// Overlay drawing (see overlay.h): the device-level part of DesertStormFix's overlay.cpp (games/ds1/src/features/
// overlay.cpp) - icon textures from the RCDATA resource game_icons.bin (A8R8G8B8, managed pool, so they survive a
// Reset), PNG resources through WIC, pre-transformed quads (FVF 0x144) drawn right before Present in a scene of their
// own, with every render / texture stage state, the texture, shaders and the viewport read back first and restored.
// TGE's device = renderer [0x79ED20]
// +0x40530, back buffer size = present params +0x84 / +0x88. Text = GDI (DrawText into a 32-bit DIB -> texture),
// cached by (text, height, weight); entries unused for 10 s are released.
#include "features/overlay.h"

#include <windows.h>
#include <wincodec.h>
#undef small

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "core/log.h"
#include "core/pse_host.h"
#include "loader.h"

namespace {
enum Device : int {
    kCreateTexture = 20, kSetViewport = 40, kGetViewport = 41, kSetRenderState = 50, kGetRenderState = 51,
    kGetTexture = 60, kSetTexture = 61, kGetTss = 62, kSetTss = 63, kDrawPrimitiveUP = 72, kSetVertexShader = 76,
    kGetVertexShader = 77, kSetPixelShader = 88, kGetPixelShader = 89,
};
enum Texture : int { kRelease = 2, kLockRect = 16, kUnlockRect = 17 };
constexpr uint32_t kFvf = 0x144;   // XYZRHW | DIFFUSE | TEX1
constexpr uint32_t kFormatArgb = 21, kPoolManaged = 1, kTriangleStrip = 5;
constexpr uint32_t kRenderer = 0x79ED20;

struct Vertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};
struct Queued {
    int icon;        // overlay::Icon, or -1: `texture` (nullptr = solid colour)
    void* texture;
    Vertex quad[4];  // triangle strip
};
std::vector<Queued> g_queue;
void* g_device = nullptr;
void* g_textures[static_cast<int>(overlay::Icon::Count)] = {};

void** Vt(void* obj) { return *static_cast<void***>(obj); }
template <class Fn>
Fn M(void* obj, int i) { return reinterpret_cast<Fn>(Vt(obj)[i]); }

HMODULE Self() {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&Self), &self);
    return self;
}
bool Resource(int id, const void*& data, DWORD& size) {
    HMODULE self = Self();
    HRSRC res = FindResourceA(self, MAKEINTRESOURCEA(id), MAKEINTRESOURCEA(10) /*RT_RCDATA*/);
    HGLOBAL h = res ? LoadResource(self, res) : nullptr;
    if (!h) return false;
    data = LockResource(h);
    size = SizeofResource(self, res);
    return data != nullptr;
}

void* DeviceNow() {
    auto* r = *reinterpret_cast<uint8_t**>(kRenderer);
    return r ? *reinterpret_cast<void**>(r + 0x40530) : nullptr;
}

void* NewTexture(void* dev, int w, int h) {
    using Create = HRESULT(__stdcall*)(void*, UINT, UINT, UINT, DWORD, uint32_t, uint32_t, void**);
    void* tex = nullptr;
    if (FAILED(M<Create>(dev, kCreateTexture)(dev, w, h, 1, 0, kFormatArgb, kPoolManaged, &tex))) return nullptr;
    return tex;
}
struct Locked {
    INT pitch;
    void* bits;
};
using Lock = HRESULT(__stdcall*)(void*, UINT, Locked*, const RECT*, DWORD);
using Unlock = HRESULT(__stdcall*)(void*, UINT);
void Release(void* t) {
    if (t) M<ULONG(__stdcall*)(void*)>(t, kRelease)(t);
}
// w x h BGRA pixels (rows of srcPitch bytes) into the texture's top-left; the rest cleared.
void Upload(void* tex, const uint8_t* px, int w, int h, int srcPitch, int texW, int texH) {
    Locked lr{};
    if (FAILED(M<Lock>(tex, kLockRect)(tex, 0, &lr, nullptr, 0))) return;
    for (int y = 0; y < texH; ++y) {
        auto* row = static_cast<uint8_t*>(lr.bits) + y * lr.pitch;
        memset(row, 0, size_t(texW) * 4);
        if (y < h) memcpy(row, px + size_t(y) * srcPitch, size_t(w) * 4);
    }
    M<Unlock>(tex, kUnlockRect)(tex, 0);
}

bool CreateIconTextures(void* dev) {
    const void* data;
    DWORD size;
    constexpr int n = gen::kGameIconSize;
    constexpr size_t need = size_t(n) * n * 4 * size_t(overlay::Icon::Count);
    if (!Resource(gen::kGameIconsResource, data, size) || size < need) {
        dslog::Write("[fail] Overlay: icon resource missing");
        return false;
    }
    const auto* pixels = static_cast<const uint8_t*>(data);
    for (int i = 0; i < static_cast<int>(overlay::Icon::Count); ++i) {
        void* tex = NewTexture(dev, n, n);
        if (!tex) return false;
        Upload(tex, pixels + size_t(i) * n * n * 4, n, n, n * 4, n, n);
        g_textures[i] = tex;
    }
    dslog::Write("Overlay: %d icon textures created", static_cast<int>(overlay::Icon::Count));
    return true;
}

// ---- text textures (GDI) ----
struct TextEntry {
    void* tex = nullptr;
    int w = 0, h = 0, texW = 0, texH = 0;
    DWORD used = 0;
};
std::map<std::tuple<std::string, int, bool>, TextEntry> g_text;

int Pow2(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

HFONT Font(int height, bool bold) {
    static std::map<std::pair<int, bool>, HFONT> fonts;
    HFONT& f = fonts[{height, bold}];
    if (!f) f = CreateFontA(-height, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Arial");
    return f;
}

SIZE Measure(const std::string& text, int height, bool bold) {
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, Font(height, bold));
    SIZE s{};
    GetTextExtentPoint32A(dc, text.c_str(), static_cast<int>(text.size()), &s);
    SelectObject(dc, old);
    DeleteDC(dc);
    return s;
}

TextEntry* TextTexture(const std::string& text, int height, bool bold) {
    void* dev = DeviceNow();
    if (!dev || text.empty()) return nullptr;
    TextEntry& e = g_text[{text, height, bold}];
    e.used = GetTickCount();
    if (e.tex) return &e;
    const SIZE s = Measure(text, height, bold);
    e.w = s.cx + 2, e.h = s.cy + 2;
    e.texW = Pow2(e.w), e.texH = Pow2(e.h);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof bi.bmiHeader, e.w, -e.h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        DeleteDC(dc);
        return nullptr;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp), oldFont = SelectObject(dc, Font(height, bold));
    memset(bits, 0, size_t(e.w) * e.h * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutA(dc, 1, 1, text.c_str(), static_cast<int>(text.size()));
    GdiFlush();
    auto* p = static_cast<uint8_t*>(bits);   // white on black -> white with the coverage as alpha
    for (int i = 0; i < e.w * e.h; ++i) {
        const uint8_t a = std::max({p[i * 4], p[i * 4 + 1], p[i * 4 + 2]});
        p[i * 4] = p[i * 4 + 1] = p[i * 4 + 2] = 255;
        p[i * 4 + 3] = a;
    }
    e.tex = NewTexture(dev, e.texW, e.texH);
    if (e.tex) Upload(e.tex, p, e.w, e.h, e.w * 4, e.texW, e.texH);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return e.tex ? &e : nullptr;
}

void ExpireText() {
    const DWORD now = GetTickCount();
    for (auto it = g_text.begin(); it != g_text.end();)
        if (now - it->second.used > 10000) {
            Release(it->second.tex);
            it = g_text.erase(it);
        } else {
            ++it;
        }
}

// ---- drawing ----
using SetRs = HRESULT(__stdcall*)(void*, DWORD, DWORD);
using GetRs = HRESULT(__stdcall*)(void*, DWORD, DWORD*);
using SetTssFn = HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD);
using GetTssFn = HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD*);
using GetTex = HRESULT(__stdcall*)(void*, DWORD, void**);
using SetTex = HRESULT(__stdcall*)(void*, DWORD, void*);
using SetDw = HRESULT(__stdcall*)(void*, DWORD);
using GetDw = HRESULT(__stdcall*)(void*, DWORD*);
using ViewportFn = HRESULT(__stdcall*)(void*, DWORD*);
using Draw = HRESULT(__stdcall*)(void*, uint32_t, UINT, const void*, UINT);

// Plain alpha-blended pre-transformed quads over the whole back buffer; every state touched is put back.
class Draw2D {
public:
    explicit Draw2D(void* dev) : dev_(dev) {
        for (size_t i = 0; i < std::size(kStates); ++i) M<GetRs>(dev, kGetRenderState)(dev, kStates[i], &rs_[i]);
        for (size_t i = 0; i < std::size(kStage0); ++i) M<GetTssFn>(dev, kGetTss)(dev, 0, kStage0[i], &tss0_[i]);
        M<GetTssFn>(dev, kGetTss)(dev, 1, 1, &tss1ColorOp_);
        M<GetTex>(dev, kGetTexture)(dev, 0, &tex0_);
        M<GetDw>(dev, kGetVertexShader)(dev, &vs_);
        M<GetDw>(dev, kGetPixelShader)(dev, &ps_);
        M<ViewportFn>(dev, kGetViewport)(dev, vp_);
        float w, h;
        overlay::ScreenSize(w, h);
        DWORD full[6] = {0, 0, static_cast<DWORD>(w), static_cast<DWORD>(h), 0, 0x3F800000};
        M<ViewportFn>(dev, kSetViewport)(dev, full);
        const DWORD set[][2] = {{7, 0}, {14, 0}, {15, 0}, {19, 5}, {20, 6}, {22, 1}, {27, 1}, {28, 0}, {52, 0}, {137, 0}};
        for (auto& s : set) M<SetRs>(dev, kSetRenderState)(dev, s[0], s[1]);
        Textured(true);
        M<SetTssFn>(dev, kSetTss)(dev, 1, 1, 1 /*DISABLE*/);
        M<SetDw>(dev, kSetPixelShader)(dev, 0);
        M<SetDw>(dev, kSetVertexShader)(dev, kFvf);
    }
    ~Draw2D() {
        M<ViewportFn>(dev_, kSetViewport)(dev_, vp_);
        M<SetTex>(dev_, kSetTexture)(dev_, 0, tex0_);
        Release(tex0_);   // GetTexture added a reference
        M<SetDw>(dev_, kSetVertexShader)(dev_, vs_);
        M<SetDw>(dev_, kSetPixelShader)(dev_, ps_);
        M<SetTssFn>(dev_, kSetTss)(dev_, 1, 1, tss1ColorOp_);
        for (size_t i = 0; i < std::size(kStage0); ++i) M<SetTssFn>(dev_, kSetTss)(dev_, 0, kStage0[i], tss0_[i]);
        for (size_t i = 0; i < std::size(kStates); ++i) M<SetRs>(dev_, kSetRenderState)(dev_, kStates[i], rs_[i]);
    }
    Draw2D(const Draw2D&) = delete;
    Draw2D& operator=(const Draw2D&) = delete;
    void Textured(bool on) {
        const DWORD op = on ? 4 /*MODULATE*/ : 3 /*SELECTARG2*/;
        const DWORD stage0[][2] = {{1, op}, {2, 2}, {3, 0}, {4, op}, {5, 2}, {6, 0}, {13, 3}, {14, 3}, {16, 2}, {17, 2}, {18, 0}};
        for (auto& s : stage0) M<SetTssFn>(dev_, kSetTss)(dev_, 0, s[0], s[1]);
        if (!on) M<SetTex>(dev_, kSetTexture)(dev_, 0, nullptr);
    }
    void Quad(const Vertex (&quad)[4]) { M<Draw>(dev_, kDrawPrimitiveUP)(dev_, kTriangleStrip, 2, quad, sizeof(Vertex)); }

private:
    static constexpr DWORD kStates[] = {7, 14, 15, 19, 20, 22, 27, 28, 52, 137};
    static constexpr DWORD kStage0[] = {1, 2, 3, 4, 5, 6, 13, 14, 16, 17, 18};
    void* dev_;
    DWORD rs_[std::size(kStates)], tss0_[std::size(kStage0)], tss1ColorOp_ = 0, vs_ = 0, ps_ = 0, vp_[6];
    void* tex0_ = nullptr;
};

void DrawQueue(void* dev) {
    if (g_queue.empty()) return;
    if (dev != g_device) {
        g_device = dev;
        if (!CreateIconTextures(dev)) g_device = nullptr;
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

overlay::LateDraw g_lateDraws[16] = {};

// Right before Present (the layer's PSE_EVENT_PRESENT): one scene of our own on top of the finished frame, whatever
// number of scenes the game drew.
int PSE_CALL OnPresent(PSEEventId, void* dev, void*) {
    if (!dev) return 0;
    for (overlay::LateDraw fn : g_lateDraws)
        if (fn) fn();
    if (!g_queue.empty() && SUCCEEDED(M<HRESULT(__stdcall*)(void*)>(dev, 34 /*BeginScene*/)(dev))) {
        DrawQueue(dev);
        M<HRESULT(__stdcall*)(void*)>(dev, 35 /*EndScene*/)(dev);
    }
    g_queue.clear();
    static DWORD lastExpire = 0;
    if (GetTickCount() - lastExpire > 2000) ExpireText(), lastExpire = GetTickCount();
    return 0;
}

DWORD Argb(uint32_t argb, float alpha) {
    const uint32_t a = static_cast<uint32_t>((argb >> 24) * std::fmax(0.0f, std::fmin(1.0f, alpha)));
    return (a << 24) | (argb & 0xFFFFFF);
}
void QueueRectangle(int icon, void* texture, float x, float y, float w, float h, float u1, float v1, DWORD c) {
    const float x0 = x - 0.5f, y0 = y - 0.5f, x1 = x0 + w, y1 = y0 + h;
    g_queue.push_back({icon, texture, {{x0, y0, 0, 1, c, 0, 0}, {x1, y0, 0, 1, c, u1, 0}, {x0, y1, 0, 1, c, 0, v1}, {x1, y1, 0, 1, c, u1, v1}}});
}
}  // namespace

void overlay::ScreenSize(float& w, float& h) {
    auto* r = *reinterpret_cast<uint8_t**>(kRenderer);
    w = r ? static_cast<float>(*reinterpret_cast<uint32_t*>(r + 0x84)) : 800.0f;
    h = r ? static_cast<float>(*reinterpret_cast<uint32_t*>(r + 0x88)) : 600.0f;
}

void overlay::QueueIcon(Icon icon, float x, float y, float size, float alpha) {
    QueueRectangle(static_cast<int>(icon), nullptr, x, y, size, size, 1.0f, 1.0f, Argb(0xFFFFFFFF, alpha));
}
void overlay::QueueImage(void* texture, float x, float y, float w, float h, float u1, float v1, float alpha) {
    if (texture) QueueRectangle(-1, texture, x, y, w, h, u1, v1, Argb(0xFFFFFFFF, alpha));
}
void overlay::QueueRect(float x, float y, float w, float h, uint32_t argb) { QueueRectangle(-1, nullptr, x, y, w, h, 1, 1, argb); }
void overlay::QueueLine(float x0, float y0, float x1, float y1, float width, uint32_t argb) {
    const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.01f) return;
    const float nx = -dy / len * width / 2, ny = dx / len * width / 2;
    g_queue.push_back({-1, nullptr, {{x0 + nx, y0 + ny, 0, 1, argb, 0, 0}, {x1 + nx, y1 + ny, 0, 1, argb, 1, 0},
                                     {x0 - nx, y0 - ny, 0, 1, argb, 0, 1}, {x1 - nx, y1 - ny, 0, 1, argb, 1, 1}}});
}
float overlay::TextWidth(const char* text, float height, bool bold) {
    return text && *text ? static_cast<float>(Measure(text, static_cast<int>(height), bold).cx) : 0.0f;
}
float overlay::QueueText(const char* text, float x, float y, float height, uint32_t argb, int align, bool bold) {
    TextEntry* e = text && *text ? TextTexture(text, static_cast<int>(height), bold) : nullptr;
    if (!e) return 0.0f;
    const float w = static_cast<float>(e->w);
    if (align == 1) x -= w / 2;
    else if (align == 2) x -= w;
    QueueRectangle(-1, e->tex, std::floor(x), std::floor(y), w, static_cast<float>(e->h), w / e->texW,
                   static_cast<float>(e->h) / e->texH, argb);
    return w;
}

void* overlay::LoadTexture(int resource, int texW, int texH) {
    static std::map<int, std::pair<void*, void*>> cache;   // resource -> (device, texture)
    void* dev = DeviceNow();
    if (!dev) return nullptr;
    auto& c = cache[resource];
    if (c.first == dev && c.second) return c.second;
    const void* data;
    DWORD size;
    if (!Resource(resource, data, size)) return nullptr;
    std::vector<uint8_t> px;
    UINT w = 0, h = 0;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)), size)) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(conv->GetSize(&w, &h)) && w <= static_cast<UINT>(texW) && h <= static_cast<UINT>(texH)) {
        px.resize(size_t(w) * h * 4);
        if (FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data()))) px.clear();
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{conv, frame, decoder, stream, factory})
        if (u) u->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    if (px.empty()) {
        dslog::Write("[fail] Overlay: picture resource %d not decoded", resource);
        return nullptr;
    }
    void* tex = NewTexture(dev, texW, texH);
    if (!tex) return nullptr;
    Upload(tex, px.data(), static_cast<int>(w), static_cast<int>(h), static_cast<int>(w) * 4, texW, texH);
    c = {dev, tex};
    return tex;
}

void overlay::AddLateDraw(LateDraw fn) {
    for (LateDraw& slot : g_lateDraws)
        if (slot == fn) return;
    for (LateDraw& slot : g_lateDraws)
        if (!slot) {
            slot = fn;
            return;
        }
    dslog::Write("[fail] Overlay: no late-draw slot left");
}

void overlay::Install() {
    static bool done = false;
    if (done) return;
    done = true;
    if (const PSEMessagingInterface* m = pse::MessagingInterface())
        m->Subscribe(pse_host::Core(), PSE_EVENT_PRESENT, &OnPresent, nullptr, 100);   // before devbg's shot (0)
}
