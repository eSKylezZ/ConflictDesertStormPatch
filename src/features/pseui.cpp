// pse.ui v1 (pse/include/pse/pse_ui.h, docs/sdk/reference/ui.md) for DS1, served by DesertStormFix's overlay:
//
//   drawing   Plugins queue rects / text / images during a game frame (tagged with pse.tasks' frame number); one
//             overlay late draw (overlay.cpp, after the game's 2D and our icon queue) draws the current frame's items in
//             call order and drops older ones. Shapes go through overlay's queue (flushed before each text), text through
//             the game's own font code (overlay::DrawLabel), so it looks like the menus.
//   texts     0xC4xxxxxx hashes per plugin + key (core/pse_ui_model.h TextTable), answered by the catalog Lookup hook
//             (ingame_input.cpp -> fixmenu.cpp FixMenuText -> PseUiText) in the fonts' 8-bit text.
//   pop-up    in a mission the game's pop-up bar (FUN_004D33A0 thiscall(view controller [0x63C98C], text, ms) - the game
//             keeps the pointer: texts in a ring); in the front end our own bar in the same late draw.
//   keyboard  CaptureKeyboard subclasses the game window [0x606A60] (on top of the console's subclass): WM_KEYDOWN /
//             WM_SYSKEYDOWN / WM_CHAR are queued for the plugin and never reach the game's key table; key-ups pass.
//             features::KeyHeld (dev.cpp) answers no meanwhile, so our own screens don't react either.
//
// Registered from ApplyFixMenu (inside DesertStormFix's Load, before any external plugin loads), owned by dsfix.overlay.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/pse_host.h"
#include "core/pse_ui_model.h"
#include "features/features.h"
#include "features/overlay.h"
#include "loader.h"
#include "pse/pse_ui.h"

namespace {
constexpr uint32_t kRenderer = 0x63C924, kViewController = 0x63C98C, kPopupShow = 0x4D33A0, kGameWindow = 0x606A60;
constexpr uint32_t kDispatchMode = 0x60EC8C, kLevelName = 0x606880, kFontSheet = 0x60EDB0, kSmallFont = 0x60EDB4, kLargeFont = 0x60EDB8;
constexpr size_t kMaxItems = 4096, kMaxKeys = 64;

template <class T>
T& Global(uint32_t va) {
    return *reinterpret_cast<T*>(va);
}

PSEPluginHandle g_owner = 0;   // dsfix.overlay
pse_ui::TextTable g_texts;
std::map<uint32_t, std::string> g_gameTexts;   // hash -> the text in the fonts' bytes (rebuilt on RegisterText)

bool Valid(PSEPluginHandle plugin) { return plugin && !pse::PluginDisabled(plugin) && pse::PluginName(plugin)[0]; }
uint32_t FrameNow() { return pse::TasksInterface() ? pse::TasksInterface()->FrameNumber() : 0; }

bool InFrontEnd() {
    const char* level = &Global<char>(kLevelName);
    return !level[0] || _strnicmp(level, "FrontEnd", 8) == 0 || _strnicmp(level, "intro", 5) == 0;
}

void* FontFor(uint32_t flags) { return Global<void*>(flags & PSE_UI_LARGE ? kLargeFont : kSmallFont); }

bool ScreenSize(int& w, int& h) {
    const auto* r = Global<const uint8_t*>(kRenderer);
    if (!r) return false;
    w = *reinterpret_cast<const int*>(r + 0x40688), h = *reinterpret_cast<const int*>(r + 0x4068C);
    return w > 0 && h > 0;
}

float FontScaleY() {
    const auto* sheet = Global<const uint8_t*>(kFontSheet);
    return sheet && (*reinterpret_cast<const uint32_t*>(sheet + 0x20) & 1) ? *reinterpret_cast<const float*>(sheet + 0x28)
                                                                           : 1.0f;
}
float FontHeight(void* font) {   // capitals, scaled (font +0x18)
    return font ? *reinterpret_cast<const int16_t*>(static_cast<uint8_t*>(font) + 0x18) * FontScaleY() : 0.0f;
}
float FontLineStep(void* font) {   // font +0x14, scaled
    return font ? *reinterpret_cast<const int16_t*>(static_cast<uint8_t*>(font) + 0x14) * FontScaleY() : 0.0f;
}
}  // namespace

