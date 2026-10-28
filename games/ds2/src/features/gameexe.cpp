#include "features/features.h"

#include <windows.h>

// Both known builds share the PE header (timestamp 0x3F4B1A8A, 2003-08-26) and the SecuROM 4.84.84 sections
// .fodb / .wpjwqo; .text is plain code in both. They differ in the entry point: the retail exe starts in SecuROM's
// loader (RVA 0x42CC17), the disc-free one in a small stub (RVA 0x3AC35D) that ends up in the same game start-up.
features::GameExe features::IdentifyExe() {
    auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleA(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x3F4B1A8A) return GameExe::Unknown;
    switch (nt->OptionalHeader.AddressOfEntryPoint) {
        case 0x42CC17: return GameExe::Retail100;
        case 0x3AC35D: return GameExe::Retail100NoDisc;
        default: return GameExe::Unknown;
    }
}

const char* features::ExeName(GameExe e) {
    switch (e) {
        case GameExe::Retail100: return "retail 1.0 (SecuROM)";
        case GameExe::Retail100NoDisc: return "retail 1.0 (no disc check)";
        default: return "unknown build";
    }
}
