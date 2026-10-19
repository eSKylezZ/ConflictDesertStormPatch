// Dev-only live test API ("tier 3", docs/sdk/devserver.md): a named pipe \\.\pipe\pse-dev-<pid> that runs Lua chunks
// inside the running game and answers with their return values as JSON. Lets many tests share one booted game
// (tests/live/, client tests/live/pse_client.py).
//
// Frames both ways: u32 length (little endian) + UTF-8 payload. Request = a Lua chunk; leading "--@" comment lines are
// options: --@async (run at once on the pipe thread - read-only work while the game thread is busy loading / waiting in
// PLEASE PRESS ANY KEY), --@limit <ms> (Lua run time limit, default 5000), --@wait <ms> (how long a sync request may
// wait for the game thread, default 60000). Default requests are queued and run on the game thread in OnFrameDevServer
// (a per-frame budget of kFrameBudgetMs, at least one request per frame). Reply: {"result": v, "print": [...], "ms": t,
// "frame": n, "thread": "game" | "pipe"} or {"error": "...", ...}; several return values -> a JSON array.
//
// Two Lua states: the game-thread state (sync requests) and the pipe-thread state (async requests, one at a time).
// Globals persist between requests of the same kind, not across the two. Bindings in table `dev` (see the doc).
// Compiled out of DS_DIST builds: there OnFrameDevServer is empty.
#include "features/features.h"

#ifndef DS_DIST

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <type_traits>
#include <string>
#include <vector>

extern "C" {
#include "../../third_party/lua/lauxlib.h"
#include "../../third_party/lua/lua.h"
#include "../../third_party/lua/lualib.h"
}

#include "core/log.h"
#include "core/settings.h"
#include "loader.h"
#include "pse/pse_game_ds1.h"
#include "pse/pse_ui.h"

namespace features {
std::vector<std::string> ConsoleRun(const std::string& line);   // console.cpp
}

namespace {
constexpr uint32_t kGameWindow = 0x606A60, kFrontEndState = 0x617C18, kDispatchMode = 0x60EC8C, kLevelName = 0x606880;
constexpr uint32_t kSplitFlag = 0x606410, kPlayerCount = 0x606414, kBlocks = 0x60F5B8, kBlockSize = 0x478;
constexpr double kFrameBudgetMs = 20.0;
constexpr int kDefaultLimitMs = 5000, kDefaultWaitMs = 60000, kMaxArgs = 16;
constexpr uint32_t kMaxFrame = 16u << 20;

std::atomic<uint32_t> g_frame{0};
std::atomic<bool> g_started{false};
LARGE_INTEGER g_freq;

double NowMs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1000.0 * static_cast<double>(t.QuadPart) / static_cast<double>(g_freq.QuadPart);
}