// The font has a real glyph for byte c (not one of the glyphs every unmapped character shares).
bool features::FontHasGlyph(unsigned char c, void* font) {
    if (!font) return true;
    const auto* f = static_cast<const uint8_t*>(font);
    const auto* table = *reinterpret_cast<const uint16_t* const*>(f + 0x20);
    const uint16_t size = *reinterpret_cast<const uint16_t*>(f + 0x24);
    if (!table || c >= size) return false;
    const uint32_t lo = *reinterpret_cast<const uint32_t*>(f + 4), hi = *reinterpret_cast<const uint32_t*>(f + 8);
    const uint16_t g = table[c];
    if (g < lo || g > hi) return false;
    if (c == ' ') return true;
    return g != table[' '] && g != table[0] && g != table[0x7F];
}

std::string features::GameText(const char* utf8, bool upper, void* font, size_t maxBytes) {
    if (!font) font = Global<void*>(kSmallFont);
    return pse_ui::ToGameText(utf8, upper, [](unsigned char c, void* f) { return features::FontHasGlyph(c, f); }, font,
                              maxBytes);
}

namespace {
// ---- drawing ----
enum class Kind { Rect, Text, Image };
struct Item {
    Kind kind;
    uint32_t frame;
    float x, y, w, h;
    uint32_t argb, flags;
    std::string text;    // game bytes
    PSEUiImage image;
};
std::vector<Item> g_items;

struct Image {
    std::wstring path;
    std::vector<uint8_t> bytes;
    void* texture = nullptr;
    void* device = nullptr;
    int w = 0, h = 0, texW = 0, texH = 0;
};
std::vector<Image> g_images;   // PSEUiImage = index + 1

void* ImageTexture(Image& im) {
    void* dev = overlay::CurrentDevice();
    if (!dev) return nullptr;
    if (im.texture && im.device == dev) return im.texture;
    im.texture = overlay::LoadTextureBytes(im.bytes.data(), im.bytes.size(), im.w, im.h, im.texW, im.texH, "pse.ui image");
    im.device = dev;
    return im.texture;
}

PSEResult Push(PSEPluginHandle plugin, Item&& item) {
    if (!Valid(plugin)) return PSE_E_INVALID_ARG;
    const uint32_t now = FrameNow();
    if (!g_items.empty() && g_items.front().frame != now)   // last frame's: gone
        g_items.erase(std::remove_if(g_items.begin(), g_items.end(), [now](const Item& i) { return i.frame != now; }),
                      g_items.end());
    if (g_items.size() >= kMaxItems) return PSE_E_NO_MEMORY;
    item.frame = now;
    g_items.push_back(std::move(item));
    return PSE_OK;
}

// Item coordinates -> pixels.
void ToPixels(const Item& i, int W, int H, float& x, float& y, float& w, float& h) {
    if (i.flags & PSE_UI_PIXELS) x = i.x, y = i.y, w = i.w, h = i.h;
    else x = i.x * W, y = i.y * H, w = i.w * W, h = i.h * H;
}
uint32_t Faded(uint32_t argb, uint32_t flags) {
    if (!(flags & PSE_UI_FOLLOW_MENU)) return argb;
    const uint32_t a = static_cast<uint32_t>(((argb >> 24) & 0xFF) * overlay::MenuAlpha() + 0.5f);
    return (a > 255 ? 255 : a) << 24 | (argb & 0xFFFFFF);
}

// ---- pop-up bar of our own (front end) ----
std::string g_popup;   // game bytes
DWORD g_popupEnd = 0;

void DrawPopup(int W, int H) {
    if (g_popup.empty()) return;
    if (static_cast<int32_t>(GetTickCount() - g_popupEnd) >= 0) {
        g_popup.clear();
        return;
    }
    void* font = Global<void*>(kSmallFont);
    if (!font) return;
    const float line = std::max(FontLineStep(font), 10.0f), pad = line * 0.5f;
    // like the game's mission bar: across the top of the screen
    const float tw = static_cast<float>(overlay::TextWidth(font, g_popup.c_str()));
    const float bw = static_cast<float>(W), bh = line + 2 * pad;
    const float bx = 0, by = 0;
    (void)H;
    overlay::QueueScreenRect(bx, by, bw, bh, 0xD0101010);
    overlay::Flush();
    overlay::DrawLabel(font, g_popup.c_str(), static_cast<int>(bx + (bw - tw) / 2),
                       static_cast<int>(by + pad + FontHeight(font)), 0xFFF0F0F0);
}

void LateDraw() {
    int W, H;
    if (!ScreenSize(W, H)) return;
    const uint32_t now = FrameNow();
    for (const Item& i : g_items) {
        if (i.frame != now) continue;
        float x, y, w, h;
        ToPixels(i, W, H, x, y, w, h);
        const uint32_t argb = Faded(i.argb, i.flags);
        if (!(argb >> 24)) continue;
        switch (i.kind) {
            case Kind::Rect: overlay::QueueScreenRect(x, y, w, h, argb); break;
            case Kind::Image: {
                if (i.image == 0 || i.image > g_images.size()) break;
                Image& im = g_images[i.image - 1];
                void* tex = ImageTexture(im);
                if (!tex || !im.texW || !im.texH) break;
                const float corners[4][2] = {{x, y}, {x + w, y}, {x, y + h}, {x + w, y + h}};
                overlay::QueueScreenQuad(tex, corners, 0, 0, static_cast<float>(im.w) / im.texW,
                                         static_cast<float>(im.h) / im.texH, argb);
                break;
            }
            case Kind::Text: {
                void* font = FontFor(i.flags);
                if (!font || i.text.empty()) break;
                overlay::Flush();   // shapes queued before it stay under it
                const int tw = overlay::TextWidth(font, i.text.c_str());
                float tx = x;
                if (i.flags & PSE_UI_CENTER) tx -= tw / 2.0f;
                else if (i.flags & PSE_UI_RIGHT) tx -= tw;
                overlay::DrawLabel(font, i.text.c_str(), static_cast<int>(tx), static_cast<int>(y + FontHeight(font)), argb);
                break;
            }
        }
    }
    DrawPopup(W, H);
}

// ---- interface functions ----
PSEResult PSE_CALL DrawRect(PSEPluginHandle plugin, float x, float y, float w, float h, uint32_t argb, uint32_t flags) {
    return Push(plugin, Item{Kind::Rect, 0, x, y, w, h, argb, flags, std::string(), 0});
}

PSEResult PSE_CALL DrawText(PSEPluginHandle plugin, float x, float y, const char* text, uint32_t argb, uint32_t flags) {
    if (!text) return PSE_E_INVALID_ARG;
    return Push(plugin, Item{Kind::Text, 0, x, y, 0, 0, argb, flags, features::GameText(text, false, FontFor(flags), 1000), 0});
}

PSEResult PSE_CALL DrawImage(PSEPluginHandle plugin, PSEUiImage image, float x, float y, float w, float h, uint32_t argb,
                             uint32_t flags) {
    if (image == 0 || image > g_images.size()) return PSE_E_INVALID_ARG;
    return Push(plugin, Item{Kind::Image, 0, x, y, w, h, argb, flags, std::string(), image});
}

PSEUiImage PSE_CALL LoadImage(PSEPluginHandle plugin, const char* path, uint32_t* width, uint32_t* height) {
    if (width) *width = 0;
    if (height) *height = 0;
    if (!Valid(plugin) || !path || !*path) return 0;
    std::wstring wpath;
    const int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (n <= 1) return 0;
    wpath.resize(n - 1);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), n);
    const bool absolute = wpath.size() > 1 && (wpath[1] == L':' || (wpath[0] == L'\\' && wpath[1] == L'\\'));
    if (!absolute) {
        wchar_t exe[MAX_PATH];
        const DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe, len);
        dir.resize(dir.find_last_of(L"\\/") + 1);
        wpath = dir + wpath;
    }
    for (size_t i = 0; i < g_images.size(); ++i)
        if (_wcsicmp(g_images[i].path.c_str(), wpath.c_str()) == 0) {
            if (width) *width = static_cast<uint32_t>(g_images[i].w);
            if (height) *height = static_cast<uint32_t>(g_images[i].h);
            return static_cast<PSEUiImage>(i + 1);
        }
    HANDLE f = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        dslog::Write("[fail] pse.ui: %s - image %s not found", pse::PluginName(plugin), path);
        return 0;
    }
    Image im;
    im.path = wpath;
    const DWORD size = GetFileSize(f, nullptr);
    DWORD got = 0;
    if (size != INVALID_FILE_SIZE && size < (64u << 20)) {
        im.bytes.resize(size);
        if (!ReadFile(f, im.bytes.data(), size, &got, nullptr) || got != size) im.bytes.clear();
    }
    CloseHandle(f);
    if (im.bytes.empty() || !ImageTexture(im)) {
        dslog::Write("[fail] pse.ui: %s - image %s not readable", pse::PluginName(plugin), path);
        return 0;
    }
    if (width) *width = static_cast<uint32_t>(im.w);
    if (height) *height = static_cast<uint32_t>(im.h);
    g_images.push_back(std::move(im));
    dslog::Write("pse.ui: %s loaded image %s (%dx%d)", pse::PluginName(plugin), path, g_images.back().w, g_images.back().h);
    return static_cast<PSEUiImage>(g_images.size());
}

