#include "features/features.h"

#include <windows.h>

// Known builds in H:\SCI Games\Conflict Vietnam:
//   "Vietnam - Original.exe"  timestamp 0x411BBEAF (2004-08-12), EP RVA 0x570782, SecuROM sections .crdb / .ycfant
//   "Vietnam 1.0 Fixed.exe"   same timestamp, EP RVA 0x4B9A05 (disc check bypassed)
//   "Vietnam.exe"             timestamp 0x414ECA49 (2004-09-20), EP RVA 0x2723BB, plain .text/.rdata/.data/.rsrc
//                             (later build, no SecuROM sections) - the one the tools and the plugin target
features::GameExe features::IdentifyExe() {
    auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleA(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const DWORD stamp = nt->FileHeader.TimeDateStamp, ep = nt->OptionalHeader.AddressOfEntryPoint;
    if (stamp == 0x414ECA49 && ep == 0x2723BB) return GameExe::Patched;
    if (stamp == 0x411BBEAF && ep == 0x570782) return GameExe::Retail100;
    if (stamp == 0x411BBEAF && ep == 0x4B9A05) return GameExe::Retail100Fixed;
    return GameExe::Unknown;
}

const char* features::ExeName(GameExe e) {
    switch (e) {
        case GameExe::Retail100: return "retail 1.0 (SecuROM)";
        case GameExe::Retail100Fixed: return "retail 1.0 (no disc check)";
        case GameExe::Patched: return "Sep 2004 build (no SecuROM)";
        default: return "unknown build";
    }
}