// ---- guarded memory access / calls (no C++ objects in these frames: __try) ----
bool TryRead(uintptr_t address, void* out, size_t n) {
    __try {
        memcpy(out, reinterpret_cast<const void*>(address), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool TryCopy(uintptr_t address, const void* src, size_t n) {
    __try {
        memcpy(reinterpret_cast<void*>(address), src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Writes; a read-only / code page is made writable for the write (and set back).
bool TryWrite(uintptr_t address, const void* src, size_t n) {
    if (TryCopy(address, src, n)) return true;
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), n, PAGE_EXECUTE_READWRITE, &old)) return false;
    const bool ok = TryCopy(address, src, n);
    DWORD dummy;
    VirtualProtect(reinterpret_cast<void*>(address), n, old, &dummy);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), n);
    return ok;
}
// cstring: up to max bytes; false = the first byte was unreadable.
bool TryCString(uintptr_t address, char* out, size_t max, size_t* len) {
    size_t i = 0;
    __try {
        for (; i < max; ++i) {
            const char c = reinterpret_cast<const char*>(address)[i];
            if (!c) break;
            out[i] = c;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (i == 0) return false;
    }
    *len = i;
    return true;
}

enum Conv { kCdecl, kStdcall, kThiscall, kFastcall };
struct CallSpec {
    uint32_t fn;
    uint32_t ecx, edx;
    uint32_t count;          // stack arguments
    const uint32_t* stack;   // first stack argument (pushed last)
    uint32_t wantFloat;
    uint32_t eax;
    double st0;
    DWORD code;              // exception code
};
__declspec(noinline) void CallRaw(CallSpec* c) {
    uint32_t fn = c->fn, ecxv = c->ecx, edxv = c->edx, cnt = c->count, wantF = c->wantFloat;
    const uint32_t* args = c->stack;
    uint32_t savedEsp = 0, ra = 0;
    double f = 0;
    __asm {
        mov savedEsp, esp
        mov esi, args
        mov eax, cnt
    pushloop:
        test eax, eax
        jz pushed
        dec eax
        push dword ptr [esi + eax * 4]
        jmp pushloop
    pushed:
        mov ecx, ecxv
        mov edx, edxv
        call dword ptr [fn]
        mov ra, eax
        cmp wantF, 0
        je nofloat
        fstp qword ptr [f]
    nofloat:
        mov esp, savedEsp
    }
    c->eax = ra;
    c->st0 = f;
}
bool TryCall(CallSpec* c) {
    __try {
        CallRaw(c);
        return true;
    } __except (c->code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- per-state context ----
struct Ctx {
    bool async = false;
    double deadline = 0;   // NowMs() limit of the running chunk
    int limitMs = kDefaultLimitMs;
    std::vector<std::string> printed;
};
Ctx* CtxOf(lua_State* L) { return *static_cast<Ctx**>(lua_getextraspace(L)); }

// luaL_error with the full printf format (luaL_error knows only %d %s %f %p %c %U %%): position + message, raised.
int Fail(lua_State* L, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    luaL_where(L, 1);
    lua_pushstring(L, buf);
    lua_concat(L, 2);
    return lua_error(L);
}

uintptr_t Addr(lua_State* L, int i) { return static_cast<uintptr_t>(static_cast<uint32_t>(luaL_checkinteger(L, i))); }

int ReadFail(lua_State* L, uintptr_t a, size_t n) {
    return Fail(L, "can't read %d byte(s) at 0x%08x", static_cast<int>(n), static_cast<unsigned>(a));
}

template <class T>
int LRead(lua_State* L) {
    const uintptr_t a = Addr(L, 1);
    T v{};
    if (!TryRead(a, &v, sizeof v)) return ReadFail(L, a, sizeof v);
    if constexpr (std::is_floating_point_v<T>) lua_pushnumber(L, static_cast<lua_Number>(v));
    else lua_pushinteger(L, static_cast<lua_Integer>(v));
    return 1;
}
template <class T>
int LWrite(lua_State* L) {
    const uintptr_t a = Addr(L, 1);
    T v;
    if constexpr (std::is_floating_point_v<T>) v = static_cast<T>(luaL_checknumber(L, 2));
    else v = static_cast<T>(luaL_checkinteger(L, 2));
    if (!TryWrite(a, &v, sizeof v))
        return Fail(L, "can't write %d byte(s) at 0x%08x", static_cast<int>(sizeof v), static_cast<unsigned>(a));
    return 0;
}

// dev.bytes(addr, n) -> lower-case hex string
int LBytes(lua_State* L) {
    const uintptr_t a = Addr(L, 1);
    const lua_Integer n = luaL_checkinteger(L, 2);
    luaL_argcheck(L, n >= 0 && n <= (1 << 20), 2, "0..1048576 bytes");
    luaL_Buffer b;
    char* hex = luaL_buffinitsize(L, &b, static_cast<size_t>(n) * 2 + 1);
    static const char digits[] = "0123456789abcdef";
    for (lua_Integer i = 0; i < n; ++i) {
        uint8_t v;
        if (!TryRead(a + static_cast<uintptr_t>(i), &v, 1)) return ReadFail(L, a + static_cast<uintptr_t>(i), 1);
        hex[i * 2] = digits[v >> 4];
        hex[i * 2 + 1] = digits[v & 15];
    }
    luaL_pushresultsize(&b, static_cast<size_t>(n) * 2);
    return 1;
}
int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// dev.write_bytes(addr, "90 90 e8...") - spaces ignored
int LWriteBytes(lua_State* L) {
    const uintptr_t a = Addr(L, 1);
    size_t len;
    const char* s = luaL_checklstring(L, 2, &len);
    uint8_t* buf = static_cast<uint8_t*>(lua_newuserdatauv(L, len / 2 + 1, 0));
    size_t n = 0;
    int hi = -1;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] == ' ') continue;
        const int v = HexVal(s[i]);
        if (v < 0) return Fail(L, "bad hex digit '%c'", s[i]);
        if (hi < 0) hi = v;
        else buf[n++] = static_cast<uint8_t>(hi << 4 | v), hi = -1;
    }
    if (hi >= 0) return Fail(L, "odd number of hex digits");
    if (n && !TryWrite(a, buf, n)) return Fail(L, "can't write %d byte(s) at 0x%08x", static_cast<int>(n), static_cast<unsigned>(a));
    lua_pushinteger(L, static_cast<lua_Integer>(n));
    return 1;
}
// dev.cstring(addr [, max = 4096])
int LCString(lua_State* L) {
    const uintptr_t a = Addr(L, 1);
    const lua_Integer max = luaL_optinteger(L, 2, 4096);
    luaL_argcheck(L, max > 0 && max <= (1 << 20), 2, "1..1048576");
    char* buf = static_cast<char*>(lua_newuserdatauv(L, static_cast<size_t>(max), 0));
    size_t len = 0;
    if (!TryCString(a, buf, static_cast<size_t>(max), &len)) return ReadFail(L, a, 1);
    lua_pushlstring(L, buf, len);
    return 1;
}
// dev.write_cstring(addr, s) - with its NUL
int LWriteCString(lua_State* L) {
    const uintptr_t a = Addr(L, 1);
    size_t len;
    const char* s = luaL_checklstring(L, 2, &len);
    if (!TryWrite(a, s, len + 1)) return Fail(L, "can't write %d byte(s) at 0x%08x", static_cast<int>(len + 1), static_cast<unsigned>(a));
    return 0;
}

// dev.call(addr, conv, ...) -> eax; dev.callf(...) -> st0. Args: integer = dword, float = float32 bits, string =
// pointer to its bytes (valid during the call), boolean = 0/1, nil = 0.
int CallImpl(lua_State* L, bool wantFloat) {
    const uintptr_t fn = Addr(L, 1);
    const char* conv = luaL_optstring(L, 2, "cdecl");
    int c;
    if (!_stricmp(conv, "cdecl")) c = kCdecl;
    else if (!_stricmp(conv, "stdcall")) c = kStdcall;
    else if (!_stricmp(conv, "thiscall")) c = kThiscall;
    else if (!_stricmp(conv, "fastcall")) c = kFastcall;
    else return Fail(L, "calling convention must be cdecl, stdcall, thiscall or fastcall (got '%s')", conv);
    // dev.call(addr) with no convention / arguments: 0, not -1 (became a huge unsigned stack count and ended the game)
    const int n = std::max(0, lua_gettop(L) - 2);
    if (n > kMaxArgs) return Fail(L, "too many arguments (%d, max %d)", n, kMaxArgs);
    uint32_t a[kMaxArgs + 2] = {};
    for (int i = 0; i < n; ++i) {
        const int idx = i + 3;
        switch (lua_type(L, idx)) {
        case LUA_TNIL: a[i] = 0; break;
        case LUA_TBOOLEAN: a[i] = lua_toboolean(L, idx) ? 1 : 0; break;
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) a[i] = static_cast<uint32_t>(lua_tointeger(L, idx));
            else {
                const float f = static_cast<float>(lua_tonumber(L, idx));
                memcpy(&a[i], &f, 4);
            }
            break;
        case LUA_TSTRING: a[i] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lua_tostring(L, idx))); break;
        default: return Fail(L, "argument %d: %s can't be passed", i + 1, luaL_typename(L, idx));
        }
    }
    if ((c == kThiscall && n < 1) || (c == kFastcall && n < 2))
        return Fail(L, "%s needs %d register argument(s)", conv, c == kThiscall ? 1 : 2);
    const int regs = c == kThiscall ? 1 : c == kFastcall ? 2 : 0;
    CallSpec s{};
    s.fn = static_cast<uint32_t>(fn);
    s.ecx = regs >= 1 ? a[0] : 0;
    s.edx = regs >= 2 ? a[1] : 0;
    s.stack = a + regs;
    s.count = static_cast<uint32_t>(n - regs);
    s.wantFloat = wantFloat;
    if (!TryCall(&s))
        return Fail(L, "exception 0x%08x in call to 0x%08x", static_cast<unsigned>(s.code), static_cast<unsigned>(fn));
    if (wantFloat) lua_pushnumber(L, s.st0);
    else lua_pushinteger(L, static_cast<lua_Integer>(s.eax));
    return 1;
}
int LCall(lua_State* L) { return CallImpl(L, false); }
int LCallF(lua_State* L) { return CallImpl(L, true); }

// dev.alloc(n) -> address of zeroed RWX memory (kept until dev.free); dev.free(addr)
int LAlloc(lua_State* L) {
    const lua_Integer n = luaL_checkinteger(L, 1);
    luaL_argcheck(L, n > 0 && n <= (64 << 20), 1, "1..64 MB");
    void* p = VirtualAlloc(nullptr, static_cast<SIZE_T>(n), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!p) return Fail(L, "VirtualAlloc(%d) failed", static_cast<int>(n));
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(p)));
    return 1;
}
int LFree(lua_State* L) {
    lua_pushboolean(L, VirtualFree(reinterpret_cast<void*>(Addr(L, 1)), 0, MEM_RELEASE) != 0);
    return 1;
}

