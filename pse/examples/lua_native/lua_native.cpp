// example.lua_native - adds Lua functions to mission scripts with pse.lua (docs/sdk/reference/lua.md, guide:
// docs/sdk/guides/first-plugin.md "Adding Lua functions"). Built against the SDK header only: no Lua is linked - the
// plugin reads arguments and makes results through the interface's accessor functions.
//
// Scripts of .lua levels see:
//   example.add(a, b)        a + b (integer when both are integers)
//   example.greet(name)      "Hello, <name>!"
//   example.sum(list)        sum of the numbers in an array table
//   example.info()           {name = "example.lua_native", version = "1.0.0", calls = <natives called so far>}
//   example.call(fn, ...)    calls the script's global function fn(...) back through CallGlobal, returns its first result
//   example.fail(msg)        always raises a Lua error ("example.fail: <msg> (plugin example.lua_native)")
//   example.crash()          TEST ONLY: writes to address 0. The loader catches it, disables the plugin, and the
//                            script gets a Lua error; every example.* call after that raises "not loaded".
// On "pse.lua.level_start" it calls the script's global pse_example_level_start(level) if there is one and logs the
// answer.
#include <pse/pse_api.h>

#include <cstdio>
#include <cstring>

namespace {

PSEPluginHandle g_self = 0;
const PSELogInterface* g_log = nullptr;
const PSELuaInterface* g_lua = nullptr;
int64_t g_calls = 0;

// example.add(a, b)
int PSE_CALL Add(PSELuaCall* call, void*) {
    ++g_calls;
    if (g_lua->Type(call, 1) == PSE_LUA_INTEGER && g_lua->Type(call, 2) == PSE_LUA_INTEGER) {
        int64_t a = 0, b = 0;
        g_lua->CheckInteger(call, 1, &a);
        g_lua->CheckInteger(call, 2, &b);
        g_lua->PushInteger(call, a + b);
        return PSE_LUA_RETURN;
    }
    double a = 0, b = 0;
    if (!g_lua->CheckNumber(call, 1, &a) || !g_lua->CheckNumber(call, 2, &b)) return PSE_LUA_ERROR;
    g_lua->PushNumber(call, a + b);
    return PSE_LUA_RETURN;
}

// example.greet(name)
int PSE_CALL Greet(PSELuaCall* call, void*) {
    ++g_calls;
    const char* name = nullptr;
    if (!g_lua->CheckString(call, 1, &name, nullptr)) return PSE_LUA_ERROR;
    char buf[256];
    std::snprintf(buf, sizeof buf, "Hello, %s!", name);
    g_lua->PushString(call, buf, PSE_LUA_NUL);
    return PSE_LUA_RETURN;
}

// example.sum(list)
int PSE_CALL Sum(PSELuaCall* call, void*) {
    ++g_calls;
    if (!g_lua->CheckTable(call, 1)) return PSE_LUA_ERROR;
    double total = 0;
    const uint32_t n = g_lua->Length(call, 1);
    for (uint32_t i = 1; i <= n; ++i) {
        double v = 0;
        PSELuaSlot item = g_lua->GetIndex(call, 1, i);
        if (g_lua->Type(call, item) != PSE_LUA_INTEGER && g_lua->Type(call, item) != PSE_LUA_NUMBER)
            return g_lua->Error(call, "item %u is not a number", i);
        g_lua->CheckNumber(call, item, &v);
        total += v;
    }
    g_lua->PushNumber(call, total);
    return PSE_LUA_RETURN;
}

// example.info()
int PSE_CALL Info(PSELuaCall* call, void*) {
    ++g_calls;
    PSELuaSlot t = g_lua->PushTable(call);
    g_lua->SetField(call, t, "name", g_lua->NewString(call, "example.lua_native", PSE_LUA_NUL));
    g_lua->SetField(call, t, "version", g_lua->NewString(call, "1.0.0", PSE_LUA_NUL));
    g_lua->SetField(call, t, "calls", g_lua->NewInteger(call, g_calls));
    return PSE_LUA_RETURN;
}

// example.call(fn, ...): CallGlobal with the remaining arguments, the first result back.
struct Forward {
    PSELuaCall* outer;     // the native's call: its arguments 2..n are passed on
    int64_t answerInt = 0;
    double answerNum = 0;
    char answerStr[256] = {};
    PSELuaType answerType = PSE_LUA_NIL;
};
int PSE_CALL ForwardArgs(PSELuaCall* call, void* ctx) {
    auto* f = static_cast<Forward*>(ctx);
    for (uint32_t i = 2; i <= g_lua->ArgCount(f->outer); ++i) {
        switch (g_lua->Type(f->outer, static_cast<PSELuaSlot>(i))) {
            case PSE_LUA_INTEGER: {
                int64_t v = 0;
                g_lua->CheckInteger(f->outer, static_cast<PSELuaSlot>(i), &v);
                g_lua->PushInteger(call, v);
                break;
            }
            case PSE_LUA_NUMBER: {
                double v = 0;
                g_lua->CheckNumber(f->outer, static_cast<PSELuaSlot>(i), &v);
                g_lua->PushNumber(call, v);
                break;
            }
            case PSE_LUA_STRING: {
                const char* s = nullptr;
                uint32_t len = 0;
                g_lua->CheckString(f->outer, static_cast<PSELuaSlot>(i), &s, &len);
                g_lua->PushString(call, s, len);
                break;
            }
            case PSE_LUA_BOOLEAN: g_lua->PushBoolean(call, g_lua->ToBoolean(f->outer, static_cast<PSELuaSlot>(i))); break;
            default: g_lua->PushNil(call); break;   // tables etc. are not copied between calls in this example
        }
    }
    return PSE_LUA_RETURN;
}
int PSE_CALL ReadAnswer(PSELuaCall* call, void* ctx) {
    auto* f = static_cast<Forward*>(ctx);
    f->answerType = g_lua->Type(call, 1);
    if (f->answerType == PSE_LUA_INTEGER) g_lua->CheckInteger(call, 1, &f->answerInt);
    else if (f->answerType == PSE_LUA_NUMBER) g_lua->CheckNumber(call, 1, &f->answerNum);
    else if (f->answerType == PSE_LUA_STRING) {
        const char* s = nullptr;
        g_lua->CheckString(call, 1, &s, nullptr);
        std::snprintf(f->answerStr, sizeof f->answerStr, "%s", s);
    } else if (f->answerType == PSE_LUA_BOOLEAN) f->answerInt = g_lua->ToBoolean(call, 1);
    return PSE_LUA_RETURN;
}
int PSE_CALL Call(PSELuaCall* call, void*) {
    ++g_calls;
    const char* fn = nullptr;
    if (!g_lua->CheckString(call, 1, &fn, nullptr)) return PSE_LUA_ERROR;
    Forward f;
    f.outer = call;
    PSEResult r = g_lua->CallGlobal(g_self, fn, &ForwardArgs, &ReadAnswer, &f);
    if (r != PSE_OK) {
        char err[512];
        g_lua->LastError(g_self, err, sizeof err);
        return g_lua->Error(call, "calling %s failed (%d): %s", fn, static_cast<int>(r), err);
    }
    switch (f.answerType) {
        case PSE_LUA_INTEGER: g_lua->PushInteger(call, f.answerInt); break;
        case PSE_LUA_NUMBER: g_lua->PushNumber(call, f.answerNum); break;
        case PSE_LUA_STRING: g_lua->PushString(call, f.answerStr, PSE_LUA_NUL); break;
        case PSE_LUA_BOOLEAN: g_lua->PushBoolean(call, static_cast<int>(f.answerInt)); break;
        default: g_lua->PushNil(call); break;
    }
    return PSE_LUA_RETURN;
}

// example.fail(msg)
int PSE_CALL Fail(PSELuaCall* call, void*) {
    ++g_calls;
    const char* msg = "this function always fails";
    if (g_lua->Type(call, 1) == PSE_LUA_STRING) g_lua->CheckString(call, 1, &msg, nullptr);
    return g_lua->Error(call, "%s", msg);
}

// example.crash() - test only.
int PSE_CALL Crash(PSELuaCall*, void*) {
    ++g_calls;
    g_log->Write(g_self, PSE_LOG_WARN, "lua_native: example.crash() - writing to address 0 now");
    volatile int* null = nullptr;
    *null = 1;
    return PSE_LUA_RETURN;
}

// "pse.lua.level_start": ask the script.
struct Greeting {
    char text[256] = {};
};
int PSE_CALL LevelArgs(PSELuaCall* call, void* ctx) {
    g_lua->PushString(call, static_cast<const char*>(ctx), PSE_LUA_NUL);
    return PSE_LUA_RETURN;
}
Greeting g_greeting;
int PSE_CALL LevelAnswer(PSELuaCall* call, void*) {
    const char* s = nullptr;
    if (g_lua->Type(call, 1) == PSE_LUA_STRING && g_lua->CheckString(call, 1, &s, nullptr))
        std::snprintf(g_greeting.text, sizeof g_greeting.text, "%s", s);
    return PSE_LUA_RETURN;
}
int PSE_CALL OnLevelStart(PSEEventId, void* data, void*) {
    const char* level = data ? static_cast<const char*>(data) : "";
    g_greeting.text[0] = '\0';
    PSEResult r = g_lua->CallGlobal(g_self, "pse_example_level_start", &LevelArgs, &LevelAnswer,
                                    const_cast<char*>(level));
    if (r == PSE_OK)
        g_log->Write(g_self, PSE_LOG_INFO, "lua_native: level %s started, the script says \"%s\"", level, g_greeting.text);
    else if (r == PSE_E_NOT_FOUND)
        g_log->Write(g_self, PSE_LOG_INFO, "lua_native: level %s started (no pse_example_level_start)", level);
    else {
        char err[512];
        g_lua->LastError(g_self, err, sizeof err);
        g_log->Write(g_self, PSE_LOG_WARN, "lua_native: pse_example_level_start failed (%d): %s", static_cast<int>(r), err);
    }
    return 0;
}

const char* const kGames[] = {"ds1", nullptr};

}  // namespace

