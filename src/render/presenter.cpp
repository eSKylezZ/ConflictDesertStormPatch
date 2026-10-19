// See presenter.h. D3D12 / DXGI are loaded at run time (d3d12.dll, dxgi.dll - Windows components); IIDs via __uuidof.
//
// Per frame: StretchRect(back buffer -> our A8R8G8B8 render-target texture) resolves MSAA and drops the X channel
// (B8G8R8X8 and the swap chain's B8G8R8A8 can't be copied into each other), UnwrapUnderlyingResource hands us the
// texture's ID3D12Resource on our queue (left in COMMON - implicitly promoted to COPY_SOURCE / PIXEL_SHADER_RESOURCE
// by its use), then either CopyResource into the swap chain's buffer (SDR) or the HDR pass (hdr.hlsl, full-screen
// triangle, 2 SRVs + 4 root constants), a fence signal that ReturnUnderlyingResource gives back to 9On12, Present.
// HDR is re-checked every 120 frames (setting, the monitor the window is on): a change re-creates the swap chain
// buffers in the other format / colour space.
#include "render/presenter.h"

#include <d3d12.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/settings.h"
#include "hdr_ps.h"
#include "hdr_ps_sdr.h"
#include "bloom_down.h"
#include "bloom_pre.h"
#include "bloom_up.h"
#include "hdr_vs.h"

namespace {
constexpr UINT kBuffers = 3;
constexpr UINT kMips = 6;  // bloom chain: 1/2 .. 1/64 size
constexpr UINT kSlots = 3;  // per-buffer SRVs: final frame, scene copy, bloom
constexpr float kBloomStrength[3] = {0.0f, 0.06f, 0.16f};
constexpr float kKnee = 0.75f;  // SDR luminance where the highlight expansion starts (lower lit up whole deserts)
constexpr float kWhiteShare = 0.6f;  // float target: full SDR white gets this share of the way to the peak

template <class T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

IDXGIFactory4* g_factory = nullptr;  // made with the D3D12 device; also used for the swap chain

LRESULT CALLBACK HiddenProc(HWND w, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcA(w, m, wp, lp); }

// ---- render watchdog (dev builds) ----
// The presenter marks each step that can wait (g_phase); a helper thread reports when the game thread has been in one
// step for 4 s, or hasn't presented for 8 s (e.g. stuck before Present): the step, the game thread's code addresses
// as module+offset (DXGI / D3D12 / 9On12 included) and a minidump next to the exe (DesertStormFix-hang.dmp).
volatile const char* g_phase = "idle";
volatile DWORD g_phaseSince = 0, g_lastPresent = 0;
DWORD g_renderThread = 0;

void Phase(const char* p) {
    g_phase = p;
    g_phaseSince = GetTickCount();
}

#ifndef DS_DIST
void Where(uint32_t a, char* out, size_t n) {
    HMODULE m = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(a)), &m) && m) {
        GetModuleFileNameA(m, path, MAX_PATH);
        const char* base = strrchr(path, '\\');
        snprintf(out, n, "%s+%X", base ? base + 1 : path, a - static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m)));
    } else {
        snprintf(out, n, "%08X", a);
    }
}

bool InCode(uint32_t a) {
    MEMORY_BASIC_INFORMATION mi;
    if (a < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(a)), &mi, sizeof mi)) return false;
    return mi.State == MEM_COMMIT && (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) && mi.Type == MEM_IMAGE;
}

void WriteDump(HANDLE thread) {
    using DumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
    auto dump = reinterpret_cast<DumpFn>(GetProcAddress(LoadLibraryA("dbghelp.dll"), "MiniDumpWriteDump"));
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) strcpy(slash + 1, "DesertStormFix-hang.dmp");
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (!dump || f == INVALID_HANDLE_VALUE) return;
    (void)thread;
    dump(GetCurrentProcess(), GetCurrentProcessId(), f, 0x2 /* MiniDumpWithFullMemory */ | 0x1000 /* ThreadInfo */, nullptr,
         nullptr, nullptr);
    CloseHandle(f);
    dslog::Write("Presenter watchdog: dump written to %s", path);
}

void ReportStuck(const char* why, DWORD ms, bool dump) {
    dslog::Write("Presenter watchdog: %s for %lu ms (step \"%s\")", why, ms, const_cast<const char*>(g_phase));
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_renderThread);
    if (!t) return;
    // Only copy while the thread is suspended - it may hold the log's or the loader's lock.
    static uint32_t stack[4096];
    int words = 0;
    uint32_t eip = 0;
    if (SuspendThread(t) != static_cast<DWORD>(-1)) {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(t, &c)) {
            eip = c.Eip;
            const uint32_t* sp = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(c.Esp));
            for (; words < 4096; ++words) {
                __try { stack[words] = sp[words]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            }
        }
        ResumeThread(t);
    }
    char w[96];
    Where(eip, w, sizeof w);
    dslog::Write("  eip %s", w);
    for (int i = 0, found = 0; i < words && found < 28; ++i) {
        if (!InCode(stack[i])) continue;
        Where(stack[i], w, sizeof w);
        dslog::Write("  stack +%03X %s", i * 4, w);
        ++found;
    }
    if (dump) WriteDump(t);
    CloseHandle(t);
}

