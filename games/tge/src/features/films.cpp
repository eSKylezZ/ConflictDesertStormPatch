// Bink films (tgefix.films): films from the Mods folder, and films played on request in the middle of a level.
//
// How GreatEscape.exe plays films (2026-10-25): every film is an entry of the level table (type 4: "FMV02", briefings
// "BRF01", the menu loop "Att" ...) loaded like a level by Screen_Load 0x5D0DE0, which builds "data\FMV\<name>.bik"
// (0x5D0FA3 / 0x5D102A) and hands it to the film object [0x7DC278] (vtable +4 = open(path), +0x10 start, +0x18 / +8
// stop + close; Main_FrameDispatch modes 2..7 drive it). Its open FUN_00499... sets the Bink sound system once
// (BinkSetSoundSystem(BinkOpenDirectSound, 0) at 0x499673, flag object +0x6C) and calls BinkOpen at 0x4996A4 as
// `call [0x86EF18]` - the no-CD stub's import dispatcher, which finds the real import from the return address (table
// in .mythoep: {import slot 0x5F5290 = _BinkOpen@8, return address 0x4996AA}). Missions never play Bink films (no
// level DLL names one); their mid-mission cutscenes are in-engine EMovies (Data\EMovies, API TriggerEMovie), not
// films.
//
//  - Mod films: the call at 0x4996A4 is replaced by our BinkOpen(path, flags): a mod file with the film's name
//    (FMV02.bik ...) is opened instead, else the game's path. The real BinkOpen comes from binkw32.dll by name (calling
//    through the dispatcher from here would look up our return address).
//  - Films on request (features::PlayFilm, Lua later; dev: Dev\PlayFilm = name): played inside a PSE_EVENT_FRAME, in a
//    loop of our own that presents the film until it ends or Esc / Space / Enter is pressed - the game is frozen
//    meanwhile (no update, no render) and gets no keys. Frames are copied with BinkCopyToBuffer into a managed
//    texture and drawn letterboxed over a black screen; the device state is saved / restored with a state block.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include "core/log.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "features/features.h"
#include "loader.h"

