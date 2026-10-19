#pragma once
// pse.lua, Lua side: binds the loader's Lua-agnostic registry to a real lua_State. NOT part of the SDK.
// Header-only on purpose: pse_core / pse_loader.dll never link Lua; the DLL that owns the state (DS1: dsfix.dll's
// src/features/scripthost.cpp) and pse_tests include this and link lua_static themselves. It reaches the loader only
// through the private table "pse.lua.host" (lua_iface.h: plain C, so it works across the loader / host DLL boundary).
//
// What it does (docs/sdk/reference/lua.md):
//   - Install(L): every registered namespace becomes a (nested) global table of C closures; a closure keeps only the
//     function's id, so a plugin that crashed or was unloaded later gives a Lua error, never a dangling call.
//   - Sync(L): re-installs when the registry changed (game layer: once per script update).
//   - the Backend: slots = stack index - call base; raw table access only (no metamethods, so no script code and no
//     Lua error can run while plugin code is on the stack); natives' errors are raised by the closure after the
//     plugin returned and after every C++ object of the call is gone (lua_error longjmps).
//   - CallGlobal: runs a global function of the current state for a plugin, under lua_pcall + traceback.
// One state at a time (SetState); game thread only.
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "../../third_party/lua/lauxlib.h"
#include "../../third_party/lua/lua.h"
}

#include "loader.h"
#include "lua_iface.h"

