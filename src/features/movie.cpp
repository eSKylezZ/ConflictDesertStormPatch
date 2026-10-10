// Scripted 2D films for Lua missions (an opening title sequence, a gun-barrel intro ...): the script draws images and
// shapes over the game every frame from its update() and can play a WAV soundtrack, freeze the world meanwhile and
// read a skip key. Everything is drawn through the overlay queue (overlay.cpp: drawn at EndScene, on top of the game
// and its HUD) in screen fractions, so a film looks the same at every resolution.
//
// Lua (registered into `game` by scripthost.cpp):
//   game.image(name)                       -> handle or nil   (a .png / .dds of the mod; cached)
//   game.draw(h, x, y, w, h [, alpha, rot, argb])   x, y = centre, w, h = size; fractions of the screen HEIGHT for
//                                                   both axes (so circles stay round), x measured from the screen
//                                                   centre; rot in degrees; argb tint (default white)
//   game.draw_part(h, x, y, w, h, u0, v0, u1, v1 [, alpha, argb])   a region of the image (sprite sheets)
//   game.fill(x0, y0, x1, y1, argb)       a rectangle in the same units (x from the centre, y from the top)
//   game.screen()                          -> half width, height of the screen in those units (e.g. 0.889, 1)
//   game.music(name) / game.music()        plays a .wav of the mod (async) / stops it
//   game.clock()                           real seconds (keeps running while the world is frozen)
//   game.film(true, draw) / game.film(false)   world frozen, player input off and HUD hidden while on; draw() is
//                                          called once per rendered frame to draw the film (it may end it)
//   game.skip_pressed()                    Esc / Enter / Space / pad Cross pressed since the last call
#include <windows.h>
#include <mmsystem.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <string>

extern "C" {
#include "../../third_party/lua/lauxlib.h"
#include "../../third_party/lua/lua.h"
}

#include "core/log.h"
#include "features/features.h"
#include "features/overlay.h"

namespace {
constexpr uint32_t kRenderer = 0x63C924, kWidthOff = 0x40688, kHeightOff = 0x4068C;
constexpr uint32_t kTimeMultiplier = 0x5F7E64;  // float
constexpr uint32_t kBlocks = 0x60F5B8, kBlockSize = 0x478, kBlockDevices = 0x38, kBlockCount = 0x610798;

struct Image {
    void* texture = nullptr;
    float u1 = 1, v1 = 1;
    void* device = nullptr;
};
std::map<std::string, Image> g_images;
bool g_film = false;
float g_savedMultiplier = 1.0f;
int g_savedDevices[4] = {};
bool g_prevSkip = true;   // held when the film starts = not a press
std::string g_music;

bool ScreenSize(float& W, float& H) {
    auto* r = *reinterpret_cast<uint8_t**>(kRenderer);
    if (!r) return false;
    W = static_cast<float>(*reinterpret_cast<int*>(r + kWidthOff));
    H = static_cast<float>(*reinterpret_cast<int*>(r + kHeightOff));
    return W > 0 && H > 0;
}

Image* Get(lua_State* L, int arg) {
    const char* name = luaL_checkstring(L, arg);
    auto it = g_images.find(name);
    if (it == g_images.end() || !it->second.texture || it->second.device != overlay::CurrentDevice()) return nullptr;
    return &it->second;
}

uint32_t Tint(lua_State* L, int alphaArg, int argbArg) {
    uint32_t argb = lua_isinteger(L, argbArg) ? static_cast<uint32_t>(lua_tointeger(L, argbArg)) : 0xFFFFFFFF;
    double a = luaL_optnumber(L, alphaArg, 1.0);
    a = a < 0 ? 0 : a > 1 ? 1 : a;
    return (static_cast<uint32_t>(((argb >> 24) & 0xFF) * a) << 24) | (argb & 0xFFFFFF);
}

int LuaImage(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    Image& img = g_images[name];
    if (!img.texture || img.device != overlay::CurrentDevice()) {
        const std::string path = features::ModFilePath(name);
        int w = 0, h = 0, tw = 0, th = 0;
        img.texture = path.empty() ? nullptr : overlay::LoadTextureFile(path.c_str(), w, h, tw, th);
        img.device = overlay::CurrentDevice();
        if (!img.texture) {
            features::ModLog("[fail] script image %s: not found or unreadable", name);
            lua_pushnil(L);
            return 1;
        }
        img.u1 = static_cast<float>(w) / tw, img.v1 = static_cast<float>(h) / th;
    }
    lua_pushstring(L, name);
    return 1;
}

void Quad(void* texture, float cx, float cy, float w, float h, float rot, float u0, float v0, float u1, float v1,
          uint32_t argb) {
    float W, H;
    if (!ScreenSize(W, H)) return;
    const float px = W * 0.5f + cx * H, py = cy * H, hw = w * H * 0.5f, hh = h * H * 0.5f;
    const float c = std::cos(rot * 3.14159265f / 180), s = std::sin(rot * 3.14159265f / 180);
    const float local[4][2] = {{-hw, -hh}, {hw, -hh}, {-hw, hh}, {hw, hh}};
    float corners[4][2];
    for (int k = 0; k < 4; ++k) {
        corners[k][0] = px + local[k][0] * c - local[k][1] * s;
        corners[k][1] = py + local[k][0] * s + local[k][1] * c;
    }
    overlay::QueueScreenQuad(texture, corners, u0, v0, u1, v1, argb);
}

int LuaDraw(lua_State* L) {
    Image* img = Get(L, 1);
    if (!img) return 0;
    Quad(img->texture, static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)),
         static_cast<float>(luaL_checknumber(L, 4)), static_cast<float>(luaL_checknumber(L, 5)),
         static_cast<float>(luaL_optnumber(L, 7, 0)), 0, 0, img->u1, img->v1, Tint(L, 6, 8));
    return 0;
}

