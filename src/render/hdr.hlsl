// Output passes (presenter.cpp), compiled at build time by fxc (CMakeLists.txt):
//   VSMain      full-screen triangle (position + uv)
//   PSMain      HDR: the game's frame -> HDR10 (Rec.2020, PQ) for a 10-bit swap chain
//   PSMainSdr   SDR: a float frame and / or bloom -> the B8G8R8A8 swap chain
//   PSPrefilter / PSDown / PSUp   bloom: bright parts at half size, 13-tap downsample chain, tent upsample (added up)
//
// t0 = the final frame (scene + HUD), t1 = the scene alone (back buffer copied at each 3D -> 2D switch), t2 = bloom.
// Pixels where final and scene differ were drawn by the HUD / menus / fades: they stay at paper white and get no
// bloom. Scene pixels keep paper white up to a knee; above it they are stretched so full SDR white lands on `white`
// nits. With the FP16 target, additive effects (muzzle flashes, explosions, sun glare) go past 1.0 - that overrange
// takes the rest of the way to `peak`.

cbuffer Params : register(b0) {
    float paper;     // paper white, nits
    float peak;      // brightest output, nits (= paper: no expansion)
    float knee;      // SDR luminance where the expansion starts (0..1)
    float hasScene;  // 0: no scene copy this frame (menus) - everything is "HUD"
    float white;     // nits for SDR full white (= peak without the float target: no overrange to save room for)
    float bloom;     // bloom strength (0 = off)
    float2 texel;    // 1 / size of the texture being sampled (bloom passes)
};
Texture2D<float4> finalImage : register(t0);
Texture2D<float4> sceneImage : register(t1);
Texture2D<float4> bloomImage : register(t2);
SamplerState linearClamp : register(s0);

struct V {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

V VSMain(uint id : SV_VertexID) {
    V v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.pos = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}

// SDR monitors (and the game's art) are a 2.2 power curve - closer to how it looked than the piecewise sRGB curve,
// which lifts the blacks. Values above 1 (float target) continue the same curve.
float3 ToLinear(float3 c) { return pow(max(c, 0), 2.2); }

// Linear Rec.709 in units of 80 nits (scRGB scale) -> HDR10: Rec.2020 primaries, PQ (SMPTE ST 2084) of nits / 10000.
float4 ToHdr10(float3 scrgb) {
    const float3x3 to2020 = {0.6274040, 0.3292820, 0.0433136, 0.0690970, 0.9195400, 0.0113612, 0.0163916, 0.0880132, 0.8955950};
    float3 l = saturate(mul(to2020, max(scrgb, 0)) * (80.0 / 10000.0));
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(l, m1);
    return float4(pow((c1 + c2 * p) / (1 + c3 * p), m2), 1);
}

bool IsHud(int3 p, float3 f) { return hasScene < 0.5 || any(abs(f - sceneImage.Load(p).rgb) > 1.5 / 255.0); }

float4 PSMain(V v) : SV_Target {
    int3 p = int3(v.pos.xy, 0);
    float3 f = finalImage.Load(p).rgb;
    float3 lin = ToLinear(f);
    float scale = paper / 80.0;
    bool hud = IsHud(p, f);
    if (hud) {
        lin = min(lin, 1.0);  // HUD / menus: no overrange, no bloom
        if (hasScene < 0.5) return ToHdr10(lin * scale);  // menus: paper white
        // In missions the HUD follows the scene's curve, as in SDR (HUD white = scene white): a white crosshair over
        // bright sand stays visible.
    } else if (bloom > 0) {
        lin += bloomImage.Sample(linearClamp, v.uv).rgb * bloom;
    }
    float y = dot(lin, float3(0.2126, 0.7152, 0.0722));
    if (y > knee && peak > paper) {
        float nits;
        if (y <= 1.0) {
            float t = (y - knee) / (1.0 - knee);
            nits = y * paper + t * t * (white - paper);  // C1 at the knee, = white at y = 1
        } else {
            nits = white + (peak - white) * (1.0 - exp(-2.0 * (y - 1.0)));  // overrange: towards the peak
        }
        scale = nits / (80.0 * y);  // same hue: every channel scaled alike
    }
    return ToHdr10(lin * scale);
}

float4 PSMainSdr(V v) : SV_Target {
    int3 p = int3(v.pos.xy, 0);
    float3 f = finalImage.Load(p).rgb;
    if (bloom > 0 && !IsHud(p, f)) f = pow(max(ToLinear(f) + bloomImage.Sample(linearClamp, v.uv).rgb * bloom, 0), 1 / 2.2);
    return float4(saturate(f), 1);
}

// Bloom. Prefilter: 4 bilinear taps (a 4x4 box) of the scene at half size, linear, soft threshold around 0.8 so only
// the brightest parts (and the float target's overrange) glow.
float4 PSPrefilter(V v) : SV_Target {
    float3 c = 0;
    c += ToLinear(finalImage.Sample(linearClamp, v.uv + texel * float2(-1, -1)).rgb);
    c += ToLinear(finalImage.Sample(linearClamp, v.uv + texel * float2(1, -1)).rgb);
    c += ToLinear(finalImage.Sample(linearClamp, v.uv + texel * float2(-1, 1)).rgb);
    c += ToLinear(finalImage.Sample(linearClamp, v.uv + texel * float2(1, 1)).rgb);
    c *= 0.25;
    const float threshold = 0.8, softKnee = 0.3;
    float b = max(c.r, max(c.g, c.b));
    float soft = clamp(b - threshold + softKnee, 0, 2 * softKnee);
    soft = soft * soft / (4 * softKnee + 1e-4);
    return float4(c * max(soft, b - threshold) / max(b, 1e-4), 1);
}

float3 Tap(float2 uv, float2 d) { return finalImage.Sample(linearClamp, uv + texel * d).rgb; }

// 13-tap downsample (Jimenez, "Next generation post processing in Call of Duty") - no flicker from single bright pixels.
float4 PSDown(V v) : SV_Target {
    float2 uv = v.uv;
    float3 a = Tap(uv, float2(-2, -2)), b = Tap(uv, float2(0, -2)), c = Tap(uv, float2(2, -2));
    float3 d = Tap(uv, float2(-2, 0)), e = Tap(uv, float2(0, 0)), f = Tap(uv, float2(2, 0));
    float3 g = Tap(uv, float2(-2, 2)), h = Tap(uv, float2(0, 2)), i = Tap(uv, float2(2, 2));
    float3 j = Tap(uv, float2(-1, -1)), k = Tap(uv, float2(1, -1)), l = Tap(uv, float2(-1, 1)), m = Tap(uv, float2(1, 1));
    float3 r = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    return float4(r, 1);
}

// 3x3 tent upsample, blended additively onto the next larger level.
float4 PSUp(V v) : SV_Target {
    float2 uv = v.uv;
    float3 r = Tap(uv, float2(0, 0)) * 4;
    r += (Tap(uv, float2(0, -1)) + Tap(uv, float2(-1, 0)) + Tap(uv, float2(1, 0)) + Tap(uv, float2(0, 1))) * 2;
    r += Tap(uv, float2(-1, -1)) + Tap(uv, float2(1, -1)) + Tap(uv, float2(-1, 1)) + Tap(uv, float2(1, 1));
    return float4(r / 16, 1);
}
