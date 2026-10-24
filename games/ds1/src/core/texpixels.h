// Reading a Direct3D 8 texture's pixels on the CPU (no d3d8.h in the SDK): the font sheet copies of dsfix.fontsharp
// (PSE\Plugins\dsfix.fontsharp.dll) and the recoloured menu / HUD pages of barcolour.cpp (dsfix.dll) both start from
// it. Header only, so each DLL gets its own copy.
#pragma once
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace texpix {
// Direct3D 8 (no d3d8.h in the SDK).
enum : int { kAddRef = 1, kRelease = 2, kCreateTexture = 20, kGetLevelDesc = 14, kLockRect = 16, kUnlockRect = 17 };
enum : uint32_t {
    kA8R8G8B8 = 21, kX8R8G8B8 = 22, kR5G6B5 = 23, kX1R5G5B5 = 24, kA1R5G5B5 = 25, kA4R4G4B4 = 26, kA8 = 28,
    kL8 = 50, kA8L8 = 51, kDxt1 = 0x31545844, kDxt2 = 0x32545844, kDxt3 = 0x33545844, kDxt4 = 0x34545844,
    kDxt5 = 0x35545844, kPoolManaged = 1, kLockReadOnly = 0x10,
};
struct SurfaceDesc {
    uint32_t format, type, usage, pool, size, multiSample, width, height;
};
struct Locked {
    INT pitch;
    void* bits;
};

inline void** Vt(void* obj) { return *static_cast<void***>(obj); }
template <class Fn>
Fn M(void* obj, int i) {
    return reinterpret_cast<Fn>(Vt(obj)[i]);
}
inline ULONG AddRef(void* o) { return M<ULONG(__stdcall*)(void*)>(o, kAddRef)(o); }
inline ULONG Release(void* o) { return M<ULONG(__stdcall*)(void*)>(o, kRelease)(o); }

// RGBA (straight alpha, 0..255 per channel) of a whole texture level, or empty for formats we can't read.
inline std::vector<uint32_t> ReadPixels(void* tex, const SurfaceDesc& d) {
    std::vector<uint32_t> out;
    Locked lr{};
    using Lock = HRESULT(__stdcall*)(void*, UINT, Locked*, const RECT*, DWORD);
    if (FAILED(M<Lock>(tex, kLockRect)(tex, 0, &lr, nullptr, kLockReadOnly))) return out;
    const uint32_t w = d.width, h = d.height;
    out.assign(size_t(w) * h, 0);
    auto row = [&](uint32_t y) { return static_cast<const uint8_t*>(lr.bits) + size_t(y) * lr.pitch; };
    auto x5 = [](uint32_t v) { return (v << 3) | (v >> 2); };
    auto x6 = [](uint32_t v) { return (v << 2) | (v >> 4); };
    auto argb = [](uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return a << 24 | r << 16 | g << 8 | b; };
    auto rgb565 = [&](uint16_t c) { return argb(255, x5(c >> 11), x6((c >> 5) & 63), x5(c & 31)); };
    bool ok = true;
    switch (d.format) {
        case kA8R8G8B8:
        case kX8R8G8B8:
            for (uint32_t y = 0; y < h; ++y) {
                const auto* s = reinterpret_cast<const uint32_t*>(row(y));
                for (uint32_t x = 0; x < w; ++x) out[y * w + x] = d.format == kX8R8G8B8 ? s[x] | 0xFF000000 : s[x];
            }
            break;
        case kR5G6B5:
        case kX1R5G5B5:
        case kA1R5G5B5:
        case kA4R4G4B4:
            for (uint32_t y = 0; y < h; ++y) {
                const auto* s = reinterpret_cast<const uint16_t*>(row(y));
                for (uint32_t x = 0; x < w; ++x) {
                    const uint16_t c = s[x];
                    uint32_t v;
                    if (d.format == kR5G6B5) v = rgb565(c);
                    else if (d.format == kA4R4G4B4)
                        v = argb((c >> 12) * 17, ((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17);
                    else
                        v = argb(d.format == kA1R5G5B5 ? (c >> 15) * 255 : 255, x5((c >> 10) & 31), x5((c >> 5) & 31),
                                 x5(c & 31));
                    out[y * w + x] = v;
                }
            }
            break;
        case kA8:
        case kL8:
        case kA8L8:
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t* s = row(y);
                for (uint32_t x = 0; x < w; ++x) {
                    if (d.format == kA8) out[y * w + x] = argb(s[x], 255, 255, 255);
                    else if (d.format == kL8) out[y * w + x] = argb(255, s[x], s[x], s[x]);
                    else out[y * w + x] = argb(s[x * 2 + 1], s[x * 2], s[x * 2], s[x * 2]);
                }
            }
            break;
        case kDxt1:
        case kDxt2:
        case kDxt3:
        case kDxt4:
        case kDxt5: {
            const bool dxt1 = d.format == kDxt1, explicitAlpha = d.format == kDxt2 || d.format == kDxt3;
            const uint32_t blockBytes = dxt1 ? 8 : 16;
            for (uint32_t by = 0; by < (h + 3) / 4; ++by) {
                const uint8_t* b = row(by);  // D3D reports the pitch of a row of blocks
                for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx, b += blockBytes) {
                    uint32_t alpha[16];
                    const uint8_t* c = dxt1 ? b : b + 8;
                    if (explicitAlpha) {
                        for (int i = 0; i < 16; ++i) alpha[i] = ((b[i / 2] >> ((i & 1) * 4)) & 15) * 17;
                    } else if (!dxt1) {
                        uint32_t a[8] = {b[0], b[1]};
                        if (a[0] > a[1])
                            for (int i = 1; i < 7; ++i) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
                        else {
                            for (int i = 1; i < 5; ++i) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5;
                            a[6] = 0, a[7] = 255;
                        }
                        uint64_t bits = 0;
                        for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
                        for (int i = 0; i < 16; ++i) alpha[i] = a[(bits >> (3 * i)) & 7];
                    }
                    const uint16_t c0 = uint16_t(c[0] | c[1] << 8), c1 = uint16_t(c[2] | c[3] << 8);
                    uint32_t pal[4] = {rgb565(c0), rgb565(c1)};
                    auto mix = [](uint32_t p, uint32_t q, int wp, int wq, int div) {
                        uint32_t v = 0xFF000000;
                        for (int s = 0; s < 24; s += 8)
                            v |= ((((p >> s) & 255) * wp + ((q >> s) & 255) * wq) / div) << s;
                        return v;
                    };
                    if (c0 > c1 || !dxt1) {
                        pal[2] = mix(pal[0], pal[1], 2, 1, 3);
                        pal[3] = mix(pal[0], pal[1], 1, 2, 3);
                    } else {
                        pal[2] = mix(pal[0], pal[1], 1, 1, 2);
                        pal[3] = 0;  // transparent black
                    }
                    const uint32_t idx = c[4] | c[5] << 8 | c[6] << 16 | uint32_t(c[7]) << 24;
                    for (int i = 0; i < 16; ++i) {
                        const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                        if (x >= w || y >= h) continue;
                        uint32_t v = pal[(idx >> (2 * i)) & 3];
                        if (!dxt1) v = (v & 0xFFFFFF) | alpha[i] << 24;
                        out[y * w + x] = v;
                    }
                }
            }
            break;
        }
        default:
            ok = false;
    }
    M<HRESULT(__stdcall*)(void*, UINT)>(tex, kUnlockRect)(tex, 0);
    if (!ok) out.clear();
    return out;
}

// RGBA of level 0 and its size, or empty (w = h = 0) when it can't be read.
inline std::vector<uint32_t> ReadTexture(void* tex, uint32_t& w, uint32_t& h) {
    SurfaceDesc d{};
    w = h = 0;
    if (!tex || FAILED(M<HRESULT(__stdcall*)(void*, UINT, SurfaceDesc*)>(tex, kGetLevelDesc)(tex, 0, &d))) return {};
    std::vector<uint32_t> px = ReadPixels(tex, d);
    if (!px.empty()) w = d.width, h = d.height;
    return px;
}
}  // namespace texpix
