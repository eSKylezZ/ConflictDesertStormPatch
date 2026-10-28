#pragma once
// The games' sample format (Desert Storm and Desert Storm II alike) - WAV in, ADPCM out, and back. Header-only so an
// offline round-trip test can compile it without the game (scratch harness: encode -> decode, SNR).
//
// PFSM chunk = "PFSM", u32 size, then i32 language (-1 = every language), u32 sample id, u32 data size, u16 rate,
// u8 bits (4 = ADPCM, 8 = 8-bit PCM), u8 pad (0xCC), data. ADPCM = 16-byte blocks: header byte (high nibble filter
// 0-4, low nibble shift), 15 bytes = 30 samples, low nibble first; nibble n (signed) -> (n << 12) >> shift - c0*s1 -
// c1*s2 with the PlayStation VAG filter table (DS1 0x5FE64C, DS2 0x6196F0 - the same five pairs).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace adpcm {

inline const double kFilters[5][2] = {{0.0, 0.0}, {-0.9375, 0.0}, {-1.796875, 0.8125}, {-1.53125, 0.859375},
                                      {-1.90625, 0.9375}};

struct Pcm {
    std::vector<int16_t> samples;  // mono
    uint32_t rate = 0;
};

// RIFF WAVE in memory: PCM 8 / 16 / 24 / 32-bit or float, any channel count (mixed down to mono), rate <= 65535.
inline bool ParseWav(const std::string& f, Pcm& out) {
    if (f.size() < 12 || memcmp(f.data(), "RIFF", 4) || memcmp(f.data() + 8, "WAVE", 4)) return false;
    uint16_t format = 0, channels = 0, bits = 0;
    const char* data = nullptr;
    uint32_t dataSize = 0;
    for (size_t p = 12; p + 8 <= f.size();) {
        uint32_t size;
        memcpy(&size, f.data() + p + 4, 4);
        if (!memcmp(f.data() + p, "fmt ", 4) && size >= 16 && p + 8 + 16 <= f.size()) {
            memcpy(&format, f.data() + p + 8, 2);
            memcpy(&channels, f.data() + p + 10, 2);
            memcpy(&out.rate, f.data() + p + 12, 4);
            memcpy(&bits, f.data() + p + 22, 2);
            if (format == 0xFFFE && size >= 26 && p + 8 + 26 <= f.size()) memcpy(&format, f.data() + p + 32, 2);
        } else if (!memcmp(f.data() + p, "data", 4)) {
            data = f.data() + p + 8;
            dataSize = static_cast<uint32_t>(std::min<size_t>(size, f.size() - p - 8));
        }
        if (size > f.size()) break;
        p += 8 + size + (size & 1);
    }
    if (!data || !channels || !out.rate || out.rate > 65535 || !(format == 1 || format == 3)) return false;
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) return false;
    const uint32_t step = bits / 8 * channels, frames = step ? dataSize / step : 0;
    out.samples.resize(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        double sum = 0;
        for (uint16_t c = 0; c < channels; ++c) {
            const char* s = data + i * step + c * (bits / 8);
            double v = 0;
            if (format == 3 && bits == 32) { float x; memcpy(&x, s, 4); v = x * 32767.0; }
            else if (bits == 8) v = (static_cast<uint8_t>(*s) - 128) * 256.0;
            else if (bits == 16) { int16_t x; memcpy(&x, s, 2); v = x; }
            else if (bits == 24) v = static_cast<int32_t>((static_cast<uint8_t>(s[0]) << 8) | (static_cast<uint8_t>(s[1]) << 16) |
                                                          (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 24)) / 65536.0;
            else if (bits == 32) { int32_t x; memcpy(&x, s, 4); v = x / 65536.0; }
            sum += v;
        }
        out.samples[i] = static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, sum / channels)));
    }
    return frames > 0;
}

// One block: best filter + shift by trial encoding against the decoder's own history.
inline void EncodeBlock(const int16_t* in, double& s1, double& s2, uint8_t out[16]) {
    double bestErr = 1e300, bestS1 = 0, bestS2 = 0;
    for (int f = 0; f < 5; ++f) {
        double peak = 0, p1 = s1, p2 = s2;
        for (int i = 0; i < 30; ++i) {
            const double r = in[i] + kFilters[f][0] * p1 + kFilters[f][1] * p2;
            peak = std::max(peak, std::fabs(r));
            p2 = p1, p1 = in[i];
        }
        int shift = 12;
        while (shift > 0 && peak * (1 << shift) / 4096.0 > 7.0) --shift;
        uint8_t block[16] = {static_cast<uint8_t>(f << 4 | shift)};
        double d1 = s1, d2 = s2, err = 0;
        for (int i = 0; i < 30; ++i) {
            const double pred = -kFilters[f][0] * d1 - kFilters[f][1] * d2;
            int n = static_cast<int>(std::lround((in[i] - pred) * (1 << shift) / 4096.0));
            n = std::max(-8, std::min(7, n));
            double v = n * 4096.0 / (1 << shift) + pred;
            v = std::max(-32768.0, std::min(32767.0, v));
            err += (v - in[i]) * (v - in[i]);
            d2 = d1, d1 = v;
            block[1 + i / 2] |= static_cast<uint8_t>((n & 15) << (i & 1 ? 4 : 0));
        }
        if (err < bestErr) bestErr = err, bestS1 = d1, bestS2 = d2, memcpy(out, block, 16);
    }
    s1 = bestS1, s2 = bestS2;
}

// The decoder side (as the game: 30 samples per block, history carried across blocks).
inline std::vector<int16_t> Decode(const uint8_t* data, size_t size) {
    std::vector<int16_t> out;
    double s1 = 0, s2 = 0;
    for (size_t b = 0; b + 16 <= size; b += 16) {
        const int f = std::min(4, data[b] >> 4), shift = data[b] & 15;
        for (int i = 0; i < 30; ++i) {
            int n = (data[b + 1 + i / 2] >> (i & 1 ? 4 : 0)) & 15;
            if (n & 8) n -= 16;
            double v = n * 4096.0 / (1 << shift) - kFilters[f][0] * s1 - kFilters[f][1] * s2;
            v = std::max(-32768.0, std::min(32767.0, v));
            s2 = s1, s1 = v;
            out.push_back(static_cast<int16_t>(std::lround(v)));
        }
    }
    return out;
}

// A PFSM chunk (header included) for `pcm` under sample id `id`, language -1.
inline std::string EncodeSample(const Pcm& pcm, uint32_t id) {
    std::string data;
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < pcm.samples.size(); i += 30) {
        int16_t block[30] = {};
        memcpy(block, pcm.samples.data() + i, std::min<size_t>(30, pcm.samples.size() - i) * 2);
        uint8_t out[16];
        EncodeBlock(block, s1, s2, out);
        data.append(reinterpret_cast<const char*>(out), 16);
    }
    std::string c = "PFSM";
    auto put = [&c](uint32_t v) { c.append(reinterpret_cast<const char*>(&v), 4); };
    put(static_cast<uint32_t>(16 + data.size()));
    put(0xFFFFFFFF);
    put(id);
    put(static_cast<uint32_t>(data.size()));
    put(pcm.rate | 4u << 16 | 0xCCu << 24);
    return c + data;
}

}  // namespace adpcm
