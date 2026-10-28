// Waiting for SteamStub (2026-10-25). ConflictDeniedOps.exe's .text is ENCRYPTED on disk (the packed and the
// Steamless-unpacked file differ only in .text and the entry point; .rdata / .data / the import table are plain). The
// PSE shim runs the loader at the exe's entry = the stub's code (RVA 0xB4D3DB, .bind), before the stub decrypts
// .text - so at PSEPlugin_Load every code site still holds encrypted bytes and a code patch would fail its signature
// check (or worse, be overwritten by the decryption). The stub then jumps to the game's CRT start-up (0x8AF6C0 area),
// which calls MSVCR80!__set_app_type through the import slot 0x90E2F4 first thing - that slot is plain data the
// Windows loader filled, so it is hooked at load time (broker HookSlot) and its first call runs the code patches.
// The call passes straight on to the CRT afterwards; the slot keeps our pass-through (it is called once).
#include <windows.h>

#include <cstring>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "features/features.h"

namespace {
constexpr uint32_t kSetAppTypeSlot = 0x90E2F4;   // MSVCR80.dll!__set_app_type
// A decrypted site: File_Open 0x5E24B0 starts mov eax,[esp+4]; sub esp,0x108 (encrypted: 8C DF 09 E8 ...).
constexpr uint32_t kProbe = 0x5E24B0;
constexpr uint8_t kProbeBytes[] = {0x8B, 0x44, 0x24, 0x04, 0x81, 0xEC, 0x08, 0x01, 0x00, 0x00};

using SetAppTypeFn = void(__cdecl*)(int);
SetAppTypeFn g_next = nullptr;
struct Pending {
    void (*apply)();
    PSEPluginHandle owner;
};
std::vector<Pending> g_pending;
bool g_ran = false;

void RunPending() {
    if (g_ran) return;
    g_ran = true;
    dslog::Write("SteamStub: game code decrypted - applying %zu code feature(s)", g_pending.size());
    for (const Pending& p : g_pending) {
        patch::OwnerScope scope(p.owner);
        p.apply();
    }
}

void __cdecl SetAppType(int type) {
    if (!g_ran) {
        if (features::TextDecrypted()) RunPending();
        else dslog::Write("[warn] SteamStub: __set_app_type called before the game code was decrypted");
    }
    g_next(type);
}
}  // namespace

bool features::TextDecrypted() { return patch::Matches(kProbe, kProbeBytes, sizeof kProbeBytes); }

// Runs `apply` (inside the owner scope active now) once the game's code is decrypted: at once if it already is.
void features::AfterUnpack(void (*apply)()) {
    PSEPluginHandle owner = patch::CurrentOwner();
    if (g_ran) {
        apply();
        return;
    }
    g_pending.push_back({apply, owner});
    if (g_next) return;
    if (TextDecrypted()) {   // plugin loaded late (no stub, or already unpacked): run now
        RunPending();
        return;
    }
    patch::OwnerScope scope(pse_host::Core());
    if (!patch::HookSlot(kSetAppTypeSlot, reinterpret_cast<const void*>(&SetAppType), reinterpret_cast<void**>(&g_next)))
        dslog::Write("[fail] SteamStub: CRT start-up slot 0x%08X not hooked - code features off", kSetAppTypeSlot);
    else
        dslog::Write("SteamStub: game code still encrypted - code features wait for the CRT start-up");
}