int LFrame(lua_State* L) {
    lua_pushinteger(L, g_frame.load());
    return 1;
}
uint32_t Peek32(uint32_t va) {
    uint32_t v = 0;
    TryRead(va, &v, 4);
    return v;
}
std::string LevelName() {
    char scene[33] = {};
    size_t len = 0;
    if (!TryCString(kLevelName, scene, 32, &len)) return {};
    return std::string(scene, len);
}
HWND GameWindow() { return reinterpret_cast<HWND>(static_cast<uintptr_t>(Peek32(kGameWindow))); }

int LFeState(lua_State* L) {
    lua_pushinteger(L, Peek32(kFrontEndState));
    return 1;
}
int LDispatch(lua_State* L) {
    int8_t v = 0;
    TryRead(kDispatchMode, &v, 1);
    lua_pushinteger(L, v);
    return 1;
}
int LLevel(lua_State* L) {
    const std::string s = LevelName();
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}
int LFocused(lua_State* L) {
    const HWND w = GameWindow();
    lua_pushboolean(L, w && GetForegroundWindow() == w);
    return 1;
}
int LAsync(lua_State* L) {
    lua_pushboolean(L, CtxOf(L)->async);
    return 1;
}
int LTimeMs(lua_State* L) {
    lua_pushnumber(L, NowMs());
    return 1;
}
// dev.state() -> {frame, fe_state, dispatch, level, focused, split, players, window}
int LState(lua_State* L) {
    lua_createtable(L, 0, 8);
    lua_pushinteger(L, g_frame.load());
    lua_setfield(L, -2, "frame");
    LFeState(L);
    lua_setfield(L, -2, "fe_state");
    LDispatch(L);
    lua_setfield(L, -2, "dispatch");
    LLevel(L);
    lua_setfield(L, -2, "level");
    LFocused(L);
    lua_setfield(L, -2, "focused");
    lua_pushinteger(L, Peek32(kSplitFlag) & 0xFF);
    lua_setfield(L, -2, "split");
    lua_pushinteger(L, Peek32(kPlayerCount));
    lua_setfield(L, -2, "players");
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(GameWindow())));
    lua_setfield(L, -2, "window");
    return 1;
}
// dev.player([n = 0]) -> {block, soldier, x, y, z, health} or nil (no soldier). n = player index 0..3.
int LPlayer(lua_State* L) {
    const lua_Integer n = luaL_optinteger(L, 1, 0);
    luaL_argcheck(L, n >= 0 && n < 4, 1, "player 0..3");
    const uint32_t block = kBlocks + static_cast<uint32_t>(n) * kBlockSize;
    const uint32_t soldier = Peek32(block + 0x310);
    float pos[3] = {}, health = 0;
    if (!soldier || !TryRead(soldier + 0x30, pos, sizeof pos) || !TryRead(soldier + 0x54, &health, 4)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 6);
    lua_pushinteger(L, block);
    lua_setfield(L, -2, "block");
    lua_pushinteger(L, soldier);
    lua_setfield(L, -2, "soldier");
    const char* names[] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i) {
        lua_pushnumber(L, pos[i]);
        lua_setfield(L, -2, names[i]);
    }
    lua_pushnumber(L, health);
    lua_setfield(L, -2, "health");
    return 1;
}

