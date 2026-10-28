"""gen_weather_sounds.py <folder> - the weather plugin's sounds from "Weather Elements" by Helton Yan
(https://heltonyan.itch.io/weatherelements, CC BY 4.0): the unzipped pack's WAVs (44.1 kHz 16-bit stereo) ->
assets/weather-elements/*.adpcm, which dsfix.weather.dll carries as RCDATA (plugins/weather/weather_sounds.rc) and
XAudio2 plays as they are.

- Loops (rain outside / inside, light / heavy; wind whistle light / heavy): the last 1.5 s are crossfaded into the start
  (equal power) so the 60 s loop has no seam, cut to whole ADPCM blocks. Inside rain is resampled to 22.05 kHz (it is
  muffled anyway - half the size).
- Thunder: the silent tail is trimmed (below -60 dB for good), 50 ms fade out.
- MS-ADPCM (WAVE_FORMAT_ADPCM, the 7 standard coefficient pairs, 512 samples per block): about 4:1. The predictor per
  block and channel is picked by least squares, then encoded with the real adaptive step; every file is decoded again
  and its signal-to-noise ratio printed.

The pack's WAVs aren't kept in the repo (they are 650 MB); run this again to change the choice below.
"""
import os
import struct
import sys
import wave

import numpy as np

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "assets", "weather-elements")

# (pack file, our name, kind, rate)
CHOICE = [
    ("WE Light Outside Rain 1", "rain_light_outside", "loop", 44100),
    ("WE Heavy Outside Rain 1", "rain_heavy_outside", "loop", 44100),
    ("WE Light Inside Rain 1", "rain_light_inside", "loop", 22050),
    ("WE Heavy Inside Rain 1", "rain_heavy_inside", "loop", 22050),
    ("WE Light Wind Whistle 1", "wind_light", "loop", 44100),
    ("WE Heavy Wind Whistle 1", "wind_heavy", "loop", 44100),
] + [(f"WE Thunder {n}", f"thunder_{i}", "oneshot", 44100) for i, n in enumerate((1, 5, 10, 15, 20, 26, 29, 31))]

SPB = 512   # samples per block
COEF = [(256, 0), (512, -256), (0, 0), (192, 64), (240, 0), (460, -208), (392, -232)]
ADAPT = [230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230]


def read_wav(path):
    w = wave.open(path)
    assert w.getsampwidth() == 2, path
    ch, rate = w.getnchannels(), w.getframerate()
    a = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64).reshape(-1, ch)
    if ch == 1: a = np.repeat(a, 2, axis=1)
    return a, rate


def resample_half(a):
    """44.1 -> 22.05 kHz: windowed-sinc low-pass at 10 kHz, then every second sample."""
    n = 63
    t = np.arange(n) - (n - 1) / 2
    h = np.sinc(t * 2 * 10000 / 44100) * np.hamming(n)
    h /= h.sum()
    return np.stack([np.convolve(a[:, c], h, mode="same")[::2] for c in range(a.shape[1])], axis=1)


