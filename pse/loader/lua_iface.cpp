// pse.lua, loader side: the namespace / function registry and the PSELuaInterface table (docs/sdk/reference/lua.md).
//
// Lua-agnostic - no Lua header here. Values are reached through the Backend of the binding that made the call
// (lua_bridge.h for a real lua_State); CallGlobal goes to the game layer's Host. Calls into external plugins (natives,
// CallGlobal callbacks) run under the crash guard (guard.cpp); a fault disables the plugin (PluginCrashed ->
// ForgetLua) and the script gets a Lua error.
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

#include "internal.h"
#include "lua_iface.h"

namespace pse::lua {
namespace {

struct Entry {
    uint32_t id;
    std::string ns, name, qualified;   // qualified = "ns.name"
    PSEPluginHandle owner;
    PSELuaNative fn;
    void* ctx;
    bool alive;
};

std::mutex g_mutex;                                  // PluginCrashed -> ForgetLua may come from another thread
std::deque<Entry> g_entries;                         // id = index + 1; never erased (closures keep ids)
std::map<PSEPluginHandle, std::string> g_namespaces; // owner -> its namespace
std::map<PSEPluginHandle, std::string> g_lastError;
std::atomic<uint32_t> g_generation{1};
const Host* g_host = nullptr;

constexpr const char* kReserved[] = {
    // PSE / game layer
    "game", "dev", "pse",
    // Lua's libraries (also the ones the game doesn't open) and base functions
    "string", "table", "math", "utf8", "coroutine", "io", "os", "package", "debug", "bit32", "assert",
    "collectgarbage", "dofile", "error", "getmetatable", "ipairs", "load", "loadfile", "next", "pairs", "pcall",
    "print", "rawequal", "rawget", "rawlen", "rawset", "require", "select", "setmetatable", "tonumber", "tostring",
    "type", "xpcall", "warn",
    // mission script callbacks
    "init", "update", "on_message",
};
constexpr const char* kKeywords[] = {"and",  "break", "do",   "else", "elseif", "end",   "false", "for",
                                     "function", "goto", "if", "in", "local", "nil", "not", "or", "repeat",
                                     "return", "then", "true", "until", "while"};

bool Keyword(const std::string& s) {
    for (const char* k : kKeywords)
        if (s == k) return true;
    return false;
}

// One namespace part: [a-z][a-z0-9_]*, not a keyword.
bool ValidPart(const std::string& s) {
    if (s.empty() || !(s[0] >= 'a' && s[0] <= 'z')) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return !Keyword(s);
}

bool ValidNamespace(const char* ns) {
    if (!ns) return false;
    const std::string s = ns;
    if (s.empty() || s.size() > 64) return false;
    size_t start = 0;
    bool first = true;
    while (true) {
        size_t dot = s.find('.', start);
        std::string part = s.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!ValidPart(part)) return false;
        if (first && Reserved(part.c_str())) return false;
        first = false;
        if (dot == std::string::npos) return true;
        start = dot + 1;
    }
}

bool ValidFunctionName(const char* name) {
    if (!name || !*name || std::strlen(name) > 64) return false;
    const std::string s = name;
    auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (!alpha(s[0])) return false;
    for (char c : s)
        if (!alpha(c) && !(c >= '0' && c <= '9')) return false;
    return !Keyword(s);
}

// a == b, or one is a dotted prefix of the other ("example" vs "example.sub").
bool Overlap(const std::string& a, const std::string& b) {
    if (a == b) return true;
    const std::string& shorter = a.size() < b.size() ? a : b;
    const std::string& longer = a.size() < b.size() ? b : a;
    return longer.compare(0, shorter.size(), shorter) == 0 && longer[shorter.size()] == '.';
}

void Log(PSELogLevel level, const std::string& text) { LogLine(detail::LoaderHandle(), level, text.c_str()); }

bool GameThread() {
    const PSETasksInterface* t = TasksInterface();
    return t && t->IsGameThread();
}

// Claims ns for plugin (lock held by the caller).
PSEResult Claim(PSEPluginHandle plugin, const std::string& ns) {
    auto own = g_namespaces.find(plugin);
    if (own != g_namespaces.end()) return own->second == ns ? PSE_OK : PSE_E_CONFLICT;
    for (const auto& [other, name] : g_namespaces)
        if (Overlap(name, ns)) return PSE_E_CONFLICT;
    g_namespaces[plugin] = ns;
    g_generation++;
    return PSE_OK;
}

// ---- interface functions ----------------------------------------------------------------------------------------

PSEResult PSE_CALL RegisterNamespace(PSEPluginHandle plugin, const char* ns) {
    if (!detail::ValidPlugin(plugin) || !ValidNamespace(ns)) return PSE_E_INVALID_ARG;
    if (!GameThread()) return PSE_E_WRONG_PHASE;
    PSEResult r;
    {
        std::lock_guard lock(g_mutex);
        r = Claim(plugin, ns);
    }
    if (r == PSE_E_CONFLICT)
        Log(PSE_LOG_WARN, std::string("pse.lua: ") + PluginName(plugin) + " can't have namespace '" + ns +
                              "' (taken by another plugin, or it already has one)");
    return r;
}

PSEResult PSE_CALL RegisterFunction(PSEPluginHandle plugin, const char* ns, const char* name, PSELuaNative fn,
                                    void* ctx) {
    if (!detail::ValidPlugin(plugin) || !ValidNamespace(ns) || !ValidFunctionName(name) || !fn)
        return PSE_E_INVALID_ARG;
    if (!GameThread()) return PSE_E_WRONG_PHASE;
    std::lock_guard lock(g_mutex);
    PSEResult r = Claim(plugin, ns);
    if (r != PSE_OK) {
        Log(PSE_LOG_WARN, std::string("pse.lua: ") + PluginName(plugin) + " can't have namespace '" + ns + "'");
        return r;
    }
    for (const Entry& e : g_entries)
        if (e.alive && e.ns == ns && e.name == name) return PSE_E_EXISTS;
    Entry& e = g_entries.emplace_back();
    e.id = static_cast<uint32_t>(g_entries.size());
    e.ns = ns;
    e.name = name;
    e.qualified = e.ns + "." + e.name;
    e.owner = plugin;
    e.fn = fn;
    e.ctx = ctx;
    e.alive = true;
    g_generation++;
    return PSE_OK;
}

bool Usable(PSELuaCall* call) { return call && call->backend; }

uint32_t PSE_CALL ArgCount(PSELuaCall* call) { return call ? call->argCount : 0; }

PSELuaType PSE_CALL Type(PSELuaCall* call, PSELuaSlot slot) {
    return Usable(call) ? call->backend->type(call, slot) : PSE_LUA_NONE;
}

const char* TypeName(PSELuaType t) {
    switch (t) {
        case PSE_LUA_NONE: return "no value";
        case PSE_LUA_NIL: return "nil";
        case PSE_LUA_BOOLEAN: return "boolean";
        case PSE_LUA_INTEGER:
        case PSE_LUA_NUMBER: return "number";
        case PSE_LUA_STRING: return "string";
        case PSE_LUA_TABLE: return "table";
        case PSE_LUA_FUNCTION: return "function";
        default: return "userdata";
    }
}

void Copy(char* dst, uint32_t cap, const std::string& text) {
    if (!dst || !cap) return;
    size_t n = text.size() < cap - 1 ? text.size() : cap - 1;
    std::memcpy(dst, text.data(), n);
    dst[n] = '\0';
}

void Record(PSELuaCall* call, const std::string& text) {
    if (!call) return;
    if (!call->failed) Copy(call->error, kErrorMax, text);
    call->failed = 1;
}

int BadValue(PSELuaCall* call, PSELuaSlot slot, const char* expected, const char* got = nullptr) {
    char buf[160];
    const char* kind = call->what && *call->what ? "argument" : "value";
    if (got)
        std::snprintf(buf, sizeof buf, "bad %s #%d (%s)", kind, static_cast<int>(slot), got);
    else
        std::snprintf(buf, sizeof buf, "bad %s #%d (%s expected, got %s)", kind, static_cast<int>(slot), expected,
                      TypeName(call->backend->type(call, slot)));
    Record(call, buf);
    return 0;
}

int PSE_CALL CheckInteger(PSELuaCall* call, PSELuaSlot slot, int64_t* out) {
    if (!Usable(call)) return 0;
    int64_t v = 0;
    if (call->backend->toInteger(call, slot, &v)) {
        if (out) *out = v;
        return 1;
    }
    PSELuaType t = call->backend->type(call, slot);
    double d = 0;
    if ((t == PSE_LUA_NUMBER || t == PSE_LUA_STRING) && call->backend->toNumber(call, slot, &d))
        return BadValue(call, slot, "integer", "number has no integer representation");
    return BadValue(call, slot, "integer");
}

int PSE_CALL CheckNumber(PSELuaCall* call, PSELuaSlot slot, double* out) {
    if (!Usable(call)) return 0;
    double v = 0;
    if (call->backend->toNumber(call, slot, &v)) {
        if (out) *out = v;
        return 1;
    }
    return BadValue(call, slot, "number");
}

int PSE_CALL CheckString(PSELuaCall* call, PSELuaSlot slot, const char** out, uint32_t* len) {
    if (!Usable(call)) return 0;
    uint32_t n = 0;
    const char* s = call->backend->toString(call, slot, &n);
    if (s) {
        if (out) *out = s;
        if (len) *len = n;
        return 1;
    }
    return BadValue(call, slot, "string");
}

int PSE_CALL CheckTable(PSELuaCall* call, PSELuaSlot slot) {
    if (!Usable(call)) return 0;
    if (call->backend->type(call, slot) == PSE_LUA_TABLE) return 1;
    return BadValue(call, slot, "table");
}

int PSE_CALL ToBoolean(PSELuaCall* call, PSELuaSlot slot) {
    return Usable(call) ? call->backend->toBoolean(call, slot) : 0;
}

uint32_t PSE_CALL Length(PSELuaCall* call, PSELuaSlot table) {
    return Usable(call) ? call->backend->length(call, table) : 0;
}

PSELuaSlot PSE_CALL GetField(PSELuaCall* call, PSELuaSlot table, const char* key) {
    return Usable(call) && key ? call->backend->getField(call, table, key) : 0;
}

PSELuaSlot PSE_CALL GetIndex(PSELuaCall* call, PSELuaSlot table, int64_t index) {
    return Usable(call) ? call->backend->getIndex(call, table, index) : 0;
}

int PSE_CALL Next(PSELuaCall* call, PSELuaSlot table, PSELuaSlot* key, PSELuaSlot* value) {
    return Usable(call) && key && value ? call->backend->next(call, table, key, value) : 0;
}

PSELuaSlot PSE_CALL NewNil(PSELuaCall* call) { return Usable(call) ? call->backend->newNil(call) : 0; }
PSELuaSlot PSE_CALL NewBoolean(PSELuaCall* call, int v) { return Usable(call) ? call->backend->newBoolean(call, v) : 0; }
PSELuaSlot PSE_CALL NewInteger(PSELuaCall* call, int64_t v) {
    return Usable(call) ? call->backend->newInteger(call, v) : 0;
}
PSELuaSlot PSE_CALL NewNumber(PSELuaCall* call, double v) { return Usable(call) ? call->backend->newNumber(call, v) : 0; }
PSELuaSlot PSE_CALL NewString(PSELuaCall* call, const char* s, uint32_t len) {
    if (!Usable(call)) return 0;
    if (!s) s = "", len = 0;
    return call->backend->newString(call, s, len == PSE_LUA_NUL ? std::strlen(s) : len);
}
PSELuaSlot PSE_CALL NewTable(PSELuaCall* call) { return Usable(call) ? call->backend->newTable(call) : 0; }

int PSE_CALL SetField(PSELuaCall* call, PSELuaSlot table, const char* key, PSELuaSlot value) {
    return Usable(call) && key ? call->backend->setField(call, table, key, value) : 0;
}
int PSE_CALL SetIndex(PSELuaCall* call, PSELuaSlot table, int64_t index, PSELuaSlot value) {
    return Usable(call) ? call->backend->setIndex(call, table, index, value) : 0;
}

int PSE_CALL Return(PSELuaCall* call, PSELuaSlot slot) {
    return Usable(call) && slot > 0 ? call->backend->returnValue(call, slot) : 0;
}
int PSE_CALL PushNil(PSELuaCall* call) { return Return(call, NewNil(call)); }
int PSE_CALL PushBoolean(PSELuaCall* call, int v) { return Return(call, NewBoolean(call, v)); }
int PSE_CALL PushInteger(PSELuaCall* call, int64_t v) { return Return(call, NewInteger(call, v)); }
int PSE_CALL PushNumber(PSELuaCall* call, double v) { return Return(call, NewNumber(call, v)); }
int PSE_CALL PushString(PSELuaCall* call, const char* s, uint32_t len) { return Return(call, NewString(call, s, len)); }
PSELuaSlot PSE_CALL PushTable(PSELuaCall* call) {
    PSELuaSlot t = NewTable(call);
    return t && Return(call, t) ? t : 0;
}

int PSE_CALL Error(PSELuaCall* call, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::string text = fmt ? detail::VFormat(fmt, args) : std::string("failed");
    va_end(args);
    Record(call, text);
    return PSE_LUA_ERROR;
}

const char* PSE_CALL FunctionName(PSELuaCall* call) { return call && call->what ? call->what : ""; }

PSEResult PSE_CALL CallGlobal(PSEPluginHandle plugin, const char* fn, PSELuaNative args, PSELuaNative results,
                              void* ctx) {
    if (!detail::ValidPlugin(plugin) || !fn || !*fn) return PSE_E_INVALID_ARG;
    std::string error;
    PSEResult r;
    const Host* host = g_host;
    if (!GameThread()) {
        error = "CallGlobal: not on the game thread";
        r = PSE_E_WRONG_PHASE;
    } else if (!host || !host->running || !host->running()) {
        error = "CallGlobal: no Lua level is running";
        r = PSE_E_WRONG_PHASE;
    } else {
        char buf[2048] = {};
        r = host->callGlobal(plugin, fn, args, results, ctx, buf, sizeof buf);
        error = buf;
    }
    if (r == PSE_E_SCRIPT)
        Log(PSE_LOG_WARN, std::string("pse.lua: ") + PluginName(plugin) + " called " + fn + ": " + error);
    std::lock_guard lock(g_mutex);
    if (r == PSE_OK) g_lastError.erase(plugin);
    else g_lastError[plugin] = error;
    return r;
}

uint32_t PSE_CALL LastError(PSEPluginHandle plugin, char* buf, uint32_t bufLen) {
    std::string text;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_lastError.find(plugin);
        if (it != g_lastError.end()) text = it->second;
    }
    if (buf && bufLen) {
        size_t n = text.size() < bufLen - 1 ? text.size() : bufLen - 1;
        std::memcpy(buf, text.data(), n);
        buf[n] = '\0';
    }
    return static_cast<uint32_t>(text.size());
}

