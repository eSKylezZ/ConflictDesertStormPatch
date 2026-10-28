#pragma once
// Tier 2 "image tests": DesertStorm.exe mapped at its image base 0x400000 inside pse_tests.exe - never run (no entry
// point, no TLS callbacks, no CRT init). Lets tests read the original bytes / data tables and call pure game functions.
//
//   PSE_TEST("game: archive hash") {
//       PSE_REQUIRE_EXE();                                        // maps the exe, or SKIPs the test
//       auto r = exe::Cdecl<uint32_t>(0x4B9A10, "WEAPONS.RFX");
//       PSE_REQUIRE(r.ok);                                         // false = the call faulted (r.fault says how)
//       PSE_CHECK_EQ(r.value, 0x4475E8D7u);
//   }
//
// Every IAT slot points at a trap stub: a game function that calls an import stops with a Fault naming the import
// (e.g. "KERNEL32.dll!QueryPerformanceCounter") instead of crashing. Any other exception inside a call (access
// violation, ...) is a Fault too. Calls are single-threaded and not re-entrant.
// Exe lookup: env PSE_GAME_EXE, else the Steam install, else the GOG install. Only the known build is mapped
// (SHA-256 D712F5EC..., with the 4 GB flag cleared before hashing).
// Desert Storm II (2026-10-19): MapGame("ds2") maps CDS II.exe 1.0 disc-free (SHA-256 E9B46420..., env
// PSE_GAME_EXE_DS2, else a standard install path) into the same range - ONE exe at a time: MapGame takes the
// other one out first, Map() / PSE_REQUIRE_EXE() put DS1 back. Tests use PSE_REQUIRE_EXE_FOR("ds2").
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

#include "pse_test.h"

namespace exe {
constexpr uint32_t kImageBase = 0x400000;
constexpr uint32_t kImportTrapCode = 0xE0D5A001;  // exception code raised by an IAT trap stub

// Maps DS1's exe (later calls: already there, or put back after another game's). whyNot = reason when it can't be.
bool Map(std::string* whyNot = nullptr);
// Maps a game's exe ("ds1", "ds2") into the range, taking out the one that is there. A missing / unknown exe is
// reported (and not looked for again).
bool MapGame(const char* gameId, std::string* whyNot = nullptr);
bool Mapped();
const char* MappedGame();   // "ds1" / "ds2", "" = none
const std::string& Path();  // file that was mapped

// Puts every section back to the file's bytes and re-fills the IAT traps (undo what earlier calls wrote).
void Restore();

// Original file bytes at a VA (0 beyond a section's raw data, like the loader). False = VA not in any section.
bool OriginalBytes(uint32_t va, void* out, size_t len);
// Section of a VA: name, virtual start / size. False = none.
bool SectionOf(uint32_t va, std::string* name, uint32_t* start = nullptr, uint32_t* size = nullptr);
// "DLL!Function" for an IAT slot VA, "" if it isn't one.
std::string ImportName(uint32_t iatSlot);
size_t ImportCount();

template <class T>
T Read(uint32_t va) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(static_cast<uintptr_t>(va)), sizeof v);
    return v;
}
inline void* Ptr(uint32_t va) { return reinterpret_cast<void*>(static_cast<uintptr_t>(va)); }

struct Fault {
    uint32_t code = 0;     // exception code (kImportTrapCode for an import)
    uint32_t address = 0;  // faulting EIP (for an import: the stub; see caller)
    uint32_t caller = 0;   // for an import: return address in the game code that called it
    uint32_t dataAddress = 0;  // access violation: address read / written
    std::string import;    // "DLL!Function" when an import was called
    std::string Describe() const;
};

template <class R>
struct Result {
    bool ok = false;
    R value{};
    Fault fault;
};
template <>
struct Result<void> {
    bool ok = false;
    Fault fault;
};

// Runs fn(ctx) with exceptions caught by a vectored handler: returns false + fault on any exception.
bool Guard(void(__cdecl* fn)(void*), void* ctx, Fault* fault);

namespace detail {
template <class R, class F>
Result<R> Run(F&& f) {
    Result<R> r;
    struct Ctx {
        F* f;
        Result<R>* r;
    } c{&f, &r};
    auto thunk = [](void* p) {
        auto* k = static_cast<Ctx*>(p);
        if constexpr (std::is_void_v<R>) (*k->f)();
        else k->r->value = (*k->f)();
    };
    r.ok = Guard(static_cast<void(__cdecl*)(void*)>(thunk), &c, &r.fault);
    return r;
}
}  // namespace detail

// Game function calls by calling convention. Arguments are passed as given (pass uint32_t / pointers / float as the
// game expects; an int literal is an int). Thiscall: first argument = this (ecx). Fastcall: first two in ecx / edx.
template <class R, class... A>
Result<R> Cdecl(uint32_t addr, A... a) {
    using Fn = R(__cdecl*)(A...);
    return detail::Run<R>([&]() -> R { return reinterpret_cast<Fn>(static_cast<uintptr_t>(addr))(a...); });
}
template <class R, class... A>
Result<R> Stdcall(uint32_t addr, A... a) {
    using Fn = R(__stdcall*)(A...);
    return detail::Run<R>([&]() -> R { return reinterpret_cast<Fn>(static_cast<uintptr_t>(addr))(a...); });
}
template <class R, class T, class... A>
Result<R> Thiscall(uint32_t addr, T self, A... a) {
    using Fn = R(__thiscall*)(T, A...);
    return detail::Run<R>([&]() -> R { return reinterpret_cast<Fn>(static_cast<uintptr_t>(addr))(self, a...); });
}
template <class R, class... A>
Result<R> Fastcall(uint32_t addr, A... a) {
    using Fn = R(__fastcall*)(A...);
    return detail::Run<R>([&]() -> R { return reinterpret_cast<Fn>(static_cast<uintptr_t>(addr))(a...); });
}
}  // namespace exe

// Maps the exe or skips the current test (prints the reason).
#define PSE_REQUIRE_EXE()                                \
    do {                                                 \
        std::string pse_why_;                            \
        if (!::exe::Map(&pse_why_)) PSE_SKIP(pse_why_);  \
    } while (0)
// The same for another game's exe ("ds2").
#define PSE_REQUIRE_EXE_FOR(game)                                  \
    do {                                                           \
        std::string pse_why_;                                      \
        if (!::exe::MapGame(game, &pse_why_)) PSE_SKIP(pse_why_);  \
    } while (0)
