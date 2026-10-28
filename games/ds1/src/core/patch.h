#pragma once
#include <cstddef>
#include <cstdint>

#include "pse/pse_api.h"

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

// Everything here goes through the PSE hooks broker (pse.hooks, docs/sdk/reference/hooks.md "DS1 host"): every byte
// group, call hook, indirect call hook, pointer hook, function hook and jump gets an owner - the internal plugin of the OwnerScope that is active
// (one per feature: "dsfix.hud", "dsfix.splitscreen" ...), else "dsfix". Semantics are the same as before the broker.
namespace patch {
// While alive, patches made on this thread belong to `plugin` (0 = "dsfix"). Nests.
class OwnerScope {
public:
    explicit OwnerScope(PSEPluginHandle plugin);
    ~OwnerScope();
    OwnerScope(const OwnerScope&) = delete;
    OwnerScope& operator=(const OwnerScope&) = delete;

private:
    PSEPluginHandle prev_;
};
// The scope's plugin, or "dsfix" (pse_host::Core()).
PSEPluginHandle CurrentOwner();

// All-or-nothing: every site must hold its original (or already patched) bytes, else nothing is written.
// The group name is the key (unique across all features): a group that exists already is re-applied / reverted as
// its owner, whatever scope is active (live toggles from the in-game menu run outside any scope).
bool ApplyGroup(const char* group, const BytePatch* patches, size_t count);
template <size_t N>
bool ApplyGroup(const char* group, const BytePatch (&patches)[N]) { return ApplyGroup(group, patches, N); }
// Inverse of ApplyGroup (sites must hold patched or original bytes); used when a feature is switched off.
bool RevertGroup(const char* group, const BytePatch* patches, size_t count);
template <size_t N>
bool RevertGroup(const char* group, const BytePatch (&patches)[N]) { return RevertGroup(group, patches, N); }

bool Matches(uint32_t va, const void* bytes, size_t len);
// Writes code/data and leaves the pages executable (caves also live in .rdata padding). Bytes of a call hook or jump
// made through this file are handed back to the broker first (the hook is removed; a `call X` written over a chain
// that contains X removes only the hooks in front of X), so no stale broker record is left behind.
bool Write(uint32_t va, const void* bytes, size_t len);
template <class T>
bool WriteValue(uint32_t va, const T& value) { return Write(va, &value, sizeof value); }

// Redirects the `call rel32` at `site` (which must currently call `expectedTarget`) to `fn`. Broker call hook with
// an internal `next` (fn calls the original itself). Rules on a site that is already hooked:
//  - fn already runs first there: true (idempotent);
//  - expectedTarget = the hook that runs first (hand-chaining: fn calls that hook itself): fn joins in front of it;
//  - fn = the site's original target: every hook there is removed (the site calls the original again);
//  - expectedTarget = the original target, fn a different function: false + "[fail]" (it would never run).
bool HookCall(uint32_t site, const void* fn, uint32_t expectedTarget);
// Replaces the 6-byte `call dword ptr [iatSlot]` at `site` with `call fn; nop` (fn has the API's signature).
bool HookIndirectCall(uint32_t site, const void* fn, uint32_t iatSlot);
// Writes `jmp rel32` to `fn` at `site` (broker byte group "jmp@0x<site>", anyOriginal, owned by the scope; a later
// WriteJump at the same site replaces it, a Write over it reverts it first).
bool WriteJump(uint32_t site, const void* fn);
// Same over `len` bytes (5..64; the rest become nop - one group, so nothing is left unowned). `original` (len bytes)
// must be at the site (or already the jump), else `[fail]` and nothing is written; null = not checked.
bool WriteJump(uint32_t site, const void* fn, size_t len, const void* original = nullptr);
// `call rel32` to fn at `site` over code that is not a call to hook (the stub replays what it replaced), the rest of
// `len` bytes nop: byte group "call@0x<site>" owned by the scope, same rules as WriteJump.
bool WriteCall(uint32_t site, const void* fn, size_t len = 5, const void* original = nullptr);
// Takes a WriteJump / WriteCall at `site` back (the bytes it replaced are restored). Nothing there = true.
bool RevertCode(uint32_t site);

// A shared call site: fn joins the broker chain at `site` (`call originalTarget` before anyone hooked it) at
// `priority` (higher runs first). *next is kept pointing at the next subscriber or the original target - call it
// like the original. Several features can subscribe to one site this way (e.g. the texture bind 0x53CC9B).
bool SubscribeCall(uint32_t site, const void* fn, uint32_t originalTarget, int32_t priority, void** next);

// A code pointer stored in memory - vtable slot, IAT slot, script API table entry, the imm32 of a `push <callback>` -
// gets fn (broker HookPointer). `next` (may be null = an internal one) is set to the value the slot held before the
// first hook, or to the next subscriber, and follows the chain - pass the variable the feature used to save the old
// value in, so code calling it keeps working and other subscribers stay in the chain. `expected` != 0: the slot's
// original value must be that (`[fail]` otherwise, nothing written). Same fn again = true. Bytes a byte group / jump /
// other hook kind owns: written outside the broker with a `[warn]` (*next = the old value), like HookCall.
bool HookSlot(uint32_t slot, const void* fn, void** next = nullptr, uint32_t expected = 0, int32_t priority = 0);
// Removes fn's hook from the slot (the slot gets its original value back when the last hook goes). Not hooked = true.
bool UnhookSlot(uint32_t slot, const void* fn);

// Entry detour (broker HookFunction): the first whole instructions of the function (>= 5 bytes; `prologueLen` 0 =
// the shortest run) move into a broker trampoline, the entry jumps to fn. `next` (may be null when fn never runs the
// original) = the trampoline or the next subscriber - call it like the original function. `expected` /
// `expectedLen`: the original first bytes (any length) must match, checked before the first hook (`[fail]`, nothing
// written). Same fn again = true. Prologues the broker can't move (loop / jecxz, rel16, a branch into the moved
// bytes, unknown opcodes) fail with a `[fail]` line.
bool HookFunction(uint32_t entry, const void* fn, void** next, const void* expected = nullptr, size_t expectedLen = 0,
                  uint32_t prologueLen = 0, int32_t priority = 0);
// Removes fn's entry hook (the prologue comes back when the last hook goes). Not hooked = true.
bool UnhookFunction(uint32_t entry, const void* fn);
// Length HookFunction would move at `entry` for prologueLen 0 (the shortest run of whole instructions >= 5) on the
// bytes there now, 0 when undecodable. For tests and log lines.
uint32_t PrologueLength(uint32_t entry);

// Tests only: forgets this file's records (the broker's own ResetForTests forgets its side).
void ResetForTests();
}  // namespace patch
