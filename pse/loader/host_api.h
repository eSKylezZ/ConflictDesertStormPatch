/*
 * PSE PRIVATE host ABI (docs/sdk/layout.md, docs/sdk/reference/plugins.md "Host plugins"). NOT part of the public SDK:
 * no stability promise, version 0, may change in any release. Shared between the parts PSE ships together:
 *
 *   - the shim (dinput8.dll ...)      -> pse_loader.dll   PSELoader_Start
 *   - pse_loader.dll                  -> PSE\Games\pse_<game>.dll   PSEGame_Info / PSEGame_Detect / PSEGame_Init
 *   - pse_loader.dll                  -> trusted HOST plugins (DesertStormFix's dsfix.dll) and game layers: the
 *                                        "pse.host" table, reached only through their own PSELoadInterface.Query.
 *
 * Plain C, 32-bit x86, __cdecl (PSE_CALL), like the public API. Each side has its own CRT: strings / buffers handed
 * across are copied, never freed by the other side.
 */
#ifndef PSE_HOST_API_H
#define PSE_HOST_API_H

#include <stdint.h>
#include <wchar.h>

#include "pse/pse_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- shim -> loader ---------------------------------------------------------------------------------------------- */

typedef struct PSEShimInfo {
    uint32_t size;               /* sizeof(PSEShimInfo) */
    uint32_t version;            /* 1 */
    void* shimModule;            /* HMODULE of the proxy DLL */
    const wchar_t* proxyName;    /* "dinput8.dll" */
} PSEShimInfo;
/* Exported by pse_loader.dll. Main thread, at the exe's entry point (before the CRT / WinMain), outside the loader
 * lock. Returns 1 = PSE runs (a game layer recognised the exe), 0 = vanilla (nothing patched). */
typedef int(PSE_CALL* PSELoaderStartFn)(const PSEShimInfo* info);
#define PSE_LOADER_START_EXPORT "PSELoader_Start"

/* ---- loader -> game layer (PSE\Games\pse_<game>.dll) -------------------------------------------------------------- */

typedef struct PSEGameDetectInfo {
    uint32_t size;               /* sizeof(PSEGameDetectInfo) */
    const char* exeSha256;       /* upper-case hex SHA-256 of the exe FILE with IMAGE_FILE_LARGE_ADDRESS_AWARE cleared
                                    ("" if it couldn't be read) */
    uint32_t imageBase;          /* where the exe is mapped */
    uint32_t peTimestamp;        /* IMAGE_FILE_HEADER.TimeDateStamp of the mapped exe */
    const wchar_t* exePath;
} PSEGameDetectInfo;

typedef struct PSEGameLayerInfo {
    uint32_t size;               /* sizeof(PSEGameLayerInfo) */
    uint32_t infoVersion;        /* 1 */
    const char* gameId;          /* "ds1": settings live in HKCU\Software\PSE\<gameId>, plugins' `games` lists */
    const PSEPluginInfo* plugin; /* the layer's own plugin ("pse.game.ds1"), registered by the loader */
    const char* const* hostPlugins;   /* NULL-terminated names of plugins that may be host plugins ("dsfix") */
} PSEGameLayerInfo;

/* Exports of a game layer DLL:
 *   __declspec(dllexport) extern const PSEGameLayerInfo PSEGame_Info;
 *   __declspec(dllexport) int PSE_CALL PSEGame_Detect(const PSEGameDetectInfo* d);   // 1 = this exe is mine
 *   __declspec(dllexport) int PSE_CALL PSEGame_Init(const PSELoadInterface* pse);    // 1 = ready
 * Detect runs before the loader services exist (no Query calls); it must not patch anything. Init runs after
 * pse::Init, before any plugin; its Query also answers "pse.host". */
typedef int(PSE_CALL* PSEGameDetectFn)(const PSEGameDetectInfo* d);
typedef int(PSE_CALL* PSEGameInitFn)(const PSELoadInterface* pse);

/* ---- pse.host (version 0): loader internals for trusted host plugins and game layers ------------------------------ */

#define PSE_HOST_INTERFACE "pse.host"
#define PSE_HOST_INTERFACE_VERSION 0u