DWORD WINAPI Watchdog(void*) {
    const char* reportedPhase = nullptr;
    DWORD reportedPresent = 0;
    HWND lastForeground = nullptr;
    for (;;) {
        Sleep(500);
        const DWORD now = GetTickCount();
        // Which window has the keyboard: log changes (keys went missing on the user's screen)
        if (HWND fg = GetForegroundWindow(); fg != lastForeground) {
            lastForeground = fg;
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            char cls[64] = "-";
            if (fg) GetClassNameA(fg, cls, sizeof cls);
            dslog::Write("Presenter watchdog: foreground window now %s (%s)", cls, pid == GetCurrentProcessId() ? "this game" : "another program");
        }
        const char* phase = const_cast<const char*>(g_phase);
        if (strcmp(phase, "idle") != 0 && now - g_phaseSince > 4000) {
            if (reportedPhase != phase) ReportStuck("game thread inside the presenter", now - g_phaseSince, true);
            reportedPhase = phase;
        } else {
            reportedPhase = nullptr;
        }
        const DWORD since = now - g_lastPresent;
        if (g_lastPresent && since > 8000 && reportedPresent != g_lastPresent) {
            ReportStuck("no frame presented", since, false);
            reportedPresent = g_lastPresent;
        }
    }
}
#endif

bool DevForceHdr() {
#ifndef DS_DIST
    DWORD v = 0, size = sizeof v;
    return RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "ForceHdr", RRF_RT_REG_DWORD, nullptr, &v,
                        &size) == ERROR_SUCCESS && v;
#else
    return false;
#endif
}
}  // namespace

struct render::Presenter::State {
    IDirect3DDevice9* d9 = nullptr;
    IDirect3DDevice9On12* on12 = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain3* swap = nullptr;
    ID3D12Resource* buffers[kBuffers] = {};
    ID3D12CommandAllocator* allocators[kBuffers] = {};
    UINT64 frameFence[kBuffers] = {};
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fenceValue = 0;
    HANDLE event = nullptr;
    IDirect3DTexture9* resolveTex = nullptr;
    IDirect3DSurface9* resolve = nullptr;
    UINT w = 0, h = 0;
    bool tearing = false;
    bool loggedFormat = false;
    UINT frames = 0;
    UINT refreshHz = 60;                  // the window's monitor (re-read every 120 frames)
    HANDLE latency = nullptr;             // waitable swap chain: at most one frame queued (input latency)

    // HDR
    bool hdr = false;  // swap chain is FP16 scRGB
    float peakNits = 1000.0f;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;     // HDR pass (FP16 swap chain)
    ID3D12PipelineState* psoSdr = nullptr;  // float frame / bloom -> B8G8R8A8 swap chain
    ID3D12PipelineState *psoPre = nullptr, *psoDown = nullptr, *psoUp = nullptr;  // bloom
    ID3D12Resource* mips[kMips] = {};  // bloom chain (R16G16B16A16_FLOAT, PIXEL_SHADER_RESOURCE between passes)
    UINT mipW[kMips] = {}, mipH[kMips] = {};
    UINT mipsFor = 0;                  // w * h the chain was made for
    ID3D12DescriptorHeap* srvHeap = nullptr;  // kSlots per buffer, then one per bloom level
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    UINT srvStep = 0, rtvStep = 0;
    IDirect3DTexture9* sceneTex = nullptr;
    IDirect3DSurface9* scene = nullptr;
    bool sceneThisFrame = false;
    UINT snapshots = 0, maxSnapshots = 0;

