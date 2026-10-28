// Menu / HUD scaling (tgefix.hud): the GUI (GUI.cpp, CGUI* classes) is laid out for a 640 x 480 design and never
// scaled - at 1440p the menus and the HUD are tiny. How it places things (2026-10-24):
//  - CGUI::Load 0x4BA870 (thiscall(gui, name) ret 4) reads the renderer's size (+0x40690 / +0x40694) and the file's
//    design size, and sets [0x7AB304] = W / 640, [0x7AB300] = H / 480 (scale) and [0x7AB2FC] = W - 640, [0x7AB2F8] =
//    H - 480 (extra space); each control then gets its place from those (0x4BE930: flag 0x40 = centred, i.e. shifted by
//    half the extra space; no flag = stretched); the GUI's screen rectangle comes from 0x4C19F0 = (0, 0, W, H). Controls
//    keep their design pixel size - only their position moves with the screen.
//  - every control draws inside CGUIControl::Render 0x4BE510 (thiscall, 5 args, ret 0x14; a virtual method of every
//    control class, derived draws run inside it) - images 0x44AF60, frames, text - into the 2D draw 0x446F00 (fan of
//    pretransformed FVF 0x144 vertices), which runs the engine's 2D transform 0x443450 (menu transitions) first
//    (call at 0x446F1D). The menu cursor is the one GUI image drawn outside a control render (0x5CCB11).
// So, the virtual-screen approach DesertStorm2Fix uses (games/ds2/src/features/frontend.cpp): while a GUI loads or a
// control renders, the renderer size reads as (W / k) x (H / k) - the GUI lays itself out for that smaller screen - and
// the vertices those controls draw are multiplied by k after the game's own 2D transform. Everything else (the 3D world,
// films, fades, world sprites) keeps real pixels. k = max(1, min(W / 800, H / 600)) x HudScalePercent / 100, like DS1 /
// DS2: 800 x 600 looks as before. The menu cursor (display.cpp's mouse mapping) goes through the same mapping backwards
// (GuiToVirtual), so the GUI's hit tests still match. The objectives notebook has a layout of its own (below).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kRenderer = 0x79ED20;                 // renderer*: +0x40690 W, +0x40694 H (what the GUI reads)
constexpr uint32_t kRendererW = 0x40690, kRendererH = 0x40694, kRendererFvf = 0x409A4;
constexpr uint32_t kGuiLoad = 0x4BA870;
constexpr uint8_t kGuiLoadPrologue[] = {0x83, 0xEC, 0x68, 0x8B, 0x54, 0x24, 0x6C};        // sub esp, 0x68; mov edx, [esp+0x6C]
constexpr uint32_t kControlRender = 0x4BE510;
constexpr uint8_t kControlRenderPrologue[] = {0x55, 0x56, 0x8B, 0xF1, 0x66, 0x8B, 0x96, 0x80, 0x00, 0x00, 0x00};
constexpr uint32_t kTransformCall = 0x446F1D, kTransform = 0x443450;
constexpr uint32_t kBookPlace = 0x58CF60, kBookPlaceCalls[] = {0x58D9EF, 0x58E91A};   // the notebook's book (below)
// The menu cursor: image 0x56 of the sheet [0x7DC2F0] at the cursor globals (0x7A9D20 / 0x7A9510 - virtual coordinates
// with the mouse mapping) through FUN_0044AC60 thiscall(sheet, image, x, y) ret 0xC, outside any control render.
constexpr uint32_t kCursorDrawCall = 0x5CCB11, kImageDraw = 0x44AC60;
// The TV-safe area (0x4BA600: 7.5 % in from each edge, via 0x4BA510) is taken from the renderer's CURRENT VIEWPORT
// (getter 0x4413B0 = renderer +0x40670, called at 0x4BA519), not from the size the callers pass. Screens with the "fit"
// flag (0x80, the mission HUD) place their top-level controls in that area (0x4BDF50 from the layout 0x4BE930); from the
// real 1920 x 1080 viewport the action icon, pickup picture, compass and health bar landed off the virtual screen. In a
// scope the getter answers the virtual screen. The GUI's own copy (GUI +0x448, made right after the GUI by 0x58F389 ->
// 0x4BAD80 -> 0x4BAD40) is made in a scope for the same reason.
constexpr uint32_t kGuiResetCall = 0x58F389, kGuiReset = 0x4BAD80;
constexpr uint32_t kSafeViewportCall = 0x4BA519, kViewportGetter = 0x4413B0;

