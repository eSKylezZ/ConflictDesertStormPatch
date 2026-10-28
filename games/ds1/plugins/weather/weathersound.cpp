// Weather sound (weather.cpp): rain, wind and thunder from "Weather Elements" by Helton Yan (CC BY 4.0,
// https://heltonyan.itch.io/weatherelements) - built into this DLL as MS-ADPCM WAVs (RCDATA 400.., weather_sounds.rc,
// made by games/ds1/scripts/gen_weather_sounds.py) - plus a sandy hiss made from shaped noise, all played through
// XAudio2 2.9 (xaudio2_9.dll, Windows 10+, loaded at run time; missing = silent weather). The game's sound manager isn't
// used: its ambient sounds are positional and capped.
//
// - Rain: four 60 s loops - light / heavy, heard outside / inside a building - always playing, their volumes mixed:
//   light <-> heavy by the rain shown, outside <-> inside by where view 0's camera is (weather.cpp CameraCover(): a
//   building interior = inside, under a roof / canopy outdoors = between the two).
// - Wind: light / heavy whistle loops by wind strength (heard from about 4 m/s), quieter indoors; the hiss of blowing
//   sand on top in dust / sandstorms.
// - Thunder: one of eight claps when lightning strikes, after the distance delay, on up to three voices; indoors
//   quieter and low-passed.
// A worker thread eases every volume towards the targets the game thread sets each frame (silent while paused / in
// the background / loading) and feeds the hiss. Volume follows the game's SFX volume (sound manager [0x63C9AC] +0x18).
#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "weather_local.h"

namespace {
constexpr int kRate = 44100, kBlock = 2048, kQueued = 3;
constexpr uint32_t kSoundManager = 0x63C9AC, kDispatchMode = 0x60EC8C, kFrontEndState = 0x617C18;

// The loops in weather_sounds.rc order (ids 400 + index), with a level that evens them out (the pack's light inside
// rain is very quiet, its heavy wind whistle loud).
enum Loop { kRainLightOut, kRainHeavyOut, kRainLightIn, kRainHeavyIn, kWindLight, kWindHeavy, kLoops };
constexpr float kLoopGain[kLoops] = {1.0f, 0.85f, 1.8f, 0.9f, 1.4f, 0.45f};
constexpr int kFirstThunder = 406, kThunders = 8, kThunderVoices = 3;

struct Wav {
    const WAVEFORMATEX* fmt = nullptr;
    const BYTE* data = nullptr;
    UINT32 bytes = 0;
};
// A RIFF / WAVE resource of this DLL (read-only, mapped for the DLL's life: XAudio2 plays straight from it).
Wav LoadWav(int id) {
    Wav w;
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&LoadWav), &self);
    HRSRC res = FindResourceA(self, MAKEINTRESOURCEA(id), MAKEINTRESOURCEA(10) /*RT_RCDATA*/);
    if (!res) return w;
    const auto* p = static_cast<const BYTE*>(LockResource(LoadResource(self, res)));
    const DWORD size = SizeofResource(self, res);
    if (!p || size < 12 || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0) return w;
    for (DWORD at = 12; at + 8 <= size;) {
        const DWORD len = *reinterpret_cast<const DWORD*>(p + at + 4);
        if (at + 8 + len > size) break;
        if (!memcmp(p + at, "fmt ", 4)) w.fmt = reinterpret_cast<const WAVEFORMATEX*>(p + at + 8);
        if (!memcmp(p + at, "data", 4)) w.data = p + at + 8, w.bytes = len;
        at += 8 + len + (len & 1);
    }
    if (!w.fmt || !w.data) w = {};
    return w;
}

IXAudio2* g_xa = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
IXAudio2SourceVoice* g_loop[kLoops] = {};
IXAudio2SourceVoice* g_hissVoice = nullptr;
IXAudio2SourceVoice* g_thunderVoice[kThunderVoices] = {};
Wav g_thunderWav[kThunders];
HANDLE g_thread = nullptr;
std::atomic<bool> g_stop{false};
std::atomic<float> g_target[kLoops] = {};
std::atomic<float> g_hissTarget{0}, g_master01{1}, g_cover{0};
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

