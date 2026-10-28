// The dsfix kit's overlay client: features/overlay.h for a plugin DLL, on dsfix.dll's interface dsfix.overlay v1
// (core/dsfix_overlay.h). A moved feature keeps calling overlay::QueueRect(...) etc.; without dsfix.dll's interface
// the calls do nothing and the loaders return null. Not here (dsfix.dll's own business): Install, Render, OnFrame,
// SetImageProbe. LoadTexture(resource) reads the resource from the calling plugin's own DLL.
#include <windows.h>

#include <vector>

#include "core/dsfix_overlay.h"
#include "features/overlay.h"
#include "kit/kit.h"

namespace {
const DsfixOverlayV1* O() {
    static const DsfixOverlayV1* o = nullptr;
    static bool asked = false;
    if (!asked && kit::Messaging()) {
        asked = true;
        o = static_cast<const DsfixOverlayV1*>(kit::Query(DSFIX_OVERLAY_INTERFACE, 1));
        if (o && o->size < sizeof(DsfixOverlayV1)) o = nullptr;
        if (!o && kit::Log())
            kit::Log()->Write(kit::Self(), PSE_LOG_ERROR, "%s: no " DSFIX_OVERLAY_INTERFACE " 1 (DesertStormFix's dsfix.dll "
                              "missing or older) - nothing drawn", kit::Name());
    }
    return o;
}
}  // namespace

namespace overlay {
void QueueIcon(Icon icon, float x, float y, float size, float alpha) {
    if (auto* o = O()) o->QueueIcon(static_cast<int>(icon), x, y, size, alpha);
}
void QueueImage(void* t, float x, float y, float w, float h, float u1, float v1, float alpha) {
    if (auto* o = O()) o->QueueImage(t, x, y, w, h, u1, v1, alpha);
}
void QueueScreenImage(void* t, float x, float y, float w, float h, float u1, float v1) {
    if (auto* o = O()) o->QueueScreenImage(t, x, y, w, h, u1, v1);
}
void QueueScreenQuad(void* t, const float (&corners)[4][2], float u0, float v0, float u1, float v1, uint32_t argb) {
    if (auto* o = O()) o->QueueScreenQuad(t, &corners[0][0], u0, v0, u1, v1, argb);
}
void QueueScreenRect(float x, float y, float w, float h, uint32_t argb) {
    if (auto* o = O()) o->QueueScreenRect(x, y, w, h, argb);
}
void QueueRect(float x, float y, float w, float h, uint32_t argb) {
    if (auto* o = O()) o->QueueRect(x, y, w, h, argb);
}
void QueueLine(float x0, float y0, float x1, float y1, float width, uint32_t argb) {
    if (auto* o = O()) o->QueueLine(x0, y0, x1, y1, width, argb);
}

void* LoadTexture(int resource, int texW, int texH) {
    struct Cached {
        int resource;
        void* device;
        void* texture;
    };
    static std::vector<Cached> cache;
    auto* o = O();
    void* dev = o ? o->CurrentDevice() : nullptr;
    if (!dev) return nullptr;
    for (const Cached& c : cache)
        if (c.resource == resource && c.device == dev) return c.texture;
    cache.push_back({resource, dev, nullptr});  // failures are not retried
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&LoadTexture), &self);
    HRSRC res = FindResourceA(self, MAKEINTRESOURCEA(resource), MAKEINTRESOURCEA(10) /*RT_RCDATA*/);
    HGLOBAL data = res ? LoadResource(self, res) : nullptr;
    if (!data) return nullptr;
    char what[48];
    wsprintfA(what, "%s %d", kit::Name(), resource);
    cache.back().texture = o->LoadTextureImage(LockResource(data), SizeofResource(self, res), texW, texH, what);
    return cache.back().texture;
}
void* LoadTextureImage(const void* bytes, size_t size, int texW, int texH, const char* what) {
    auto* o = O();
    return o ? o->LoadTextureImage(bytes, size, texW, texH, what) : nullptr;
}
void* LoadTextureFile(const char* path, int& w, int& h, int& texW, int& texH) {
    w = h = texW = texH = 0;
    auto* o = O();
    return o ? o->LoadTextureFile(path, &w, &h, &texW, &texH) : nullptr;
}
void* LoadTextureBytes(const void* bytes, size_t size, int& w, int& h, int& texW, int& texH, const char* what) {
    w = h = texW = texH = 0;
    auto* o = O();
    return o ? o->LoadTextureBytes(bytes, size, &w, &h, &texW, &texH, what) : nullptr;
}
const void* DrawingSheet() { auto* o = O(); return o ? o->DrawingSheet() : nullptr; }
int DrawingImage() { auto* o = O(); return o ? o->DrawingImage() : -1; }
void* CreateTexture(int texW, int texH) { auto* o = O(); return o ? o->CreateTexture(texW, texH) : nullptr; }
bool UploadTexture(void* t, const uint32_t* px, int w, int h) { auto* o = O(); return o && o->UploadTexture(t, px, w, h); }
void ReleaseTexture(void* t) { if (auto* o = O()) o->ReleaseTexture(t); }
void* CurrentDevice() { auto* o = O(); return o ? o->CurrentDevice() : nullptr; }
float MenuAlpha() { auto* o = O(); return o ? o->MenuAlpha() : 1.0f; }
void SetLateDraw(LateDraw fn) { if (auto* o = O()) o->SetLateDraw(fn); }
void AddLateDraw(LateDraw fn) { if (auto* o = O()) o->AddLateDraw(fn); }
void SetTopDraw(LateDraw fn) { if (auto* o = O()) o->SetTopDraw(fn); }
void Flush() { if (auto* o = O()) o->Flush(); }
void FillRects(const float (*rects)[4], int count, uint32_t argb) {
    if (auto* o = O()) o->FillRects(&rects[0][0], count, argb);
}
const char* IconChar(Icon icon) { auto* o = O(); return o ? o->IconChar(static_cast<int>(icon)) : ""; }
bool GlyphsReady() { auto* o = O(); return o && o->GlyphsReady(); }
void* MenuFont() { auto* o = O(); return o ? o->MenuFont() : nullptr; }
int TextWidth(void* font, const char* text) { auto* o = O(); return o ? o->TextWidth(font, text) : 0; }
void DrawLabel(void* font, const char* text, int x, int y, uint32_t argb) {
    if (auto* o = O()) o->DrawLabel(font, text, x, y, argb);
}
}  // namespace overlay
