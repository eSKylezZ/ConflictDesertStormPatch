// Calls from the loader into EXTERNAL plugins' code under SEH (docs/sdk/reference/plugins.md "Crash guard").
//
// Every callback a plugin registers with a loader service (message listeners - PostLoad / PostPostLoad included -,
// event subscribers, tasks, settings OnChange, co-save save / load / revert) is called through one of the Guarded*
// functions below when its owner is an external plugin. A fault (access violation, stack overflow, an escaping C++
// exception ...) ends that call; the service then calls PluginCrashed (plugins.cpp), which disables the plugin the
// same way a failed PSEPlugin_Load does - hooks, byte groups, listeners, subscriptions, interfaces, tasks, settings
// and co-save callbacks taken back - and the game carries on.
//
// Internal plugins (PSE_PLUGIN_INTERNAL: DesertStormFix's own features) are called directly: no __try, no change.
//
// The functions hold no C++ objects (a __try function can't unwind them). One __try costs a few instructions on x86
// (an exception registration record pushed onto fs:[0]); test_plugins.cpp measures it for PSE_EVENT_FRAME.
#include <windows.h>

#include <malloc.h>

#include <cstdio>

#include "internal.h"

namespace pse::detail {
namespace {

int Filter(EXCEPTION_POINTERS* ep, Fault* f) {
    f->code = ep->ExceptionRecord->ExceptionCode;
    f->address = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

void AfterFault(const Fault* f) {
    if (f->code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();   // put the guard page back, or the next one kills us
}

}  // namespace

int GuardedEvent(PSEEventCallback cb, PSEEventId event, void* data, void* ctx, int* result, Fault* fault) {
    __try {
        *result = cb(event, data, ctx);
        return 1;
    } __except (Filter(GetExceptionInformation(), fault)) {
        AfterFault(fault);
        return 0;
    }
}

int GuardedMessage(PSEMessageCallback cb, const PSEMessage* msg, void* ctx, Fault* fault) {
    __try {
        cb(msg, ctx);
        return 1;
    } __except (Filter(GetExceptionInformation(), fault)) {
        AfterFault(fault);
        return 0;
    }
}

int GuardedTask(PSETaskFn fn, void* ctx, Fault* fault) {
    __try {
        fn(ctx);
        return 1;
    } __except (Filter(GetExceptionInformation(), fault)) {
        AfterFault(fault);
        return 0;
    }
}

int GuardedSettingChanged(PSESettingChanged cb, const char* key, void* ctx, Fault* fault) {
    __try {
        cb(key, ctx);
        return 1;
    } __except (Filter(GetExceptionInformation(), fault)) {
        AfterFault(fault);
        return 0;
    }
}

int GuardedSave(PSESaveCallback cb, PSESaveWriter* w, void* ctx, Fault* fault) {
    __try {
        cb(w, ctx);
        return 1;
    } __except (Filter(GetExceptionInformation(), fault)) {
        AfterFault(fault);
        return 0;
    }
}

int GuardedLoadChunks(PSELoadCallback cb, PSESaveReader* r, void* ctx, Fault* fault) {
    __try {
        cb(r, ctx);
        return 1;
    } __except (Filter(GetExceptionInformation(), fault)) {
        AfterFault(fault);
        return 0;
    }
}

int GuardedRevert(PSERevertCallback cb, void* ctx, Fault* fault) {
    __try {
        cb(ctx);
        return 1;
    } __except (Filter(GetExceptionInformation(), fault)) {
        AfterFault(fault);
        return 0;
    }
}

int GuardedLoadFn(PSEPluginLoadFn fn, const PSELoadInterface* li, int* result, Fault* fault) {
    __try {
        *result = fn(li);
        return 1;
    } __except (Filter(GetExceptionInformation(), fault)) {
        AfterFault(fault);
        return 0;
    }
}

int GuardedUnloadFn(PSEPluginUnloadFn fn, Fault* fault) {
    __try {
        fn();
        return 1;
    } __except (Filter(GetExceptionInformation(), fault)) {
        AfterFault(fault);
        return 0;
    }
}

// pse.lua natives and CallGlobal callbacks (lua_iface.cpp). Never longjmps: the Lua bindings raise Lua errors only after
// the plugin's code has returned (reference/lua.md).
int GuardedVfsLocate(int(PSE_CALL* fn)(const PSEVfsQuery*, PSEVfsLocation*, void*), const PSEVfsQuery* q,
                     PSEVfsLocation* out, void* ctx, int* result, Fault* fault) {
    __try {
        *result = fn(q, out, ctx);
        return 1;
    } __except (Filter(GetExceptionInformation(), fault)) {
        AfterFault(fault);
        return 0;
    }
}

int GuardedVfsRead(int32_t(PSE_CALL* fn)(const PSEVfsQuery*, uint32_t, void*, uint32_t, void*), const PSEVfsQuery* q,
                   uint32_t offset, void* buf, uint32_t len, void* ctx, int32_t* result, Fault* fault) {
    __try {
        *result = fn(q, offset, buf, len, ctx);
        return 1;
    } __except (Filter(GetExceptionInformation(), fault)) {
        AfterFault(fault);
        return 0;
    }
}

int GuardedLuaNative(PSELuaNative fn, PSELuaCall* call, void* ctx, int* result, Fault* fault) {
    __try {
        *result = fn(call, ctx);
        return 1;
    } __except (Filter(GetExceptionInformation(), fault)) {
        AfterFault(fault);
        return 0;
    }
}

std::string DescribeFault(const Fault& f) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "0x%08X", static_cast<unsigned>(f.address));
    std::string where = buf;
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(f.address), &mod)) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(mod, path, MAX_PATH);
        const wchar_t* file = wcsrchr(path, L'\\');
        std::snprintf(buf, sizeof buf, "+0x%X", static_cast<unsigned>(f.address - reinterpret_cast<uintptr_t>(mod)));
        where = Narrow(file ? file + 1 : path) + buf;
    }
    std::snprintf(buf, sizeof buf, "exception 0x%08X at ", static_cast<unsigned>(f.code));
    return buf + where;
}

