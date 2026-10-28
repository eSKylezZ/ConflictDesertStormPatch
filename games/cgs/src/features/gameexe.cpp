#include "features/features.h"

#include <windows.h>

// Known builds of ConflictGlobal.exe (same compile, 2005-08-17, PE timestamp 0x43032523):
//   retail (SecuROM 7, disc check; "ConflictGlobal - Original.exe")  EP RVA 0xA98330 (.init), .text encrypted on disk
//   NoCD (RELOADED dump, also the Magipack repack's exe)            EP RVA 0x2A8EB9 (the CRT entry), 14 475 401 bytes
// Only the NoCD exe runs on current Windows; it still runs SecuROM's VM for ~1080 functions (window creation, the
// launcher dialogs, the resolution filter ...), which is why window / dialog hooks go through the import slots.
features::GameExe features::IdentifyExe() {
    auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleA(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const DWORD stamp = nt->FileHeader.TimeDateStamp, ep = nt->OptionalHeader.AddressOfEntryPoint;
    if (stamp != 0x43032523) return GameExe::Unknown;
    if (ep == 0x2A8EB9) return GameExe::NoCd;
    if (ep == 0xA98330) return GameExe::Retail;
    return GameExe::Unknown;
}

const char* features::ExeName(GameExe e) {
    switch (e) {
        case GameExe::Retail: return "retail 1.0.0.1 (SecuROM 7)";
        case GameExe::NoCd: return "1.0.0.1 NoCD";
        default: return "unknown build";
    }
}