typedef struct PSEHostPluginResult {
    uint32_t size;
    const char* name;            /* strings valid during the callback only */
    const char* displayName;
    const char* file;            /* full DLL path (or the manifest when no DLL was read) */
    uint32_t version, flags;
    uint32_t state;              /* 0 loaded, 1 refused, 2 failed, 3 unloaded (pse::PluginState) */
    const char* reason;
    PSEPluginHandle handle;      /* 0 = never registered */
} PSEHostPluginResult;
typedef void(PSE_CALL* PSEHostResultFn)(const PSEHostPluginResult* r, void* ctx);
typedef void(PSE_CALL* PSEHostLogSink)(const char* line);
typedef void(PSE_CALL* PSEHostReportSink)(const char* line, int error);

typedef struct PSEHostInterface {
    uint32_t size;
    uint32_t version;            /* PSE_HOST_INTERFACE_VERSION */

    /* The loader's service tables (the same pointers Query returns). */
    const PSELogInterface* log;
    const PSEHooksInterface* hooks;
    const PSEMessagingInterface* messaging;
    const PSETasksInterface* tasks;
    const PSESettingsInterface* settings;
    const PSESerializationInterface* serialization;

    /* Log + report routing: every loader / plugin line goes to the sink. Lines logged before the first SetLogSink
     * (loader start-up, game layer) are handed to it at once. NULL = back to the loader's own output. */
    void (PSE_CALL* SetLogSink)(PSEHostLogSink sink);
    void (PSE_CALL* SetReportSink)(PSEHostReportSink sink);
    void (PSE_CALL* LogLine)(PSEPluginHandle plugin, PSELogLevel level, const char* text);

    /* Registry. RegisterInternalPlugin: a plugin compiled into the caller (sub-plugin: PSE_PLUGIN_INTERNAL, called
     * without the crash guard, strings copied, retired with its parent). */
    PSEPluginHandle (PSE_CALL* RegisterInternalPlugin)(PSEPluginHandle parent, const PSEPluginInfo* info);
    PSEPluginHandle (PSE_CALL* FindPlugin)(const char* name);
    const char* (PSE_CALL* PluginName)(PSEPluginHandle plugin);               /* "" unknown; stays valid */
    uint32_t (PSE_CALL* PluginCount)(void);
    const PSEPluginInfo* (PSE_CALL* PluginInfo)(PSEPluginHandle plugin);      /* NULL unknown; stays valid */
    int (PSE_CALL* PluginExternal)(PSEPluginHandle plugin);
    int (PSE_CALL* PluginDisabled)(PSEPluginHandle plugin);
    int (PSE_CALL* PluginHost)(PSEPluginHandle plugin);                       /* host plugin or game layer */
    uint32_t (PSE_CALL* PluginsInOrder)(PSEPluginHandle* out, uint32_t max);  /* enabled ones; returns the total */
    const PSELoadInterface* (PSE_CALL* LoadInterfaceFor)(PSEPluginHandle plugin);
    /* Copies "<registryRoot>\Plugins\<name>" into buf (NUL-terminated); returns the full length (0 = unknown). */
    uint32_t (PSE_CALL* PluginSettingsKey)(PSEPluginHandle plugin, char* buf, uint32_t bufLen);
    /* External plugin load results (console `pse plugins`), in load order; returns the count. */
    uint32_t (PSE_CALL* PluginResults)(PSEHostResultFn fn, void* ctx);

    /* Game-layer side of the loader (the host drives these from its game hooks). */
    void (PSE_CALL* Frame)(void);
    void (PSE_CALL* Broadcast)(uint32_t type, const void* data, uint32_t len);
    int (PSE_CALL* RaiseEvent)(PSEEventId event, void* data);
    int (PSE_CALL* CosaveSave)(const char* coSavePath);
    int (PSE_CALL* CosaveLoad)(const char* coSavePath);
    int (PSE_CALL* CosaveLoadBytes)(const void* data, uint32_t len, const char* label);
    void (PSE_CALL* CosaveNewGame)(void);
    /* Window destroyed: PSE_MSG_EXIT, then external plugins unloaded (host plugins: Unload only, never freed). */
    void (PSE_CALL* Shutdown)(void);

    const wchar_t* pseDir;       /* <game>\PSE */
} PSEHostInterface;

#ifdef __cplusplus
}
#endif

#endif /* PSE_HOST_API_H */
