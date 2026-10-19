// See pse_saves.h. The old sidecar: "<name>.sav.dsfix", one text line per spawned weapon / gear piece / vehicle swap
// (core/pse_saves_format.h ParseSidecar) - read, never written or deleted.
#include "core/pse_saves.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/pse_saves_format.h"

namespace {
bool ReadText(const std::string& path, std::string* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) out->append(b, n);
    fclose(f);
    return true;
}
}  // namespace

uint32_t PSE_CALL pse_saves::LegacyCosave(const char* savPath, void* buf, uint32_t bufLen) {
    std::string text;
    if (!savPath || !ReadText(SidecarPathFor(savPath), &text)) return 0;
    const std::vector<uint8_t> image = SidecarToCosave(ParseSidecar(text));
    if (buf && bufLen) std::memcpy(buf, image.data(), image.size() < bufLen ? image.size() : bufLen);
    return static_cast<uint32_t>(image.size());
}
