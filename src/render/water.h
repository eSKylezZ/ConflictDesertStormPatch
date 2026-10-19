#pragma once
// Puddle reflections for the D3D9 renderers (1-3) - water.cpp / water.hlsl. Renderer 0 (the system's D3D8) has no
// shaders for it: puddles there stay the plain dark decals.
#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace render {
// One piece of a puddle decal: the game's own decal geometry (world space, 0x24-byte vertices = FVF 0x152: position,
// normal, colour, uv) - the reflection lies exactly on the ground the decal was projected onto.
struct PuddlePart {
    const void* vertices;
    int vertexCount;
    const uint16_t* indices;
    int indexCount;
    float opacity;     // 0..1 (drying)
};
struct WaterLook {
    float sky[3];          // reflected where the mirrored point is off the view
    float fogColour[3];
    float fogStart, fogEnd;   // cm
    float timeSeconds, rain;  // ripples
    void* mask;               // the puddle picture (the game's D3D8 texture; alpha = water)
};

// dev8 = the game's D3D8 device. Draws the puddles' reflections into the current view (after the world and the
// transparents, before the HUD). False when the device isn't our D3D9 one (Renderer 0) or the shaders failed.
bool DrawPuddleReflections(void* dev8, const PuddlePart* parts, int count, const WaterLook& look);

// d3d8to9.cpp: the D3D9 device behind the game's D3D8 one (nullptr on Renderer 0), a D3D8 texture's D3D9 one, and the
// device reset.
IDirect3DDevice9* Device9For(void* dev8);
IDirect3DTexture9* Texture9For(void* tex8);
void WaterBeforeReset();
}  // namespace render