// ---- blowing sand: a band-passed hiss ----
struct HissGen {
    OnePole hp, lp;
    float gain = 0;
    void Fill(float* out, int n, float target) {
        for (int i = 0; i < n; ++i) {
            gain += (target - gain) * 0.0002f;
            const float h = Noise();
            out[i] = lp.Run(h - hp.Run(h, Coef(1800)), Coef(7000)) * gain * 0.35f;
        }
    }
};

int PickThunder() {   // a random clap, not the last one
    static int last = -1;
    int i = static_cast<int>(GetTickCount() * 2654435761u >> 16) % kThunders;
    if (i == last) i = (i + 1) % kThunders;
    return last = i;
}

DWORD WINAPI Worker(void*) {
    HissGen hiss;
    std::vector<float> buffers[kQueued];
    for (auto& b : buffers) b.assign(kBlock, 0.0f);
    int next = 0;
    float level[kLoops] = {};
    while (!g_stop) {
        for (int k = 0; k < kLoops; ++k) {   // ease in / out over about half a second
            level[k] += (g_target[k] - level[k]) * 0.06f;
            if (g_loop[k]) g_loop[k]->SetVolume(level[k] * kLoopGain[k]);
        }
        XAUDIO2_VOICE_STATE st{};
        g_hissVoice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        while (st.BuffersQueued < kQueued) {
            std::vector<float>& b = buffers[next];
            next = (next + 1) % kQueued;
            hiss.Fill(b.data(), kBlock, g_hissTarget);
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = kBlock * sizeof(float);
            xb.pAudioData = reinterpret_cast<const BYTE*>(b.data());
            g_hissVoice->SubmitSourceBuffer(&xb);
            ++st.BuffersQueued;
        }
        if (g_thunderPending && static_cast<int>(GetTickCount()) - g_thunderDueMs >= 0) {
            g_thunderPending = false;
            for (IXAudio2SourceVoice* v : g_thunderVoice) {   // a free voice, else this clap is skipped
                if (!v) continue;
                v->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                if (st.BuffersQueued) continue;
                const Wav& w = g_thunderWav[PickThunder()];
                if (!w.data) break;
                const float cover = g_cover;
                v->SetVolume((0.45f + 0.55f * g_thunderPower) * (1.0f - 0.45f * cover));
                // indoors the crack is muffled: low-pass from ~16 kHz (open) to ~900 Hz (inside)
                const float hz = 16000.0f - 15100.0f * cover;   // XAudio2's "radians": 2 sin(pi f / rate), at most 1
                XAUDIO2_FILTER_PARAMETERS f{LowPassFilter, std::min(1.0f, 2.0f * std::sin(3.14159265f * hz / kRate)), 1.0f};
                v->SetFilterParameters(&f);
                XAUDIO2_BUFFER xb{};
                xb.AudioBytes = w.bytes;
                xb.pAudioData = w.data;
                xb.Flags = XAUDIO2_END_OF_STREAM;
                v->SubmitSourceBuffer(&xb);
                break;
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
    int loops = 0;
    for (int k = 0; k < kLoops; ++k) {   // every loop plays all the time, silent until wanted
        const Wav w = LoadWav(400 + k);
        if (!w.data || FAILED(g_xa->CreateSourceVoice(&g_loop[k], w.fmt))) {
            dslog::Write("[fail] Weather sound: loop %d not loaded", k);
            g_loop[k] = nullptr;
            continue;
        }
        XAUDIO2_BUFFER xb{};
        xb.AudioBytes = w.bytes;
        xb.pAudioData = w.data;
        xb.LoopCount = XAUDIO2_LOOP_INFINITE;
        xb.Flags = XAUDIO2_END_OF_STREAM;
        g_loop[k]->SetVolume(0.0f);
        g_loop[k]->SubmitSourceBuffer(&xb);
        g_loop[k]->Start();
        ++loops;
    }
    for (int i = 0; i < kThunders; ++i) g_thunderWav[i] = LoadWav(kFirstThunder + i);
    if (g_thunderWav[0].data)
        for (IXAudio2SourceVoice*& v : g_thunderVoice)
            if (SUCCEEDED(g_xa->CreateSourceVoice(&v, g_thunderWav[0].fmt, XAUDIO2_VOICE_USEFILTER))) v->Start();
            else v = nullptr;
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = kRate;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = kRate * 4;
    if (FAILED(g_xa->CreateSourceVoice(&g_hissVoice, &fmt))) {
        dslog::Write("[fail] Weather sound: voice not created");
        return false;
    }
    g_hissVoice->Start();
    g_thread = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    dslog::Write("Weather sound: started (XAudio2, %d of %d loops, %s)", loops, kLoops,
                 g_thunderVoice[0] ? "thunder" : "no thunder");
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

float Smooth(float x) {   // 0..1 smoothstep
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}
}  // namespace

void features::StopWeatherSound() {
    if (g_thread) {
        g_stop = true;
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    for (IXAudio2SourceVoice*& v : g_loop)
        if (v) v->DestroyVoice(), v = nullptr;
    for (IXAudio2SourceVoice*& v : g_thunderVoice)
        if (v) v->DestroyVoice(), v = nullptr;
    if (g_hissVoice) g_hissVoice->DestroyVoice(), g_hissVoice = nullptr;
    if (g_master) g_master->DestroyVoice(), g_master = nullptr;
    if (g_xa) g_xa->Release(), g_xa = nullptr;
}

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
    // kind 0 = no weather of ours: the front end (and a mission with its own weather) - all silent at once
    const bool playing = kind != 0 && *reinterpret_cast<int8_t*>(kDispatchMode) == 0 && features::GameFocused() &&
                         *reinterpret_cast<uint32_t*>(kFrontEndState) != 0xF;   // not in the pause menu
    const bool sand = kind == features::kWeatherDust || kind == features::kWeatherSandstorm;
    const bool snow = kind >= features::kWeatherLightSnow && kind <= features::kWeatherBlizzard;
    const float cover = features::CameraCover();   // 0 open, 0.6 under a roof, 1 inside a building
    g_cover = cover;
    // rain: loud as the rain shown (light rain 0.35, rain 1, storm 1.4), light -> heavy from 0.4 to 1.1
    const float r = playing ? std::min(1.3f, rain) : 0.0f;
    const float loud = std::sqrt(std::min(1.0f, r / 0.35f)) * (0.75f + 0.25f * std::min(1.0f, r));
    const float heavy = Smooth((r - 0.4f) / 0.7f);
    const float out = 1.0f - cover, in = cover;
    g_target[kRainLightOut] = loud * (1.0f - heavy) * out;
    g_target[kRainHeavyOut] = loud * heavy * out;
    g_target[kRainLightIn] = loud * (1.0f - heavy) * in;
    g_target[kRainHeavyIn] = loud * heavy * in;
    // wind from about 4 m/s; light whistle up to ~9 m/s, heavy for storms, blizzards and sandstorms
    const float w = playing ? std::clamp((wind - 3.0f) / 12.0f, 0.0f, 1.2f) : 0.0f;
    const float strong = Smooth((wind - 7.0f) / 6.0f);
    const float wl = std::min(1.0f, w * 2.0f) * (snow || sand ? 1.0f : 0.8f) * (1.0f - 0.65f * cover);
    g_target[kWindLight] = wl * (1.0f - strong);
    g_target[kWindHeavy] = wl * strong;
    g_hissTarget = playing && sand ? w * (1.0f - 0.8f * cover) : 0.0f;
    g_master01 = playing ? 0.8f * GameSfxVolume() : 0.0f;
    if (!playing) g_thunderPending = false;
}
