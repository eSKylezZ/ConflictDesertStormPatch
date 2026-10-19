// Mission runtime for translated mission DLLs (docs/sdk/mission-translation.md; generator: ModKit
// conflict_ds_tools/mission2lua.py). A translated mission is a .lua level whose code is the DLL's x86 code turned into
// Lua; it needs a few things the level's Lua state can't do by itself, provided here as raw Lua C functions:
//
//   x86rt.install()   (pse.lua native of the internal plugin dsfix.missionrt) puts the table X86RT into the running
//                     level's state (pse::lua_bridge::g_state - this DLL links the same Lua as the script host):
//   X86RT.r8/r16/r32/w8/w16/w32/rf/wf/rd/wd   real memory (the emulated DLL's memory IS process memory)
//   X86RT.copy/fill    memmove / memset for rep movs / rep stos
//   X86RT.call(t, argp, ecx, edx) -> eax, edx, ecx, popped bytes, has-float, st0
//                     a native call (game API, Windows import): 16 dwords from argp (the emulated stack) are the
//                     argument block, ecx / edx passed, stdcall pops measured, a pushed st0 returned
//   X86RT.load_image{size, base, sections, relocs, thunks, imports} -> block
//                     the DLL image in one RWX block: sections copied, relocated to the block, imports resolved by
//                     name, a 5-byte `call ThunkDispatch` at every address-taken function: when the game calls a DLL
//                     pointer (the script object's callbacks, queue_call callbacks ...) the thunk enters Lua through
//                     the global __x86_enter(va, argp, ecx, edx)
//   X86RT.stack_top() 1 MB emulated stack; X86RT.set_object(obj): the DLL's script object, installed as [0x60F578]
//                     when "pse.lua.level_start" fires (the script host has put its own there just before)
//   X86RT.fpu / f32 / fldcw / fnstcw   x87 helpers (transcendentals executed by the FPU, exact as the DLL's)
//
// "pse.lua.level_end": the generated __x86_detach() runs the DLL's DLL_PROCESS_DETACH (CRT TLS / heap freed, as
// FreeLibrary would), then the image and stack are freed.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "../../third_party/lua/lauxlib.h"
#include "../../third_party/lua/lua.h"
}

#include "core/log.h"
#include "core/pse_host.h"
#include "features/features.h"
#include "loader.h"
#include "lua_bridge.h"