// ---- log ----
std::string LogPath() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    while (n > 0 && path[n - 1] != '\\') --n;
    path[n] = '\0';
    return std::string(path) + "DesertStormFix.log";
}
// last n lines of DesertStormFix.log (the game keeps it open for writing; we read with share-write)
std::vector<std::string> LogTail(int n) {
    std::vector<std::string> lines;
    const HANDLE f = CreateFileA(LogPath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return lines;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    const LONGLONG want = std::min<LONGLONG>(size.QuadPart, 512 * 1024);
    LARGE_INTEGER pos;
    pos.QuadPart = size.QuadPart - want;
    SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
    std::string data(static_cast<size_t>(want), '\0');
    DWORD got = 0;
    ReadFile(f, data.data(), static_cast<DWORD>(want), &got, nullptr);
    CloseHandle(f);
    data.resize(got);
    size_t end = data.size();
    while (end > 0 && static_cast<int>(lines.size()) < n) {
        size_t e = end;
        while (e > 0 && (data[e - 1] == '\n' || data[e - 1] == '\r')) --e;
        if (e == 0) break;
        size_t b = data.rfind('\n', e - 1);
        b = b == std::string::npos ? 0 : b + 1;
        if (b == 0 && pos.QuadPart > 0) break;   // partial first line of the window
        lines.insert(lines.begin(), data.substr(b, e - b));
        end = b;
    }
    return lines;
}
void PushStrings(lua_State* L, const std::vector<std::string>& v) {
    lua_createtable(L, static_cast<int>(v.size()), 0);
    for (size_t i = 0; i < v.size(); ++i) {
        lua_pushlstring(L, v[i].data(), v[i].size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
}
// dev.log([n = 50]) -> last n lines of DesertStormFix.log
int LLog(lua_State* L) {
    const int n = static_cast<int>(luaL_optinteger(L, 1, 50));
    PushStrings(L, LogTail(std::max(0, std::min(n, 5000))));
    return 1;
}
// dev.log_write(text) -> a "[devserver] text" line in DesertStormFix.log
int LLogWrite(lua_State* L) {
    dslog::Write("[devserver] %s", luaL_checkstring(L, 1));
    return 0;
}
// print(...) -> collected in the reply's "print" list
int LPrint(lua_State* L) {
    const int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_tolstring(L, i, nullptr);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    CtxOf(L)->printed.emplace_back(lua_tostring(L, -1));
    return 0;
}

// dev.console(line) -> lines printed (game thread only)
int LConsole(lua_State* L) {
    if (CtxOf(L)->async) return Fail(L, "dev.console runs on the game thread only (not in --@async requests)");
    const char* line = luaL_checkstring(L, 1);
    std::vector<std::string> out = features::ConsoleRun(line);
    PushStrings(L, out);
    return 1;
}

// dev.sleep(ms) - max 10000 (sync: stalls the game frame)
int LSleep(lua_State* L) {
    const lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_argcheck(L, ms >= 0 && ms <= 10000, 1, "0..10000 ms");
    Sleep(static_cast<DWORD>(ms));
    return 0;
}

// ---- keys ----
const std::pair<const char*, UINT> kKeyNames[] = {
    {"up", VK_UP},         {"down", VK_DOWN},     {"left", VK_LEFT},     {"right", VK_RIGHT},   {"enter", VK_RETURN},
    {"return", VK_RETURN}, {"esc", VK_ESCAPE},    {"escape", VK_ESCAPE}, {"space", VK_SPACE},   {"back", VK_BACK},
    {"backspace", VK_BACK}, {"tab", VK_TAB},      {"shift", VK_SHIFT},   {"ctrl", VK_CONTROL},  {"alt", VK_MENU},
    {"delete", VK_DELETE}, {"insert", VK_INSERT}, {"home", VK_HOME},     {"end", VK_END},       {"pgup", VK_PRIOR},
    {"pgdn", VK_NEXT},     {"grave", VK_OEM_3},
};
bool Extended(UINT vk) {
    return vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_INSERT || vk == VK_DELETE ||
           vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT;
}
bool PostKey(UINT vk, bool up) {
    const HWND w = GameWindow();
    if (!w || !IsWindow(w)) return false;
    const UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    LPARAM lp = 1 | (static_cast<LPARAM>(sc) << 16) | (Extended(vk) ? (1 << 24) : 0);
    if (up) lp |= static_cast<LPARAM>(0xC0000000u);
    return PostMessageA(w, up ? WM_KEYUP : WM_KEYDOWN, vk, lp) != 0;
}
VOID CALLBACK KeyUpTimer(PVOID param, BOOLEAN) { PostKey(static_cast<UINT>(reinterpret_cast<uintptr_t>(param)), true); }
UINT CheckKey(lua_State* L, int i) {
    if (lua_isinteger(L, i)) return static_cast<UINT>(lua_tointeger(L, i)) & 0xFF;
    const char* name = luaL_checkstring(L, i);
    for (const auto& [n, vk] : kKeyNames)
        if (!_stricmp(n, name)) return vk;
    if (name[0] && !name[1]) {
        const char c = static_cast<char>(toupper(static_cast<unsigned char>(name[0])));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<UINT>(c);
    }
    if ((name[0] == 'f' || name[0] == 'F') && name[1]) {
        const int f = atoi(name + 1);
        if (f >= 1 && f <= 12) return static_cast<UINT>(VK_F1 + f - 1);
    }
    Fail(L, "unknown key '%s'", name);
    return 0;
}
// dev.key(key [, hold_ms = 80]) - WM_KEYDOWN now, WM_KEYUP after hold_ms (timer thread). key = name or VK code.
int LKey(lua_State* L) {
    const UINT vk = CheckKey(L, 1);
    const lua_Integer hold = luaL_optinteger(L, 2, 80);
    luaL_argcheck(L, hold >= 0 && hold <= 10000, 2, "0..10000 ms");
    if (!PostKey(vk, false)) return Fail(L, "no game window");
    HANDLE t = nullptr;
    if (hold == 0 || !CreateTimerQueueTimer(&t, nullptr, &KeyUpTimer, reinterpret_cast<PVOID>(static_cast<uintptr_t>(vk)),
                                            static_cast<DWORD>(hold), 0, WT_EXECUTEONLYONCE))
        PostKey(vk, true);
    lua_pushboolean(L, 1);
    return 1;
}
int LKeyDown(lua_State* L) {
    lua_pushboolean(L, PostKey(CheckKey(L, 1), false));
    return 1;
}
int LKeyUp(lua_State* L) {
    lua_pushboolean(L, PostKey(CheckKey(L, 1), true));
    return 1;
}

// ---- DesertStormFix settings (core/settings: pse.settings plugin "dsfix") ----
// dev.setting(name) -> number | nil (the value in use); dev.set_setting(name, value) -> bool (saved like a menu change);
// dev.settings_key() -> "Software\PSE\ds1\Plugins\dsfix" (under HKCU). Game thread.
int LSetting(lua_State* L) {
    uint32_t v = 0;
    if (settings::GetByName(luaL_checkstring(L, 1), v)) lua_pushinteger(L, static_cast<lua_Integer>(v));
    else lua_pushnil(L);
    return 1;
}
int LSetSetting(lua_State* L) {
    lua_pushboolean(L, settings::SetByName(luaL_checkstring(L, 1), static_cast<uint32_t>(luaL_checkinteger(L, 2))));
    return 1;
}
int LSettingsKey(lua_State* L) {
    lua_pushstring(L, settings::Key());
    return 1;
}
// dev.plugin_setting(plugin, key) -> int or nil (plugin not loaded); dev.set_plugin_setting(plugin, key, v) -> bool.
// Any plugin's pse.settings value (e.g. "dsfix.discord", "DiscordPresence"); a set runs the plugin's OnChange.
int LPluginSetting(lua_State* L) {
    const PSEPluginHandle h = pse::FindPlugin(luaL_checkstring(L, 1));
    if (h) lua_pushinteger(L, pse::SettingsInterface()->GetInt(h, luaL_checkstring(L, 2)));
    else lua_pushnil(L);
    return 1;
}
int LSetPluginSetting(lua_State* L) {
    const PSEPluginHandle h = pse::FindPlugin(luaL_checkstring(L, 1));
    lua_pushboolean(L, h && pse::SettingsInterface()->SetInt(h, luaL_checkstring(L, 2),
                                                             static_cast<int32_t>(luaL_checkinteger(L, 3))) == PSE_OK);
    return 1;
}

// ---- pse.game.ds1, as a plugin sees it (the registered interface, not the host's code) ----
const PSEGameDs1V1* GameDs1() {
    return static_cast<const PSEGameDs1V1*>(
        pse::MessagingInterface()->QueryInterface(pse::FindPlugin("dsfix"), PSE_GAME_DS1_INTERFACE, 1));
}
// dev.game_ds1() -> {fe_state, dispatch, level, kind, cutscene, title, split, local_players, players = {{block, soldier,
// x, y, z, health}, ...}, online = {active, role, local_player, players, peers, in_mission}, window, exe_sha256,
// exe_known} or nil (interface missing). Game thread.
int LGameDs1(lua_State* L) {
    const PSEGameDs1V1* g = GameDs1();
    if (!g) {
        lua_pushnil(L);
        return 1;
    }
    auto field = [L](const char* name, lua_Integer v) {
        lua_pushinteger(L, v);
        lua_setfield(L, -2, name);
    };
    char buf[256];
    lua_newtable(L);
    field("fe_state", g->FrontEndState());
    field("dispatch", g->DispatchMode());
    g->LevelName(buf, sizeof buf);
    lua_pushstring(L, buf);
    lua_setfield(L, -2, "level");
    field("kind", g->LevelKind());
    lua_pushboolean(L, g->InCutscene());
    lua_setfield(L, -2, "cutscene");
    g->LevelTitle(buf, sizeof buf);
    lua_pushstring(L, buf);
    lua_setfield(L, -2, "title");
    field("split", g->SplitScreenPlayers());
    const uint32_t n = g->LocalPlayerCount();
    field("local_players", n);
    lua_newtable(L);
    for (uint32_t i = 0; i < n; ++i) {
        PSEDs1Player p{};
        p.size = sizeof p;
        if (!g->GetLocalPlayer(i, &p)) continue;
        lua_newtable(L);
        field("index", i);
        field("block", p.block);
        field("soldier", p.soldier);
        lua_pushnumber(L, p.position.x);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, p.position.y);
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, p.position.z);
        lua_setfield(L, -2, "z");
        lua_pushnumber(L, p.health);
        lua_setfield(L, -2, "health");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "players");
    PSEDs1Online o{};
    o.size = sizeof o;
    g->GetOnline(&o);
    lua_newtable(L);
    field("active", o.active);
    field("role", o.role);
    field("local_player", o.localPlayer);
    field("players", o.players);
    field("peers", o.connectedPeers);
    field("in_mission", o.inMission);
    lua_setfield(L, -2, "online");
    field("window", static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(g->GameWindow())));
    lua_pushstring(L, g->ExeSha256());
    lua_setfield(L, -2, "exe_sha256");
    lua_pushboolean(L, g->ExeKnown());
    lua_setfield(L, -2, "exe_known");
    return 1;
}
// dev.game_text(key | hash) -> text (UTF-8) or nil: pse.game.ds1 LookupText (of TextHash(key) for a string).
int LGameText(lua_State* L) {
    const PSEGameDs1V1* g = GameDs1();
    if (!g) return 0;
    const uint32_t hash = lua_type(L, 1) == LUA_TSTRING ? g->TextHash(lua_tostring(L, 1))
                                                        : static_cast<uint32_t>(luaL_checkinteger(L, 1));
    std::string text(g->LookupText(hash, nullptr, 0) + 1, '\0');
    if (!g->LookupText(hash, text.data(), static_cast<uint32_t>(text.size()))) {
        lua_pushnil(L);
        return 1;
    }
    text.resize(text.size() - 1);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// ---- PSE introspection (pse/loader) ----
// dev.pse_plugins() -> {{handle, name, version "x.y.z", flags}, ...}; game thread or async (registry is append-only).
int LPsePlugins(lua_State* L) {
    lua_newtable(L);
    int n = 0;
    for (PSEPluginHandle h = 1; n < static_cast<int>(pse::PluginCount()) && h < 4096; ++h) {
        const PSEPluginInfo* info = pse::PluginInfo(h);
        if (!info) continue;
        lua_newtable(L);
        lua_pushinteger(L, h);
        lua_setfield(L, -2, "handle");
        lua_pushstring(L, info->name);
        lua_setfield(L, -2, "name");
        char v[32];
        snprintf(v, sizeof v, "%u.%u.%u", PSE_VERSION_MAJOR(info->version), PSE_VERSION_MINOR(info->version),
                 PSE_VERSION_PATCH(info->version));
        lua_pushstring(L, v);
        lua_setfield(L, -2, "version");
        lua_pushinteger(L, info->flags);
        lua_setfield(L, -2, "flags");
        lua_pushboolean(L, pse::PluginExternal(h));   // a DLL in PSE\Plugins (dsfix itself, examples, dsfix.discord)
        lua_setfield(L, -2, "external");
        lua_pushboolean(L, pse::PluginHost(h));       // host plugin / game layer (gets pse.host)
        lua_setfield(L, -2, "host");
        lua_pushboolean(L, pse::PluginDisabled(h));
        lua_setfield(L, -2, "disabled");
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}
// dev.module_path(name) -> full path of a loaded module ("pse_loader.dll", "dsfix.dll" ...) or nil (docs/sdk/layout.md).
int LModulePath(lua_State* L) {
    HMODULE m = GetModuleHandleA(luaL_checkstring(L, 1));
    char path[MAX_PATH] = {};
    const DWORD n = m ? GetModuleFileNameA(m, path, MAX_PATH) : 0;
    if (n && n < MAX_PATH) lua_pushlstring(L, path, n);
    else lua_pushnil(L);
    return 1;
}
// ---- pse.ui (pseui.cpp) and the PLUGINS menu (pluginsmenu.cpp) ----
// dev.plugins_menu() -> {screen = "list" | "page" | nil, row (0-based, absolute), offset, plugin, labels = {...},
// values = {...}, footer = {...}} - the texts as the game draws them (8-bit). Game thread.
int LPluginsMenu(lua_State* L) {
    if (CtxOf(L)->async) return luaL_error(L, "dev.plugins_menu: game thread only");
    const features::PluginsMenuDebug d = features::PluginsMenuState();
    lua_newtable(L);
    if (d.screen) {
        lua_pushstring(L, d.screen == 1 ? "list" : "page");
        lua_setfield(L, -2, "screen");
    }
    lua_pushinteger(L, d.row);
    lua_setfield(L, -2, "row");
    lua_pushinteger(L, d.offset);
    lua_setfield(L, -2, "offset");
    lua_pushstring(L, d.plugin.c_str());
    lua_setfield(L, -2, "plugin");
    PushStrings(L, d.labels);
    lua_setfield(L, -2, "labels");
    PushStrings(L, d.values);
    lua_setfield(L, -2, "values");
    PushStrings(L, d.footer);
    lua_setfield(L, -2, "footer");
    return 1;
}
// dev.plugins_menu_open([plugin]) -> bool: the PLUGINS list (no argument) or a plugin's page, as the menu would open
// them (front end; false while a transition runs - try again).
int LPluginsMenuOpen(lua_State* L) {
    if (CtxOf(L)->async) return luaL_error(L, "dev.plugins_menu_open: game thread only");
    lua_pushboolean(L, features::PluginsMenuOpen(luaL_optstring(L, 1, nullptr)));
    return 1;
}
// dev.plugins_menu_row(n) -> bool: cursor onto absolute row n (0-based) of the open list / page.
// dev.plugins_menu_activate([dir = 0 [, row]]) -> bool: what Enter (0) / Left (-1) / Right (+1) do on the row (the
// cursor's, or `row` first). dev.plugins_menu_back() -> bool: Esc. Game thread; the screens' own code, no keys.
int LPluginsMenuRow(lua_State* L) {
    if (CtxOf(L)->async) return luaL_error(L, "dev.plugins_menu_row: game thread only");
    lua_pushboolean(L, features::PluginsMenuSetRow(static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}
int LPluginsMenuActivate(lua_State* L) {
    if (CtxOf(L)->async) return luaL_error(L, "dev.plugins_menu_activate: game thread only");
    if (!lua_isnoneornil(L, 2) && !features::PluginsMenuSetRow(static_cast<int>(luaL_checkinteger(L, 2)))) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, features::PluginsMenuActivate(static_cast<int>(luaL_optinteger(L, 1, 0))));
    return 1;
}
int LPluginsMenuBack(lua_State* L) {
    if (CtxOf(L)->async) return luaL_error(L, "dev.plugins_menu_back: game thread only");
    lua_pushboolean(L, features::PluginsMenuBack());
    return 1;
}
// dev.colour_picker([op [, 0xRRGGBB]]) -> {open, colour} with no op; op "set" / "ok" / "cancel" -> bool (false = the
// wheel isn't open). The wheel of OPTIONS > DESERTSTORMFIX colours and plugin colour settings (barcolour.cpp).
int LColourPicker(lua_State* L) {
    if (CtxOf(L)->async) return luaL_error(L, "dev.colour_picker: game thread only");
    if (lua_isnoneornil(L, 1)) {
        lua_newtable(L);
        lua_pushboolean(L, features::BarColourPickerOpen());
        lua_setfield(L, -2, "open");
        lua_pushinteger(L, features::ColourPickerColour());
        lua_setfield(L, -2, "colour");
        return 1;
    }
    lua_pushboolean(L, features::ColourPickerDev(luaL_checkstring(L, 1), static_cast<uint32_t>(luaL_optinteger(L, 2, 0))));
    return 1;
}
// pse.ui as a plugin sees it (the registered table), used as plugin dsfix.dev.
const PSEUiInterface* Ui() {
    return static_cast<const PSEUiInterface*>(
        pse::MessagingInterface()->QueryInterface(pse::FindPlugin("dsfix"), PSE_UI_INTERFACE, 1));
}
PSEPluginHandle DevPlugin() { return pse::FindPlugin("dsfix.dev"); }
// dev.ui() -> {registered, items, texts (items this frame), popup (our own bar's text), capture (plugin name or ""),
// keys (queued), text_count}
int LUi(lua_State* L) {
    const features::UiDebug d = features::PseUiDebug();
    lua_newtable(L);
    lua_pushboolean(L, Ui() != nullptr);
    lua_setfield(L, -2, "registered");
    lua_pushinteger(L, d.items);
    lua_setfield(L, -2, "items");
    lua_pushinteger(L, d.texts);
    lua_setfield(L, -2, "texts");
    lua_pushstring(L, d.popup.c_str());
    lua_setfield(L, -2, "popup");
    lua_pushstring(L, d.capture.c_str());
    lua_setfield(L, -2, "capture");
    lua_pushinteger(L, d.keysQueued);
    lua_setfield(L, -2, "keys");
    lua_pushinteger(L, d.textCount);
    lua_setfield(L, -2, "text_count");
    return 1;
}
// dev.ui_capture(on) -> PSEResult (0 ok): pse.ui CaptureKeyboard as plugin dsfix.dev. Game thread.
int LUiCapture(lua_State* L) {
    if (CtxOf(L)->async) return luaL_error(L, "dev.ui_capture: game thread only");
    const PSEUiInterface* ui = Ui();
    lua_pushinteger(L, ui ? ui->CaptureKeyboard(DevPlugin(), lua_toboolean(L, 1)) : -1);
    return 1;
}
// dev.ui_keys() -> {{type = "down" | "char", code, repeat}, ...}: ReadKey until empty (as dsfix.dev). Game thread.
int LUiKeys(lua_State* L) {
    if (CtxOf(L)->async) return luaL_error(L, "dev.ui_keys: game thread only");
    const PSEUiInterface* ui = Ui();
    lua_newtable(L);
    PSEUiKey k{sizeof(PSEUiKey), 0, 0, 0};
    for (int n = 1; ui && ui->ReadKey(DevPlugin(), &k); ++n) {
        lua_newtable(L);
        lua_pushstring(L, k.type == PSE_UI_KEY_DOWN ? "down" : "char");
        lua_setfield(L, -2, "type");
        lua_pushinteger(L, k.code);
        lua_setfield(L, -2, "code");
        lua_pushinteger(L, k.repeat);
        lua_setfield(L, -2, "repeat");
        lua_rawseti(L, -2, n);
    }
    return 1;
}
// dev.ui_popup(text [, ms]) -> PSEResult: pse.ui ShowPopup as dsfix.dev. Game thread.
int LUiPopup(lua_State* L) {
    if (CtxOf(L)->async) return luaL_error(L, "dev.ui_popup: game thread only");
    const PSEUiInterface* ui = Ui();
    lua_pushinteger(L, ui ? ui->ShowPopup(DevPlugin(), luaL_checkstring(L, 1),
                                          static_cast<uint32_t>(luaL_optinteger(L, 2, 0)))
                          : -1);
    return 1;
}

// dev.pse_hooks() -> every broker record (byte patches + hook subscribers), see docs/sdk/reference/hooks.md.
// dev.loadout_open() / dev.loadout_close() -> bool: the briefing / loadout screen (customise.cpp). Game thread only:
// posted keys don't reliably reach it in background runs.
int LLoadoutOpen(lua_State* L) {
    lua_pushboolean(L, features::CustomiseOpen());
    return 1;
}
int LLoadoutClose(lua_State* L) {
    if (CtxOf(L)->async) return luaL_error(L, "dev.loadout_close: game thread only (not in --@async requests)");
    lua_pushboolean(L, features::CustomiseClose());
    return 1;
}
int LPseHooks(lua_State* L) {
    const PSEHooksInterface* hooks = pse::HooksInterface();
    std::vector<PSEHookInfo> list(hooks->ListHooks(nullptr, 0) + 16);
    for (auto& e : list) e.size = sizeof e;
    const uint32_t n = (std::min)(hooks->ListHooks(list.data(), static_cast<uint32_t>(list.size())),
                                  static_cast<uint32_t>(list.size()));
    static const char* const kinds[] = {"bytes", "call_site", "function", "indirect_call", "pointer"};
    lua_createtable(L, static_cast<int>(n), 0);
    for (uint32_t i = 0; i < n; ++i) {
        const PSEHookInfo& e = list[i];
        lua_newtable(L);
        lua_pushstring(L, e.kind <= PSE_HOOK_POINTER ? kinds[e.kind] : "?");
        lua_setfield(L, -2, "kind");
        lua_pushstring(L, e.ownerName ? e.ownerName : "");
        lua_setfield(L, -2, "owner");
        lua_pushstring(L, e.name ? e.name : "");
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, e.va);
        lua_setfield(L, -2, "va");
        lua_pushinteger(L, e.len);
        lua_setfield(L, -2, "len");
        lua_pushinteger(L, e.priority);
        lua_setfield(L, -2, "priority");
        lua_pushinteger(L, e.chainPosition);
        lua_setfield(L, -2, "chain");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// ---- states ----
void Hook(lua_State* L, lua_Debug*) {
    Ctx* c = CtxOf(L);
    if (NowMs() > c->deadline) Fail(L, "time limit (%d ms) exceeded", c->limitMs);
}
int Traceback(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    luaL_traceback(L, L, msg ? msg : "(error object is not a string)", 1);
    return 1;
}

lua_State* NewState(bool async) {
    lua_State* L = luaL_newstate();
    if (!L) return nullptr;
    *static_cast<Ctx**>(lua_getextraspace(L)) = new Ctx{async};
    static const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base},        {LUA_TABLIBNAME, luaopen_table},
                                    {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
                                    {LUA_UTF8LIBNAME, luaopen_utf8},  {LUA_COLIBNAME, luaopen_coroutine}};
    for (const auto& lib : libs) {
        luaL_requiref(L, lib.name, lib.func, 1);
        lua_pop(L, 1);
    }
    lua_pushcfunction(L, LPrint);
    lua_setglobal(L, "print");
    static const luaL_Reg dev[] = {
        {"u8", LRead<uint8_t>},          {"i8", LRead<int8_t>},           {"u16", LRead<uint16_t>},
        {"i16", LRead<int16_t>},         {"u32", LRead<uint32_t>},        {"i32", LRead<int32_t>},
        {"f32", LRead<float>},           {"f64", LRead<double>},          {"write_u8", LWrite<uint8_t>},
        {"write_u16", LWrite<uint16_t>}, {"write_u32", LWrite<uint32_t>}, {"write_i32", LWrite<int32_t>},
        {"write_f32", LWrite<float>},    {"write_f64", LWrite<double>},   {"bytes", LBytes},
        {"write_bytes", LWriteBytes},    {"cstring", LCString},           {"write_cstring", LWriteCString},
        {"call", LCall},                 {"callf", LCallF},               {"alloc", LAlloc},
        {"free", LFree},                 {"frame", LFrame},               {"fe_state", LFeState},
        {"dispatch", LDispatch},         {"level", LLevel},               {"focused", LFocused},
        {"state", LState},               {"player", LPlayer},             {"log", LLog},
        {"log_write", LLogWrite},        {"console", LConsole},           {"key", LKey},
        {"key_down", LKeyDown},          {"key_up", LKeyUp},              {"sleep", LSleep},
        {"is_async", LAsync},            {"time_ms", LTimeMs},            {"pse_plugins", LPsePlugins},
        {"pse_hooks", LPseHooks},        {"loadout_open", LLoadoutOpen},
        {"loadout_close", LLoadoutClose},        {"setting", LSetting},           {"set_setting", LSetSetting},
        {"settings_key", LSettingsKey},  {"plugin_setting", LPluginSetting},
        {"set_plugin_setting", LSetPluginSetting}, {"game_ds1", LGameDs1}, {"game_text", LGameText},
        {"module_path", LModulePath}, {"plugins_menu", LPluginsMenu}, {"plugins_menu_open", LPluginsMenuOpen},
        {"plugins_menu_row", LPluginsMenuRow}, {"plugins_menu_activate", LPluginsMenuActivate},
        {"plugins_menu_back", LPluginsMenuBack}, {"colour_picker", LColourPicker}, {"ui", LUi},{"ui_capture", LUiCapture}, {"ui_keys", LUiKeys}, {"ui_popup", LUiPopup}, {nullptr, nullptr}};
    luaL_newlib(L, dev);
    lua_pushinteger(L, static_cast<lua_Integer>(GetCurrentProcessId()));
    lua_setfield(L, -2, "pid");
    lua_setglobal(L, "dev");
    return L;
}

// ---- JSON ----
void JsonString(std::string& out, const char* s, size_t n) {
    out += '"';
    for (size_t i = 0; i < n;) {
        const auto c = static_cast<unsigned char>(s[i]);
        char esc[8];
        if (c == '"' || c == '\\') out += '\\', out += static_cast<char>(c), ++i;
        else if (c == '\n') out += "\\n", ++i;
        else if (c == '\r') out += "\\r", ++i;
        else if (c == '\t') out += "\\t", ++i;
        else if (c < 0x20 || c == 0x7F) snprintf(esc, sizeof esc, "\\u%04x", c), out += esc, ++i;
        else if (c < 0x80) out += static_cast<char>(c), ++i;
        else {
            // valid UTF-8 sequence copied, any other byte as \u00XX (Latin-1)
            const int len = c >= 0xF0 && c < 0xF5 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : 0;
            bool ok = len > 0 && i + len <= n;
            for (int k = 1; ok && k < len; ++k) ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
            if (ok) out.append(s + i, len), i += len;
            else snprintf(esc, sizeof esc, "\\u%04x", c), out += esc, ++i;
        }
    }
    out += '"';
}

// value at idx -> JSON; false + message on a cycle / too deep
bool Json(lua_State* L, int idx, std::string& out, int depth, std::string& err) {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL: out += "null"; return true;
    case LUA_TBOOLEAN: out += lua_toboolean(L, idx) ? "true" : "false"; return true;
    case LUA_TNUMBER: {
        char buf[40];
        if (lua_isinteger(L, idx)) snprintf(buf, sizeof buf, "%lld", static_cast<long long>(lua_tointeger(L, idx)));
        else {
            const double d = lua_tonumber(L, idx);
            if (!std::isfinite(d)) snprintf(buf, sizeof buf, "null");
            else {
                snprintf(buf, sizeof buf, "%.17g", d);
                if (!strpbrk(buf, ".eEn")) strcat_s(buf, ".0");   // stays a float in JSON (150.0, not 150)
            }
        }
        out += buf;
        return true;
    }
    case LUA_TSTRING: {
        size_t n;
        const char* s = lua_tolstring(L, idx, &n);
        JsonString(out, s, n);
        return true;
    }
    case LUA_TTABLE: {
        if (depth > 32) {
            err = "result nested deeper than 32 (cycle?)";
            return false;
        }
        if (!lua_checkstack(L, 4)) {
            err = "Lua stack overflow while encoding";
            return false;
        }
        lua_Integer count = 0;
        bool array = true;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            ++count;
            if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1) array = false;
            lua_pop(L, 1);
        }
        if (array && count && static_cast<lua_Integer>(lua_rawlen(L, idx)) != count) array = false;
        if (array) {
            out += '[';
            for (lua_Integer i = 1; i <= count; ++i) {
                if (i > 1) out += ',';
                lua_rawgeti(L, idx, i);
                const bool ok = Json(L, -1, out, depth + 1, err);
                lua_pop(L, 1);
                if (!ok) return false;
            }
            out += ']';
            return true;
        }
        out += '{';
        bool first = true;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            if (!first) out += ',';
            first = false;
            lua_pushvalue(L, -2);   // key copy: lua_tolstring would turn a number key into a string in place
            size_t n;
            const char* k = luaL_tolstring(L, -1, &n);
            JsonString(out, k, n);
            lua_pop(L, 2);
            out += ':';
            if (!Json(L, -1, out, depth + 1, err)) {
                lua_pop(L, 2);
                return false;
            }
            lua_pop(L, 1);
        }
        out += '}';
        return true;
    }
    default: {
        char buf[64];
        snprintf(buf, sizeof buf, "<%s %p>", luaL_typename(L, idx), lua_topointer(L, idx));
        JsonString(out, buf, strlen(buf));
        return true;
    }
    }
}