float PSE_CALL TextWidth(PSEPluginHandle plugin, const char* text, uint32_t flags) {
    int W, H;
    void* font = FontFor(flags);
    if (!Valid(plugin) || !text || !font || !ScreenSize(W, H)) return 0.0f;
    const float px = static_cast<float>(overlay::TextWidth(font, features::GameText(text, false, font, 1000).c_str()));
    return flags & PSE_UI_PIXELS ? px : px / W;
}

PSEResult PSE_CALL GetScreen(PSEUiScreen* out) {
    if (!out || out->size < sizeof(PSEUiScreen)) return PSE_E_INVALID_ARG;
    int W, H;
    if (!ScreenSize(W, H)) return PSE_E_NOT_FOUND;
    out->width = W;
    out->height = H;
    const auto* sheet = Global<const uint8_t*>(0x60EE18);   // the HUD art sheet's scale = the game's 2D scale
    out->hudScale = sheet && (*reinterpret_cast<const uint32_t*>(sheet + 0x20) & 1)
                        ? *reinterpret_cast<const float*>(sheet + 0x24) : 1.0f;
    out->lineHeight = FontLineStep(Global<void*>(kSmallFont));
    out->frontEnd = InFrontEnd() ? 1 : 0;
    return PSE_OK;
}

uint32_t PSE_CALL RegisterText(PSEPluginHandle plugin, const char* key, const char* text) {
    if (!Valid(plugin) || !key || !text) return 0;
    const uint32_t hash = g_texts.Register(plugin, key, text);
    if (hash) g_gameTexts[hash] = features::GameText(text, false, nullptr, 2000);
    else dslog::Write("[fail] pse.ui: %s - text \"%s\" not registered", pse::PluginName(plugin), key);
    return hash;
}

