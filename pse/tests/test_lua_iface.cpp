// pse.lua (pse/loader/lua_iface.cpp + lua_bridge.h): namespace rules, marshalling against a real Lua 5.4 state,
// live registration, CallGlobal, errors, a forgotten plugin, and the external example.lua_native (incl. its crash).
#include <windows.h>

#include <string>
#include <vector>

#include "internal.h"
#include "loader.h"
#include "loader_fixture.h"
#include "lua_bridge.h"
#include "pse_test.h"

extern "C" {
#include "../../third_party/lua/lualib.h"
}

using namespace pse_fixture;

namespace {

const PSELuaInterface* Lua() {
    return static_cast<const PSELuaInterface*>(pse::detail::QueryAny(PSE_LUA_INTERFACE, 1));
}

// A fresh loader + a Lua state with the game's libraries, plugin functions installed, set as the current state.
struct State {
    lua_State* L = nullptr;
    State() {
        L = luaL_newstate();
        luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
        luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
        luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
        luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
        lua_settop(L, 0);
        pse::lua_bridge::Attach(pse::LoadInterfaceFor(pse::detail::LoaderHandle()));
        pse::lua_bridge::Install(L);
        pse::lua_bridge::SetState(L);
    }
    ~State() {
        pse::lua_bridge::SetState(nullptr);
        lua_close(L);
    }
    // Runs a chunk; the first result as text (tostring), or "ERR: <message>".
    std::string Run(const char* code) {
        const int top = lua_gettop(L);
        std::string out;
        if (luaL_loadstring(L, code) != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
            out = std::string("ERR: ") + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        } else {
            out = luaL_tolstring(L, -1, nullptr);
            lua_pop(L, 1);
        }
        lua_settop(L, top);
        return out;
    }
};

bool Contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// ---- natives used by the marshalling tests -------------------------------------------------------------------------

int PSE_CALL NAdd(PSELuaCall* call, void*) {
    int64_t a = 0, b = 0;
    if (!Lua()->CheckInteger(call, 1, &a) || !Lua()->CheckInteger(call, 2, &b)) return PSE_LUA_ERROR;
    Lua()->PushInteger(call, a + b);
    return PSE_LUA_RETURN;
}

// types(...) -> "nil,boolean,integer,..." of its arguments, and its argument count as the 2nd result.
int PSE_CALL NTypes(PSELuaCall* call, void*) {
    static const char* names[] = {"none", "nil", "boolean", "integer", "number", "string", "table", "function", "other"};
    std::string s;
    for (uint32_t i = 1; i <= Lua()->ArgCount(call) + 1; ++i) {   // one past the end: "none"
        if (!s.empty()) s += ",";
        s += names[Lua()->Type(call, static_cast<PSELuaSlot>(i))];
    }
    Lua()->PushString(call, s.c_str(), PSE_LUA_NUL);
    Lua()->PushInteger(call, Lua()->ArgCount(call));
    return PSE_LUA_RETURN;
}

// echo(...) -> every argument back (Return on argument slots).
int PSE_CALL NEcho(PSELuaCall* call, void*) {
    for (uint32_t i = 1; i <= Lua()->ArgCount(call); ++i) Lua()->Return(call, static_cast<PSELuaSlot>(i));
    return PSE_LUA_RETURN;
}

// conv(x) -> integer?, number?, string?, boolean (each nil when the check fails; no error kept: clears via a new call)
int PSE_CALL NString(PSELuaCall* call, void*) {
    const char* s = nullptr;
    uint32_t len = 0;
    if (!Lua()->CheckString(call, 1, &s, &len)) return PSE_LUA_ERROR;
    Lua()->PushString(call, s, len);   // embedded NULs survive
    Lua()->PushInteger(call, len);
    return PSE_LUA_RETURN;
}

int PSE_CALL NNumber(PSELuaCall* call, void*) {
    double d = 0;
    if (!Lua()->CheckNumber(call, 1, &d)) return PSE_LUA_ERROR;
    Lua()->PushNumber(call, d * 2);
    return PSE_LUA_RETURN;
}

int PSE_CALL NTruth(PSELuaCall* call, void*) {
    Lua()->PushBoolean(call, Lua()->ToBoolean(call, 1));
    return PSE_LUA_RETURN;
}

// table_info(t) -> #t, t.name, t[2], sum of integer values via Next
int PSE_CALL NTableInfo(PSELuaCall* call, void*) {
    if (!Lua()->CheckTable(call, 1)) return PSE_LUA_ERROR;
    Lua()->PushInteger(call, Lua()->Length(call, 1));
    Lua()->Return(call, Lua()->GetField(call, 1, "name"));
    Lua()->Return(call, Lua()->GetIndex(call, 1, 2));
    int64_t sum = 0;
    PSELuaSlot k = 0, v = 0;
    int count = 0;
    while (Lua()->Next(call, 1, &k, &v)) {
        int64_t x = 0;
        if (Lua()->Type(call, v) == PSE_LUA_INTEGER && Lua()->CheckInteger(call, v, &x)) sum += x;
        ++count;
    }
    Lua()->PushInteger(call, sum);
    Lua()->PushInteger(call, count);
    return PSE_LUA_RETURN;
}

// make() -> {name = "made", list = {10, 20, 30}, flag = true, pi = 3.5, [1] = "first"}
int PSE_CALL NMake(PSELuaCall* call, void*) {
    PSELuaSlot t = Lua()->PushTable(call);
    Lua()->SetField(call, t, "name", Lua()->NewString(call, "made", PSE_LUA_NUL));
    PSELuaSlot list = Lua()->NewTable(call);
    for (int i = 1; i <= 3; ++i) Lua()->SetIndex(call, list, i, Lua()->NewInteger(call, i * 10));
    Lua()->SetField(call, t, "list", list);
    Lua()->SetField(call, t, "flag", Lua()->NewBoolean(call, 1));
    Lua()->SetField(call, t, "pi", Lua()->NewNumber(call, 3.5));
    Lua()->SetIndex(call, t, 1, Lua()->NewString(call, "first", PSE_LUA_NUL));
    PSE_CHECK_EQ(Lua()->SetField(call, 999, "x", t), 0);   // no slot 999: refused, no crash
    PSE_CHECK_EQ(Lua()->SetField(call, list + 1, "x", t), 0);   // an integer slot is not a table
    PSE_CHECK_EQ(Lua()->Return(call, 0), 0);
    return PSE_LUA_RETURN;
}

int PSE_CALL NFail(PSELuaCall* call, void*) { return Lua()->Error(call, "went wrong %d", 42); }
int PSE_CALL NFailSilently(PSELuaCall*, void*) { return PSE_LUA_ERROR; }
int PSE_CALL NName(PSELuaCall* call, void*) {
    Lua()->PushString(call, Lua()->FunctionName(call), PSE_LUA_NUL);
    return PSE_LUA_RETURN;
}

PSEPluginHandle g_caller = 0;   // for NCallBack
// callback(fn, x) -> CallGlobal(fn, x) from inside a native, first result back
struct Back {
    PSELuaCall* outer;
    int64_t answer = -1;
};
int PSE_CALL BackArgs(PSELuaCall* call, void* ctx) {
    int64_t x = 0;
    Lua()->CheckInteger(static_cast<Back*>(ctx)->outer, 2, &x);
    Lua()->PushInteger(call, x);
    return PSE_LUA_RETURN;
}
int PSE_CALL BackResults(PSELuaCall* call, void* ctx) {
    return Lua()->CheckInteger(call, 1, &static_cast<Back*>(ctx)->answer) ? PSE_LUA_RETURN : PSE_LUA_ERROR;
}
int PSE_CALL NCallBack(PSELuaCall* call, void*) {
    const char* fn = nullptr;
    if (!Lua()->CheckString(call, 1, &fn, nullptr)) return PSE_LUA_ERROR;
    Back b{call};
    PSEResult r = Lua()->CallGlobal(g_caller, fn, &BackArgs, &BackResults, &b);
    if (r != PSE_OK) return Lua()->Error(call, "CallGlobal %d", static_cast<int>(r));
    Lua()->PushInteger(call, b.answer);
    return PSE_LUA_RETURN;
}

}  // namespace