struct Options {
    bool async = false;
    int limitMs = kDefaultLimitMs, waitMs = kDefaultWaitMs;
};
Options ParseOptions(const std::string& code) {
    Options o;
    size_t p = 0;
    while (p < code.size() && code.compare(p, 3, "--@") == 0) {
        size_t e = code.find('\n', p);
        if (e == std::string::npos) e = code.size();
        std::string line = code.substr(p + 3, e - p - 3);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line == "async") o.async = true;
        else if (line.rfind("limit ", 0) == 0) o.limitMs = std::max(1, atoi(line.c_str() + 6));
        else if (line.rfind("wait ", 0) == 0) o.waitMs = std::max(0, atoi(line.c_str() + 5));
        p = e + 1;
    }
    return o;
}

std::string Run(lua_State* L, const std::string& code, const Options& o) {
    Ctx* c = CtxOf(L);
    c->printed.clear();
    c->limitMs = o.limitMs;
    const double t0 = NowMs();
    c->deadline = t0 + o.limitMs;
    lua_settop(L, 0);
    lua_pushcfunction(L, Traceback);
    std::string out = "{";
    std::string err;
    bool ok = luaL_loadbufferx(L, code.data(), code.size(), "=request", "t") == LUA_OK;
    if (ok) {
        lua_sethook(L, &Hook, LUA_MASKCOUNT, 1000);
        ok = lua_pcall(L, 0, LUA_MULTRET, 1) == LUA_OK;
        lua_sethook(L, nullptr, 0, 0);
    }
    if (!ok) {
        size_t n;
        const char* m = lua_tolstring(L, -1, &n);
        out += "\"error\":";
        if (m) JsonString(out, m, n);
        else out += "\"(error object is not a string)\"";
    } else {
        const int results = lua_gettop(L) - 1;
        std::string value;
        bool enc = true;
        if (results == 0) value = "null";
        else if (results == 1) enc = Json(L, 2, value, 0, err);
        else {
            value = "[";
            for (int i = 0; enc && i < results; ++i) {
                if (i) value += ',';
                enc = Json(L, 2 + i, value, 0, err);
            }
            value += ']';
        }
        if (enc) out += "\"result\":" + value;
        else {
            out += "\"error\":";
            JsonString(out, err.data(), err.size());
        }
    }
    lua_settop(L, 0);
    char tail[160];
    snprintf(tail, sizeof tail, ",\"ms\":%.3f,\"frame\":%u,\"thread\":\"%s\",\"print\":", NowMs() - t0, g_frame.load(),
             c->async ? "pipe" : "game");
    out += tail;
    out += '[';
    for (size_t i = 0; i < c->printed.size(); ++i) {
        if (i) out += ',';
        JsonString(out, c->printed[i].data(), c->printed[i].size());
    }
    out += "]}";
    lua_gc(L, LUA_GCSTEP, 0);
    return out;
}