// ---- call helpers for the services (one place decides internal = direct, external = guarded) ------------------------

void CallSettingChanged(PSEPluginHandle owner, PSESettingChanged cb, const char* key, void* ctx) {
    if (!PluginExternal(owner)) {
        cb(key, ctx);
        return;
    }
    if (PluginDisabled(owner)) return;   // crashed earlier in this round
    Fault f;
    if (!GuardedSettingChanged(cb, key, ctx, &f))
        PluginCrashed(owner, ("a settings change callback (" + std::string(key ? key : "?") + ")").c_str(), f);
}

void CallSave(PSEPluginHandle owner, PSESaveCallback cb, PSESaveWriter* w, void* ctx) {
    if (!PluginExternal(owner)) {
        cb(w, ctx);
        return;
    }
    if (PluginDisabled(owner)) return;
    Fault f;
    if (!GuardedSave(cb, w, ctx, &f)) PluginCrashed(owner, "its co-save save callback", f);
}

void CallLoad(PSEPluginHandle owner, PSELoadCallback cb, PSESaveReader* r, void* ctx) {
    if (!PluginExternal(owner)) {
        cb(r, ctx);
        return;
    }
    if (PluginDisabled(owner)) return;
    Fault f;
    if (!GuardedLoadChunks(cb, r, ctx, &f)) PluginCrashed(owner, "its co-save load callback", f);
}

void CallRevert(PSEPluginHandle owner, PSERevertCallback cb, void* ctx) {
    if (!PluginExternal(owner)) {
        cb(ctx);
        return;
    }
    if (PluginDisabled(owner)) return;
    Fault f;
    if (!GuardedRevert(cb, ctx, &f)) PluginCrashed(owner, "its co-save revert callback", f);
}

}  // namespace pse::detail