uint32_t PSE_CALL TextHash(PSEPluginHandle plugin, const char* key) {
    return Valid(plugin) && key ? g_texts.Hash(plugin, key) : 0;
}

// Shown from the next game frame that plays (dispatch mode [0x60EC8C] = 0), never from inside a level load: a plugin
// asking at PSE_EVENT_LEVEL_LOAD_END handed the game bar a text while the level's view controller was still being set
// up (access violation at 0x4CCF82 in the live tests).
struct PendingPopup {
    PSEPluginHandle plugin = 0;
    std::string text;
    uint32_t ms = 0;
};
std::vector<PendingPopup> g_pendingPopups;

void ShowNow(const PendingPopup& p) {
    void* controller = Global<void*>(kViewController);
    const bool game = controller && !InFrontEnd();
    if (game) {
        static std::string ring[16];
        static int next = 0;
        std::string& slot = ring[next++ % 16];
        slot = p.text;
        reinterpret_cast<void(__thiscall*)(void*, const char*, int)>(kPopupShow)(controller, slot.c_str(),
                                                                                static_cast<int>(p.ms));
    } else {
        g_popup = p.text;
        g_popupEnd = GetTickCount() + p.ms;
    }
    dslog::Write("pse.ui: %s pop-up \"%s\" (%u ms, %s)", pse::PluginName(p.plugin), p.text.c_str(), p.ms,
                 game ? "game bar" : "own bar");
}

int PSE_CALL PopupFrame(PSEEventId, void*, void*) {
    if (g_pendingPopups.empty() || Global<int8_t>(kDispatchMode) != 0) return 0;
    std::vector<PendingPopup> due;
    due.swap(g_pendingPopups);
    for (const PendingPopup& p : due) ShowNow(p);   // in order: the game bar keeps the newest
    return 0;
}

PSEResult PSE_CALL ShowPopup(PSEPluginHandle plugin, const char* text, uint32_t ms) {
    if (!Valid(plugin) || !text) return PSE_E_INVALID_ARG;
    if (g_pendingPopups.size() >= 16) g_pendingPopups.erase(g_pendingPopups.begin());
    g_pendingPopups.push_back({plugin, features::GameText(text, false, nullptr, 200), ms ? ms : 4000});
    return PSE_OK;
}

// ---- keyboard ----
PSEPluginHandle g_capture = 0;
std::deque<PSEUiKey> g_keys;
WNDPROC g_prevProc = nullptr;
HWND g_hooked = nullptr;

bool Capturing() {
    if (g_capture && !Valid(g_capture)) {   // the plugin failed / was unloaded: the game gets its keyboard back
        dslog::Write("pse.ui: keyboard released (plugin gone)");
        g_capture = 0;
        g_keys.clear();
    }
    return g_capture != 0;
}