using GuiLoadFn = void(__thiscall*)(void*, const char*);
using RenderFn = int(__thiscall*)(void*, int, int, int, int, int);
using TransformFn = void(__thiscall*)(void*, uint8_t*, int);
using ImageDrawFn = int(__thiscall*)(void*, int, int, int);
GuiLoadFn g_guiLoad = nullptr;
RenderFn g_render = nullptr;
TransformFn g_transform = reinterpret_cast<TransformFn>(kTransform);

bool g_on = false;

uint8_t* Renderer() { return *reinterpret_cast<uint8_t**>(kRenderer); }
int& W(uint8_t* r) { return *reinterpret_cast<int*>(r + kRendererW); }
int& H(uint8_t* r) { return *reinterpret_cast<int*>(r + kRendererH); }

float FactorFor(int w, int h) {
    const float base = std::max(1.0f, std::min(w / 800.0f, h / 600.0f));
    return std::max(0.5f, base * static_cast<float>(features::Get().hudScalePercent) / 100.0f);
}

// A layout the GUI is drawn in: it sees a vw x vh screen, its vertices become x * k + ox, y * k + oy.
struct Mode {
    float k = 1.0f, ox = 0.0f, oy = 0.0f;
    int vw = 0, vh = 0;
    bool notebook = false;
};
Mode NormalMode(int w, int h) {
    Mode m;
    m.k = FactorFor(w, h);
    m.vw = static_cast<int>(w / m.k + 0.5f);
    m.vh = static_cast<int>(h / m.k + 0.5f);
    return m;
}