namespace {
constexpr uint32_t kBinkOpenSite = 0x4996A4;   // call dword ptr [0x86EF18] (no-CD import dispatcher)
constexpr uint32_t kDispatcherSlot = 0x86EF18;
constexpr uint32_t kRenderer = 0x79ED20;       // renderer*: +0x40530 IDirect3DDevice8*
constexpr uint32_t kGameWindow = 0x7DC238;

// binkw32 (RAD Bink 1.x, stdcall). HBINK starts {Width, Height, Frames, FrameNum, ...}.
struct Bink {
    uint32_t width, height, frames, frameNum;
};
using BinkOpenFn = Bink*(__stdcall*)(const char*, uint32_t);
using BinkHandleFn = int(__stdcall*)(Bink*);
using BinkCloseFn = void(__stdcall*)(Bink*);
using BinkCopyFn = int(__stdcall*)(Bink*, void*, int, uint32_t, uint32_t, uint32_t, uint32_t);
using BinkSurfaceTypeFn = int(__stdcall*)(void*);
using BinkSoundFn = int(__stdcall*)(void*, void*);
struct BinkApi {
    BinkOpenFn open = nullptr;
    BinkHandleFn doFrame = nullptr, wait = nullptr;
    BinkCloseFn nextFrame = nullptr, close = nullptr;
    BinkCopyFn copy = nullptr;
    BinkSurfaceTypeFn surfaceType = nullptr;
    BinkSoundFn setSoundSystem = nullptr;
    void* openDirectSound = nullptr;
    bool ok = false;
} g_bink;

bool LoadBink() {
    static bool tried = false;
    if (tried) return g_bink.ok;
    tried = true;
    HMODULE m = GetModuleHandleA("binkw32.dll");
    if (!m) m = LoadLibraryA("binkw32.dll");
    if (!m) return false;
    g_bink.open = reinterpret_cast<BinkOpenFn>(GetProcAddress(m, "_BinkOpen@8"));
    g_bink.doFrame = reinterpret_cast<BinkHandleFn>(GetProcAddress(m, "_BinkDoFrame@4"));
    g_bink.wait = reinterpret_cast<BinkHandleFn>(GetProcAddress(m, "_BinkWait@4"));
    g_bink.nextFrame = reinterpret_cast<BinkCloseFn>(GetProcAddress(m, "_BinkNextFrame@4"));
    g_bink.close = reinterpret_cast<BinkCloseFn>(GetProcAddress(m, "_BinkClose@4"));
    g_bink.copy = reinterpret_cast<BinkCopyFn>(GetProcAddress(m, "_BinkCopyToBuffer@28"));
    g_bink.surfaceType = reinterpret_cast<BinkSurfaceTypeFn>(GetProcAddress(m, "_BinkDX8SurfaceType@4"));
    g_bink.setSoundSystem = reinterpret_cast<BinkSoundFn>(GetProcAddress(m, "_BinkSetSoundSystem@8"));
    g_bink.openDirectSound = reinterpret_cast<void*>(GetProcAddress(m, "_BinkOpenDirectSound@4"));
    g_bink.ok = g_bink.open && g_bink.doFrame && g_bink.wait && g_bink.nextFrame && g_bink.close && g_bink.copy &&
                g_bink.surfaceType;
    return g_bink.ok;
}

const char* BaseName(const char* path) {
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    return base;
}

bool g_filmFills = false;

// The game's film open, at 0x4996A4.
Bink* __stdcall GameBinkOpen(const char* path, uint32_t flags) {
    if (!LoadBink()) return nullptr;
    // FilmsFill zooms only the main menu's background (FEPAL) and the story films (FMV*: their own letterbox bars go);
    // the legal screen, logos, chapter cards and briefings carry text up to the frame's edges and keep their side bars.
    const char* base = path ? BaseName(path) : "";
    g_filmFills = _strnicmp(base, "FE", 2) == 0 || _strnicmp(base, "FMV", 3) == 0;
    if (path && *path)
        if (const char* mod = features::ModFilePath(BaseName(path))) {
            dslog::Write("Films: %s from the Mods folder (%s)", BaseName(path), mod);
            if (Bink* b = g_bink.open(mod, flags)) return b;
            features::ModLog("[fail] Films: %s isn't a Bink film the game can open - the game's own film plays", mod);
        }
    return g_bink.open(path, flags);
}

}  // namespace
bool features::FilmFillsScreen() { return g_filmFills; }
namespace {

// ---- films on request ----
std::string g_pending;

void** Vt(void* com) { return *reinterpret_cast<void***>(com); }
template <class Fn, class... A>
auto Call(void* com, int slot, A... a) {
    return reinterpret_cast<Fn>(Vt(com)[slot])(com, a...);
}
enum : int {   // IDirect3DDevice8 / IDirect3DTexture8 vtable slots
    kTestCooperativeLevel = 3, kPresent = 15, kCreateTexture = 20, kBeginScene = 34, kEndScene = 35, kClear = 36,
    kSetViewport = 40, kSetRenderState = 50, kApplyStateBlock = 54, kDeleteStateBlock = 56, kCreateStateBlock = 57,
    kSetTexture = 61, kSetTextureStageState = 63, kDrawPrimitiveUP = 72, kSetVertexShader = 76,
    kRelease = 2, kGetSurfaceLevel = 15, kLockRect = 16, kUnlockRect = 17,
};

std::string FindFilm(const std::string& name) {
    std::string file = name;
    if (file.size() < 4 || _stricmp(file.c_str() + file.size() - 4, ".bik") != 0) file += ".bik";
    if (const char* mod = features::ModFilePath(BaseName(file.c_str()))) return mod;
    if (strchr(file.c_str(), ':')) return GetFileAttributesA(file.c_str()) != INVALID_FILE_ATTRIBUTES ? file : "";
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    dir.resize(dir.find_last_of("\\/") + 1);
    for (const std::string& p : {dir + "Data\\FMV\\" + file, dir + file})
        if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    return "";
}

uint32_t Pow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

struct LockedRect {
    INT Pitch;
    void* pBits;
};

struct Vertex {
    float x, y, z, rhw, u, v;
};

// Plays one film to its end (or until skipped); the game doesn't run meanwhile. false = couldn't play it.
bool Play(const std::string& name) {
    if (!LoadBink()) {
        dslog::Write("[fail] Films: binkw32.dll not found");
        return false;
    }
    const std::string path = FindFilm(name);
    if (path.empty()) {
        features::ModLog("[fail] Films: no film \"%s\" (Mods folder, Data\\FMV)", name.c_str());
        return false;
    }
    auto* renderer = *reinterpret_cast<uint8_t**>(kRenderer);
    void* dev = renderer ? *reinterpret_cast<void**>(renderer + 0x40530) : nullptr;
    if (!dev) return false;
    Bink* bink = g_bink.open(path.c_str(), 0);
    if (!bink) {
        features::ModLog("[fail] Films: %s isn't a Bink film", path.c_str());
        return false;
    }
    const uint32_t tw = Pow2(bink->width), th = Pow2(bink->height);
    void* tex = nullptr;
    if (FAILED((Call<HRESULT(__stdcall*)(void*, UINT, UINT, UINT, DWORD, UINT, UINT, void**)>(
            dev, kCreateTexture, tw, th, 1u, 0ul, 22u /* X8R8G8B8 */, 1u /* MANAGED */, &tex))) || !tex) {
        g_bink.close(bink);
        dslog::Write("[fail] Films: no %ux%u texture for %s", tw, th, path.c_str());
        return false;
    }
    int surfaceType = 3;   // BINKSURFACE32
    void* surface = nullptr;
    if (SUCCEEDED((Call<HRESULT(__stdcall*)(void*, UINT, void**)>(tex, kGetSurfaceLevel, 0u, &surface))) && surface) {
        surfaceType = g_bink.surfaceType(surface);
        Call<ULONG(__stdcall*)(void*)>(surface, kRelease);
    }
    DWORD block = 0;
    const bool haveBlock = SUCCEEDED((Call<HRESULT(__stdcall*)(void*, DWORD, DWORD*)>(dev, kCreateStateBlock, 1ul /* ALL */, &block)));
    const auto* pp = renderer + 0x84;   // present params: back buffer W, H
    const float W = static_cast<float>(*reinterpret_cast<const uint32_t*>(pp)), H = static_cast<float>(*reinterpret_cast<const uint32_t*>(pp + 4));
    const float scale = std::min(W / bink->width, H / bink->height);
    const float w = bink->width * scale, h = bink->height * scale, x0 = (W - w) / 2 - 0.5f, y0 = (H - h) / 2 - 0.5f;
    const float u1 = static_cast<float>(bink->width) / tw, v1 = static_cast<float>(bink->height) / th;
    const Vertex quad[4] = {{x0, y0, 0, 1, 0, 0}, {x0 + w, y0, 0, 1, u1, 0}, {x0, y0 + h, 0, 1, 0, v1}, {x0 + w, y0 + h, 0, 1, u1, v1}};
    dslog::Write("Films: playing %s (%ux%u, %u frames)", path.c_str(), bink->width, bink->height, bink->frames);
    HWND wnd = *reinterpret_cast<HWND*>(kGameWindow);
    bool skipped = false, quit = false, lost = false;
    const DWORD start = GetTickCount();
    bool done = false, framed = false;
    while (!done) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
                if (msg.wParam == VK_ESCAPE || msg.wParam == VK_SPACE || msg.wParam == VK_RETURN) skipped = true;
                continue;   // keys don't reach the game
            }
            if (msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP || msg.message == WM_CHAR) continue;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (quit || skipped || !IsWindow(wnd)) break;
        if ((Call<HRESULT(__stdcall*)(void*)>(dev, kTestCooperativeLevel)) != S_OK) {
            lost = true;   // alt-tabbed out of full screen: the game resets the device itself, the film ends
            break;
        }
        if (!g_bink.wait(bink)) {
            g_bink.doFrame(bink);
            LockedRect lr{};
            if (SUCCEEDED((Call<HRESULT(__stdcall*)(void*, UINT, LockedRect*, const RECT*, DWORD)>(tex, kLockRect, 0u, &lr, nullptr, 0ul)))) {
                g_bink.copy(bink, lr.pBits, lr.Pitch, bink->height, 0, 0, static_cast<uint32_t>(surfaceType));
                Call<HRESULT(__stdcall*)(void*, UINT)>(tex, kUnlockRect, 0u);
                framed = true;
            }
            if (bink->frameNum >= bink->frames) done = true;
            else g_bink.nextFrame(bink);
        }
        Call<HRESULT(__stdcall*)(void*, DWORD, const void*, DWORD, DWORD, float, DWORD)>(
            dev, kClear, 0ul, nullptr, 1ul /* TARGET */, 0xFF000000ul, 1.0f, 0ul);
        if (framed && SUCCEEDED((Call<HRESULT(__stdcall*)(void*)>(dev, kBeginScene)))) {
            const struct { DWORD x, y, w, h; float mn, mx; } vp{0, 0, static_cast<DWORD>(W), static_cast<DWORD>(H), 0, 1};
            Call<HRESULT(__stdcall*)(void*, const void*)>(dev, kSetViewport, &vp);
            for (const auto& rs : {std::pair<DWORD, DWORD>{7, 0} /* ZENABLE */, {14, 0} /* ZWRITE */, {27, 0} /* ALPHABLEND */,
                                   {15, 0} /* ALPHATEST */, {22, 1} /* CULL NONE */, {28, 0} /* FOG */, {137, 0} /* LIGHTING */})
                Call<HRESULT(__stdcall*)(void*, DWORD, DWORD)>(dev, kSetRenderState, rs.first, rs.second);
            Call<HRESULT(__stdcall*)(void*, DWORD, void*)>(dev, kSetTexture, 0ul, tex);
            for (const auto& ts : {std::pair<DWORD, DWORD>{1, 2} /* COLOROP SELECTARG1 */, {2, 2} /* COLORARG1 TEXTURE */,
                                   {4, 2} /* ALPHAOP SELECTARG1 */, {5, 2}, {16, 2} /* MAG LINEAR */, {17, 2} /* MIN LINEAR */,
                                   {18, 0} /* MIP NONE */, {13, 3} /* ADDRESSU CLAMP */, {14, 3}})
                Call<HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD)>(dev, kSetTextureStageState, 0ul, ts.first, ts.second);
            Call<HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD)>(dev, kSetTextureStageState, 1ul, 1ul, 1ul);   // stage 1 COLOROP DISABLE
            Call<HRESULT(__stdcall*)(void*, DWORD)>(dev, kSetVertexShader, 0x104ul /* XYZRHW | TEX1 */);
            Call<HRESULT(__stdcall*)(void*, DWORD, UINT, const void*, UINT)>(dev, kDrawPrimitiveUP, 5ul /* TRIANGLESTRIP */, 2u, quad,
                                                                             static_cast<UINT>(sizeof(Vertex)));
            Call<HRESULT(__stdcall*)(void*)>(dev, kEndScene);
        }
        Call<HRESULT(__stdcall*)(void*, const RECT*, const RECT*, HWND, const void*)>(dev, kPresent, nullptr, nullptr, nullptr, nullptr);
        Sleep(1);
    }
    g_bink.close(bink);
    Call<HRESULT(__stdcall*)(void*, DWORD, void*)>(dev, kSetTexture, 0ul, nullptr);
    Call<ULONG(__stdcall*)(void*)>(tex, kRelease);
    if (haveBlock) {
        Call<HRESULT(__stdcall*)(void*, DWORD)>(dev, kApplyStateBlock, block);
        Call<HRESULT(__stdcall*)(void*, DWORD)>(dev, kDeleteStateBlock, block);
    }
    dslog::Write("Films: %s %s after %.1f s", BaseName(path.c_str()),
                 quit ? "stopped (the game is closing)" : lost ? "stopped (display device lost)" : skipped ? "skipped" : "ended",
                 (GetTickCount() - start) / 1000.0);
    return true;
}