namespace {
constexpr uint32_t kScriptObject = 0x60F578;
constexpr uint32_t kStackSize = 1024 * 1024;

lua_State* g_L = nullptr;        // the state that loaded the image
uint8_t* g_image = nullptr;
uint32_t g_imageSize = 0, g_origBase = 0;
uint8_t* g_stack = nullptr;
uint32_t g_object = 0;           // DLL script object, installed at level start
DWORD g_thread = 0;
int g_depth = 0;                 // native calls in progress (X86RT.call)
uint32_t g_target[64] = {};      // their targets (apitrace: a script API call made by the translated mission)

uint32_t Addr(lua_State* L, int i) { return static_cast<uint32_t>(luaL_checkinteger(L, i)); }

template <class T>
bool ReadSafe(uint32_t a, T* out) {
    __try {
        *out = *reinterpret_cast<const T*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T>
bool WriteSafe(uint32_t a, T v) {
    __try {
        *reinterpret_cast<T*>(a) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
int LRead(lua_State* L) {
    const uint32_t a = Addr(L, 1);
    T v{};
    if (!ReadSafe(a, &v)) return luaL_error(L, "x86: can't read %d byte(s) at %08X", static_cast<int>(sizeof(T)), a);
    lua_pushinteger(L, static_cast<lua_Integer>(v));
    return 1;
}
template <class T>
int LWrite(lua_State* L) {
    const uint32_t a = Addr(L, 1);
    const T v = static_cast<T>(static_cast<uint64_t>(luaL_checkinteger(L, 2)));
    if (!WriteSafe(a, v)) return luaL_error(L, "x86: can't write %d byte(s) at %08X", static_cast<int>(sizeof(T)), a);
    return 0;
}
int LReadF(lua_State* L) {
    const uint32_t a = Addr(L, 1);
    float v;
    if (!ReadSafe(a, &v)) return luaL_error(L, "x86: can't read a float at %08X", a);
    lua_pushnumber(L, static_cast<double>(v));
    return 1;
}
int LWriteF(lua_State* L) {
    const uint32_t a = Addr(L, 1);
    if (!WriteSafe(a, static_cast<float>(luaL_checknumber(L, 2)))) return luaL_error(L, "x86: can't write at %08X", a);
    return 0;
}
int LReadD(lua_State* L) {
    const uint32_t a = Addr(L, 1);
    double v;
    if (!ReadSafe(a, &v)) return luaL_error(L, "x86: can't read a double at %08X", a);
    lua_pushnumber(L, v);
    return 1;
}
int LWriteD(lua_State* L) {
    const uint32_t a = Addr(L, 1);
    if (!WriteSafe(a, luaL_checknumber(L, 2))) return luaL_error(L, "x86: can't write at %08X", a);
    return 0;
}
int LF32(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(static_cast<float>(luaL_checknumber(L, 1))));
    return 1;
}
bool MoveSafe(uint32_t dst, uint32_t src, uint32_t n) {
    __try {
        memmove(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
int LCopy(lua_State* L) {
    const uint32_t d = Addr(L, 1), s = Addr(L, 2), n = Addr(L, 3);
    if (!MoveSafe(d, s, n)) return luaL_error(L, "x86: copy %u bytes %08X -> %08X faulted", n, s, d);
    return 0;
}
bool FillSafe(uint32_t dst, uint32_t v, uint32_t size, uint32_t count) {
    __try {
        auto* p = reinterpret_cast<uint8_t*>(dst);
        if (size == 1) memset(p, static_cast<int>(v & 0xFF), count);
        else
            for (uint32_t k = 0; k < count; ++k) memcpy(p + k * size, &v, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
int LFill(lua_State* L) {
    const uint32_t d = Addr(L, 1), v = Addr(L, 2), size = Addr(L, 3), count = Addr(L, 4);
    if (!FillSafe(d, v, size, count)) return luaL_error(L, "x86: fill at %08X faulted", d);
    return 0;
}

// ---- native calls ---------------------------------------------------------------------------------------------------
struct CallOut {
    uint32_t eax, edx, ecx, pop;
    int hasFloat;
    double st0;
};
void DoCall(const uint32_t* args, uint32_t target, uint32_t ecxIn, uint32_t edxIn, CallOut* out) {
    uint32_t saveEsp = 0, eaxO = 0, edxO = 0, ecxO = 0, espAfter = 0;
    uint16_t sw0 = 0, sw1 = 0;
    double fv = 0;
    __asm {
        fnstsw sw0
        mov saveEsp, esp
        sub esp, 64
        mov esi, args
        mov edi, esp
        mov ecx, 16
        cld
        rep movsd
        mov ecx, ecxIn
        mov edx, edxIn
        mov eax, target
        call eax
        mov eaxO, eax
        mov edxO, edx
        mov ecxO, ecx
        mov espAfter, esp
        mov esp, saveEsp
        fnstsw sw1
    }
    const unsigned top0 = (sw0 >> 11) & 7, top1 = (sw1 >> 11) & 7;
    out->hasFloat = top1 == ((top0 - 1) & 7);
    if (out->hasFloat) {
        __asm {
            fstp fv
        }
    }
    out->eax = eaxO, out->edx = edxO, out->ecx = ecxO;
    out->pop = espAfter - (saveEsp - 64);
    out->st0 = fv;
}
int LCall(lua_State* L) {
    const uint32_t target = Addr(L, 1), argp = Addr(L, 2);
    uint32_t args[16];
    if (!MoveSafe(reinterpret_cast<uint32_t>(args), argp, sizeof args))
        return luaL_error(L, "x86: call %08X: arguments at %08X unreadable", target, argp);
    CallOut out{};
    if (g_depth < 64) g_target[g_depth] = target;
    ++g_depth;
    DoCall(args, target, static_cast<uint32_t>(luaL_optinteger(L, 3, 0)), static_cast<uint32_t>(luaL_optinteger(L, 4, 0)),
           &out);
    --g_depth;
    lua_pushinteger(L, out.eax);
    lua_pushinteger(L, out.edx);
    lua_pushinteger(L, out.ecx);
    lua_pushinteger(L, out.pop);
    lua_pushboolean(L, out.hasFloat);
    lua_pushnumber(L, out.st0);
    return 6;
}

// x87 operations whose results must be the FPU's own (same thread, same control word as the DLL would have).
int LFpu(lua_State* L) {
    const int op = static_cast<int>(luaL_checkinteger(L, 1));
    double a = luaL_optnumber(L, 2, 0), b = luaL_optnumber(L, 3, 0), r = 0;
    switch (op) {
    case 1: __asm { fld a
            fsin
            fstp r }
        break;
    case 2: __asm { fld a
            fcos
            fstp r }
        break;
    case 3: __asm { fld a
            fld b
            fpatan
            fstp r }
        break;   // atan(a / b): st1 = a, st0 = b
    case 4: __asm { fld a
            fptan
            fstp st(0)
            fstp r }
        break;
    case 5: __asm { fld a
            fld b
            fyl2x
            fstp r }
        break;   // a * log2(b)
    case 6: __asm { fld a
            f2xm1
            fstp r }
        break;
    case 7: __asm { fld b
            fld a
            fscale
            fstp r
            fstp st(0) }
        break;
    case 8: __asm { fld b
            fld a
            fprem
            fstp r
            fstp st(0) }
        break;
    case 9:
        switch (static_cast<int>(a)) {
        case 1: __asm { fldl2t
                fstp r }
            break;
        case 2: __asm { fldl2e
                fstp r }
            break;
        case 3: __asm { fldpi
                fstp r }
            break;
        case 4: __asm { fldlg2
                fstp r }
            break;
        default: __asm { fldln2
                fstp r }
            break;
        }
        break;
    default: return luaL_error(L, "x86: fpu op %d", op);
    }
    lua_pushnumber(L, r);
    return 1;
}
int LFldcw(lua_State* L) {
    const uint16_t cw = static_cast<uint16_t>(luaL_checkinteger(L, 1));
    __asm fldcw cw
    return 0;
}
int LFnstcw(lua_State* L) {
    uint16_t cw;
    __asm fnstcw cw
    lua_pushinteger(L, cw);
    return 1;
}

// ---- thunks: the game calls a DLL function ----------------------------------------------------------------------------
int Traceback(lua_State* L) {
    luaL_traceback(L, L, lua_tostring(L, 1), 1);
    return 1;
}
uint64_t __cdecl EnterLua(uint32_t thunkRet, uint32_t argp, uint32_t ecx, uint32_t edx) {
    const uint32_t va = thunkRet - 5 - reinterpret_cast<uint32_t>(g_image) + g_origBase;
    lua_State* L = g_L;
    if (!L || !g_image || L != pse::lua_bridge::g_state) {
        dslog::Write("[fail] Mission runtime: DLL function %08X called with no translated mission running", va);
        return 0;
    }
    if (GetCurrentThreadId() != g_thread)
        dslog::Write("[warn] Mission runtime: DLL function %08X called from another thread", va);
    const int top = lua_gettop(L);
    lua_pushcfunction(L, Traceback);
    lua_getglobal(L, "__x86_enter");
    lua_pushinteger(L, va);
    lua_pushinteger(L, argp);
    lua_pushinteger(L, ecx);
    lua_pushinteger(L, edx);
    uint32_t eax = 0, pop = 0;
    if (lua_pcall(L, 4, 2, top + 1) != LUA_OK) {
        dslog::Write("[fail] Mission runtime: %08X: %s", va, lua_tostring(L, -1));
        features::ModLog("[fail] translated mission: %08X: %s", va, lua_tostring(L, -1));
    } else {
        eax = static_cast<uint32_t>(lua_tointeger(L, -2));
        pop = static_cast<uint32_t>(lua_tointeger(L, -1));
    }
    lua_settop(L, top);
    return static_cast<uint64_t>(eax) | static_cast<uint64_t>(pop) << 32;
}
__declspec(naked) void ThunkDispatch() {
    __asm {
        push edx
        push ecx
        lea eax, [esp + 16]
        push eax
        push dword ptr [esp + 12]
        call EnterLua
        add esp, 16
        add esp, 4
        test edx, edx
        jz plain
        pop ecx
        add esp, edx
        jmp ecx
    plain:
        ret
    }
}

// ---- image -----------------------------------------------------------------------------------------------------------
void FreeImage() {
    if (g_image) VirtualFree(g_image, 0, MEM_RELEASE);
    if (g_stack) VirtualFree(g_stack, 0, MEM_RELEASE);
    g_image = g_stack = nullptr;
    g_imageSize = g_origBase = 0;
    g_object = 0;
}
int Hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
uint32_t Field(lua_State* L, int t, const char* name) {
    lua_getfield(L, t, name);
    const uint32_t v = static_cast<uint32_t>(luaL_checkinteger(L, -1));
    lua_pop(L, 1);
    return v;
}
int LLoadImage(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    if (g_image) FreeImage();   // a previous level's image that never got its level_end
    const uint32_t size = Field(L, 1, "size"), base = Field(L, 1, "base");
    auto* img = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!img) return luaL_error(L, "x86: no memory for a %u-byte image", size);
    const uint32_t delta = reinterpret_cast<uint32_t>(img) - base;
    // sections
    lua_getfield(L, 1, "sections");
    const lua_Integer nsec = luaL_len(L, -1);
    for (lua_Integer k = 1; k <= nsec; ++k) {
        lua_geti(L, -1, k);
        lua_geti(L, -1, 1);
        const uint32_t rva = static_cast<uint32_t>(lua_tointeger(L, -1));
        lua_geti(L, -2, 2);
        size_t len = 0;
        const char* hex = lua_tolstring(L, -1, &len);
        for (size_t i = 0; i + 1 < len && rva + i / 2 < size; i += 2)
            img[rva + i / 2] = static_cast<uint8_t>(Hex(hex[i]) << 4 | Hex(hex[i + 1]));
        lua_pop(L, 3);
    }
    lua_pop(L, 1);
    // relocations: hex of little-endian u32 rvas
    lua_getfield(L, 1, "relocs");
    size_t rlen = 0;
    const char* rh = lua_tolstring(L, -1, &rlen);
    for (size_t i = 0; rh && i + 8 <= rlen; i += 8) {
        uint32_t rva = 0;
        for (int b = 0; b < 4; ++b) rva |= static_cast<uint32_t>(Hex(rh[i + 2 * b]) << 4 | Hex(rh[i + 2 * b + 1])) << (8 * b);
        if (rva + 4 <= size) *reinterpret_cast<uint32_t*>(img + rva) += delta;
    }
    lua_pop(L, 1);
    // imports
    lua_getfield(L, 1, "imports");
    const lua_Integer nimp = luaL_len(L, -1);
    for (lua_Integer k = 1; k <= nimp; ++k) {
        lua_geti(L, -1, k);
        lua_geti(L, -1, 1);
        const uint32_t rva = static_cast<uint32_t>(lua_tointeger(L, -1));
        lua_geti(L, -2, 2);
        const char* dll = lua_tostring(L, -1);
        lua_geti(L, -3, 3);
        HMODULE m = dll ? GetModuleHandleA(dll) : nullptr;
        if (!m && dll) m = LoadLibraryA(dll);
        FARPROC fn = nullptr;
        if (m) fn = lua_type(L, -1) == LUA_TSTRING ? GetProcAddress(m, lua_tostring(L, -1))
                                                    : GetProcAddress(m, MAKEINTRESOURCEA(lua_tointeger(L, -1)));
        if (!fn) dslog::Write("[fail] Mission runtime: import %s!%s not found", dll ? dll : "?",
                              lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "#");
        if (rva + 4 <= size) *reinterpret_cast<uint32_t*>(img + rva) = reinterpret_cast<uint32_t>(fn);
        lua_pop(L, 4);
    }
    lua_pop(L, 1);
    // thunks
    lua_getfield(L, 1, "thunks");
    const lua_Integer nth = luaL_len(L, -1);
    for (lua_Integer k = 1; k <= nth; ++k) {
        lua_geti(L, -1, k);
        const uint32_t rva = static_cast<uint32_t>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        if (rva + 5 > size) continue;
        img[rva] = 0xE8;
        *reinterpret_cast<int32_t*>(img + rva + 1) =
            static_cast<int32_t>(reinterpret_cast<uint32_t>(&ThunkDispatch) - (reinterpret_cast<uint32_t>(img) + rva + 5));
    }
    lua_pop(L, 1);
    FlushInstructionCache(GetCurrentProcess(), img, size);
    g_stack = static_cast<uint8_t*>(VirtualAlloc(nullptr, kStackSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_image = img, g_imageSize = size, g_origBase = base;
    g_L = L;
    g_thread = GetCurrentThreadId();
    g_object = 0;
    lua_getglobal(L, "X86RT");
    lua_pushinteger(L, reinterpret_cast<uint32_t>(img));
    lua_setfield(L, -2, "image_lo");
    lua_pushinteger(L, reinterpret_cast<uint32_t>(img) + size);
    lua_setfield(L, -2, "image_hi");
    lua_pop(L, 1);
    dslog::Write("Mission runtime: image %08X..%08X (DLL base %08X, %d sections, %d imports, %d thunks)",
                 reinterpret_cast<uint32_t>(img), reinterpret_cast<uint32_t>(img) + size, base, static_cast<int>(nsec),
                 static_cast<int>(nimp), static_cast<int>(nth));
    lua_pushinteger(L, reinterpret_cast<uint32_t>(img));
    return 1;
}
int LStackTop(lua_State* L) {
    if (!g_stack) return luaL_error(L, "x86: no image loaded");
    lua_pushinteger(L, reinterpret_cast<uint32_t>(g_stack) + kStackSize - 64);
    return 1;
}
int LSetObject(lua_State* L) {
    g_object = Addr(L, 1);
    return 0;
}
int LLoaded(lua_State* L) {
    lua_pushboolean(L, g_image != nullptr && g_L == L);
    return 1;
}

// The script host registers the standard animation states for every .lua level before the script runs; the DLL's
// init registers them again. The game's register (API +0x04, FUN_00448d20: table 0x5EDF28, 0x38-byte entries, name +0,
// loaded anim +0x34) answers a state that already has an animation with MessageBoxA "Attempt to load anim twice!" and
// returns 1 without changing anything. X86RT.anim_loaded(name) = that test, so the runtime can take the same path
// without the box.
int LAnimLoaded(lua_State* L) {
    const char* name = nullptr;
    const uint32_t a = Addr(L, 1);
    char buf[0x34] = {};
    if (!MoveSafe(reinterpret_cast<uint32_t>(buf), a, 1)) return luaL_error(L, "x86: anim name at %08X", a);
    name = reinterpret_cast<const char*>(a);
    bool loaded = false;
    __try {
        for (uint32_t e = 0x5EDF28; e < 0x5F0FB8; e += 0x38)
            if (strcmp(reinterpret_cast<const char*>(e), name) == 0) {
                loaded = *reinterpret_cast<const uint32_t*>(e + 0x34) != 0;
                break;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return luaL_error(L, "x86: anim table unreadable");
    }
    lua_pushboolean(L, loaded);
    return 1;
}

// ---- registration ----------------------------------------------------------------------------------------------------
int PSE_CALL Install(PSELuaCall*, void*) {
    lua_State* L = pse::lua_bridge::g_state;
    if (!L) return PSE_LUA_ERROR;
    static const luaL_Reg fns[] = {{"r8", LRead<uint8_t>},   {"r16", LRead<uint16_t>}, {"r32", LRead<uint32_t>},
                                   {"w8", LWrite<uint8_t>},  {"w16", LWrite<uint16_t>}, {"w32", LWrite<uint32_t>},
                                   {"rf", LReadF},           {"wf", LWriteF},          {"rd", LReadD},
                                   {"wd", LWriteD},          {"f32", LF32},            {"copy", LCopy},
                                   {"fill", LFill},          {"call", LCall},          {"fpu", LFpu},
                                   {"fldcw", LFldcw},        {"fnstcw", LFnstcw},      {"load_image", LLoadImage},
                                   {"stack_top", LStackTop}, {"set_object", LSetObject}, {"loaded", LLoaded},
                                   {"anim_loaded", LAnimLoaded},
                                   {nullptr, nullptr}};
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "image_lo");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "image_hi");
    lua_setglobal(L, "X86RT");
    return PSE_LUA_RETURN;
}

PSEPluginHandle g_self = 0;
int PSE_CALL OnLevelStart(PSEEventId, void* data, void*) {
    if (g_object && g_image && g_L == pse::lua_bridge::g_state) {
        *reinterpret_cast<uint32_t*>(kScriptObject) = g_object;
        dslog::Write("Mission runtime: %s - the translated DLL's script object %08X is the level's script object",
                     data ? static_cast<const char*>(data) : "?", g_object);
    }
    return 0;
}
int PSE_CALL OnLevelEnd(PSEEventId, void*, void*) {
    if (!g_image) return 0;
    if (g_L && g_L == pse::lua_bridge::g_state) {
        lua_State* L = g_L;
        const int top = lua_gettop(L);
        lua_pushcfunction(L, Traceback);
        lua_getglobal(L, "__x86_detach");
        if (lua_isfunction(L, -1)) {
            if (lua_pcall(L, 0, 0, top + 1) != LUA_OK) dslog::Write("[fail] Mission runtime: detach: %s", lua_tostring(L, -1));
            else dslog::Write("Mission runtime: DLL_PROCESS_DETACH done");
        }
        lua_settop(L, top);
    }
    if (*reinterpret_cast<uint32_t*>(kScriptObject) == g_object) *reinterpret_cast<uint32_t*>(kScriptObject) = 0;
    FreeImage();
    g_L = nullptr;
    return 0;
}
}  // namespace

bool features::MissionRtRanges(uint32_t* imageLo, uint32_t* imageHi, uint32_t* origBase, uint32_t* stackLo,
                               uint32_t* stackHi) {
    if (!g_image) return false;
    *imageLo = reinterpret_cast<uint32_t>(g_image), *imageHi = *imageLo + g_imageSize, *origBase = g_origBase;
    *stackLo = reinterpret_cast<uint32_t>(g_stack), *stackHi = *stackLo + kStackSize;
    return true;
}
uint32_t features::MissionRtCallTarget() { return g_depth > 0 && g_depth <= 64 ? g_target[g_depth - 1] : 0; }

void features::ApplyMissionRt() {
    g_self = pse_host::Find("dsfix.missionrt");
    if (!g_self) g_self = pse_host::Core();
    const PSELoadInterface* load = pse::LoadInterfaceFor(g_self);
    const auto* lua = load ? static_cast<const PSELuaInterface*>(load->Query(g_self, PSE_LUA_INTERFACE, 1)) : nullptr;
    const PSEMessagingInterface* msg = pse::MessagingInterface();
    if (!lua || lua->RegisterFunction(g_self, "x86rt", "install", &Install, nullptr) != PSE_OK || !msg) {
        dslog::Write("[fail] Mission runtime: x86rt.install not registered - translated missions can't run");
        return;
    }
    msg->Subscribe(g_self, msg->EventId(PSE_LUA_EVENT_LEVEL_START), &OnLevelStart, nullptr, 0);
    msg->Subscribe(g_self, msg->EventId(PSE_LUA_EVENT_LEVEL_END), &OnLevelEnd, nullptr, 0);
    dslog::Write("[ok]   Mission runtime: x86rt for translated mission DLLs (mission2lua)");
}