// The objectives notebook (Notebook.gui, Tab in a mission) is a 3D book with GUI pages laid onto it, designed for 4:3:
// at 800 x 600 the book fills most of the screen and the text sits on its pages, while on a wide screen the game's own
// layout comes apart (the book follows the height, the page text the 640 x 480 design centred in the width: the text
// slid off the pages, the map past the book's edge). So the notebook is laid out on a centred 4:3 screen of 800 x 600
// scaled to the height: Notebook.gui loads for 800 x 600, its screens draw in that mode, and the book is drawn into the
// same 4:3 part of the screen (D3D viewport). Its screens = the ones its load adds to the GUI's screen list (GUI
// [0x7D8EC4] +0x8C: +8 array, +0xC count; lookup 0x4BB0C0): 0x82B9E96B (the book opening), 0x8C949DE9 (Objectives),
// ... The screen being drawn = GUI +0xA4 (CGUI::Render points it at each screen it draws; between frames = the top one).
constexpr uint32_t kGui = 0x7D8EC4;
const void* g_notebookScreens[24] = {};
int g_notebookScreenCount = 0;
struct ScreenList { uint32_t pad[2]; void** items; uint32_t count; };   // GUI +0x8C
ScreenList* Screens(void* gui) { return reinterpret_cast<ScreenList*>(static_cast<uint8_t*>(gui) + 0x8C); }
// Every control of those screens (screen +0x3C = the screen's control list {+4 array, +8 count}, what FindControl
// 0x4C1CB0 walks): while the notebook opens, GUI +0xA4 still names the screen before it for a few frames, so the pages
// drew in the wide layout and slid into place - a control of the notebook draws in its layout whatever +0xA4 says.
const void* g_notebookControls[1024] = {};
int g_notebookControlCount = 0;
void CollectNotebookControls() {
    g_notebookControlCount = 0;
    __try {
        for (int i = 0; i < g_notebookScreenCount; ++i) {
            const auto* list = static_cast<const uint8_t*>(g_notebookScreens[i]) + 0x3C;
            void* const* items = *reinterpret_cast<void* const* const*>(list + 4);
            const uint32_t n = *reinterpret_cast<const uint32_t*>(list + 8);
            for (uint32_t k = 0; items && k < n && k < 512 && g_notebookControlCount < 1024; ++k)
                g_notebookControls[g_notebookControlCount++] = items[k];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_notebookControlCount = 0;
    }
}
bool NotebookControl(const void* control) {
    for (int i = 0; control && i < g_notebookControlCount; ++i)
        if (g_notebookControls[i] == control) return true;
    return false;
}
bool NotebookScreen() {
    __try {
        const auto* gui = *reinterpret_cast<const uint8_t* const*>(kGui);
        const void* screen = gui ? *reinterpret_cast<const void* const*>(gui + 0xA4) : nullptr;
        for (int i = 0; screen && i < g_notebookScreenCount; ++i)
            if (g_notebookScreens[i] == screen) return true;
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
Mode NotebookMode(int w, int h) {
    Mode m;
    m.notebook = true;
    m.vw = 800, m.vh = 600;
    m.k = std::min(w / 800.0f, h / 600.0f);
    m.ox = (w - 800.0f * m.k) * 0.5f;
    m.oy = (h - 600.0f * m.k) * 0.5f;
    return m;
}

// CGUI::Load's screen mapping globals [0x7AB2F8] H - 480, [0x7AB2FC] W - 640, [0x7AB300] H / 480, [0x7AB304] W / 640:
// controls read them when they draw (0x4BE930) and they hold the LAST load's values. The notebook's are kept from its
// load and swapped in while a notebook screen draws.
constexpr uint32_t kGuiMapping = 0x7AB2F8;
uint32_t g_notebookMapping[4] = {};
bool g_haveNotebookMapping = false;
void SwapMapping() {
    auto* m = reinterpret_cast<uint32_t*>(kGuiMapping);
    for (int i = 0; i < 4; ++i) std::swap(m[i], g_notebookMapping[i]);
}

int g_depth = 0;          // inside a GUI load / control render / cursor or book draw, nests
int g_realW = 0, g_realH = 0;
Mode g_mode;              // the innermost scope's

// Enter / leave a layout: the renderer's size reads as the mode's virtual screen; the previous one comes back after.
struct Scope {
    uint8_t* r = nullptr;
    Mode prev;
    bool swapped = false;
    explicit Scope(bool notebook, bool swapMapping = true) {
        if (!g_on || !(r = Renderer())) return;
        if (g_depth == 0) g_realW = W(r), g_realH = H(r);
        const bool wasNotebook = g_depth > 0 && g_mode.notebook;
        prev = g_mode;
        g_mode = notebook ? NotebookMode(g_realW, g_realH) : NormalMode(g_realW, g_realH);
        ++g_depth;
        W(r) = g_mode.vw, H(r) = g_mode.vh;
        if (swapMapping && notebook != wasNotebook && g_haveNotebookMapping) SwapMapping(), swapped = true;
    }
    ~Scope() {
        if (!r) return;
        if (swapped) SwapMapping();
        g_mode = prev;
        if (--g_depth == 0) W(r) = g_realW, H(r) = g_realH;
        else W(r) = g_mode.vw, H(r) = g_mode.vh;
    }
};

void __fastcall GuiLoad(void* gui, void*, const char* name) {
    if (g_on && name && _stricmp(name, "Notebook.gui") == 0) {
        const uint32_t before = gui ? Screens(gui)->count : 0;
        // The load writes the mapping globals for the notebook's 800 x 600; the HUD and every other GUI read the same
        // globals when they draw, so the values from before the load come back after it (else the mission HUD - compass,
        // health bar, action icon, current item - was placed for 800 x 600: the bottom ones off the screen).
        uint32_t before4[4];
        memcpy(before4, reinterpret_cast<const void*>(kGuiMapping), sizeof before4);
        {
            Scope s(true, false);   // a load writes the mapping itself: kept from it, no swap around it
            g_guiLoad(gui, name);
            memcpy(g_notebookMapping, reinterpret_cast<const void*>(kGuiMapping), sizeof g_notebookMapping);
            g_haveNotebookMapping = true;
        }
        memcpy(reinterpret_cast<void*>(kGuiMapping), before4, sizeof before4);
        g_notebookScreenCount = 0;
        if (ScreenList* list = gui ? Screens(gui) : nullptr)
            for (uint32_t i = before; i < list->count && g_notebookScreenCount < 24; ++i)
                g_notebookScreens[g_notebookScreenCount++] = list->items[i];
        CollectNotebookControls();
        dslog::Write("HUD: Notebook.gui laid out on a 4:3 screen (%d screens, %d controls)", g_notebookScreenCount,
                     g_notebookControlCount);
        return;
    }
    Scope s(false);
    g_guiLoad(gui, name);
    if (s.r && g_depth == 1)
        dslog::Write("HUD: %s laid out for %dx%d (x%.2f of %dx%d), gui %p", name ? name : "?", W(s.r), H(s.r), g_mode.k,
                     g_realW, g_realH, gui);
}

int __fastcall ControlRender(void* control, void*, int a, int b, int c, int d, int e) {
    // a child draws inside its parent's render: it keeps the parent's layout
    Scope s(NotebookControl(control) || (g_depth > 0 && g_mode.notebook) || NotebookScreen());
    return g_render(control, a, b, c, d, e);
}

struct SafeViewport8 { uint32_t x, y, w, h; float minZ, maxZ; };   // D3DVIEWPORT8
SafeViewport8 g_safeViewport{};

const void* __fastcall SafeViewport(void* renderer, void*) {
    const void* real = reinterpret_cast<const void*(__thiscall*)(void*)>(kViewportGetter)(renderer);
    if (g_depth <= 0 || g_mode.vw <= 0 || g_mode.vh <= 0) return real;
    g_safeViewport = {0, 0, static_cast<uint32_t>(g_mode.vw), static_cast<uint32_t>(g_mode.vh), 0.0f, 1.0f};
    return &g_safeViewport;
}

void __fastcall GuiReset(void* gui, void*) {
    Scope s(false);
    reinterpret_cast<void(__thiscall*)(void*)>(kGuiReset)(gui);
}

int __fastcall CursorDraw(void* sheet, void*, int image, int x, int y) {
    Scope s(NotebookScreen());
    return reinterpret_cast<ImageDrawFn>(kImageDraw)(sheet, image, x, y);
}

// The book: 0x58CF60 (input ebx = the book control [0x7DC0A8], no stack args) fits the model into the control's
// rectangle with a projection made from the rectangle over the renderer size; its two callers (0x58D9EF, 0x58E91A) run
// outside any control render. It runs in the notebook's mode, drawn into the 4:3 part of the screen: the device's
// viewport (IDirect3DDevice8 SetViewport 40 / GetViewport 41; renderer +0x40530 = the device) and the renderer's
// current-viewport pointer +0x40670 (read by 0x4413B0: the projection 0x44E540 takes its aspect from it - with the
// full 16:9 viewport the book came out 1.33 times too narrow).
struct Viewport8 { DWORD x, y, w, h; float minZ, maxZ; };
Scope* g_bookScope = nullptr;
Viewport8 g_bookViewport = {}, g_bookArea = {};
void* g_bookDevice = nullptr;
void* g_bookOldCurrent = nullptr;
template <class T> T VSlot(void* obj, int slot) { return reinterpret_cast<T>((*reinterpret_cast<void***>(obj))[slot]); }
using GetViewportFn = HRESULT(__stdcall*)(void*, Viewport8*);
using SetViewportFn = HRESULT(__stdcall*)(void*, const Viewport8*);
void __cdecl BookEnter() {
    g_bookScope = new Scope(true);
    g_bookDevice = nullptr;
    uint8_t* r = Renderer();
    void* dev = r ? *reinterpret_cast<void**>(r + 0x40530) : nullptr;
    if (!g_on || !dev || VSlot<GetViewportFn>(dev, 41)(dev, &g_bookViewport) < 0) return;
    Viewport8 v = g_bookViewport;
    v.x = static_cast<DWORD>(g_mode.ox + 0.5f), v.y = static_cast<DWORD>(g_mode.oy + 0.5f);
    v.w = static_cast<DWORD>(800.0f * g_mode.k + 0.5f), v.h = static_cast<DWORD>(600.0f * g_mode.k + 0.5f);
    if (VSlot<SetViewportFn>(dev, 40)(dev, &v) < 0) return;
    g_bookDevice = dev;
    g_bookArea = v;
    g_bookOldCurrent = *reinterpret_cast<void**>(r + 0x40670);
    *reinterpret_cast<void**>(r + 0x40670) = &g_bookArea;
}
void __cdecl BookLeave() {
    if (g_bookDevice) {
        VSlot<SetViewportFn>(g_bookDevice, 40)(g_bookDevice, &g_bookViewport);
        if (uint8_t* r = Renderer()) *reinterpret_cast<void**>(r + 0x40670) = g_bookOldCurrent;
    }
    g_bookDevice = nullptr;
    delete g_bookScope;
    g_bookScope = nullptr;
}
__declspec(naked) void BookStub() {
    __asm {
        push ecx               // (ecx / ebx as the caller left them: ebx is 0x58CF60's input)
        call BookEnter
        pop ecx
        mov eax, 0x58CF60      // kBookPlace (literals: a constexpr in __asm is a memory operand)
        call eax
        push eax
        call BookLeave
        pop eax
        ret
    }
}

// Loading screen "LOADING" cover (2026-10-25): once a level has loaded, 0x5843A0 (input edi = the loading screen object,
// called from 0x5D2A1A / 0x5D2A29 after Level_Init and from 0x58739B) paints a black box over its LOADING control
// (control [edi +0x84]; rect from its getters +0xBC / +0xB4 / +0xA0 / +0x9C, drawn by 0x461730) before "Please Press
// Any Key" goes on top. It runs outside any control render, so the box came out in virtual coordinates on the real
// screen - a black bar over the briefing art and LOADING left showing under the prompt. The call runs in a scope.
constexpr uint32_t kLoadCover = 0x5843A0, kLoadCoverCalls[] = {0x5D2A1A, 0x5D2A29, 0x58739B};
Scope* g_coverScope = nullptr;
void __cdecl CoverEnter() { g_coverScope = new Scope(false); }
void __cdecl CoverLeave() {
    delete g_coverScope;
    g_coverScope = nullptr;
}
__declspec(naked) void LoadCoverStub() {
    __asm {
        pushad                 // edi is 0x5843A0's input
        call CoverEnter
        popad
        mov eax, 0x5843A0      // kLoadCover
        call eax
        pushad
        call CoverLeave
        popad
        ret
    }
}

// The 2D draw's transform call: the game's transform first, then (inside a scope) virtual -> real pixels.
// The game's 2D animation (the notebook opening: pages growing with the book) runs every quad through a matrix first:
// renderer +0x409BC on and +0x409C0 bits 3 -> object [renderer +0x406D0] (applied by 0x43D840: scale +0x00 / +0x14,
// translation +0x30 / +0x34). The game sets it to scale about the REAL screen's centre (translation = centre x
// (1 - scale): 632.9 / 356.0 at scale 0.011 on 1280 x 720), while inside a scope the quads are in the layout's virtual
// screen - the notebook's 800 x 600 - so the pages grew from the wrong point and swooped in from the bottom right. For
// the draw the translation is re-centred on the virtual screen's centre.
void __fastcall Transform(void* renderer, void*, uint8_t* verts, int count) {
    auto* r = static_cast<uint8_t*>(renderer);
    float* matrix = nullptr;
    float saved[2] = {};
    if (g_depth > 0 && *reinterpret_cast<uint32_t*>(r + 0x409BC) && (*reinterpret_cast<uint32_t*>(r + 0x409C0) & 3))
        matrix = *reinterpret_cast<float**>(r + 0x406D0);
    if (matrix) {
        const float sx = matrix[0], sy = matrix[5];
        saved[0] = matrix[12], saved[1] = matrix[13];
        const float realX = g_realW * 0.5f, realY = g_realH * 0.5f;
        // only a scale about the real centre is moved (one about the virtual centre is right already)
        if (sx < 0.999f && std::fabs(saved[0] - realX * (1 - sx)) < 2.0f && std::fabs(saved[1] - realY * (1 - sy)) < 2.0f) {
            matrix[12] = g_mode.vw * 0.5f * (1 - sx);
            matrix[13] = g_mode.vh * 0.5f * (1 - sy);
        } else {
            matrix = nullptr;
        }
    }
    g_transform(renderer, verts, count);
    if (matrix) matrix[12] = saved[0], matrix[13] = saved[1];
    if (g_depth <= 0 || !verts || *reinterpret_cast<int*>(static_cast<uint8_t*>(renderer) + kRendererFvf) != 0x144) return;
    for (int i = 0; i < count; ++i) {
        float* p = reinterpret_cast<float*>(verts + i * 0x1C);
        p[0] = p[0] * g_mode.k + g_mode.ox;
        p[1] = p[1] * g_mode.k + g_mode.oy;
    }
}
}  // namespace

void features::GuiToVirtual(long& x, long& y) {
    uint8_t* r = Renderer();
    if (!g_on || !r) return;
    const int w = g_depth > 0 ? g_realW : W(r), h = g_depth > 0 ? g_realH : H(r);
    const Mode m = NotebookScreen() ? NotebookMode(w, h) : NormalMode(w, h);
    if (m.k <= 0.0f) return;
    x = static_cast<long>((x - m.ox) / m.k);
    y = static_cast<long>((y - m.oy) / m.k);
}

float features::GuiDrawScale() { return g_on && g_depth > 0 ? g_mode.k : 1.0f; }

void features::ApplyGuiScale() {
    if (!Get().hudScaling) {
        dslog::Write("[off]  HUD: menus and HUD at the game's pixel size (HudScaling)");
        return;
    }
    const bool load = patch::HookFunction(kGuiLoad, reinterpret_cast<const void*>(&GuiLoad),
                                          reinterpret_cast<void**>(&g_guiLoad), kGuiLoadPrologue, sizeof kGuiLoadPrologue);
    const bool render = patch::HookFunction(kControlRender, reinterpret_cast<const void*>(&ControlRender),
                                            reinterpret_cast<void**>(&g_render), kControlRenderPrologue,
                                            sizeof kControlRenderPrologue);
    const bool draw = patch::HookCall(kTransformCall, reinterpret_cast<const void*>(&Transform), kTransform);
    const bool cursor = patch::HookCall(kCursorDrawCall, reinterpret_cast<const void*>(&CursorDraw), kImageDraw);
    const bool reset = patch::HookCall(kGuiResetCall, reinterpret_cast<const void*>(&GuiReset), kGuiReset) &&
                       patch::HookCall(kSafeViewportCall, reinterpret_cast<const void*>(&SafeViewport), kViewportGetter);
    for (uint32_t site : kLoadCoverCalls)
        if (!patch::HookCall(site, reinterpret_cast<const void*>(&LoadCoverStub), kLoadCover))
            dslog::Write("[fail] HUD: loading-screen call at 0x%08X not recognised - LOADING may show under the prompt",
                         site);
    for (uint32_t site : kBookPlaceCalls)
        if (!patch::HookCall(site, reinterpret_cast<const void*>(&BookStub), kBookPlace))
            dslog::Write("[fail] HUD: notebook book call at 0x%08X not recognised - the book won't match its pages", site);
    if (!cursor) dslog::Write("[fail] HUD: menu cursor draw at 0x%08X not recognised - the arrow won't follow the mouse",
                              kCursorDrawCall);
    g_on = load && render && draw && reset;
    if (!g_on) {   // all or nothing: a GUI laid out small but drawn unscaled (or the reverse) would be worse
        if (load) patch::UnhookFunction(kGuiLoad, reinterpret_cast<const void*>(&GuiLoad));
        if (render) patch::UnhookFunction(kControlRender, reinterpret_cast<const void*>(&ControlRender));
        if (draw) patch::HookCall(kTransformCall, reinterpret_cast<const void*>(kTransform), kTransform);
        if (reset) patch::HookCall(kGuiResetCall, reinterpret_cast<const void*>(kGuiReset), kGuiReset),
                   patch::HookCall(kSafeViewportCall, reinterpret_cast<const void*>(kViewportGetter), kViewportGetter);
        dslog::Write("[fail] HUD: scaling hooks not all in (load %d, render %d, draw %d, safe area %d) - left off", load,
                     render, draw, reset);
        return;
    }
    dslog::Write("[ok]   HUD: menus and HUD scaled with the resolution (%u%%)", Get().hudScalePercent);
}
