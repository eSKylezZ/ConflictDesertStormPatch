// Puddle reflections (water.cpp) for the D3D9 renderers - Shader Model 3, compiled by fxc at build time.
//
// Drawn on the puddle decal's own triangles (they follow the ground: nothing floats or clips), masked by the puddle
// picture through the decal's UVs (which grow with the puddle). Each pixel mirrors the view ray at the water (normal
// rippled by the rain), guesses the reflected point at the same distance as the puddle is from the eye, and samples
// the scene copy (the frame so far) where that point lands on screen; off screen / behind the eye it takes the sky
// colour. Fresnel: little reflection looking straight down, much at a glancing angle. The level's fog last.

float4x4 g_viewProj : register(c0);
float4 g_eye : register(c4);        // xyz eye (cm), w time (s)
float4 g_viewport : register(c5);   // x, y, w, h of the view in pixels
float4 g_target : register(c6);     // 1 / scene width, 1 / scene height, rain 0..1, -
float4 g_sky : register(c7);        // rgb sky colour, a = puddle opacity (drying)
float4 g_fogColour : register(c8);  // rgb fog colour
float4 g_fog : register(c9);        // fog start, 1 / (end - start) (cm), -, -

sampler2D g_scene : register(s0);
sampler2D g_mask : register(s1);    // the puddle picture (alpha = water)

struct VsIn {
    float3 pos : POSITION;
    float2 uv : TEXCOORD0;
};
struct VsOut {
    float4 pos : POSITION;
    float3 world : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

VsOut VSMain(VsIn v) {
    VsOut o;
    o.pos = mul(float4(v.pos, 1.0f), g_viewProj);
    o.world = v.pos;
    o.uv = v.uv;
    return o;
}

float Hash(float2 p) { return frac(sin(dot(p, float2(127.1f, 311.7f))) * 43758.5453f); }

// Rain rings: one ring per 30 cm cell, each starting at its own moment, spreading and fading within ~0.8 s.
float2 Ripples(float2 xz, float t, float rain) {
    float2 n = 0;
    const float cell = 30.0f;
    float2 base = floor(xz / cell);
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            float2 c = base + float2(i, j);
            float h = Hash(c);
            float period = 0.6f + h * 0.9f;
            float age = frac(t / period + h) * period;
            float2 centre = (c + float2(Hash(c + 1.7f), Hash(c + 3.1f))) * cell;
            float2 d = xz - centre;
            float r = length(d);
            float front = age * 40.0f;   // cm / s
            float ring = exp(-pow((r - front) / 3.0f, 2.0f)) * saturate(1.0f - age / 0.8f);
            n += (r > 0.01f ? d / r : 0) * ring;
        }
    return n * 0.35f * rain;
}

float4 PSMain(VsOut i) : COLOR {
    float mask = saturate(tex2D(g_mask, i.uv).a * 1.6f);
    if (mask <= 0.0f) discard;

    float3 v = normalize(i.world - g_eye.xyz);
    float2 rip = Ripples(i.world.xz, g_eye.w, g_target.z);
    float3 n = normalize(float3(rip.x, 1.0f, rip.y));
    float3 r = reflect(v, n);
    float dist = length(i.world - g_eye.xyz);
    float3 q = i.world + r * max(dist, 300.0f);
    float4 clip = mul(float4(q, 1.0f), g_viewProj);
    float3 colour = g_sky.rgb;
    if (clip.w > 1.0f) {
        float2 ndc = clip.xy / clip.w;
        float2 px = g_viewport.xy + (float2(ndc.x, -ndc.y) * 0.5f + 0.5f) * g_viewport.zw;
        float2 uv = (px + 0.5f) * g_target.xy;
        float2 inView = step(g_viewport.xy, px) * step(px, g_viewport.xy + g_viewport.zw);
        colour = lerp(g_sky.rgb, tex2D(g_scene, uv).rgb, inView.x * inView.y);
    }
    float cosV = saturate(dot(-v, n));
    float fresnel = 0.03f + 0.97f * pow(1.0f - cosV, 5.0f);
    float amount = lerp(0.35f, 0.92f, fresnel);   // puddles are dark: even looking down they mirror some
    float3 water = float3(0.05f, 0.055f, 0.065f);
    float3 c = lerp(water, colour * 0.9f, amount);
    float fog = saturate((dist - g_fog.x) * g_fog.y);
    c = lerp(c, g_fogColour.rgb, fog);
    return float4(c, mask * g_sky.a * 0.85f * (1.0f - fog * 0.6f));
}