    std::string shotPath;  // dev: read the next frame back into this file
    int burstLeft = 0;                // dev "path|N": the brightest of the next N frames (FP16 only)
    float burstMax = -1.0f;
    std::vector<BYTE> burstBest;
    ID3D12Resource* readback = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};

    void WaitFor(UINT64 value) {
        if (fence->GetCompletedValue() >= value) return;
        fence->SetEventOnCompletion(value, event);
        WaitForSingleObject(event, 2000);
    }
    void WaitIdle() {
        queue->Signal(fence, ++fenceValue);
        WaitFor(fenceValue);
    }
    // HDR = HDR10 (10-bit, Rec.2020, PQ). An FP16 scRGB swap chain hung the GPU on explosions (NVIDIA, 9On12 sharing
    // our queue): 4 of 10 runs, 0 of 14 with 8- / 10-bit swap chains.
    DXGI_FORMAT Format() const { return hdr ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM; }
    UINT Flags() const {
        return DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    }
    // V-Sync: every n-th refresh so the frame limit (never above 240) is met - 180 Hz at 240 = 1, 60 fps cap on 180 Hz
    // = 3, 360 Hz = 2, 1000 Hz = 4 (DXGI's maximum; the frame cap does the rest).
    UINT SyncInterval(bool vsync) const {
        if (!vsync) return 0;
        const UINT cap = std::min<UINT>(settings::Get().fpsCap ? settings::Get().fpsCap : 240, 240);
        return std::clamp<UINT>((refreshHz + cap - 1) / cap, 1, 4);
    }
    bool GetBuffers() {
        for (UINT i = 0; i < kBuffers; ++i)
            if (FAILED(swap->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&buffers[i])))) return false;
        if (rtvHeap) GetRtvs();
        return true;
    }
    void GetRtvs() {
        for (UINT i = 0; i < kBuffers; ++i) device->CreateRenderTargetView(buffers[i], nullptr, Rtv(i));
        for (UINT i = 0; i < kBuffers; ++i)
            if (buffers[i]) buffers[i]->SetName(i == 0 ? L"DSFix swap buffer 0" : i == 1 ? L"DSFix swap buffer 1" : L"DSFix swap buffer 2");
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE at = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        at.ptr += i * rtvStep;
        return at;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Table(UINT slot) const {
        D3D12_GPU_DESCRIPTOR_HANDLE g = srvHeap->GetGPUDescriptorHandleForHeapStart();
        g.ptr += static_cast<UINT64>(slot) * srvStep;
        return g;
    }

    // Bloom chain at 1/2 .. 1/64 of the frame, re-made when the size changes.
    bool MakeMips() {
        if (mipsFor == w * h && mips[0]) return true;
        WaitIdle();
        for (auto& m : mips) SafeRelease(m);
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        for (UINT l = 0; l < kMips; ++l) {
            mipW[l] = std::max(1u, w >> (l + 1));
            mipH[l] = std::max(1u, h >> (l + 1));
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = mipW[l];
            d.Height = mipH[l];
            d.DepthOrArraySize = d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                       nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&mips[l]))))
                return false;
            device->CreateRenderTargetView(mips[l], nullptr, Rtv(kBuffers + l));
            mips[l]->SetName(L"DSFix bloom level");
            Srv(mips[l], kSlots * kBuffers + l);
        }
        mipsFor = w * h;
        dslog::Write("Presenter: bloom chain %ux%u .. %ux%u", mipW[0], mipH[0], mipW[kMips - 1], mipH[kMips - 1]);
        return true;
    }
    void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        list->ResourceBarrier(1, &b);
    }
    // One full-screen pass into bloom level `l`, reading the table at `slot` (t0) with texels of `tw` x `th`.
    void BloomPass(UINT l, ID3D12PipelineState* p, UINT slot, UINT tw, UINT th) {
        Barrier(mips[l], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float c[8] = {0, 0, 0, 0, 0, 0, 1.0f / tw, 1.0f / th};
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(kBuffers + l);
        D3D12_VIEWPORT vp{0, 0, static_cast<float>(mipW[l]), static_cast<float>(mipH[l]), 0, 1};
        D3D12_RECT sc{0, 0, static_cast<LONG>(mipW[l]), static_cast<LONG>(mipH[l])};
        list->SetPipelineState(p);
        list->SetGraphicsRootDescriptorTable(0, Table(slot));
        list->SetGraphicsRoot32BitConstants(1, 8, c, 0);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
        list->DrawInstanced(3, 1, 0, 0);
        Barrier(mips[l], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    void ReleaseBuffers() {
        for (auto& b : buffers) SafeRelease(b);
    }
    // A render-target texture for copies of the back buffer: A8R8G8B8, or A16B16G16R16F for the float target.
    bool MakeTexture(IDirect3DTexture9*& tex, IDirect3DSurface9*& surf, IDirect3DSurface9* like) {
        D3DSURFACE_DESC want{}, have{};
        like->GetDesc(&want);
        const D3DFORMAT format = want.Format == D3DFMT_A16B16G16R16F ? D3DFMT_A16B16G16R16F : D3DFMT_A8R8G8B8;
        if (surf && SUCCEEDED(surf->GetDesc(&have)) && have.Format == format) return true;
        SafeRelease(surf);
        SafeRelease(tex);
        if (FAILED(d9->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &tex, nullptr))) return false;
        return SUCCEEDED(tex->GetSurfaceLevel(0, &surf));
    }
    void SetColourSpace() {
#ifndef DS_DIST
        DWORD skip = 0, size = sizeof skip;   // dev test: HDR pass + format, but Windows not told it's HDR
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "R9NoColourSpace", RRF_RT_REG_DWORD, nullptr, &skip,
                         &size) == ERROR_SUCCESS && skip)
            return;
#endif
        UINT support = 0;
        const DXGI_COLOR_SPACE_TYPE cs = hdr ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        if (SUCCEEDED(swap->CheckColorSpaceSupport(cs, &support)) && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
            swap->SetColorSpace1(cs);
    }

    // The monitor the window is on: HDR mode on? Its peak brightness.
    bool OutputHdr(float* peak) {
        IDXGIOutput* out = nullptr;
        bool on = false;
        if (SUCCEEDED(swap->GetContainingOutput(&out))) {
            DXGI_OUTPUT_DESC od;
            DEVMODEW dm{};
            dm.dmSize = sizeof dm;
            if (SUCCEEDED(out->GetDesc(&od)) && EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm) &&
                dm.dmDisplayFrequency > 1)
                refreshHz = dm.dmDisplayFrequency;
            IDXGIOutput6* o6 = nullptr;
            if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&o6)))) {
                DXGI_OUTPUT_DESC1 d;
                if (SUCCEEDED(o6->GetDesc1(&d))) {
                    on = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                    if (peak && d.MaxLuminance > 100.0f) *peak = d.MaxLuminance;
                }
                o6->Release();
            }
            out->Release();
        }
        return on;
    }

    bool MakePipeline() {
        if (pso) return true;
        auto serialize = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
            GetProcAddress(GetModuleHandleA("d3d12.dll"), "D3D12SerializeRootSignature"));
        if (!serialize) return false;
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSlots, 0, 0, 0};
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = {1, &range};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = {0, 0, 8};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd{2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ID3DBlob *blob = nullptr, *err = nullptr;
        HRESULT hr = serialize(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (SUCCEEDED(hr))
            hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                             reinterpret_cast<void**>(&root));
        SafeRelease(blob);
        SafeRelease(err);
        if (FAILED(hr)) return false;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root;
        pd.VS = {g_hdrVs, sizeof g_hdrVs};
        pd.PS = {g_hdrPs, sizeof g_hdrPs};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;  // HDR10 swap chain
        pd.SampleDesc.Count = 1;
        hr = device->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&pso));
        if (SUCCEEDED(hr)) {
            pd.PS = {g_hdrPsSdr, sizeof g_hdrPsSdr};
            pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
            hr = device->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&psoSdr));
        }
        pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (SUCCEEDED(hr)) {
            pd.PS = {g_bloomPre, sizeof g_bloomPre};
            hr = device->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&psoPre));
        }
        if (SUCCEEDED(hr)) {
            pd.PS = {g_bloomDown, sizeof g_bloomDown};
            hr = device->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&psoDown));
        }
        if (SUCCEEDED(hr)) {  // upsample: added onto the larger level
            pd.PS = {g_bloomUp, sizeof g_bloomUp};
            D3D12_RENDER_TARGET_BLEND_DESC& bl = pd.BlendState.RenderTarget[0];
            bl.BlendEnable = TRUE;
            bl.SrcBlend = bl.DestBlend = bl.SrcBlendAlpha = bl.DestBlendAlpha = D3D12_BLEND_ONE;
            bl.BlendOp = bl.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            hr = device->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&psoUp));
        }
        if (FAILED(hr)) {
            dslog::Write("[fail] Presenter: HDR pipeline (0x%08X)", static_cast<unsigned>(hr));
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSlots * kBuffers + kMips + kSlots,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&srvHeap));
        hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBuffers + kMips, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&rtvHeap));
        srvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        rtvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        if (!srvHeap || !rtvHeap) return false;
        // Every slot starts as a valid null view (tables span 3 slots, not all of them always in use).
        D3D12_SHADER_RESOURCE_VIEW_DESC nd{};
        nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nd.Texture2D.MipLevels = 1;
        for (UINT i = 0; i < kSlots * kBuffers + kMips + kSlots; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE at = srvHeap->GetCPUDescriptorHandleForHeapStart();
            at.ptr += i * srvStep;
            device->CreateShaderResourceView(nullptr, &nd, at);
        }
        if (buffers[0]) GetRtvs();  // pipeline made after the swap chain's buffers (bloom switched on live)
        return true;
    }

    // Switches the swap chain between SDR and HDR when the setting / the monitor says so.
    void CheckHdr(bool force) {
        const settings::Values& v = settings::Get();
        float peak = 1000.0f;
        const bool displayHdr = OutputHdr(&peak);
        if (v.bloom) MakePipeline();
        const bool want = v.hdr && (displayHdr || DevForceHdr()) && MakePipeline();
        peakNits = v.hdrPeak ? static_cast<float>(v.hdrPeak) : peak;
        if (want == hdr && !force) return;
        WaitIdle();
        ReleaseBuffers();
        hdr = want;
        HRESULT hr = swap->ResizeBuffers(kBuffers, w, h, Format(), Flags());
        if (FAILED(hr)) {
            dslog::Write("[fail] Presenter: switching to %s (0x%08X)", hdr ? "HDR" : "SDR", static_cast<unsigned>(hr));
            hdr = false;
            swap->ResizeBuffers(kBuffers, w, h, Format(), Flags());
        }
        SetColourSpace();
        GetBuffers();
        for (auto& f : frameFence) f = 0;
        dslog::Write("Presenter: %s output (display HDR %s, peak %.0f nits, paper white %u nits, highlights %u%%, %u Hz)",
                     hdr ? "HDR10" : "SDR", displayHdr ? "on" : "off", peakNits, v.hdrPaperWhite, v.hdrHighlights,
                     refreshHz);
    }

    // The GPU dropped the device: why, and (dev builds, DRED) the unfinished command lists' last operations and the
    // faulting address's allocations.
    void ReportRemoved() {
        static bool reported = false;
        if (reported) return;
        reported = true;
        dslog::Write("[fail] Presenter: device removed - reason 0x%08X", static_cast<unsigned>(device->GetDeviceRemovedReason()));
#ifndef DS_DIST
        ID3D12DeviceRemovedExtendedData* dred = nullptr;
        if (FAILED(device->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData), reinterpret_cast<void**>(&dred)))) return;
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs))) {
            int lists = 0;
            for (const D3D12_AUTO_BREADCRUMB_NODE* n = crumbs.pHeadAutoBreadcrumbNode; n && lists < 12; n = n->pNext) {
                const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
                if (done >= n->BreadcrumbCount) continue;  // finished lists are not interesting
                ++lists;
                char name[128] = "?";
                if (n->pCommandListDebugNameW) WideCharToMultiByte(CP_UTF8, 0, n->pCommandListDebugNameW, -1, name, sizeof name, nullptr, nullptr);
                else if (n->pCommandListDebugNameA) snprintf(name, sizeof name, "%s", n->pCommandListDebugNameA);
                char ops[512] = "";
                int len = 0;
                for (UINT k = done >= 4 ? done - 4 : 0; k < n->BreadcrumbCount && k < done + 4; ++k)
                    len += snprintf(ops + len, sizeof ops - len, "%s%d", k == done ? " [>" : " ", static_cast<int>(n->pCommandHistory[k])),
                    len += k == done ? snprintf(ops + len, sizeof ops - len, "<]") : 0;
                dslog::Write("  DRED list \"%s\": %u of %u ops done; ops around the stop (D3D12_AUTO_BREADCRUMB_OP):%s", name,
                             done, n->BreadcrumbCount, ops);
            }
            if (!lists) dslog::Write("  DRED: every recorded command list had finished");
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)) && fault.PageFaultVA) {
            dslog::Write("  DRED page fault at GPU address 0x%llX", fault.PageFaultVA);
            auto names = [](const D3D12_DRED_ALLOCATION_NODE* n, const char* kind) {
                for (int k = 0; n && k < 10; n = n->pNext, ++k) {
                    char name[128] = "(unnamed)";
                    if (n->ObjectNameW) WideCharToMultiByte(CP_UTF8, 0, n->ObjectNameW, -1, name, sizeof name, nullptr, nullptr);
                    else if (n->ObjectNameA) snprintf(name, sizeof name, "%s", n->ObjectNameA);
                    dslog::Write("    %s allocation \"%s\" type %d", kind, name, static_cast<int>(n->AllocationType));
                }
            };
            names(fault.pHeadExistingAllocationNode, "existing");
            names(fault.pHeadRecentFreedAllocationNode, "recently freed");
        } else {
            dslog::Write("  DRED: no page fault recorded (a hang / timeout instead)");
        }
        dred->Release();
