/* ds2fix.memory (version 1): the DS2 memory plugin ds2fix.memory (games/ds2/plugins/memory) - the game's "Global Heap"
 * size and the memory figures, for ds2fix (mods.cpp) and other DS2 plugins (e.g. one that raises engine pools and needs
 * more room in that heap).
 *
 * Published by ds2fix.memory in its PSEPlugin_Load (only when its heap hook went in). Plugins load in name order after
 * the host plugin ds2fix, so query it at PSE_MSG_POST_LOAD (every plugin is loaded by then) - the heap itself is made
 * later, in the game's start-up (CDS II.exe 0x4136FC), after POST_LOAD:
 *
 *   const Ds2FixMemoryV1* mem = (const Ds2FixMemoryV1*)m->QueryInterface(pse->plugin, DS2FIX_MEMORY_INTERFACE, 1);
 *   if (mem && mem->size >= sizeof(Ds2FixMemoryV1)) mem->AddHeapReserve(pse->plugin, 2 << 20);
 *
 * Without the plugin a plugin that needs heap room subscribes the heap set-up call 0x4136FC itself (pse.hooks
 * HookCallSite, expected target 0x4C0650, thiscall(heap, size, name)) and adds its bytes to `size` before calling next;
 * ds2fix.mods (behind it, priority 0) adds the largest mod file on top. With the plugin, nothing behind it sizes the heap
 * (it makes the heap itself): use AddHeapReserve. Docs: docs/sdk/reference/game-ds2.md "ds2fix.memory". Game thread or
 * plugin load only. */
#pragma once
#include <stdint.h>

#include "pse/pse_api.h"

#define DS2FIX_MEMORY_INTERFACE "ds2fix.memory"

typedef struct Ds2FixMemoryStatsV1 {
    uint32_t size;               /* set by the caller: sizeof(Ds2FixMemoryStatsV1) */
    uint32_t laa;                /* 1 = 4 GB address space (the exe has the Large Address Aware flag, 64-bit Windows) */
    uint64_t addressTotal;       /* user address space, bytes */
    uint64_t addressUsed;
    uint64_t largestFree;        /* largest free block of address space */
    uint64_t privateBytes;
    uint32_t heapSize;           /* Global Heap: bytes, 0 before it is made */
    uint32_t heapUsed;
    uint32_t heapPeak;
    uint32_t heapFails;          /* allocations that didn't fit */
    uint32_t heapOurs;           /* 1 = the plugin's own buffer (VirtualAlloc) */
    uint32_t bigReads;           /* texture files read outside the heap (too big for its free space) */
    uint32_t textures;           /* Direct3D textures alive (created through IDirect3DDevice9::CreateTexture) */
    uint64_t textureBytesDefault;/* their size estimate per pool: D3DPOOL_DEFAULT (video memory) */
    uint64_t textureBytesManaged;
    uint64_t textureBytesSystem; /* SYSTEMMEM + SCRATCH */
    uint32_t d3dFreeMB;          /* IDirect3DDevice9::GetAvailableTextureMem / 1 MB, 0 = no device */
} Ds2FixMemoryStatsV1;

typedef struct Ds2FixMemoryV1 {
    uint32_t size;
    uint32_t version; /* 1 */
    /* More Global Heap for `plugin` (bytes, added once per call). Before the heap is made only: returns 1, or 0 when it
     * exists already (too late - nothing changes). Counts towards the cap (64 MB, 256 MB with 4 GB). */
    int (PSE_CALL* AddHeapReserve)(PSEPluginHandle plugin, uint32_t bytes);
    uint32_t (PSE_CALL* HeapSize)(void);   /* the heap's size in bytes, 0 before it is made */
    uint32_t (PSE_CALL* HeapCap)(void);    /* the most the plugin makes it: 64 MB / 256 MB with 4 GB, <= 1/4 free block */
    int (PSE_CALL* LargeAddressAware)(void);
    /* 1 = texture files bigger than the heap's free space are read outside it (through the game allocator), so a mod's
     * texture isn't limited by the heap size; models and other files still are. */
    int (PSE_CALL* LoadsBigTexturesOutsideHeap)(void);
    void (PSE_CALL* GetStats)(Ds2FixMemoryStatsV1* out);   /* out->size = sizeof(Ds2FixMemoryStatsV1) first */
} Ds2FixMemoryV1;
