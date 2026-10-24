#pragma once
// pse.lua, loader <-> Lua binding contract - NOT part of the SDK (docs/sdk/reference/lua.md "Internals").
//
// The loader (lua_iface.cpp, in pse_loader.dll) is Lua-agnostic: it keeps the registry of namespaces / functions and
// serves the public PSELuaInterface. The Lua side (lua_bridge.h, header-only) lives in whatever DLL owns the
// lua_State (DS1: dsfix.dll's scripthost.cpp; pse_tests). The two talk through the private table "pse.lua.host"
// below - plain C types only, because they are different DLLs with their own CRTs (no std:: objects cross).
#include <cstddef>
#include <cstdint>

#include "pse/pse_api.h"

#define PSE_LUA_HOST_INTERFACE "pse.lua.host"   // loader-private, version 1

namespace pse::lua {

// Value access of one Lua binding. Slots are the binding's own numbers (lua_bridge.h: stack index - call base).
struct Backend {
    PSELuaType (*type)(PSELuaCall* call, PSELuaSlot slot);
    int (*toInteger)(PSELuaCall* call, PSELuaSlot slot, int64_t* out);              // 1 if convertible
    int (*toNumber)(PSELuaCall* call, PSELuaSlot slot, double* out);
    const char* (*toString)(PSELuaCall* call, PSELuaSlot slot, uint32_t* len);      // null if not string / number
    int (*toBoolean)(PSELuaCall* call, PSELuaSlot slot);
    uint32_t (*length)(PSELuaCall* call, PSELuaSlot table);
    PSELuaSlot (*getField)(PSELuaCall* call, PSELuaSlot table, const char* key);
    PSELuaSlot (*getIndex)(PSELuaCall* call, PSELuaSlot table, int64_t index);
    int (*next)(PSELuaCall* call, PSELuaSlot table, PSELuaSlot* key, PSELuaSlot* value);
    PSELuaSlot (*newNil)(PSELuaCall* call);
    PSELuaSlot (*newBoolean)(PSELuaCall* call, int value);
    PSELuaSlot (*newInteger)(PSELuaCall* call, int64_t value);
    PSELuaSlot (*newNumber)(PSELuaCall* call, double value);
    PSELuaSlot (*newString)(PSELuaCall* call, const char* s, size_t len);
    PSELuaSlot (*newTable)(PSELuaCall* call);
    int (*setField)(PSELuaCall* call, PSELuaSlot table, const char* key, PSELuaSlot value);
    int (*setIndex)(PSELuaCall* call, PSELuaSlot table, int64_t index, PSELuaSlot value);
    int (*returnValue)(PSELuaCall* call, PSELuaSlot slot);   // Return(): append to the binding's result list
};

constexpr uint32_t kErrorMax = 512;

}  // namespace pse::lua

// The call record behind the opaque PSELuaCall: made by the binding on its stack for each call, filled by the loader's
// accessors. POD (crosses the DLL boundary).
struct PSELuaCall {
    const pse::lua::Backend* backend;
    void* state;            // binding: lua_State*
    int base;               // binding: absolute stack index of slot 0
    uint32_t argCount;
    void* results;          // binding: its result list (returnValue)
    const char* what;       // "ns.fn" while a native runs ("" in CallGlobal callbacks); set by the loader
    int failed;
    char error[pse::lua::kErrorMax];   // first recorded error (without the "ns.fn: " prefix)
};

namespace pse::lua {

inline void InitCall(PSELuaCall* c, const Backend* backend, void* state, int base, uint32_t argCount, void* results) {
    c->backend = backend;
    c->state = state;
    c->base = base;
    c->argCount = argCount;
    c->results = results;
    c->what = "";
    c->failed = 0;
    c->error[0] = '\0';
}

struct FunctionRecord {
    uint32_t id;            // never reused; the binding's closures keep it (an upvalue)
    char ns[72];            // "example" / "myname.aliens"
    char name[72];          // "add"
    PSEPluginHandle owner;
};

// The binding (game layer side). callGlobal: run global `fn` of the current state; returns PSE_OK /
// PSE_E_WRONG_PHASE / PSE_E_NOT_FOUND / PSE_E_INVALID_ARG / PSE_E_SCRIPT / PSE_E_ACCESS / PSE_E_NO_MEMORY and the
// error text in err. running: a level's state exists.
struct Host {
    PSEResult (*callGlobal)(PSEPluginHandle plugin, const char* fn, PSELuaNative args, PSELuaNative results,
                            void* ctx, char* err, uint32_t errLen);
    int (*running)();
};

// "pse.lua.host" v1 (loader-private; reached with PSELoadInterface.Query like any table).
struct HostInterface {
    uint32_t size;
    uint32_t version;  // 1
    void (*SetHost)(const Host* host);   // null = no Lua (CallGlobal -> PSE_E_WRONG_PHASE)
    uint32_t (*Generation)();            // changes whenever the function list does
    // Alive functions, by id: fills up to max, returns the total count.
    uint32_t (*ListFunctions)(FunctionRecord* out, uint32_t max);
    // Calls function `id`: internal owners directly, external ones under the crash guard. 0 = success (results through
    // returnValue); 1 = error, msg = the full Lua error text ("ns.fn: ... (plugin x)").
    int (*Invoke)(uint32_t id, PSELuaCall* call, char* msg, uint32_t msgLen);
    // A plugin's CallGlobal callback under the same rules: 0 ok, 1 recorded error / PSE_LUA_ERROR, 2 crashed
    // (plugin disabled; call->error says how).
    int (*InvokeCallback)(PSEPluginHandle plugin, PSELuaNative fn, PSELuaCall* call, void* ctx, const char* where);
    // Raise "pse.lua.level_start" / "pse.lua.level_end" (data = level name).
    void (*LevelStarted)(const char* level);
    void (*LevelEnded)(const char* level);
};

// Loader side (lua_iface.cpp).
const PSELuaInterface* Interface();
const HostInterface* HostTable();
bool Reserved(const char* name);   // names refused as (the first part of) a namespace

}  // namespace pse::lua