extern "C" {

__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info = {
    sizeof(PSEPluginInfo), 1, "example.lua_native", "Lua natives (example plugin)", "PSE",
    PSE_MAKE_VERSION(1, 0, 0), 0, kGames,
    PSE_PLUGIN_CLIENT_ONLY,   // adds script functions only; a mission that uses them is the mod's business online
};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    g_self = pse->plugin;
    g_log = static_cast<const PSELogInterface*>(pse->Query(g_self, PSE_LOG_INTERFACE, 1));
    g_lua = static_cast<const PSELuaInterface*>(pse->Query(g_self, PSE_LUA_INTERFACE, 1));
    auto messaging = static_cast<const PSEMessagingInterface*>(pse->Query(g_self, PSE_MESSAGING_INTERFACE, 1));
    if (!g_log) return 0;
    if (!g_lua || !messaging) {
        g_log->Write(g_self, PSE_LOG_ERROR, "lua_native: the loader has no pse.lua v1");
        return 0;
    }
    struct Fn {
        const char* name;
        PSELuaNative fn;
    };
    const Fn fns[] = {{"add", &Add},   {"greet", &Greet}, {"sum", &Sum},     {"info", &Info},
                      {"call", &Call}, {"fail", &Fail},   {"crash", &Crash}};
    if (g_lua->RegisterNamespace(g_self, "example") != PSE_OK) {
        g_log->Write(g_self, PSE_LOG_ERROR, "lua_native: namespace 'example' is taken");
        return 0;
    }
    for (const Fn& f : fns)
        if (g_lua->RegisterFunction(g_self, "example", f.name, f.fn, nullptr) != PSE_OK) {
            g_log->Write(g_self, PSE_LOG_ERROR, "lua_native: example.%s not registered", f.name);
            return 0;
        }
    messaging->Subscribe(g_self, messaging->EventId(PSE_LUA_EVENT_LEVEL_START), &OnLevelStart, nullptr, 0);
    g_log->Write(g_self, PSE_LOG_INFO, "lua_native: example.add / greet / sum / info / call / fail / crash registered");
    return 1;
}

}  // extern "C"