#endif
    }

    void Srv(ID3D12Resource* r, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = Float(r) ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE at = srvHeap->GetCPUDescriptorHandleForHeapStart();
        at.ptr += slot * srvStep;
        device->CreateShaderResourceView(r, &sd, at);
    }

    static bool Float(ID3D12Resource* r) {
        const DXGI_FORMAT f = r->GetDesc().Format;
        return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R16G16B16A16_TYPELESS;
    }

    void CopyForShot(ID3D12Resource* src) {
        D3D12_RESOURCE_DESC d = src->GetDesc();
        UINT64 total = 0;
        device->GetCopyableFootprints(&d, 0, 1, 0, &footprint, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   __uuidof(ID3D12Resource), reinterpret_cast<void**>(&readback))))
            return;
        D3D12_TEXTURE_COPY_LOCATION dst{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, from{src};
        dst.PlacedFootprint = footprint;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
    }
    // After the frame's fence. 8-bit images as .bmp; FP16 (HDR) raw: u32 width, u32 height, then rows of 4 halves.
    static float Half(uint16_t h) {
        const uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF;
        if (e == 0 || e == 31) return 0.0f;  // denormals / inf / nan don't matter for "brightest"
        uint32_t bits = ((h & 0x8000u) << 16) | ((e + 112) << 23) | (m << 13);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }
    void WriteShot() {
        WaitFor(fenceValue);
        BYTE* p = nullptr;
        const UINT fw = footprint.Footprint.Width, fh = footprint.Footprint.Height;
        const bool half = footprint.Footprint.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        const bool pq = footprint.Footprint.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
                        footprint.Footprint.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS;   // HDR10
        const UINT bpp = half ? 8 : 4;
        if (burstLeft > 0 && (half || pq) && SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&p)))) {
            float m = 0.0f;  // brightest luminance (scRGB units: 1 = 80 nits), every 2nd pixel / row
            for (UINT y = 0; y < fh; y += 2) {
                const BYTE* row = p + footprint.Offset + y * footprint.Footprint.RowPitch;
                for (UINT x = 0; x < fw; x += 2) {
                    if (half) {
                        const uint16_t* c = reinterpret_cast<const uint16_t*>(row) + x * 4;
                        m = std::max(m, 0.2126f * Half(c[0]) + 0.7152f * Half(c[1]) + 0.0722f * Half(c[2]));
                    } else {
                        const uint32_t v = reinterpret_cast<const uint32_t*>(row)[x];
                        static float lut[1024];  // PQ (ST 2084) code -> nits
                        if (lut[1023] == 0.0f)
                            for (int k = 0; k < 1024; ++k) {
                                const float e = std::pow(k / 1023.0f, 1.0f / 78.84375f);
                                lut[k] = 10000.0f * std::pow(std::max(e - 0.8359375f, 0.0f) / (18.8515625f - 18.6875f * e),
                                                             1.0f / 0.1593017578125f);
                            }
                        auto nits = [](uint32_t code) { return lut[code]; };
                        const float y2020 = 0.2627f * nits(v & 1023) + 0.6780f * nits((v >> 10) & 1023) + 0.0593f * nits((v >> 20) & 1023);
                        m = std::max(m, y2020 / 80.0f);
                    }
                }
            }
            if (m > burstMax) {
                burstMax = m;
                burstBest.resize(static_cast<size_t>(fw) * fh * bpp);
                for (UINT y = 0; y < fh; ++y)
                    memcpy(&burstBest[static_cast<size_t>(y) * fw * bpp], p + footprint.Offset + y * footprint.Footprint.RowPitch, fw * bpp);
            }
            readback->Unmap(0, nullptr);
            SafeRelease(readback);
            if (--burstLeft > 0) return;
            if (FILE* f = fopen(shotPath.c_str(), "wb")) {
                const UINT tag = fw | (pq ? 0x80000000u : 0);   // top bit: HDR10 (u32 R10G10B10A2 PQ) instead of FP16
                fwrite(&tag, 4, 1, f);
                fwrite(&fh, 4, 1, f);
                fwrite(burstBest.data(), 1, burstBest.size(), f);
                fclose(f);
            }
            dslog::Write("Presenter: burst shot - brightest frame %.0f nits", burstMax * 80.0f);
            burstBest.clear();
            burstMax = -1.0f;
            shotPath.clear();
            return;
        }
        if (SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&p)))) {
            if (FILE* f = fopen(shotPath.c_str(), "wb")) {
                if (half) {
                    fwrite(&fw, 4, 1, f);
                    fwrite(&fh, 4, 1, f);
                } else {
                    BITMAPINFOHEADER bih{sizeof bih, static_cast<LONG>(fw), -static_cast<LONG>(fh), 1, 32};
                    BITMAPFILEHEADER bfh{0x4D42, 0, 0, 0, sizeof bfh + sizeof bih};
                    bfh.bfSize = bfh.bfOffBits + fw * fh * 4;
                    fwrite(&bfh, sizeof bfh, 1, f);
                    fwrite(&bih, sizeof bih, 1, f);
                }
                for (UINT y = 0; y < fh; ++y)
                    fwrite(p + footprint.Offset + y * footprint.Footprint.RowPitch, half ? 8 : 4, fw, f);
                fclose(f);
            }
            readback->Unmap(0, nullptr);
        }
        SafeRelease(readback);
        shotPath.clear();
    }
};

