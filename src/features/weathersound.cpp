// Weather sound (weather.cpp): rain, wind / blowing sand and thunder, made from shaped noise (no sound files - nothing
// of the game's or anyone else's is used) and played through XAudio2 2.9 (xaudio2_9.dll, Windows 10+, loaded at run
// time; missing = silent weather). The game's sound manager isn't used: its ambient sounds are positional and capped.
//
// A worker thread keeps two streaming voices (rain, wind) fed with 2048-sample buffers; their loudness follows targets
// the game thread sets every frame (weather, wind strength, the camera under a roof - rain duller and quieter, the game
// paused / in the background - silent). Thunder is rendered whole when lightning strikes and started after the
// distance delay, on a third voice. Volume follows the game's SFX volume (sound manager [0x63C9AC] +0x18).
#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/log.h"
#include "features/features.h"

namespace {
constexpr int kRate = 44100, kBlock = 2048, kQueued = 3;
constexpr uint32_t kSoundManager = 0x63C9AC, kDispatchMode = 0x60EC8C, kFrontEndState = 0x617C18;

IXAudio2* g_xa = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
IXAudio2SourceVoice* g_rainVoice = nullptr;
IXAudio2SourceVoice* g_windVoice = nullptr;
IXAudio2SourceVoice* g_thunderVoice = nullptr;
HANDLE g_thread = nullptr;
std::atomic<bool> g_stop{false};
std::atomic<float> g_rainTarget{0}, g_windTarget{0}, g_hissTarget{0}, g_shelter{0}, g_master01{1};
// thunder: requested by the game thread, started by the worker
std::atomic<int> g_thunderDueMs{0};
std::atomic<float> g_thunderPower{0};
std::atomic<bool> g_thunderPending{false};

uint32_t g_seed = 0x9E3779B9u;
float Noise() {   // white, -1..1
    g_seed ^= g_seed << 13, g_seed ^= g_seed >> 17, g_seed ^= g_seed << 5;
    return static_cast<int32_t>(g_seed) * (1.0f / 2147483648.0f);
}

struct OnePole {   // low-pass
    float y = 0;
    float Run(float x, float a) { return y += a * (x - y); }
};
float Coef(float hz) { return 1.0f - std::exp(-6.2831853f * hz / kRate); }

// ---- rain: band-limited hiss plus single drops ----
struct RainGen {
    OnePole lp, hp, lpRoof;
    float gain = 0;
    void Fill(float* out, int n, float target, float shelter) {
        for (int i = 0; i < n; ++i) {
            gain += (target - gain) * 0.0004f;
            float x = Noise();
            const float low = hp.Run(x, Coef(500));
            x = lp.Run(x - low, Coef(6500 - 4500 * shelter));
            if (Noise() > 0.9994f - 0.0008f * gain) x += Noise() * 2.5f;   // a drop hitting something near
            x = lpRoof.Run(x, Coef(9000 - 7800 * shelter));
            out[i] = x * gain * (0.55f - 0.25f * shelter);
        }
    }
};

// ---- wind: low rumble that swells and whistles a little when strong, plus a sandy hiss ----
struct WindGen {
    OnePole lp1, lp2, sweep, hissHp, hissLp;
    float gain = 0, hiss = 0, phase = 0;
    void Fill(float* out, int n, float target, float hissTarget) {
        for (int i = 0; i < n; ++i) {
            gain += (target - gain) * 0.0002f;
            hiss += (hissTarget - hiss) * 0.0002f;
            phase += 0.35f / kRate;
            const float swell = 0.65f + 0.35f * std::sin(phase * 6.2831853f) * std::sin(phase * 2.3f);
            const float cut = 180.0f + 900.0f * std::min(1.0f, gain) * swell;
            float x = lp2.Run(lp1.Run(Noise(), Coef(cut)), Coef(cut * 1.4f)) * 3.5f;
            float h = Noise();
            h = hissLp.Run(h - hissHp.Run(h, Coef(1800)), Coef(7000));
            out[i] = x * gain * swell + h * hiss * 0.35f;
        }
    }
};

// ---- thunder: a crack, then a long low rumble ----
std::vector<float> RenderThunder(float power) {
    const int n = static_cast<int>(kRate * (3.5f + power * 2.0f));
    std::vector<float> out(n);
    OnePole lp1, lp2, crack;
    float brown = 0;
    int nextBurst = 0;
    float burst = 0;
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / kRate;
        brown = std::clamp(brown + Noise() * 0.06f, -1.0f, 1.0f);
        if (i >= nextBurst) {   // the rumble rolls in a few bursts
            burst = 0.5f + 0.5f * std::fabs(Noise());
            nextBurst = i + static_cast<int>(kRate * (0.25f + 0.6f * std::fabs(Noise())));
        }
        const float env = std::min(1.0f, t * 12.0f) * std::exp(-t * 0.9f) * (0.6f + 0.4f * burst);
        float x = lp2.Run(lp1.Run(brown, Coef(160)), Coef(220)) * 4.0f * env;
        if (t < 0.25f) x += crack.Run(Noise(), Coef(2500)) * (0.25f - t) * 4.0f * power;   // the near crack
        out[i] = x * (0.5f + 0.5f * power);
    }
    return out;
}
std::vector<float> g_thunder;   // the buffer playing (kept alive while queued)

struct Stream {
    IXAudio2SourceVoice* voice;
    std::vector<float> buffers[kQueued];
    int next = 0;
};