std::string ErrorReply(const char* msg) {
    std::string out = "{\"error\":";
    JsonString(out, msg, strlen(msg));
    char tail[64];
    snprintf(tail, sizeof tail, ",\"frame\":%u,\"print\":[]}", g_frame.load());
    return out + tail;
}

// ---- requests ----
struct Request {
    std::string code;
    Options opt;
    std::string reply;
    bool started = false;
    HANDLE done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    ~Request() { CloseHandle(done); }
};
std::mutex g_queueLock;
std::deque<std::shared_ptr<Request>> g_queue;
std::mutex g_asyncLock;
lua_State* g_asyncLua = nullptr;
lua_State* g_gameLua = nullptr;

std::string Handle(const std::string& code) {
    const Options o = ParseOptions(code);
    if (o.async) {
        std::lock_guard<std::mutex> lock(g_asyncLock);
        if (!g_asyncLua) g_asyncLua = NewState(true);
        if (!g_asyncLua) return ErrorReply("couldn't create the Lua state");
        return Run(g_asyncLua, code, o);
    }
    auto r = std::make_shared<Request>();
    r->code = code;
    r->opt = o;
    {
        std::lock_guard<std::mutex> lock(g_queueLock);
        g_queue.push_back(r);
    }
    if (WaitForSingleObject(r->done, static_cast<DWORD>(o.waitMs)) != WAIT_OBJECT_0) {
        {
            std::lock_guard<std::mutex> lock(g_queueLock);
            if (!r->started) {
                for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
                    if (*it == r) {
                        g_queue.erase(it);
                        break;
                    }
                char msg[200];
                snprintf(msg, sizeof msg,
                         "the game thread didn't run the request within %d ms (loading, PLEASE PRESS ANY KEY or a modal "
                         "loop?) - use --@async for reads",
                         o.waitMs);
                return ErrorReply(msg);
            }
        }
        WaitForSingleObject(r->done, INFINITE);   // running: the Lua time limit ends it
    }
    return r->reply;
}