bool render::CreateD3D12(IUnknown** device, IUnknown** queue) {
    *device = *queue = nullptr;
    auto createDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(LoadLibraryA("d3d12.dll"), "D3D12CreateDevice"));
    auto createFactory = reinterpret_cast<HRESULT(WINAPI*)(UINT, REFIID, void**)>(
        GetProcAddress(LoadLibraryA("dxgi.dll"), "CreateDXGIFactory2"));
    if (!createDevice || !createFactory) return false;
    if (!g_factory && FAILED(createFactory(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&g_factory)))) return false;
#ifndef DS_DIST
    // Dev: Device Removed Extended Data - which GPU operation / address faulted when the device is removed.
    auto debugIface = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(GetModuleHandleA("d3d12.dll"),
                                                                                    "D3D12GetDebugInterface"));
    ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
    if (debugIface && SUCCEEDED(debugIface(__uuidof(ID3D12DeviceRemovedExtendedDataSettings), reinterpret_cast<void**>(&dred)))) {
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->Release();
        dslog::Write("Presenter: DRED on (dev build)");
    }
#endif
    IDXGIAdapter1* adapter = nullptr;  // D3D9 adapter 0 = the primary = DXGI adapter 0
    g_factory->EnumAdapters1(0, &adapter);
    ID3D12Device* dev = nullptr;
    HRESULT hr = createDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&dev));
    SafeRelease(adapter);
    if (FAILED(hr)) {
        dslog::Write("[fail] Presenter: D3D12CreateDevice 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr;
    if (FAILED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&q)))) {
        dev->Release();
        return false;
    }
    *device = dev;
    *queue = q;
    return true;
}