#ifndef DS_DIST
void CheckDevRequest() {
    static int frame = 0;
    if (++frame % 30) return;
    char name[260] = {};
    DWORD size = sizeof name;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\GreatEscapeFix\\Dev", "PlayFilm", RRF_RT_REG_SZ, nullptr, name, &size) !=
            ERROR_SUCCESS || !*name)
        return;
    RegDeleteKeyValueA(HKEY_CURRENT_USER, "Software\\GreatEscapeFix\\Dev", "PlayFilm");
    g_pending = name;
}
#endif

int PSE_CALL OnFrame(PSEEventId, void*, void*) {
#ifndef DS_DIST
    CheckDevRequest();
#endif
    if (g_pending.empty()) return 0;
    const std::string name = g_pending;
    g_pending.clear();
    Play(name);
    return 0;
}
}  // namespace

void features::PlayFilm(const char* name) {
    if (name && *name) g_pending = name;
}

void features::ApplyFilms() {
    if (!patch::HookIndirectCall(kBinkOpenSite, reinterpret_cast<const void*>(&GameBinkOpen), kDispatcherSlot))
        dslog::Write("[fail] Films: film open at 0x%08X not recognised - films from the Mods folder aren't used", kBinkOpenSite);
    if (const PSEMessagingInterface* m = pse::MessagingInterface())
        m->Subscribe(pse_host::Find("tgefix.films"), PSE_EVENT_FRAME, &OnFrame, nullptr, 0);
    dslog::Write("[ok]   Films: Mods folder films, films on request");
}
