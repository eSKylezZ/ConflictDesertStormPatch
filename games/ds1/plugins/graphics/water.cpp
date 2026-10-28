// Puddle reflections (see water.h, shader water.hlsl). Per view: the frame so far (world + transparents) is copied
// into a texture of the render target's size and format (StretchRect - resolves MSAA too), then each puddle decal's
// own triangles are drawn again with the reflection shader (depth bias one step past the decal's), blended over it. All D3D9
// state is captured in a state block first and applied back after, so the D3D8 wrapper's view of the device stays true.
#include "render/water.h"

#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"

#include "water_ps.h"
#include "water_vs.h"

namespace {
IDirect3DDevice9* g_dev = nullptr;
IDirect3DVertexShader9* g_vs = nullptr;
IDirect3DPixelShader9* g_ps = nullptr;
IDirect3DTexture9* g_scene = nullptr;   // D3DPOOL_DEFAULT: released before every Reset
UINT g_sceneW = 0, g_sceneH = 0;
D3DFORMAT g_sceneFormat = D3DFMT_UNKNOWN;
bool g_failed = false;

template <class T>
void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

bool Shaders(IDirect3DDevice9* d9) {
    if (d9 != g_dev) {   // a new device: everything again
        Release(g_vs), Release(g_ps), Release(g_scene);
        g_dev = d9;
        g_failed = false;
    }
    if (g_failed) return false;
    if (!g_vs && FAILED(d9->CreateVertexShader(reinterpret_cast<const DWORD*>(g_waterVs), &g_vs))) g_failed = true;
    if (!g_ps && FAILED(d9->CreatePixelShader(reinterpret_cast<const DWORD*>(g_waterPs), &g_ps))) g_failed = true;
    if (g_failed) dslog::Write("[fail] Water: reflection shaders not created - puddles stay plain");
    return !g_failed;
}

// The scene copy for this render target (re-made when its size / format changes).
bool SceneCopy(IDirect3DDevice9* d9, IDirect3DSurface9* target, const D3DSURFACE_DESC& d) {
    if (g_scene && (g_sceneW != d.Width || g_sceneH != d.Height || g_sceneFormat != d.Format)) Release(g_scene);
    if (!g_scene) {
        if (FAILED(d9->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &g_scene,
                                     nullptr)))
            return false;
        g_sceneW = d.Width, g_sceneH = d.Height, g_sceneFormat = d.Format;
    }
    IDirect3DSurface9* dst = nullptr;
    if (FAILED(g_scene->GetSurfaceLevel(0, &dst))) return false;
    const HRESULT hr = d9->StretchRect(target, nullptr, dst, nullptr, D3DTEXF_NONE);
    dst->Release();
    return SUCCEEDED(hr);
}

void Multiply(const D3DMATRIX& a, const D3DMATRIX& b, float out[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[r][k] * b.m[k][c];
            out[c * 4 + r] = s;   // transposed: the shader's column-major constants
        }
}

}  // namespace

void render::WaterBeforeReset() { Release(g_scene); }

bool render::DrawPuddleReflections(void* dev8, const PuddlePart* parts, int count, const WaterLook& look) {
    IDirect3DDevice9* d9 = Device9For(dev8);
    IDirect3DTexture9* mask = Texture9For(look.mask);
    if (!d9 || !mask || count <= 0 || !Shaders(d9)) return false;
    IDirect3DSurface9* target = nullptr;
    if (FAILED(d9->GetRenderTarget(0, &target)) || !target) return false;
    D3DSURFACE_DESC desc{};
    target->GetDesc(&desc);
    D3DVIEWPORT9 vp{};
    d9->GetViewport(&vp);
    const bool copied = SceneCopy(d9, target, desc);
    target->Release();
    if (!copied) {
        static bool logged = false;
        if (!logged) logged = true, dslog::Write("[fail] Water: scene copy failed (%ux%u format %d)", desc.Width, desc.Height,
                                                 static_cast<int>(desc.Format));
        return false;
    }

    D3DMATRIX view, proj;
    d9->GetTransform(D3DTS_VIEW, &view);
    d9->GetTransform(D3DTS_PROJECTION, &proj);
    float vp16[16];
    Multiply(view, proj, vp16);
    float eye[3];   // -translation x rotation^T
    for (int j = 0; j < 3; ++j) eye[j] = -(view._41 * view.m[j][0] + view._42 * view.m[j][1] + view._43 * view.m[j][2]);

    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(d9->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) return false;
    d9->SetVertexShader(g_vs);
    d9->SetPixelShader(g_ps);
    d9->SetFVF(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);   // the decal's 0x152
    d9->SetTexture(0, g_scene);
    d9->SetTexture(1, mask);
    for (DWORD s = 2; s < 8; ++s) d9->SetTexture(s, nullptr);
    for (DWORD s = 0; s < 2; ++s) {
        d9->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        d9->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        d9->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        d9->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        d9->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d9->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    }
    d9->SetRenderState(D3DRS_ZENABLE, TRUE);
    d9->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d9->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    d9->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d9->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    d9->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d9->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d9->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    d9->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d9->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d9->SetRenderState(D3DRS_FOGENABLE, FALSE);   // the shader fogs
    d9->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d9->SetRenderState(D3DRS_LIGHTING, FALSE);
    d9->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d9->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    d9->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    // one step past the decal's own ZBIAS 2 (the wrapper's scale: one step = -1 / 65535) - drawn over it, never under
    const float bias = 3.0f * (-1.0f / 65535.0f), slope = 0.0f;
    d9->SetRenderState(D3DRS_DEPTHBIAS, *reinterpret_cast<const DWORD*>(&bias));
    d9->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, *reinterpret_cast<const DWORD*>(&slope));

    d9->SetVertexShaderConstantF(0, vp16, 4);
    d9->SetPixelShaderConstantF(0, vp16, 4);
    const float eyeT[4] = {eye[0], eye[1], eye[2], look.timeSeconds};
    const float viewport[4] = {static_cast<float>(vp.X), static_cast<float>(vp.Y), static_cast<float>(vp.Width),
                               static_cast<float>(vp.Height)};
    const float targetC[4] = {1.0f / desc.Width, 1.0f / desc.Height, look.rain, 0};
    const float fogColour[4] = {look.fogColour[0], look.fogColour[1], look.fogColour[2], 0};
    const float fog[4] = {look.fogStart, 1.0f / std::max(1.0f, look.fogEnd - look.fogStart), 0, 0};
    d9->SetPixelShaderConstantF(4, eyeT, 1);
    d9->SetPixelShaderConstantF(5, viewport, 1);
    d9->SetPixelShaderConstantF(6, targetC, 1);
    d9->SetPixelShaderConstantF(8, fogColour, 1);
    d9->SetPixelShaderConstantF(9, fog, 1);

    for (int i = 0; i < count; ++i) {
        const PuddlePart& p = parts[i];
        if (p.vertexCount <= 0 || p.indexCount < 3 || p.opacity <= 0.01f) continue;
        const float skyA[4] = {look.sky[0], look.sky[1], look.sky[2], p.opacity};
        d9->SetPixelShaderConstantF(7, skyA, 1);
        d9->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, p.vertexCount, p.indexCount / 3, p.indices, D3DFMT_INDEX16,
                                   p.vertices, 0x24);
    }
    saved->Apply();
    saved->Release();
#ifndef DS_DIST
    static bool logged = false;
    if (!logged) logged = true, dslog::Write("[dev] Water: reflections drawn (%d parts, target %ux%u format %d)", count,
                                           desc.Width, desc.Height, static_cast<int>(desc.Format));
#endif
    return true;
}