LRESULT CALLBACK CaptureProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (Capturing()) {
        PSEUiKey k{sizeof(PSEUiKey), 0, 0, 0};
        switch (msg) {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                if (wp == VK_F4 && msg == WM_SYSKEYDOWN) break;   // Alt+F4 still closes the game
                k.type = PSE_UI_KEY_DOWN, k.code = static_cast<uint32_t>(wp), k.repeat = (lp >> 30) & 1;
                break;
            case WM_CHAR:
                k.type = PSE_UI_KEY_CHAR, k.code = static_cast<uint32_t>(wp);
                break;
            case WM_DEADCHAR:
                return 0;
            default: break;
        }
        if (k.type) {
            if (g_keys.size() >= kMaxKeys) g_keys.pop_front();
            g_keys.push_back(k);
            return 0;
        }
    }
    return CallWindowProcW(g_prevProc, wnd, msg, wp, lp);
}

PSEResult PSE_CALL CaptureKeyboard(PSEPluginHandle plugin, int on) {
    if (!Valid(plugin)) return PSE_E_INVALID_ARG;
    Capturing();
    if (!on) {
        if (g_capture == plugin) {
            g_capture = 0;
            g_keys.clear();
            dslog::Write("pse.ui: %s released the keyboard", pse::PluginName(plugin));
        }
        return PSE_OK;
    }
    if (g_capture && g_capture != plugin) return PSE_E_CONFLICT;
    HWND wnd = Global<HWND>(kGameWindow);
    if (!wnd) return PSE_E_WRONG_PHASE;
    if (g_hooked != wnd) {   // on top of every other subclass (the console's)
        g_prevProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&CaptureProc)));
        if (!g_prevProc) return PSE_E_ACCESS;
        g_hooked = wnd;
    }
    if (g_capture != plugin) {
        g_capture = plugin;
        g_keys.clear();
        dslog::Write("pse.ui: %s has the keyboard", pse::PluginName(plugin));
    }
    return PSE_OK;
}

int PSE_CALL ReadKey(PSEPluginHandle plugin, PSEUiKey* out) {
    if (!out || out->size < sizeof(PSEUiKey) || !Capturing() || g_capture != plugin || g_keys.empty()) return 0;
    *out = g_keys.front();
    g_keys.pop_front();
    return 1;
}

const PSEUiInterface kTable = {sizeof(PSEUiInterface), 1, &DrawRect, &DrawText, &DrawImage, &LoadImage, &TextWidth,
                               &GetScreen, &RegisterText, &TextHash, &ShowPopup, &CaptureKeyboard, &ReadKey};

void PSE_CALL OnLoaderMessage(const PSEMessage* msg, void*) {
    if (msg && msg->type == PSE_MSG_POST_LOAD) features::NotePluginSettingsAtStart();
}
}  // namespace

void features::RegisterPseUi() {
    static bool done = false;
    if (done) return;
    done = true;
    g_owner = pse_host::Find("dsfix.overlay");
    if (!g_owner) g_owner = pse_host::Core();
    const PSEMessagingInterface* m = pse::MessagingInterface();
    const bool ok = m && m->RegisterInterface(g_owner, PSE_UI_INTERFACE, 1, &kTable) == PSE_OK;
    if (m) m->RegisterListener(g_owner, "pse", &OnLoaderMessage, nullptr);
    if (m) m->Subscribe(g_owner, PSE_EVENT_FRAME, &PopupFrame, nullptr, 0);
    overlay::AddLateDraw(&LateDraw);
    dslog::Write(ok ? "[ok]   pse.ui 1: registered (drawing, texts, pop-ups, keyboard)"
                    : "[fail] pse.ui: interface not registered");
}

const char* features::PseUiText(uint32_t hash) {
    if (!pse_ui::TextTable::Ours(hash)) return nullptr;
    auto it = g_gameTexts.find(hash);
    return it == g_gameTexts.end() ? nullptr : it->second.c_str();
}

bool features::UiKeyboardCaptured() { return Capturing(); }

features::UiDebug features::PseUiDebug() {
    UiDebug d;
    const uint32_t now = FrameNow();
    for (const Item& i : g_items)
        if (i.frame == now) ++d.items, d.texts += i.kind == Kind::Text ? 1 : 0;
    d.popup = g_popup;
    d.popupsPending = static_cast<int>(g_pendingPopups.size());
    d.capture = Capturing() ? pse::PluginName(g_capture) : "";
    d.keysQueued = static_cast<int>(g_keys.size());
    d.textCount = static_cast<int>(g_gameTexts.size());
    return d;
}
