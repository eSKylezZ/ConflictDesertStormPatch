#pragma once
// ds2fix.widescreen (external PSE plugin) only: games/ds2/src/core/patch.h's API on top of the public pse.hooks broker
// (pse/include/pse/pse_api.h), so the shared feature sources (src/features/hud.cpp, frontend.cpp ...) build unchanged.
// CMake puts this folder first on the plugin's include path. Implementation: ../patch.cpp.
//   ApplyGroup / Write      -> PatchGroup (Write: one group per address, re-written by revert + patch)
//   HookCall / SubscribeCall -> HookCallSite      HookIndirectCall -> HookIndirectCall
//   WriteJump               -> HookFunction (the shortest run of whole instructions >= 5; the function is replaced)
#include <cstddef>
#include <cstdint>

#ifdef DS_DIST
#define DS_PATCH_NAME(s) nullptr
#else
#define DS_PATCH_NAME(s) s
#endif

struct BytePatch {
    const char* name;
    uint32_t va;
    const uint8_t* original;
    const uint8_t* patched;
    uint32_t len;
    bool anyOriginal;
};

struct PSEHooksInterface;
typedef uint32_t PSEPluginHandle;

namespace patch {
void Attach(const PSEHooksInterface* hooks, PSEPluginHandle plugin);   // plugin.cpp, before any feature applies

bool ApplyGroup(const char* group, const BytePatch* patches, size_t count);
template <size_t N>
bool ApplyGroup(const char* group, const BytePatch (&patches)[N]) { return ApplyGroup(group, patches, N); }

bool Matches(uint32_t va, const void* bytes, size_t len);
bool Write(uint32_t va, const void* bytes, size_t len);
template <class T>
bool WriteValue(uint32_t va, const T& value) { return Write(va, &value, sizeof value); }

bool HookCall(uint32_t site, const void* fn, uint32_t expectedTarget);
// Chained on a call site other plugins / the game layer hook too: *next = what to call instead of originalTarget.
bool SubscribeCall(uint32_t site, const void* fn, uint32_t originalTarget, int32_t priority, void** next);
bool HookIndirectCall(uint32_t site, const void* fn, uint32_t iatSlot);
bool WriteJump(uint32_t site, const void* fn);
}  // namespace patch