int PSE_CALL ScriptRunning() {
    const Host* host = g_host;
    return host && host->running && host->running() ? 1 : 0;
}

const PSELuaInterface kTable = {
    sizeof(PSELuaInterface), 1,
    &RegisterNamespace, &RegisterFunction,
    &ArgCount, &Type, &CheckInteger, &CheckNumber, &CheckString, &CheckTable, &ToBoolean,
    &Length, &GetField, &GetIndex, &Next,
    &NewNil, &NewBoolean, &NewInteger, &NewNumber, &NewString, &NewTable, &SetField, &SetIndex,
    &Return, &PushNil, &PushBoolean, &PushInteger, &PushNumber, &PushString, &PushTable,
    &Error, &FunctionName,
    &CallGlobal, &LastError, &ScriptRunning,
};

// Runs fn for owner: direct for internal plugins, guarded for external ones. 1 = returned (*result), 0 = crashed
// (already handled: plugin disabled, *crash = the description).
int Run(PSEPluginHandle owner, PSELuaNative fn, PSELuaCall* call, void* ctx, const char* where, int* result,
        std::string* crash) {
    if (!PluginExternal(owner)) {
        *result = fn(call, ctx);
        return 1;
    }
    detail::Fault f;
    if (detail::GuardedLuaNative(fn, call, ctx, result, &f)) return 1;
    *crash = detail::DescribeFault(f);
    detail::PluginCrashed(owner, where, f);
    return 0;
}