int LuaDrawPart(lua_State* L) {
    Image* img = Get(L, 1);
    if (!img) return 0;
    const float u0 = static_cast<float>(luaL_checknumber(L, 6)), v0 = static_cast<float>(luaL_checknumber(L, 7));
    const float u1 = static_cast<float>(luaL_checknumber(L, 8)), v1 = static_cast<float>(luaL_checknumber(L, 9));
    Quad(img->texture, static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)),
         static_cast<float>(luaL_checknumber(L, 4)), static_cast<float>(luaL_checknumber(L, 5)), 0, u0 * img->u1,
         v0 * img->v1, u1 * img->u1, v1 * img->v1, Tint(L, 10, 11));
    return 0;
}

int LuaFill(lua_State* L) {
    float W, H;
    if (!ScreenSize(W, H)) return 0;
    const float x0 = static_cast<float>(luaL_checknumber(L, 1)), y0 = static_cast<float>(luaL_checknumber(L, 2));
    const float x1 = static_cast<float>(luaL_checknumber(L, 3)), y1 = static_cast<float>(luaL_checknumber(L, 4));
    const uint32_t argb = static_cast<uint32_t>(luaL_checkinteger(L, 5));
    overlay::QueueScreenRect(W * 0.5f + x0 * H, y0 * H, (x1 - x0) * H, (y1 - y0) * H, argb);
    return 0;
}

int LuaScreen(lua_State* L) {
    float W = 4, H = 3;
    ScreenSize(W, H);
    lua_pushnumber(L, W / H * 0.5);
    lua_pushnumber(L, 1.0);
    return 2;
}

// game.music(name[, loop]): a mod .wav through PlaySound (one at a time); no name stops it.
int LuaMusic(lua_State* L) {
    PlaySoundA(nullptr, nullptr, 0);
    g_music.clear();
    if (lua_isnoneornil(L, 1)) return 0;
    const char* name = luaL_checkstring(L, 1);
    const DWORD loop = lua_toboolean(L, 2) ? SND_LOOP : 0;
    g_music = features::ModFilePath(name);
    if (g_music.empty() || !PlaySoundA(g_music.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT | loop)) {
        features::ModLog("[fail] script music %s: not found or not a playable .wav", name);
        g_music.clear();
    }
    return 0;
}

