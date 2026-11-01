// On-screen prompts for Lua missions (DS1's tutorial texts: Denied Ops has no tutorial-prompt system and no script
// event that shows a line of text). features::SetPrompt(utf8) sets the line; it is drawn every frame until it hasn't
// been set again for 1.5 s (DS1's scripts show their prompt every frame while it applies).
//
// Drawing: d3d9.dll's IDirect3DDevice9::Present and ::Reset are detoured (their addresses come from a throw-away
// NULLREF device's vtable - the game's device vtable is a heap copy d3d9 may replace, the code isn't), the text goes
// through D3DX's font (d3dx9_35.dll, which the game itself imports) in its own BeginScene / EndScene right before the
// game's Present. The font is released before a Reset and made again after.
#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <string>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
// ID3DXFont (d3dx9 has no header in the Windows SDK): the slots used here
struct Font {
    void** vt;
    ULONG Release() { return reinterpret_cast<ULONG(__stdcall*)(Font*)>(vt[2])(this); }
    INT DrawTextW(const wchar_t* text, RECT* rect, DWORD format, D3DCOLOR color) {
        return reinterpret_cast<INT(__stdcall*)(Font*, void*, const wchar_t*, INT, RECT*, DWORD, D3DCOLOR)>(vt[15])(
            this, nullptr, text, -1, rect, format, color);
    }
};
using CreateFontWFn = HRESULT(WINAPI*)(IDirect3DDevice9*, INT, UINT, UINT, UINT, BOOL, DWORD, DWORD, DWORD, DWORD,
                                       const wchar_t*, Font**);

using PresentFn = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
PresentFn g_nextPresent = nullptr;
ResetFn g_nextReset = nullptr;
CreateFontWFn g_createFont = nullptr;

std::wstring g_text;
DWORD g_textTime = 0;
Font* g_font = nullptr;
IDirect3DDevice9* g_fontDevice = nullptr;
int g_fontHeight = 0;

void DropFont() {
    if (g_font) g_font->Release();
    g_font = nullptr;
    g_fontDevice = nullptr;
}

void Draw(IDirect3DDevice9* dev) {
    if (g_text.empty() || GetTickCount() - g_textTime > 1500 || !g_createFont) return;
    IDirect3DSurface9* back = nullptr;
    D3DSURFACE_DESC desc{};
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return;
    back->GetDesc(&desc);
    back->Release();
    const int height = static_cast<int>(desc.Height / 26);
    if (g_font && (g_fontDevice != dev || g_fontHeight != height)) DropFont();
    if (!g_font && FAILED(g_createFont(dev, height, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial", &g_font))) {
        g_font = nullptr;
        return;
    }
    g_fontDevice = dev;
    g_fontHeight = height;
    if (FAILED(dev->BeginScene())) return;
    const LONG w = static_cast<LONG>(desc.Width), h = static_cast<LONG>(desc.Height);
    RECT r{w / 10, h * 70 / 100, w - w / 10, h};
    RECT shadow{r.left + 2, r.top + 2, r.right + 2, r.bottom + 2};
    const DWORD fmt = DT_CENTER | DT_TOP | DT_WORDBREAK;
    g_font->DrawTextW(g_text.c_str(), &shadow, fmt, D3DCOLOR_ARGB(220, 0, 0, 0));
    g_font->DrawTextW(g_text.c_str(), &r, fmt, D3DCOLOR_ARGB(255, 255, 236, 160));
    dev->EndScene();
}

HRESULT __stdcall Present(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* d) {
    Draw(dev);
    return g_nextPresent(dev, a, b, w, d);
}

HRESULT __stdcall Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    DropFont();   // a D3DX font holds default-pool resources: gone before the reset, made again on the next draw
    return g_nextReset(dev, pp);
}

// d3d9.dll's Present / Reset code addresses, read from a NULLREF device's vtable (slots 17 / 16)
bool DeviceFunctions(void** present, void** reset) {
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9) d3d9 = LoadLibraryA("d3d9.dll");
    auto create = d3d9 ? reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(d3d9, "Direct3DCreate9")) : nullptr;
    IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    if (!d3d) return false;
    HWND wnd = CreateWindowExA(0, "STATIC", "", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, nullptr, nullptr);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    IDirect3DDevice9* dev = nullptr;
    const HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, wnd,
                                         D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (SUCCEEDED(hr) && dev) {
        void** vt = *reinterpret_cast<void***>(dev);
        *present = vt[17];
        *reset = vt[16];
        dev->Release();
    }
    d3d->Release();
    DestroyWindow(wnd);
    return SUCCEEDED(hr);
}

void Apply() {
    HMODULE d3dx = LoadLibraryA("d3dx9_35.dll");
    g_createFont = d3dx ? reinterpret_cast<CreateFontWFn>(GetProcAddress(d3dx, "D3DXCreateFontW")) : nullptr;
    void *present = nullptr, *reset = nullptr;
    if (!g_createFont || !DeviceFunctions(&present, &reset)) {
        dslog::Write("Prompt: [fail] no D3DX font / d3d9 device functions - prompts stay in the log");
        return;
    }
    const bool ok = patch::HookFunction(reinterpret_cast<uint32_t>(present), reinterpret_cast<void*>(&Present),
                                        reinterpret_cast<void**>(&g_nextPresent)) &&
                    patch::HookFunction(reinterpret_cast<uint32_t>(reset), reinterpret_cast<void*>(&Reset),
                                        reinterpret_cast<void**>(&g_nextReset));
    dslog::Write("Prompt: Present %p / Reset %p %s", present, reset, ok ? "hooked" : "[fail] not hooked");
}
}  // namespace

void features::SetPrompt(const char* utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], n);
    g_text = w;
    g_textTime = GetTickCount();
}

void features::ApplyPrompt() { AfterUnpack(&Apply); }