HWND render::HiddenDeviceWindow() {
    static HWND wnd = nullptr;
    if (wnd) return wnd;
    WNDCLASSA wc{};
    wc.lpfnWndProc = &HiddenProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "DesertStormFixD3D9";
    RegisterClassA(&wc);
    wnd = CreateWindowExA(WS_EX_TOOLWINDOW, wc.lpszClassName, "", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    return wnd;
}

render::Presenter* render::Presenter::Create(IDirect3DDevice9* d9, IUnknown* queueUnk, HWND window, UINT w, UINT h) {
    auto* s = new State;
    s->d9 = d9;
    s->w = w;
    s->h = h;
    auto fail = [&](const char* what, HRESULT hr) {
        dslog::Write("[fail] Presenter: %s (0x%08X) - D3D9 presents instead", what, static_cast<unsigned>(hr));
        Presenter tmp;
        tmp.s_ = s;  // the destructor releases what was made
        return static_cast<Presenter*>(nullptr);
    };
    HRESULT hr = d9->QueryInterface(__uuidof(IDirect3DDevice9On12), reinterpret_cast<void**>(&s->on12));
    if (FAILED(hr)) return fail("not a 9On12 device", hr);
    hr = queueUnk->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&s->queue));
    if (FAILED(hr)) return fail("queue", hr);
    s->queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&s->device));

    BOOL tearing = FALSE;
    IDXGIFactory5* f5 = nullptr;
    if (g_factory && SUCCEEDED(g_factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&f5)))) {
        f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
        f5->Release();
    }
    s->tearing = tearing != FALSE;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = w;
    sd.Height = h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBuffers;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = s->Flags();
    IDXGISwapChain1* sc1 = nullptr;
    hr = g_factory ? g_factory->CreateSwapChainForHwnd(s->queue, window, &sd, nullptr, nullptr, &sc1) : E_FAIL;
    if (FAILED(hr)) return fail("CreateSwapChainForHwnd", hr);
    sc1->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&s->swap));
    sc1->Release();
    g_factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    IDXGISwapChain2* sc2 = nullptr;
    if (s->swap && SUCCEEDED(s->swap->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&sc2)))) {
        sc2->SetMaximumFrameLatency(1);
        s->latency = sc2->GetFrameLatencyWaitableObject();
        sc2->Release();
    }
    if (!s->swap || !s->GetBuffers()) return fail("swap chain buffers", E_FAIL);

    for (auto& a : s->allocators)
        if (FAILED(hr = s->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                                         reinterpret_cast<void**>(&a))))
            return fail("command allocator", hr);
    hr = s->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s->allocators[0], nullptr,
                                      __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&s->list));
    if (FAILED(hr)) return fail("command list", hr);
    s->list->Close();
    s->list->SetName(L"DesertStormFix presenter");
    s->queue->SetName(L"DesertStormFix queue (9On12 + presenter)");
    hr = s->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&s->fence));
    if (FAILED(hr)) return fail("fence", hr);
    s->event = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    auto* p = new Presenter;
    p->s_ = s;
    g_renderThread = GetCurrentThreadId();