bool ReadAll(HANDLE h, void* buf, DWORD n) {
    auto* p = static_cast<uint8_t*>(buf);
    while (n) {
        DWORD got = 0;
        if (!ReadFile(h, p, n, &got, nullptr) || !got) return false;
        p += got, n -= got;
    }
    return true;
}
bool WriteAll(HANDLE h, const void* buf, DWORD n) {
    auto* p = static_cast<const uint8_t*>(buf);
    while (n) {
        DWORD put = 0;
        if (!WriteFile(h, p, n, &put, nullptr) || !put) return false;
        p += put, n -= put;
    }
    return true;
}

DWORD WINAPI ClientThread(LPVOID param) {
    const HANDLE h = param;
    for (;;) {
        uint32_t len = 0;
        if (!ReadAll(h, &len, 4) || len > kMaxFrame) break;
        std::string code(len, '\0');
        if (len && !ReadAll(h, code.data(), len)) break;
        const std::string reply = Handle(code);
        const uint32_t n = static_cast<uint32_t>(reply.size());
        if (!WriteAll(h, &n, 4) || !WriteAll(h, reply.data(), n)) break;
    }
    FlushFileBuffers(h);
    DisconnectNamedPipe(h);
    CloseHandle(h);
    return 0;
}

DWORD WINAPI ListenThread(LPVOID) {
    wchar_t name[64];
    swprintf_s(name, L"\\\\.\\pipe\\pse-dev-%lu", GetCurrentProcessId());
    bool logged = false;
    for (;;) {
        // default security: the creator (this user), SYSTEM and admins may write; remote clients refused
        const HANDLE h = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX,
                                          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                          PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            dslog::Write("[fail] Dev server: CreateNamedPipe error %lu", GetLastError());
            Sleep(1000);
            continue;
        }
        if (!logged) {
            dslog::Write("Dev server: listening on \\\\.\\pipe\\pse-dev-%lu", GetCurrentProcessId());
            logged = true;
        }
        if (!ConnectNamedPipe(h, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(h);
            continue;
        }
        if (const HANDLE t = CreateThread(nullptr, 0, &ClientThread, h, 0, nullptr)) CloseHandle(t);
        else CloseHandle(h);
    }
}
}  // namespace

void features::OnFrameDevServer() {
    if (!g_started.exchange(true)) {
        QueryPerformanceFrequency(&g_freq);
        if (const HANDLE t = CreateThread(nullptr, 0, &ListenThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    g_frame.fetch_add(1);
    const double t0 = NowMs();
    for (bool first = true; first || NowMs() - t0 < kFrameBudgetMs; first = false) {
        std::shared_ptr<Request> r;
        {
            std::lock_guard<std::mutex> lock(g_queueLock);
            if (g_queue.empty()) return;
            r = g_queue.front();
            g_queue.pop_front();
            r->started = true;
        }
        if (!g_gameLua) g_gameLua = NewState(false);
        r->reply = g_gameLua ? Run(g_gameLua, r->code, r->opt) : ErrorReply("couldn't create the Lua state");
        SetEvent(r->done);
    }
}

#else

void features::OnFrameDevServer() {}

#endif  // DS_DIST