DWORD WINAPI Worker(void*) {
    RainGen rain;
    WindGen wind;
    Stream streams[2] = {{g_rainVoice}, {g_windVoice}};
    for (Stream& s : streams)
        for (auto& b : s.buffers) b.assign(kBlock, 0.0f);
    while (!g_stop) {
        for (int k = 0; k < 2; ++k) {
            Stream& s = streams[k];
            XAUDIO2_VOICE_STATE st{};
            s.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            while (st.BuffersQueued < kQueued) {
                std::vector<float>& b = s.buffers[s.next];
                s.next = (s.next + 1) % kQueued;
                if (k == 0) rain.Fill(b.data(), kBlock, g_rainTarget, g_shelter);
                else wind.Fill(b.data(), kBlock, g_windTarget, g_hissTarget);
                XAUDIO2_BUFFER xb{};
                xb.AudioBytes = kBlock * sizeof(float);
                xb.pAudioData = reinterpret_cast<const BYTE*>(b.data());
                s.voice->SubmitSourceBuffer(&xb);
                ++st.BuffersQueued;
            }
        }
        if (g_thunderPending && static_cast<int>(GetTickCount()) - g_thunderDueMs >= 0) {
            g_thunderPending = false;
            XAUDIO2_VOICE_STATE st{};
            g_thunderVoice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (st.BuffersQueued == 0) {   // one roll at a time
                g_thunder = RenderThunder(g_thunderPower);
                XAUDIO2_BUFFER xb{};
                xb.AudioBytes = static_cast<UINT32>(g_thunder.size() * sizeof(float));
                xb.pAudioData = reinterpret_cast<const BYTE*>(g_thunder.data());
                xb.Flags = XAUDIO2_END_OF_STREAM;
                g_thunderVoice->SubmitSourceBuffer(&xb);
            }
        }
        g_master->SetVolume(g_master01);
        Sleep(15);
    }
    return 0;
}

bool Start() {
    static bool tried = false;
    if (tried) return g_xa != nullptr;
    tried = true;
    HMODULE dll = LoadLibraryA("xaudio2_9.dll");
    using CreateFn = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
    auto create = dll ? reinterpret_cast<CreateFn>(GetProcAddress(dll, "XAudio2Create")) : nullptr;
    if (!create || FAILED(create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) || !g_xa) {
        dslog::Write("[fail] Weather sound: XAudio2 2.9 not available - weather is silent");
        g_xa = nullptr;
        return false;
    }
    if (FAILED(g_xa->CreateMasteringVoice(&g_master))) {
        dslog::Write("[fail] Weather sound: no audio device");
        g_xa->Release();
        g_xa = nullptr;
        return false;
    }
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = kRate;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = kRate * 4;
    for (IXAudio2SourceVoice** v : {&g_rainVoice, &g_windVoice, &g_thunderVoice})
        if (FAILED(g_xa->CreateSourceVoice(v, &fmt))) {
            dslog::Write("[fail] Weather sound: voice not created");
            return false;
        }
    g_rainVoice->Start();
    g_windVoice->Start();
    g_thunderVoice->Start();
    g_thread = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    dslog::Write("Weather sound: started (XAudio2)");
    return true;
}

float GameSfxVolume() {
    const auto* mgr = *reinterpret_cast<const uint8_t* const*>(kSoundManager);
    if (!mgr) return 1.0f;
    const float v = *reinterpret_cast<const float*>(mgr + 0x18);
    static bool logged = false;
    if (!logged) logged = true, dslog::Write("Weather sound: game SFX volume field %.3f (0x%08X)", v,
                                             *reinterpret_cast<const uint32_t*>(mgr + 0x18));
    return v == 0.0f || (v > 1e-4f && v <= 1.0f) ? v : 1.0f;
}
}  // namespace

void features::WeatherThunder(float delaySeconds, float power) {
    if (!g_xa) return;
    g_thunderPower = std::clamp(power, 0.0f, 1.0f);
    g_thunderDueMs = static_cast<int>(GetTickCount() + static_cast<DWORD>(delaySeconds * 1000.0f));
    g_thunderPending = true;
}

// Every frame (game thread): targets from the weather.
void features::OnFrameWeatherSound() {
    const int kind = features::WeatherKind();
    const float rain = features::WeatherRain();
    const float wind = features::WindStrength();
    if (!g_xa && (rain > 0 || wind > 6.0f) && !Start()) return;
    if (!g_xa) return;
    const bool playing = *reinterpret_cast<int8_t*>(kDispatchMode) == 0 && features::GameFocused() &&
                         *reinterpret_cast<uint32_t*>(kFrontEndState) != 0xF;   // not in the pause menu
    const bool sand = kind == features::kWeatherDust || kind == features::kWeatherSandstorm;
    const bool snow = kind >= features::kWeatherLightSnow && kind <= features::kWeatherBlizzard;
    const float shelter = features::CameraSheltered() ? 1.0f : 0.0f;
    g_rainTarget = playing ? std::min(1.2f, rain) * (1.0f - 0.45f * shelter) : 0.0f;
    // wind is heard from about 4 m/s up
    const float w = std::clamp((wind - 3.0f) / 12.0f, 0.0f, 1.2f);
    g_windTarget = playing ? w * (snow ? 0.9f : 0.7f) * (1.0f - 0.5f * shelter) : 0.0f;
    g_hissTarget = playing && sand ? w * (1.0f - 0.6f * shelter) : 0.0f;
    g_shelter = shelter;
    g_master01 = playing ? 0.8f * GameSfxVolume() : 0.0f;
    if (!playing) g_thunderPending = false;
}