void HSetHost(const Host* host) { g_host = host; }
uint32_t HGeneration() { return g_generation.load(); }

uint32_t HListFunctions(FunctionRecord* out, uint32_t max) {
    std::lock_guard lock(g_mutex);
    uint32_t n = 0;
    for (const Entry& e : g_entries) {
        if (!e.alive) continue;
        if (out && n < max) {
            FunctionRecord& r = out[n];
            r.id = e.id;
            Copy(r.ns, sizeof r.ns, e.ns);
            Copy(r.name, sizeof r.name, e.name);
            r.owner = e.owner;
        }
        ++n;
    }
    return n;
}

int HInvoke(uint32_t id, PSELuaCall* call, char* msg, uint32_t msgLen) {
    PSEPluginHandle owner = 0;
    PSELuaNative fn = nullptr;
    void* ctx = nullptr;
    std::string qualified;
    bool alive = false;
    {
        std::lock_guard lock(g_mutex);
        if (id >= 1 && id <= g_entries.size()) {
            const Entry& e = g_entries[id - 1];
            owner = e.owner;
            fn = e.fn;
            ctx = e.ctx;
            qualified = e.qualified;
            alive = e.alive;
        }
    }
    if (!fn || !call) {
        Copy(msg, msgLen, "unknown PSE Lua function");
        return 1;
    }
    const std::string plugin = PluginName(owner);
    if (!alive || !detail::ValidPlugin(owner)) {
        Copy(msg, msgLen, qualified + ": plugin " + plugin + " is not loaded (it crashed or was unloaded)");
        return 1;
    }
    call->what = qualified.c_str();
    int result = PSE_LUA_ERROR;
    std::string crash;
    const std::string where = "the Lua function " + qualified;
    const int returned = Run(owner, fn, call, ctx, where.c_str(), &result, &crash);
    call->what = "";
    if (!returned) {
        Copy(msg, msgLen, qualified + ": plugin " + plugin + " crashed (" + crash + ") and was disabled");
        return 1;
    }
    if (result != PSE_LUA_RETURN || call->failed) {
        Copy(msg, msgLen, qualified + ": " + (call->error[0] ? std::string(call->error) : std::string("failed")) +
                              " (plugin " + plugin + ")");
        return 1;
    }
    return 0;
}