int LuaClock(lua_State* L) {
    static const double kSecondsPerTick = [] {  // the frequency is fixed at boot
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    lua_pushnumber(L, static_cast<double>(t.QuadPart) * kSecondsPerTick);
    return 1;
}

int BlockCount() {
    const int n = *reinterpret_cast<int*>(kBlockCount);
    return n < 0 ? 0 : n > 4 ? 4 : n;
}

void SetFilm(bool on) {
    if (on == g_film) return;
    g_film = on;
    auto* mult = reinterpret_cast<float*>(kTimeMultiplier);
    if (on) {
        g_savedMultiplier = *mult;
        *mult = 0.0001f;   // (exactly 0 turns the scene into fog colour - see customise.cpp)
        for (int i = 0; i < BlockCount(); ++i) {
            int* dev = reinterpret_cast<int*>(kBlocks + i * kBlockSize + kBlockDevices);
            g_savedDevices[i] = *dev;
            *dev = 0;
        }
        g_prevSkip = true;
    } else {
        *mult = g_savedMultiplier;
        for (int i = 0; i < BlockCount(); ++i)
            *reinterpret_cast<int*>(kBlocks + i * kBlockSize + kBlockDevices) = g_savedDevices[i];
    }
    dslog::Write("Script: film %s", on ? "on (world frozen)" : "off");
}

// The film's draw function runs once per RENDERED frame (an overlay late draw), not from update(): while the world is
// frozen the game renders frames without updating, and a film drawn from update() let the game show through on those.
lua_State* g_lua = nullptr;
int g_draw = LUA_NOREF;
bool g_drawFailed = false;

void DropDraw() {
    if (g_lua && g_draw != LUA_NOREF) luaL_unref(g_lua, LUA_REGISTRYINDEX, g_draw);
    g_draw = LUA_NOREF;
}

void FilmLateDraw() {
    if (!g_film || !g_lua || g_draw == LUA_NOREF) return;
    lua_rawgeti(g_lua, LUA_REGISTRYINDEX, g_draw);
    if (lua_pcall(g_lua, 0, 0, 0) != LUA_OK) {
        if (!g_drawFailed) features::ModLog("[fail] script film: %s", lua_tostring(g_lua, -1));
        g_drawFailed = true;
        lua_pop(g_lua, 1);
        SetFilm(false);
    }
}

// game.film(true [, draw]) / game.film(false)
int LuaFilm(lua_State* L) {
    const bool on = lua_toboolean(L, 1) != 0;
    DropDraw();
    g_lua = L;
    if (on && lua_isfunction(L, 2)) {
        lua_pushvalue(L, 2);
        g_draw = luaL_ref(L, LUA_REGISTRYINDEX);
        g_drawFailed = false;
        overlay::AddLateDraw(&FilmLateDraw);
    }
    SetFilm(on);
    return 0;
}

int LuaSkipPressed(lua_State* L) {
    bool now = features::KeyHeld(VK_ESCAPE) || features::KeyHeld(VK_RETURN) || features::KeyHeld(VK_SPACE);
    for (int j = 0; j < 8 && !now; ++j) now = (features::ReadJoystick(j) & (1u << 2)) != 0;
    const bool pressed = now && !g_prevSkip;
    g_prevSkip = now;
    lua_pushboolean(L, pressed);
    return 1;
}

const luaL_Reg kFunctions[] = {{"image", LuaImage},   {"draw", LuaDraw},   {"draw_part", LuaDrawPart},
                               {"fill", LuaFill},     {"screen", LuaScreen}, {"music", LuaMusic},
                               {"clock", LuaClock},   {"film", LuaFilm},   {"skip_pressed", LuaSkipPressed},
                               {nullptr, nullptr}};
}  // namespace

// The `game` table is on top of the stack.
void features::RegisterFilmFunctions(lua_State* L) { luaL_setfuncs(L, kFunctions, 0); }

bool features::ScriptFilmPlaying() { return g_film; }

// A new level (or the script failed / ended): no film left running.
void features::ResetScriptFilm() {
    SetFilm(false);
    g_draw = LUA_NOREF;   // (the Lua state is about to close: no unref)
    g_lua = nullptr;
    if (!g_music.empty()) PlaySoundA(nullptr, nullptr, 0);
    g_music.clear();
}
