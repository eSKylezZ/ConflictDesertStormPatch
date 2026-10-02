#pragma once
#include <cstddef>
#include <cstdint>

// Patch names are only used for the log, which distribution builds leave out.
#ifdef DS_DIST
#define DS_PATCH_NAME(s) nullptr
#else
#define DS_PATCH_NAME(s) s
#endif

// One verified byte patch at a fixed VA in DesertStorm.exe (image base 0x400000, no relocations).
struct BytePatch {
    const char* name;
    uint32_t va;
    const uint8_t* original;
    const uint8_t* patched;
    uint32_t len;
    bool anyOriginal;  // site holds a tunable value (scale, deadzone...) - original is not checked
};

namespace patch {
// All-or-nothing: every site must hold its original (or already patched) bytes, else nothing is written.
bool ApplyGroup(const char* group, const BytePatch* patches, size_t count);
template <size_t N>
bool ApplyGroup(const char* group, const BytePatch (&patches)[N]) { return ApplyGroup(group, patches, N); }
// Inverse of ApplyGroup (sites must hold patched or original bytes); used when a feature is switched off.
bool RevertGroup(const char* group, const BytePatch* patches, size_t count);
template <size_t N>
bool RevertGroup(const char* group, const BytePatch (&patches)[N]) { return RevertGroup(group, patches, N); }

bool Matches(uint32_t va, const void* bytes, size_t len);
// Writes code/data and leaves the pages executable (caves also live in .rdata padding).
bool Write(uint32_t va, const void* bytes, size_t len);
template <class T>
bool WriteValue(uint32_t va, const T& value) { return Write(va, &value, sizeof value); }

// Redirects the `call rel32` at `site` (which must currently call `expectedTarget`) to `fn`.
bool HookCall(uint32_t site, const void* fn, uint32_t expectedTarget);
// Replaces the 6-byte `call dword ptr [iatSlot]` at `site` with `call fn; nop` (fn has the API's signature).
bool HookIndirectCall(uint32_t site, const void* fn, uint32_t iatSlot);
// Writes `jmp rel32` to `fn` at `site`.
bool WriteJump(uint32_t site, const void* fn);
}  // namespace patch