#ifndef DS_DIST
    static bool watching = false;
    if (!watching) {
        watching = true;
        CloseHandle(CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr));
    }
#endif
    dslog::Write("Presenter: DXGI flip swap chain %ux%u, %u buffers, tearing %s", w, h, kBuffers, s->tearing ? "yes" : "no");
    s->CheckHdr(true);  // HDR or not, refresh rate, latency-waitable buffers
    return p;
}

render::Presenter::~Presenter() {
    if (!s_) return;
    if (s_->queue && s_->fence) s_->WaitIdle();
    SafeRelease(s_->resolve);
    SafeRelease(s_->resolveTex);
    SafeRelease(s_->scene);
    SafeRelease(s_->sceneTex);
    s_->ReleaseBuffers();
    for (auto& a : s_->allocators) SafeRelease(a);
    SafeRelease(s_->list);
    SafeRelease(s_->fence);
    SafeRelease(s_->pso);
    SafeRelease(s_->psoSdr);
    SafeRelease(s_->psoPre);
    SafeRelease(s_->psoDown);
    SafeRelease(s_->psoUp);
    for (auto& m : s_->mips) SafeRelease(m);
    SafeRelease(s_->root);
    SafeRelease(s_->srvHeap);
    SafeRelease(s_->rtvHeap);
    SafeRelease(s_->swap);
    SafeRelease(s_->queue);
    SafeRelease(s_->device);
    SafeRelease(s_->on12);
    if (s_->event) CloseHandle(s_->event);
    if (s_->latency) CloseHandle(s_->latency);
    delete s_;
}

bool render::Presenter::Tearing() const { return s_->tearing; }
void render::Presenter::RequestShot(const char* path) {
    s_->shotPath = path;
    s_->burstLeft = 0;
    const size_t bar = s_->shotPath.find('|');
    if (bar != std::string::npos) {
        s_->burstLeft = std::max(1, atoi(s_->shotPath.c_str() + bar + 1));
        s_->shotPath.resize(bar);
    }
}
bool render::Presenter::WantsScene() const { return s_->hdr || (settings::Get().bloom && s_->psoPre); }
bool render::Presenter::WantsFloatScene() const { return s_->hdr && s_->pso; }

void render::Presenter::SnapshotScene(IDirect3DSurface9* backBuffer, const RECT& r) {
    State& s = *s_;
    if (!WantsScene() || !backBuffer || !s.MakeTexture(s.sceneTex, s.scene, backBuffer)) return;
    s.d9->StretchRect(backBuffer, &r, s.scene, &r, D3DTEXF_NONE);
    s.sceneThisFrame = true;
    ++s.snapshots;
}

void render::Presenter::BeforeReset() {
    SafeRelease(s_->resolve);
    SafeRelease(s_->resolveTex);
    SafeRelease(s_->scene);
    SafeRelease(s_->sceneTex);
}

void render::Presenter::AfterReset(UINT w, UINT h) {
    if (w == s_->w && h == s_->h) return;
    s_->w = w;
    s_->h = h;
    s_->CheckHdr(true);  // re-creates the buffers at the new size
    dslog::Write("Presenter: swap chain now %ux%u", w, h);
}