namespace pse::lua_bridge {

inline const lua::HostInterface* g_api = nullptr;   // "pse.lua.host" (Attach)
inline lua_State* g_state = nullptr;                // the running level's state (SetState)
inline uint32_t g_installed = 0;                    // registry generation last installed into g_state

inline lua_State* StateOf(PSELuaCall* c) { return static_cast<lua_State*>(c->state); }
inline bool SlotValid(PSELuaCall* c, PSELuaSlot s) { return s >= 1 && c->base + s <= lua_gettop(StateOf(c)); }
inline int Abs(PSELuaCall* c, PSELuaSlot s) { return c->base + s; }
// The newest stack value as a slot (after a push).
inline PSELuaSlot Top(PSELuaCall* c) { return static_cast<PSELuaSlot>(lua_gettop(StateOf(c)) - c->base); }
inline bool IsTable(PSELuaCall* c, PSELuaSlot s) {
    return SlotValid(c, s) && lua_type(StateOf(c), Abs(c, s)) == LUA_TTABLE;
}

inline PSELuaType BType(PSELuaCall* c, PSELuaSlot s) {
    if (!SlotValid(c, s)) return PSE_LUA_NONE;
    lua_State* L = StateOf(c);
    switch (lua_type(L, Abs(c, s))) {
        case LUA_TNIL: return PSE_LUA_NIL;
        case LUA_TBOOLEAN: return PSE_LUA_BOOLEAN;
        case LUA_TNUMBER: return lua_isinteger(L, Abs(c, s)) ? PSE_LUA_INTEGER : PSE_LUA_NUMBER;
        case LUA_TSTRING: return PSE_LUA_STRING;
        case LUA_TTABLE: return PSE_LUA_TABLE;
        case LUA_TFUNCTION: return PSE_LUA_FUNCTION;
        default: return PSE_LUA_OTHER;
    }
}
inline int BToInteger(PSELuaCall* c, PSELuaSlot s, int64_t* out) {
    if (!SlotValid(c, s)) return 0;
    int ok = 0;
    lua_Integer v = lua_tointegerx(StateOf(c), Abs(c, s), &ok);
    if (ok) *out = static_cast<int64_t>(v);
    return ok;
}
inline int BToNumber(PSELuaCall* c, PSELuaSlot s, double* out) {
    if (!SlotValid(c, s)) return 0;
    int ok = 0;
    lua_Number v = lua_tonumberx(StateOf(c), Abs(c, s), &ok);
    if (ok) *out = static_cast<double>(v);
    return ok;
}
inline const char* BToString(PSELuaCall* c, PSELuaSlot s, uint32_t* len) {
    if (!SlotValid(c, s)) return nullptr;
    const int t = lua_type(StateOf(c), Abs(c, s));
    if (t != LUA_TSTRING && t != LUA_TNUMBER) return nullptr;
    size_t n = 0;
    const char* p = lua_tolstring(StateOf(c), Abs(c, s), &n);   // a number is converted in place (as luaL_checklstring)
    *len = static_cast<uint32_t>(n);
    return p;
}
inline int BToBoolean(PSELuaCall* c, PSELuaSlot s) { return SlotValid(c, s) ? lua_toboolean(StateOf(c), Abs(c, s)) : 0; }
inline uint32_t BLength(PSELuaCall* c, PSELuaSlot t) {
    return IsTable(c, t) ? static_cast<uint32_t>(lua_rawlen(StateOf(c), Abs(c, t))) : 0;
}
inline PSELuaSlot BGetField(PSELuaCall* c, PSELuaSlot t, const char* key) {
    if (!IsTable(c, t) || !lua_checkstack(StateOf(c), 2)) return 0;
    lua_pushstring(StateOf(c), key);
    lua_rawget(StateOf(c), Abs(c, t));
    return Top(c);
}
inline PSELuaSlot BGetIndex(PSELuaCall* c, PSELuaSlot t, int64_t i) {
    if (!IsTable(c, t) || !lua_checkstack(StateOf(c), 1)) return 0;
    lua_rawgeti(StateOf(c), Abs(c, t), static_cast<lua_Integer>(i));
    return Top(c);
}
inline int BNext(PSELuaCall* c, PSELuaSlot t, PSELuaSlot* key, PSELuaSlot* value) {
    lua_State* L = StateOf(c);
    if (!IsTable(c, t) || !lua_checkstack(L, 3)) return 0;
    if (*key == 0) lua_pushnil(L);
    else if (SlotValid(c, *key)) lua_pushvalue(L, Abs(c, *key));
    else return 0;
    if (!lua_next(L, Abs(c, t))) return 0;   // popped the key
    *value = Top(c);
    *key = *value - 1;
    return 1;
}
inline PSELuaSlot BNewNil(PSELuaCall* c) {
    if (!lua_checkstack(StateOf(c), 1)) return 0;
    lua_pushnil(StateOf(c));
    return Top(c);
}
inline PSELuaSlot BNewBoolean(PSELuaCall* c, int v) {
    if (!lua_checkstack(StateOf(c), 1)) return 0;
    lua_pushboolean(StateOf(c), v);
    return Top(c);
}
inline PSELuaSlot BNewInteger(PSELuaCall* c, int64_t v) {
    if (!lua_checkstack(StateOf(c), 1)) return 0;
    lua_pushinteger(StateOf(c), static_cast<lua_Integer>(v));
    return Top(c);
}
inline PSELuaSlot BNewNumber(PSELuaCall* c, double v) {
    if (!lua_checkstack(StateOf(c), 1)) return 0;
    lua_pushnumber(StateOf(c), static_cast<lua_Number>(v));
    return Top(c);
}
inline PSELuaSlot BNewString(PSELuaCall* c, const char* s, size_t len) {
    if (!lua_checkstack(StateOf(c), 1)) return 0;
    lua_pushlstring(StateOf(c), s, len);
    return Top(c);
}
inline PSELuaSlot BNewTable(PSELuaCall* c) {
    if (!lua_checkstack(StateOf(c), 1)) return 0;
    lua_newtable(StateOf(c));
    return Top(c);
}
inline int BSetField(PSELuaCall* c, PSELuaSlot t, const char* key, PSELuaSlot v) {
    lua_State* L = StateOf(c);
    if (!IsTable(c, t) || !SlotValid(c, v) || !lua_checkstack(L, 2)) return 0;
    lua_pushstring(L, key);
    lua_pushvalue(L, Abs(c, v));
    lua_rawset(L, Abs(c, t));
    return 1;
}
inline int BSetIndex(PSELuaCall* c, PSELuaSlot t, int64_t i, PSELuaSlot v) {
    lua_State* L = StateOf(c);
    if (!IsTable(c, t) || !SlotValid(c, v) || !lua_checkstack(L, 1)) return 0;
    lua_pushvalue(L, Abs(c, v));
    lua_rawseti(L, Abs(c, t), static_cast<lua_Integer>(i));
    return 1;
}
inline int BReturn(PSELuaCall* c, PSELuaSlot s) {
    if (!SlotValid(c, s) || !c->results) return 0;
    static_cast<std::vector<PSELuaSlot>*>(c->results)->push_back(s);
    return 1;
}

inline const lua::Backend kBackend = {&BType,       &BToInteger,  &BToNumber,  &BToString,  &BToBoolean,
                                      &BLength,     &BGetField,   &BGetIndex,  &BNext,      &BNewNil,
                                      &BNewBoolean, &BNewInteger, &BNewNumber, &BNewString, &BNewTable,
                                      &BSetField,   &BSetIndex,   &BReturn};

// ---- natives --------------------------------------------------------------------------------------------------------

// >= 0: number of results on top. -1: the error message is on top (raised by NativeEntry once this function's C++
// objects are gone - lua_error longjmps).
inline int NativeCall(lua_State* L) {
    const uint32_t id = static_cast<uint32_t>(lua_tointeger(L, lua_upvalueindex(1)));
    if (!g_api) {
        lua_pushstring(L, "PSE Lua functions are not available (no pse.lua.host)");
        return -1;
    }
    std::vector<PSELuaSlot> results;
    PSELuaCall call;
    lua::InitCall(&call, &kBackend, L, 0, static_cast<uint32_t>(lua_gettop(L)), &results);
    char message[1024];
    if (g_api->Invoke(id, &call, message, sizeof message)) {
        luaL_where(L, 1);   // "chunk:line:" of the script line that called the native
        lua_pushstring(L, message);
        lua_concat(L, 2);
        return -1;
    }
    const int n = static_cast<int>(results.size());
    if (!lua_checkstack(L, n)) {
        lua_pushstring(L, "too many results from a PSE Lua function");
        return -1;
    }
    for (PSELuaSlot s : results) lua_pushvalue(L, s);
    return n;
}

inline int NativeEntry(lua_State* L) {
    const int n = NativeCall(L);
    if (n < 0) return lua_error(L);
    return n;
}

inline void Log(PSELogLevel level, const std::string& text) { LogLine(FindPlugin("pse"), level, text.c_str()); }

inline std::vector<lua::FunctionRecord> ListFunctions() {
    std::vector<lua::FunctionRecord> out;
    if (!g_api) return out;
    out.resize(g_api->ListFunctions(nullptr, 0) + 8);   // room for a few registered in between
    const uint32_t n = g_api->ListFunctions(out.data(), static_cast<uint32_t>(out.size()));
    out.resize(std::min<size_t>(n, out.size()));
    return out;
}

// Every alive function into L (protected: run through lua_pcall by Install).
inline int InstallAll(lua_State* L) {
    const std::vector<lua::FunctionRecord> fns = ListFunctions();
    const int top = lua_gettop(L);
    for (const lua::FunctionRecord& f : fns) {
        const std::string ns = f.ns, name = f.name;
        lua_pushglobaltable(L);
        bool ok = true;
        size_t start = 0;
        while (true) {
            const size_t dot = ns.find('.', start);
            const std::string part = ns.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            lua_pushlstring(L, part.data(), part.size());
            lua_rawget(L, -2);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                lua_newtable(L);
                lua_pushlstring(L, part.data(), part.size());
                lua_pushvalue(L, -2);
                lua_rawset(L, -4);
            } else if (!lua_istable(L, -1)) {
                Log(PSE_LOG_WARN, "pse.lua: '" + ns + "' is not a table in this script - " + ns + "." + name +
                                      " not installed");
                ok = false;
                break;
            }
            lua_remove(L, -2);   // the parent
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        if (ok) {
            lua_pushlstring(L, name.data(), name.size());
            lua_pushinteger(L, static_cast<lua_Integer>(f.id));
            lua_pushcclosure(L, &NativeEntry, 1);
            lua_rawset(L, -3);
        }
        lua_settop(L, top);
    }
    return 0;
}

// Installs every registered namespace into L (right after the standard libraries are opened). Returns the number of
// registered functions (0 also on a Lua error, which is logged).
inline size_t Install(lua_State* L) {
    if (!L || !g_api) return 0;
    const uint32_t generation = g_api->Generation();
    lua_pushcfunction(L, &InstallAll);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        Log(PSE_LOG_ERROR, std::string("pse.lua: installing plugin functions failed: ") +
                               (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?"));
        lua_pop(L, 1);
        return 0;
    }
    if (L == g_state) g_installed = generation;
    return g_api->ListFunctions(nullptr, 0);
}

// Re-installs when functions were registered / dropped since the last Install into the current state.
inline void Sync(lua_State* L) {
    if (L && L == g_state && g_api && g_api->Generation() != g_installed) Install(L);
}

// ---- CallGlobal -----------------------------------------------------------------------------------------------------

inline int Traceback(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    luaL_traceback(L, L, msg ? msg : "(error object is not a string)", 1);
    return 1;
}

inline void Put(char* err, uint32_t len, const std::string& text) {
    if (!err || !len) return;
    const size_t n = std::min<size_t>(text.size(), len - 1);
    std::memcpy(err, text.data(), n);
    err[n] = '\0';
}

inline PSEResult CallGlobal(PSEPluginHandle plugin, const char* fn, PSELuaNative args, PSELuaNative results,
                            void* ctx, char* err, uint32_t errLen) {
    lua_State* L = g_state;
    if (!L || !g_api) {
        Put(err, errLen, "CallGlobal: no Lua level is running");
        return PSE_E_WRONG_PHASE;
    }
    const int top = lua_gettop(L);
    if (!lua_checkstack(L, 4)) {
        Put(err, errLen, "CallGlobal: Lua stack full");
        return PSE_E_NO_MEMORY;
    }
    lua_pushcfunction(L, &Traceback);
    const int handler = lua_gettop(L);

    std::vector<PSELuaSlot> argSlots;
    PSELuaCall a;   // arguments: slot 1 = the first value the callback makes
    lua::InitCall(&a, &kBackend, L, handler, 0, &argSlots);
    if (args) {
        const int r = g_api->InvokeCallback(plugin, args, &a, ctx, "a pse.lua CallGlobal argument callback");
        if (r) {
            lua_settop(L, top);
            Put(err, errLen, std::string("CallGlobal(") + fn + "): argument callback " + (r == 2 ? "" : "failed: ") +
                                 a.error);
            return r == 2 ? PSE_E_ACCESS : PSE_E_INVALID_ARG;
        }
    }
    lua_pushglobaltable(L);
    lua_pushstring(L, fn);
    lua_rawget(L, -2);
    lua_remove(L, -2);
    if (lua_type(L, -1) != LUA_TFUNCTION) {
        lua_settop(L, top);
        Put(err, errLen, std::string("CallGlobal: the script has no global function '") + fn + "'");
        return PSE_E_NOT_FOUND;
    }
    const int func = lua_gettop(L);
    if (!lua_checkstack(L, static_cast<int>(argSlots.size()) + 2)) {
        lua_settop(L, top);
        Put(err, errLen, "CallGlobal: Lua stack full");
        return PSE_E_NO_MEMORY;
    }
    for (PSELuaSlot s : argSlots) lua_pushvalue(L, Abs(&a, s));
    if (lua_pcall(L, static_cast<int>(argSlots.size()), LUA_MULTRET, handler) != LUA_OK) {
        Put(err, errLen, lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error object is not a string)");
        lua_settop(L, top);
        return PSE_E_SCRIPT;
    }
    PSEResult result = PSE_OK;
    if (results) {
        std::vector<PSELuaSlot> unused;   // Return() in a results callback has nowhere to go
        PSELuaCall rc;                    // the function's results are slots 1..n
        lua::InitCall(&rc, &kBackend, L, func - 1, static_cast<uint32_t>(lua_gettop(L) - func + 1), &unused);
        const int r = g_api->InvokeCallback(plugin, results, &rc, ctx, "a pse.lua CallGlobal results callback");
        if (r) {
            Put(err, errLen, std::string("CallGlobal(") + fn + "): results callback " + (r == 2 ? "" : "failed: ") +
                                 rc.error);
            result = r == 2 ? PSE_E_ACCESS : PSE_E_INVALID_ARG;
        }
    }
    lua_settop(L, top);
    return result;
}

inline int Running() { return g_state != nullptr ? 1 : 0; }
inline const lua::Host kHost = {&CallGlobal, &Running};

// The owner of the Lua state: Attach(its PSELoadInterface) once at start-up (false = the loader hands it no
// pse.lua.host: not a host / built-in plugin, or an older loader - plugin functions are then simply absent);
// SetState(L) when a level's state is ready (after Install), SetState(nullptr) before closing it.
inline bool Attach(const PSELoadInterface* li) {
    if (!li || !li->Query) return false;
    auto t = static_cast<const lua::HostInterface*>(li->Query(li->plugin, PSE_LUA_HOST_INTERFACE, 1));
    if (!t || t->size < sizeof(lua::HostInterface)) return false;
    g_api = t;
    g_api->SetHost(&kHost);
    return true;
}
inline void SetState(lua_State* L) {
    g_state = L;
    g_installed = L && g_api ? g_api->Generation() : 0;
}
inline void LevelStarted(const char* level) {
    if (g_api) g_api->LevelStarted(level);
}
inline void LevelEnded(const char* level) {
    if (g_api) g_api->LevelEnded(level);
}

}  // namespace pse::lua_bridge
