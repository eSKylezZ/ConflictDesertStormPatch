#pragma once
// Shared between the loader's service files (registry, log, messaging, tasks, settings, cosave). Not used by the
// hooks broker (hooks.cpp only sees loader.h) and not part of the SDK.
#include <cstdarg>
#include <string>
#include <vector>

#include "loader.h"
#include "pse/pse_vfs.h"

namespace pse::detail {

const Config& Cfg();
bool Initialized();
PSEPluginHandle LoaderHandle();                 // the "pse" plugin
std::vector<PSEPluginHandle> PluginsInOrder();  // registration order (= load order)
bool ValidPlugin(PSEPluginHandle plugin);

// Interface tables registered through pse.messaging (nullptr = none at >= minVersion).
const void* QueryRegisteredInterface(const char* id, uint32_t minVersion);
// Same lookup the PSELoadInterface uses: loader tables first, then registered ones.
const void* QueryAny(const char* id, uint32_t minVersion);

// printf into a std::string (no length limit).
std::string VFormat(const char* fmt, va_list args);
std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& wide);

// Per-service start / reset (called by Init / ResetForTests in registry.cpp).
void InitLog();
void ResetLog();
void ResetMessaging();
void InitTasks();
void ResetTasks();
void ResetSettings();
void ResetCosave();
void ResetPlugins();       // plugins.cpp: forget results, free external DLLs (no Unload calls)
void ResetStartup();       // startup.cpp: free the game layer DLL, forget sinks / backlog

// Drop everything a plugin left in a service (a failed / unloaded external plugin; its DLL is about to go).
void ForgetMessaging(PSEPluginHandle plugin);   // listeners, event subscriptions, registered interfaces
void ForgetTasks(PSEPluginHandle plugin);       // queued tasks
void ForgetSettings(PSEPluginHandle plugin);    // change callbacks + declarations (values stay in the registry)
void ForgetCosave(PSEPluginHandle plugin);      // save / load / revert callbacks (its chunks in saves are kept)
void ResetLua();                                // pse.lua (lua_iface.cpp)
void ForgetLua(PSEPluginHandle plugin);         // its Lua namespace + functions (closures then raise "not loaded")
void ResetVfs();                                // pse.vfs (vfs.cpp)
void ForgetVfs(PSEPluginHandle plugin);         // its providers (+ the name hash if it set it)

// ---- crash guard for calls into EXTERNAL plugins (guard.cpp, docs/sdk/reference/plugins.md "Crash guard") ---------
// Internal plugins are always called directly. External ones go through a Guarded* call (1 = returned normally,
// 0 = it faulted, *fault says how); after a fault the service calls PluginCrashed and skips the plugin's other
// callbacks of the round it is in (PluginDisabled).
struct Fault {
    uint32_t code = 0;        // exception code (0xC0000005 access violation ...)
    uintptr_t address = 0;    // where it happened
};
int GuardedEvent(PSEEventCallback cb, PSEEventId event, void* data, void* ctx, int* result, Fault* fault);
int GuardedMessage(PSEMessageCallback cb, const PSEMessage* msg, void* ctx, Fault* fault);
int GuardedTask(PSETaskFn fn, void* ctx, Fault* fault);
int GuardedSettingChanged(PSESettingChanged cb, const char* key, void* ctx, Fault* fault);
int GuardedSave(PSESaveCallback cb, PSESaveWriter* w, void* ctx, Fault* fault);
int GuardedLoadChunks(PSELoadCallback cb, PSESaveReader* r, void* ctx, Fault* fault);
int GuardedRevert(PSERevertCallback cb, void* ctx, Fault* fault);
int GuardedLoadFn(PSEPluginLoadFn fn, const PSELoadInterface* li, int* result, Fault* fault);
int GuardedUnloadFn(PSEPluginUnloadFn fn, Fault* fault);
int GuardedLuaNative(PSELuaNative fn, PSELuaCall* call, void* ctx, int* result, Fault* fault);   // pse.lua
int GuardedVfsLocate(int(PSE_CALL* fn)(const PSEVfsQuery*, PSEVfsLocation*, void*), const PSEVfsQuery* q,
                     PSEVfsLocation* out, void* ctx, int* result, Fault* fault);                     // pse.vfs
int GuardedVfsRead(int32_t(PSE_CALL* fn)(const PSEVfsQuery*, uint32_t, void*, uint32_t, void*), const PSEVfsQuery* q,
                   uint32_t offset, void* buf, uint32_t len, void* ctx, int32_t* result, Fault* fault);
std::string DescribeFault(const Fault& f);   // "exception 0xC0000005 at example.crash.dll+0x1234"
// Internal owner: direct call. External: skipped when already disabled, else guarded (+ PluginCrashed on a fault).
void CallSettingChanged(PSEPluginHandle owner, PSESettingChanged cb, const char* key, void* ctx);
void CallSave(PSEPluginHandle owner, PSESaveCallback cb, PSESaveWriter* w, void* ctx);
void CallLoad(PSEPluginHandle owner, PSELoadCallback cb, PSESaveReader* r, void* ctx);
void CallRevert(PSEPluginHandle owner, PSERevertCallback cb, void* ctx);
// plugins.cpp: an external plugin faulted after its Load (`where` = "a PSE_EVENT_FRAME subscriber" ...). Logged +
// reported, everything it registered taken back, handle disabled, result state Failed. Its DLL stays mapped (its code
// may still be on a stack - a nested call, another thread). Any thread; a second call for the same plugin only logs.
void PluginCrashed(PSEPluginHandle plugin, const char* where, const Fault& fault);

}  // namespace pse::detail

namespace pse {
const PSEVfsInterface* VfsInterface();   // pse.vfs (vfs.cpp) - loader only, reached through Query
}