def make_loop(a, rate):
    x = int(1.5 * rate)
    head, tail = a[:x], a[-x:]
    k = np.linspace(0, np.pi / 2, x)[:, None]
    out = np.concatenate([head * np.sin(k) + tail * np.cos(k), a[x:-x]])
    return out[: len(out) // SPB * SPB]


def trim_tail(a, rate):
    level = np.abs(a).max(axis=1)
    loud = np.nonzero(level > 32768 * 10 ** (-60 / 20))[0]
    end = min(len(a), (loud[-1] if len(loud) else len(a)) + int(0.1 * rate))
    a = a[:end].copy()
    f = min(len(a), int(0.05 * rate))
    a[-f:] *= np.linspace(1, 0, f)[:, None]
    pad = (-len(a)) % SPB
    return np.concatenate([a, np.zeros((pad, a.shape[1]))])


def clamp16(v):
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def encode_channel_block(s):
    """One channel of one block: (predictor, delta, sample1, sample2, nibbles[SPB - 2])."""
    s = [int(v) for v in s]
    best = None
    x = np.array(s, dtype=np.float64)
    for p, (c1, c2) in enumerate(COEF):   # least-squares residual of each fixed predictor
        e = x[2:] - (x[1:-1] * c1 + x[:-2] * c2) / 256.0
        err = float(np.dot(e, e))
        if best is None or err < best[0]: best = (err, p)
    p = best[1]
    c1, c2 = COEF[p]
    s2, s1 = s[0], s[1]   # the block header holds the first two samples (sample2 = the first)
    first = [abs(s[i + 2] - (s[i + 1] * c1 + s[i] * c2) // 256) for i in range(min(3, len(s) - 2))]
    delta = max(16, (sum(first) // max(1, len(first))) // 4)
    d0 = delta
    nibbles = []
    for v in s[2:]:
        pred = (s1 * c1 + s2 * c2) >> 8
        n = int(round((v - pred) / delta))
        n = -8 if n < -8 else 7 if n > 7 else n
        new = clamp16(pred + n * delta)
        nibbles.append(n & 15)
        delta = max(16, (delta * ADAPT[n & 15]) >> 8)
        s2, s1 = s1, new
    return p, d0, s[1], s[0], nibbles


def encode(a):
    """a: frames x 2 floats -> (MS-ADPCM bytes, frames)."""
    pcm = np.clip(np.round(a), -32768, 32767).astype(np.int32)
    out = bytearray()
    for b in range(0, len(pcm), SPB):
        blk = pcm[b:b + SPB]
        if len(blk) < SPB: blk = np.concatenate([blk, np.zeros((SPB - len(blk), 2), dtype=np.int32)])
        L, R = encode_channel_block(blk[:, 0]), encode_channel_block(blk[:, 1])
        out += struct.pack("<BBhhhhhh", L[0], R[0], L[1], R[1], L[2], R[2], L[3], R[3])
        out += bytes((l << 4) | r for l, r in zip(L[4], R[4]))
    return bytes(out), len(pcm)


def decode(data):
    out = []
    bsize = 7 * 2 + (SPB - 2)
    for b in range(0, len(data), bsize):
        pl, pr, dl, dr, s1l, s1r, s2l, s2r = struct.unpack_from("<BBhhhhhh", data, b)
        st = [[COEF[pl], dl, s1l, s2l], [COEF[pr], dr, s1r, s2r]]
        blk = [(s2l, s2r), (s1l, s1r)]
        for byte in data[b + 14:b + bsize]:
            frame = []
            for c, nib in ((0, byte >> 4), (1, byte & 15)):
                (c1, c2), d, s1, s2 = st[c]
                n = nib - 16 if nib >= 8 else nib
                new = clamp16(((s1 * c1 + s2 * c2) >> 8) + n * d)
                st[c] = [(c1, c2), max(16, (d * ADAPT[nib]) >> 8), new, s1]
                frame.append(new)
            blk.append(tuple(frame))
        out += blk
    return np.array(out, dtype=np.float64)


def write_adpcm(path, data, frames, rate):
    block_align = (7 + (SPB - 2) // 2) * 2
    fmt = struct.pack("<HHIIHHHHH", 2, 2, rate, rate * block_align // SPB, block_align, 4, 32, SPB, len(COEF))
    fmt += b"".join(struct.pack("<hh", c1, c2) for c1, c2 in COEF)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt
    body += b"fact" + struct.pack("<II", 4, frames)
    body += b"data" + struct.pack("<I", len(data)) + data
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)


def main():
    src = sys.argv[1]
    os.makedirs(OUT, exist_ok=True)
    total = 0
    for file, name, kind, rate in CHOICE:
        a, r = read_wav(os.path.join(src, file + ".wav"))
        assert r == 44100, file
        if rate == 22050: a = resample_half(a)
        a = make_loop(a, rate) if kind == "loop" else trim_tail(a, rate)
        data, frames = encode(a)
        back = decode(data)[:frames]
        ref = np.clip(np.round(a), -32768, 32767)[:frames]
        snr = 10 * np.log10(np.sum(ref ** 2) / max(1.0, np.sum((ref - back) ** 2)))
        path = os.path.join(OUT, name + ".adpcm")
        write_adpcm(path, data, frames, rate)
        total += os.path.getsize(path)
        print(f"{name:20s} {frames / rate:5.1f} s {rate} Hz  {os.path.getsize(path) / 1e6:5.2f} MB  SNR {snr:4.1f} dB  <- {file}")
    print(f"total {total / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