int HInvokeCallback(PSEPluginHandle plugin, PSELuaNative fn, PSELuaCall* call, void* ctx, const char* where) {
    if (!fn) return 0;
    if (!detail::ValidPlugin(plugin) || !call) return 2;
    int result = PSE_LUA_ERROR;
    std::string crash;
    if (!Run(plugin, fn, call, ctx, where ? where : "a pse.lua callback", &result, &crash)) {
        call->failed = 0;
        Record(call, "crashed (" + crash + ")");
        return 2;
    }
    return result != PSE_LUA_RETURN || call->failed ? 1 : 0;
}

void HLevelStarted(const char* level) {
    const PSEMessagingInterface* m = MessagingInterface();
    if (m) RaiseEvent(m->EventId(PSE_LUA_EVENT_LEVEL_START), const_cast<char*>(level ? level : ""));
}

void HLevelEnded(const char* level) {
    const PSEMessagingInterface* m = MessagingInterface();
    if (m) RaiseEvent(m->EventId(PSE_LUA_EVENT_LEVEL_END), const_cast<char*>(level ? level : ""));
}

const HostInterface kHostTable = {sizeof(HostInterface), 1, &HSetHost, &HGeneration, &HListFunctions,
                                  &HInvoke, &HInvokeCallback, &HLevelStarted, &HLevelEnded};

}  // namespace

const PSELuaInterface* Interface() { return &kTable; }
const HostInterface* HostTable() { return &kHostTable; }

bool Reserved(const char* name) {
    if (!name) return true;
    for (const char* r : kReserved)
        if (std::strcmp(r, name) == 0) return true;
    return name[0] == '_' || Keyword(name);
}

}  // namespace pse::lua

namespace pse::detail {

void ResetLua() {
    std::lock_guard lock(lua::g_mutex);
    lua::g_entries.clear();
    lua::g_namespaces.clear();
    lua::g_lastError.clear();
    lua::g_generation++;
    lua::g_host = nullptr;
}

void ForgetLua(PSEPluginHandle plugin) {
    std::lock_guard lock(lua::g_mutex);
    bool changed = lua::g_namespaces.erase(plugin) > 0;
    for (lua::Entry& e : lua::g_entries)
        if (e.owner == plugin && e.alive) {
            e.alive = false;
            changed = true;
        }
    lua::g_lastError.erase(plugin);
    if (changed) lua::g_generation++;
}

}  // namespace pse::detail