PSE_TEST("lua: interface served, namespace and name rules") {
    Fresh();
    const PSELuaInterface* lua = Lua();
    PSE_REQUIRE(lua != nullptr);
    PSE_CHECK_EQ(lua->size, static_cast<uint32_t>(sizeof(PSELuaInterface)));
    PSE_CHECK_EQ(lua->version, 1u);
    PSE_CHECK(pse::LoadInterfaceFor(pse::detail::LoaderHandle())->Query(pse::detail::LoaderHandle(), PSE_LUA_INTERFACE, 1) == lua);
    PSE_CHECK(pse::detail::QueryAny(PSE_LUA_INTERFACE, 2) == nullptr);

    PSEPluginHandle a = AddPlugin("test.a"), b = AddPlugin("test.b"), c = AddPlugin("test.c");
    // reserved / invalid
    for (const char* bad : {"game", "dev", "pse", "string", "math", "print", "pairs", "init", "update", "on_message",
                            "_hidden", "end", "Upper", "1abc", "a-b", "", "a..b", "a.", ".a", "x.end", "x._y"})
        PSE_CHECK_EQ(lua->RegisterNamespace(a, bad), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(lua->RegisterNamespace(a, nullptr), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(lua->RegisterNamespace(a, std::string(65, 'a').c_str()), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(lua->RegisterNamespace(0, "fine"), PSE_E_INVALID_ARG);
    // "game" is reserved only as the first part
    PSE_CHECK_EQ(lua->RegisterNamespace(a, "kyle.game"), PSE_OK);
    PSE_CHECK_EQ(lua->RegisterNamespace(a, "kyle.game"), PSE_OK);              // own namespace again
    PSE_CHECK_EQ(lua->RegisterNamespace(a, "other"), PSE_E_CONFLICT);          // one per plugin
    PSE_CHECK_EQ(lua->RegisterNamespace(b, "kyle.game"), PSE_E_CONFLICT);      // taken
    PSE_CHECK_EQ(lua->RegisterNamespace(b, "kyle"), PSE_E_CONFLICT);           // prefix of a's
    PSE_CHECK_EQ(lua->RegisterNamespace(b, "kyle.game.x"), PSE_E_CONFLICT);    // inside a's
    PSE_CHECK_EQ(lua->RegisterNamespace(b, "kyle.gamer"), PSE_OK);             // not a dotted prefix
    // functions
    PSE_CHECK_EQ(lua->RegisterFunction(c, "alien", "spawn", &NAdd, nullptr), PSE_OK);   // claims "alien"
    PSE_CHECK_EQ(lua->RegisterFunction(c, "alien", "spawn", &NAdd, nullptr), PSE_E_EXISTS);
    PSE_CHECK_EQ(lua->RegisterFunction(c, "alien", "Spawn_2", &NAdd, nullptr), PSE_OK);
    PSE_CHECK_EQ(lua->RegisterFunction(c, "beta", "x", &NAdd, nullptr), PSE_E_CONFLICT);
    PSE_CHECK_EQ(lua->RegisterFunction(a, "alien", "x", &NAdd, nullptr), PSE_E_CONFLICT);
    for (const char* bad : {"", "2x", "a.b", "a-b", "end", "nil"})
        PSE_CHECK_EQ(lua->RegisterFunction(c, "alien", bad, &NAdd, nullptr), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(lua->RegisterFunction(c, "alien", "nofn", nullptr, nullptr), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(pse::lua::HostTable()->ListFunctions(nullptr, 0), 2u);
    PSE_CHECK(AnyLineContains("can't have namespace"));
    // off the game thread
    struct Arg {
        const PSELuaInterface* lua;
        PSEPluginHandle c;
        PSEResult r;
    } arg{lua, c, PSE_OK};
    HANDLE t = CreateThread(nullptr, 0, [](void* p) -> DWORD {
        auto* x = static_cast<Arg*>(p);
        x->r = x->lua->RegisterFunction(x->c, "alien", "late", &NAdd, nullptr);
        return 0;
    }, &arg, 0, nullptr);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    PSE_CHECK_EQ(arg.r, PSE_E_WRONG_PHASE);
    PSE_CHECK_EQ(lua->ScriptRunning(), 0);
    DeleteTestKey();
}

PSE_TEST("lua: marshalling - arguments, results, tables, conversions") {
    Fresh();
    const PSELuaInterface* lua = Lua();
    PSEPluginHandle p = AddPlugin("test.lua");
    struct {
        const char* name;
        PSELuaNative fn;
    } fns[] = {{"add", &NAdd},       {"types", &NTypes}, {"echo", &NEcho}, {"str", &NString},
               {"num", &NNumber},    {"truth", &NTruth}, {"tinfo", &NTableInfo}, {"make", &NMake},
               {"fail", &NFail},     {"quiet", &NFailSilently}, {"name", &NName}};
    for (auto& f : fns) PSE_CHECK_EQ(lua->RegisterFunction(p, "t", f.name, f.fn, nullptr), PSE_OK);
    State s;
    PSE_CHECK_EQ(lua->ScriptRunning(), 1);
    PSE_CHECK_STR(s.Run("return type(t)").c_str(), "table");
    PSE_CHECK_STR(s.Run("return t.add(2, 3)").c_str(), "5");
    PSE_CHECK_STR(s.Run("return math.type(t.add(2, 3))").c_str(), "integer");
    PSE_CHECK_STR(s.Run("return t.add(2.0, '3')").c_str(), "5");                    // integral float, numeric string
    PSE_CHECK_STR(s.Run("return t.add(9007199254740993, 1)").c_str(), "9007199254740994");   // 64-bit
    PSE_CHECK_STR(s.Run("return select('#', t.add(1, 1))").c_str(), "1");
    PSE_CHECK_STR(s.Run("local a, n = t.types(nil, true, 1, 1.5, 'x', {}, print); return a .. ' ' .. n").c_str(),
                  "nil,boolean,integer,number,string,table,function,none 7");
    PSE_CHECK_STR(s.Run("local a, b, c = t.echo(1, 'two', {3}); return a .. b .. c[1]").c_str(), "1two3");
    PSE_CHECK_STR(s.Run("return select('#', t.echo())").c_str(), "0");
    PSE_CHECK_STR(s.Run("local x, n = t.str('a\\0b'); return #x .. ' ' .. n").c_str(), "3 3");
    PSE_CHECK_STR(s.Run("return t.str(12)").c_str(), "12");                        // number -> string, like Lua
    PSE_CHECK_STR(s.Run("return t.num('2.5')").c_str(), "5.0");
    PSE_CHECK_STR(s.Run("return tostring(t.truth(nil)) .. tostring(t.truth(false)) .. tostring(t.truth(0)) .. tostring(t.truth())").c_str(),
                  "falsefalsetruefalse");
    PSE_CHECK_STR(s.Run("local n, name, second, sum, count = t.tinfo({5, 6, 7, name = 'x', z = 100}); "
                        "return n .. name .. second .. ' ' .. sum .. ' ' .. count").c_str(),
                  "3x6 118 5");
    PSE_CHECK_STR(s.Run("local n, name, second, sum, count = t.tinfo({}); return n .. tostring(name) .. tostring(second) .. sum .. count").c_str(),
                  "0nilnil00");
    PSE_CHECK_STR(s.Run("local m = t.make(); return m.name .. m.list[3] .. #m.list .. tostring(m.flag) .. m.pi .. m[1]").c_str(),
                  "made303true3.5first");
    PSE_CHECK_STR(s.Run("return t.name()").c_str(), "t.name");
    // metamethods are not run by the accessors (raw access)
    PSE_CHECK_STR(s.Run("local x = setmetatable({}, {__index = function() error('meta!') end, __len = function() return 99 end}); "
                        "local n, name = t.tinfo(x); return n .. tostring(name)").c_str(),
                  "0nil");
    DeleteTestKey();
}

PSE_TEST("lua: errors - bad arguments, Error, silent failure, position, script keeps running") {
    Fresh();
    const PSELuaInterface* lua = Lua();
    PSEPluginHandle p = AddPlugin("test.err");
    lua->RegisterFunction(p, "e", "add", &NAdd, nullptr);
    lua->RegisterFunction(p, "e", "fail", &NFail, nullptr);
    lua->RegisterFunction(p, "e", "quiet", &NFailSilently, nullptr);
    lua->RegisterFunction(p, "e", "num", &NNumber, nullptr);
    lua->RegisterFunction(p, "e", "tinfo", &NTableInfo, nullptr);
    State s;
    std::string r = s.Run("return e.add(1, 'x')");
    PSE_CHECK(Contains(r, "ERR: "));
    PSE_CHECK(Contains(r, "e.add: bad argument #2 (integer expected, got string) (plugin test.err)"));
    PSE_CHECK(Contains(r, "[string \"return e.add(1, 'x')\"]:1:"));   // the calling script line
    PSE_CHECK(Contains(s.Run("return e.add(1)"), "bad argument #2 (integer expected, got no value)"));
    PSE_CHECK(Contains(s.Run("return e.add(1.5, 1)"), "bad argument #1 (number has no integer representation)"));
    PSE_CHECK(Contains(s.Run("return e.num({})"), "bad argument #1 (number expected, got table)"));
    PSE_CHECK(Contains(s.Run("return e.tinfo(5)"), "bad argument #1 (table expected, got number)"));
    PSE_CHECK(Contains(s.Run("return e.fail()"), "e.fail: went wrong 42 (plugin test.err)"));
    PSE_CHECK(Contains(s.Run("return e.quiet()"), "e.quiet: failed (plugin test.err)"));
    // caught by the script itself
    PSE_CHECK_STR(s.Run("local ok, msg = pcall(e.fail); return tostring(ok) .. ' ' .. tostring(msg:find('went wrong') ~= nil)").c_str(),
                  "false true");
    PSE_CHECK_STR(s.Run("return e.add(20, 22)").c_str(), "42");   // still fine afterwards
    PSE_CHECK_EQ(lua_gettop(s.L), 0);
    DeleteTestKey();
}

PSE_TEST("lua: registration while a level runs (Sync), forgotten plugin, script globals left alone") {
    Fresh();
    const PSELuaInterface* lua = Lua();
    PSEPluginHandle p = AddPlugin("test.live"), q = AddPlugin("test.other");
    lua->RegisterFunction(p, "live", "add", &NAdd, nullptr);
    State s;
    PSE_CHECK_STR(s.Run("return live.add(1, 2)").c_str(), "3");
    PSE_CHECK(Contains(s.Run("return live.later(1, 2)"), "attempt to call a nil value"));
    lua->RegisterFunction(p, "live", "later", &NAdd, nullptr);
    lua->RegisterFunction(q, "deep.ns.here", "add", &NAdd, nullptr);
    PSE_CHECK(Contains(s.Run("return live.later(1, 2)"), "attempt to call a nil value"));   // not before the update
    pse::lua_bridge::Sync(s.L);
    PSE_CHECK_STR(s.Run("return live.later(1, 2)").c_str(), "3");
    PSE_CHECK_STR(s.Run("return deep.ns.here.add(4, 4)").c_str(), "8");
    // a script that made its own global of that name keeps it
    s.Run("other = 5");
    PSEPluginHandle r = AddPlugin("test.third");
    lua->RegisterFunction(r, "other", "f", &NAdd, nullptr);
    pse::lua_bridge::Sync(s.L);
    PSE_CHECK_STR(s.Run("return other").c_str(), "5");
    PSE_CHECK(AnyLineContains("'other' is not a table in this script"));
    // the plugin goes away (crash / unload): its closures raise, others still work
    pse::detail::ForgetLua(p);
    PSE_CHECK(Contains(s.Run("return live.add(1, 2)"), "live.add: plugin test.live is not loaded"));
    PSE_CHECK_STR(s.Run("return deep.ns.here.add(1, 1)").c_str(), "2");
    PSE_CHECK_EQ(pse::lua::HostTable()->ListFunctions(nullptr, 0), 2u);   // deep.ns.here.add + other.f
    PSE_CHECK_EQ(lua->RegisterNamespace(q, "live"), PSE_E_CONFLICT);   // q has its own already
    PSEPluginHandle n = AddPlugin("test.new");
    PSE_CHECK_EQ(lua->RegisterNamespace(n, "live"), PSE_OK);           // freed by ForgetLua
    // a new state (next level) gets everything at once
    {
        State s2;
        PSE_CHECK_STR(s2.Run("return deep.ns.here.add(2, 2) .. tostring(live)").c_str(), "4nil");
    }
    DeleteTestKey();
}

PSE_TEST("lua: CallGlobal - arguments, results, nested, not found, script error, no state") {
    Fresh();
    const PSELuaInterface* lua = Lua();
    PSEPluginHandle p = AddPlugin("test.caller");
    g_caller = p;
    lua->RegisterFunction(p, "cb", "back", &NCallBack, nullptr);
    struct Ctx {
        int64_t sum = 0;
        std::string text;
        uint32_t count = 0;
    } ctx;
    auto args = [](PSELuaCall* call, void*) -> int {
        Lua()->PushInteger(call, 40);
        Lua()->PushString(call, "x", PSE_LUA_NUL);
        PSELuaSlot t = Lua()->PushTable(call);
        Lua()->SetField(call, t, "v", Lua()->NewInteger(call, 2));
        return PSE_LUA_RETURN;
    };
    auto results = [](PSELuaCall* call, void* c) -> int {
        auto* x = static_cast<Ctx*>(c);
        x->count = Lua()->ArgCount(call);
        const char* s = nullptr;
        if (!Lua()->CheckInteger(call, 1, &x->sum) || !Lua()->CheckString(call, 2, &s, nullptr)) return PSE_LUA_ERROR;
        x->text = s;
        return PSE_LUA_RETURN;
    };
    // no state yet
    PSE_CHECK_EQ(lua->CallGlobal(p, "f", nullptr, nullptr, nullptr), PSE_E_WRONG_PHASE);
    char buf[256];
    PSE_CHECK(lua->LastError(p, buf, sizeof buf) > 0);
    PSE_CHECK(Contains(buf, "no Lua level is running"));
    {
        State s;
        s.Run("function f(a, b, t) return a + t.v, b .. '!', 'extra' end "
              "function boom() error('kaboom') end "
              "function double(x) return x * 2 end "
              "not_a_function = 3");
        PSE_CHECK_EQ(lua->CallGlobal(p, "f", args, results, &ctx), PSE_OK);
        PSE_CHECK_EQ(ctx.sum, 42);
        PSE_CHECK_STR(ctx.text.c_str(), "x!");
        PSE_CHECK_EQ(ctx.count, 3u);
        PSE_CHECK_EQ(lua->LastError(p, buf, sizeof buf), 0u);
        PSE_CHECK_EQ(lua->CallGlobal(p, "f", args, nullptr, nullptr), PSE_OK);   // results ignored
        PSE_CHECK_EQ(lua->CallGlobal(p, "missing", nullptr, nullptr, nullptr), PSE_E_NOT_FOUND);
        PSE_CHECK_EQ(lua->CallGlobal(p, "not_a_function", nullptr, nullptr, nullptr), PSE_E_NOT_FOUND);
        PSE_CHECK_EQ(lua->CallGlobal(p, "boom", nullptr, nullptr, nullptr), PSE_E_SCRIPT);
        lua->LastError(p, buf, sizeof buf);
        PSE_CHECK(Contains(buf, "kaboom"));
        PSE_CHECK(Contains(buf, "stack traceback"));
        PSE_CHECK(AnyLineContains("test.caller called boom"));
        // arguments that fail: not called
        auto badArgs = [](PSELuaCall* call, void*) -> int { return Lua()->Error(call, "no arguments for you"); };
        s.Run("called = 0 function count() called = called + 1 end");
        PSE_CHECK_EQ(lua->CallGlobal(p, "count", badArgs, nullptr, nullptr), PSE_E_INVALID_ARG);
        PSE_CHECK_STR(s.Run("return called").c_str(), "0");
        lua->LastError(p, buf, sizeof buf);
        PSE_CHECK(Contains(buf, "no arguments for you"));
        // results that fail
        auto badResults = [](PSELuaCall* call, void*) -> int {
            int64_t x = 0;
            return Lua()->CheckInteger(call, 5, &x) ? PSE_LUA_RETURN : PSE_LUA_ERROR;
        };
        PSE_CHECK_EQ(lua->CallGlobal(p, "f", args, badResults, nullptr), PSE_E_INVALID_ARG);
        lua->LastError(p, buf, sizeof buf);
        PSE_CHECK(Contains(buf, "bad value #5 (integer expected, got no value)"));
        // from inside a native, back into the script
        PSE_CHECK_STR(s.Run("return cb.back('double', 21)").c_str(), "42");
        PSE_CHECK(Contains(s.Run("return cb.back('boom', 1)"), "cb.back: CallGlobal 11"));
        PSE_CHECK_EQ(lua_gettop(s.L), 0);   // nothing left on the stack
        // bad args
        PSE_CHECK_EQ(lua->CallGlobal(p, nullptr, nullptr, nullptr, nullptr), PSE_E_INVALID_ARG);
        PSE_CHECK_EQ(lua->CallGlobal(0, "f", nullptr, nullptr, nullptr), PSE_E_INVALID_ARG);
    }
    PSE_CHECK_EQ(lua->CallGlobal(p, "f", nullptr, nullptr, nullptr), PSE_E_WRONG_PHASE);
    DeleteTestKey();
}

namespace {
struct PluginFolder {
    std::string dir;
    PluginFolder() {
        dir = TempPath("lua-plugins");
        Clean();
        CreateDirectoryA(dir.c_str(), nullptr);
        for (const char* ext : {".dll", ".pse"})
            CopyFileA((std::string(PSE_PLUGIN_DIR) + "\\example.lua_native" + ext).c_str(),
                      (dir + "\\example.lua_native" + ext).c_str(), FALSE);
    }
    ~PluginFolder() { Clean(); }
    void Clean() {
        for (const char* ext : {".dll", ".pse"}) DeleteFileA((dir + "\\example.lua_native" + ext).c_str());
        RemoveDirectoryA(dir.c_str());
    }
};
}  // namespace

PSE_TEST("lua: external example.lua_native - natives, CallGlobal on level start, crash disables it") {
    if (GetFileAttributesA((std::string(PSE_PLUGIN_DIR) + "\\example.lua_native.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
        PSE_SKIP("example.lua_native not built in " PSE_PLUGIN_DIR);
    Fresh();
    PluginFolder folder;
    PSE_CHECK_EQ(pse::LoadPlugins(std::wstring(folder.dir.begin(), folder.dir.end())), 1u);
    PSEPluginHandle h = pse::FindPlugin("example.lua_native");
    PSE_REQUIRE(h != 0);
    // the private binding table is for the state's owner only
    PSE_CHECK(pse::LoadInterfaceFor(h)->Query(h, PSE_LUA_HOST_INTERFACE, 1) == nullptr);
    PSE_CHECK(pse::LoadInterfaceFor(pse::detail::LoaderHandle())->Query(pse::detail::LoaderHandle(),
                                                                        PSE_LUA_HOST_INTERFACE, 1) != nullptr);
    PSE_CHECK(pse::LoadInterfaceFor(h)->Query(h, PSE_LUA_INTERFACE, 1) != nullptr);
    PSE_CHECK(AnyLineContains("lua_native: example.add / greet / sum / info / call / fail / crash registered"));
    {
        State s;
        s.Run("function pse_example_level_start(level) return 'hi from ' .. level end "
              "function triple(x) return x * 3 end");
        pse::lua_bridge::LevelStarted("test.lua");
        PSE_CHECK(AnyLineContains("lua_native: level test.lua started, the script says \"hi from test.lua\""));
        PSE_CHECK_STR(s.Run("return example.add(2, 3)").c_str(), "5");
        PSE_CHECK_STR(s.Run("return example.add(2, 0.5)").c_str(), "2.5");
        PSE_CHECK_STR(s.Run("return example.greet('Bradley')").c_str(), "Hello, Bradley!");
        PSE_CHECK_STR(s.Run("return example.sum({1, 2, 3.5})").c_str(), "6.5");
        PSE_CHECK(Contains(s.Run("return example.sum({1, 'x'})"), "example.sum: item 2 is not a number (plugin example.lua_native)"));
        PSE_CHECK_STR(s.Run("local i = example.info(); return i.name .. ' ' .. i.version .. ' ' .. math.type(i.calls)").c_str(),
                      "example.lua_native 1.0.0 integer");
        PSE_CHECK_STR(s.Run("return example.call('triple', 14)").c_str(), "42");
        PSE_CHECK(Contains(s.Run("return example.fail('custom')"), "example.fail: custom (plugin example.lua_native)"));
        PSE_CHECK(Contains(s.Run("return example.greet()"), "example.greet: bad argument #1 (string expected, got no value)"));
        // the crash: a Lua error, the plugin disabled, the test process (= the game) carries on
        std::string r = s.Run("return example.crash()");
        PSE_CHECK(Contains(r, "example.crash: plugin example.lua_native crashed (exception 0xC0000005"));
        PSE_CHECK(Contains(r, "and was disabled"));
        PSE_CHECK(pse::PluginDisabled(h));
        PSE_CHECK(pse::FindPlugin("example.lua_native") == 0);
        PSE_CHECK(Contains(s.Run("return example.add(1, 1)"), "example.add: plugin example.lua_native is not loaded"));
        PSE_CHECK_EQ(pse::lua::HostTable()->ListFunctions(nullptr, 0), 0u);
        PSE_CHECK(AnyLineContains("crashed in the Lua function example.crash"));
        PSE_CHECK_STR(s.Run("return 1 + 1").c_str(), "2");
        pse::lua_bridge::LevelStarted("again.lua");   // its event subscription is gone too
        PSE_CHECK(!AnyLineContains("level again.lua started"));
    }
    pse::UnloadPlugins();
    Fresh();
    DeleteTestKey();
}