HRESULT render::Presenter::Present(IDirect3DSurface9* backBuffer, bool vsync, bool loading) {
    State& s = *s_;
    // Loading screens: V-Sync on the game's ~1000 progress presents turned a 0.6 s Mission 1 load into 6 s (and Windows
    // then ghosted the window) - they present without waiting. None is skipped: the last one (full bar + PRESS ANY
    // KEY) stays on screen while the game waits for a key without presenting.
    if (loading) vsync = false;
    struct PhaseEnd {
        ~PhaseEnd() {
            Phase("idle");
            g_lastPresent = GetTickCount();
        }
    } phaseEnd;
    Phase("HDR check");
    if (++s.frames % 120 == 0) s.CheckHdr(false);
    Phase("resolve");
    if (!backBuffer || !s.MakeTexture(s.resolveTex, s.resolve, backBuffer)) return D3D_OK;
    s.d9->StretchRect(backBuffer, nullptr, s.resolve, nullptr, D3DTEXF_NONE);
    Phase("unwrap");

    ID3D12Resource *src = nullptr, *sceneRes = nullptr;
    HRESULT hr = s.on12->UnwrapUnderlyingResource(s.resolveTex, s.queue, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&src));
    if (FAILED(hr) || !src) {
        dslog::Write("[fail] Presenter: UnwrapUnderlyingResource 0x%08X", static_cast<unsigned>(hr));
        return D3D_OK;
    }
    const bool hdr = s.hdr && s.pso;
    const UINT bloomLevel = std::min<UINT>(settings::Get().bloom, 2);
    const bool bloomOn = bloomLevel && s.psoPre && s.MakeMips();
    const bool floatSrc = State::Float(src);
    ID3D12PipelineState* pass = hdr ? s.pso : (floatSrc || bloomOn) ? s.psoSdr : nullptr;
    const bool haveScene = (hdr || bloomOn) && s.sceneThisFrame && s.sceneTex &&
                           SUCCEEDED(s.on12->UnwrapUnderlyingResource(s.sceneTex, s.queue, __uuidof(ID3D12Resource),
                                                                      reinterpret_cast<void**>(&sceneRes)));
    if (!s.loggedFormat) {
        D3D12_RESOURCE_DESC d = src->GetDesc();
        dslog::Write("Presenter: game image = D3D12 resource %llux%u, format %u", d.Width, d.Height, d.Format);
        s.loggedFormat = true;
    }
    s.maxSnapshots = std::max(s.maxSnapshots, s.snapshots);
    if (hdr && s.frames % 1200 == 0) dslog::Write("Presenter: up to %u scene copies per frame", s.maxSnapshots);

    const UINT i = s.swap->GetCurrentBackBufferIndex();
    Phase("wait for the buffer's last frame");
    s.WaitFor(s.frameFence[i]);
    Phase("record");
    s.allocators[i]->Reset();
    s.list->Reset(s.allocators[i], nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = s.buffers[i];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    if (!pass) {
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        s.list->ResourceBarrier(1, &b);
        s.list->CopyResource(s.buffers[i], src);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    } else {
        s.Srv(src, i * kSlots);
        s.Srv(haveScene ? sceneRes : src, i * kSlots + 1);
        s.list->SetGraphicsRootSignature(s.root);
        s.list->SetDescriptorHeaps(1, &s.srvHeap);
        s.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (bloomOn) {  // scene (t0 = slot +1) -> half size -> down the chain -> back up, added
            s.BloomPass(0, s.psoPre, i * kSlots + 1, s.w, s.h);
            for (UINT l = 1; l < kMips; ++l) s.BloomPass(l, s.psoDown, kSlots * kBuffers + l - 1, s.mipW[l - 1], s.mipH[l - 1]);
            for (UINT l = kMips - 1; l > 0; --l) s.BloomPass(l - 1, s.psoUp, kSlots * kBuffers + l, s.mipW[l], s.mipH[l]);
            s.Srv(s.mips[0], i * kSlots + 2);
        }
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        s.list->ResourceBarrier(1, &b);
        const settings::Values& v = settings::Get();
        // Paper white never above the display's peak (a 300-nit monitor with paper white 400 clipped every bright
        // part - and a white crosshair over sand - to the same white).
        const float paper = std::min(static_cast<float>(v.hdrPaperWhite), std::max(s.peakNits, 80.0f));
        const float peak = paper + (std::max(s.peakNits, paper) - paper) * static_cast<float>(v.hdrHighlights) / 100.0f;
        const float white = floatSrc ? paper + (peak - paper) * kWhiteShare : peak;
        const float constants[8] = {paper, peak, kKnee, haveScene ? 1.0f : 0.0f, white,
                                    bloomOn ? kBloomStrength[bloomLevel] : 0.0f, 1.0f / s.w, 1.0f / s.h};
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = s.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += i * s.rtvStep;
        D3D12_GPU_DESCRIPTOR_HANDLE table = s.Table(i * kSlots);
        D3D12_VIEWPORT vp{0, 0, static_cast<float>(s.w), static_cast<float>(s.h), 0, 1};
        D3D12_RECT sc{0, 0, static_cast<LONG>(s.w), static_cast<LONG>(s.h)};
        s.list->SetPipelineState(pass);
        s.list->SetGraphicsRootDescriptorTable(0, table);
        s.list->SetGraphicsRoot32BitConstants(1, 8, constants, 0);
        s.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        s.list->RSSetViewports(1, &vp);
        s.list->RSSetScissorRects(1, &sc);
        s.list->DrawInstanced(3, 1, 0, 0);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (!s.shotPath.empty()) {  // the presented buffer itself (HDR: FP16)
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        s.list->ResourceBarrier(1, &b);
        s.CopyForShot(s.buffers[i]);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    s.list->ResourceBarrier(1, &b);
    s.list->Close();
    ID3D12CommandList* lists[] = {s.list};
    Phase("execute");
    s.queue->ExecuteCommandLists(1, lists);
    s.queue->Signal(s.fence, ++s.fenceValue);
    s.frameFence[i] = s.fenceValue;
    UINT64 value = s.fenceValue;
    ID3D12Fence* fences[] = {s.fence};
    Phase("return to 9On12");
    s.on12->ReturnUnderlyingResource(s.resolveTex, 1, &value, fences);
    src->Release();
    if (sceneRes) {
        s.on12->ReturnUnderlyingResource(s.sceneTex, 1, &value, fences);
        sceneRes->Release();
    }
    s.sceneThisFrame = false;
    s.snapshots = 0;
    if (s.readback) s.WriteShot();

    const UINT interval = s.SyncInterval(vsync);
    UINT flags = interval == 0 && s.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    Phase("DXGI Present");
    hr = s.swap->Present(interval, flags);
    // The game starts its next frame right after this: wait until the swap chain can take it, so no more than one
    // frame waits in the queue (otherwise V-Sync adds up to 3 frames of input lag).
    Phase("frame latency wait");
    if (!loading && s.latency && WaitForSingleObjectEx(s.latency, 1000, TRUE) == WAIT_TIMEOUT) {
        static int logged = 0;
        if (logged++ < 5) dslog::Write("[fail] Presenter: frame latency wait timed out (1 s)");
    }
    if (FAILED(hr)) {
        static int logged = 0;
        if (logged++ < 5) dslog::Write("[fail] Presenter: Present 0x%08X", static_cast<unsigned>(hr));
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            s.ReportRemoved();
            return D3DERR_DEVICELOST;
        }
    }
    return D3D_OK;
}
