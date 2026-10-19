// D3D8 shader bytecode -> D3D9 bytecode, token by token (d3d8to9 does it through D3DX disassembly / reassembly,
// which needs d3dx9_43.dll - not a Windows component).
//
// The instruction set and token encoding of vs/ps 1.x are the same in both versions; what differs:
//   - vs.1.0 / ps.1.0 don't exist in D3D9 -> 1.1 (the game's ps pair is instruction-identical to DS2's ps_1_1).
//   - D3D9 vertex shaders declare their inputs (dcl_<usage> vN); D3D8 bound registers through the declaration
//     passed to CreateVertexShader. A dcl is emitted for every register the declaration feeds or the code reads,
//     with the usage the vertex declaration (d3d8to9.cpp) gives the same register.
//   - the scalar outputs oFog / oPts need a full write mask in D3D9 (D3D8 code writes oFog.x).
//   - scalar instructions (rcp, rsq, exp, log, expp, logp) used src.w when the source had no swizzle; D3D9
//     requires an explicit replicate swizzle.
#include "render/shader8.h"

#include <d3d9.h>

#include <algorithm>
#include <cstdio>

namespace {
constexpr DWORD kEnd = 0x0000FFFF, kComment = 0xFFFE, kDef = 81, kDcl = 31;
constexpr DWORD kRegInput = 1, kRegAddress = 3, kRegRastOut = 4;  // rastout: 0 oPos, 1 oFog, 2 oPts
constexpr DWORD kIdentitySwizzle = 0xE4, kReplicateW = 0xFF;

bool IsParam(DWORD t) { return (t & 0x80000000) != 0; }
DWORD RegType(DWORD t) { return ((t >> 28) & 7) | ((t >> 8) & 0x18); }
DWORD RegNumber(DWORD t) { return t & 0x7FF; }
bool IsScalarOp(DWORD op) { return op == 6 || op == 7 || op == 14 || op == 15 || op == 78 || op == 79; }
}  // namespace

bool render::UsageOfRegister(DWORD reg, BYTE& usage, BYTE& index) {
    static const BYTE kUsage[17][2] = {
        {D3DDECLUSAGE_POSITION, 0}, {D3DDECLUSAGE_BLENDWEIGHT, 0}, {D3DDECLUSAGE_BLENDINDICES, 0},
        {D3DDECLUSAGE_NORMAL, 0},   {D3DDECLUSAGE_PSIZE, 0},       {D3DDECLUSAGE_COLOR, 0},
        {D3DDECLUSAGE_COLOR, 1},    {D3DDECLUSAGE_TEXCOORD, 0},    {D3DDECLUSAGE_TEXCOORD, 1},
        {D3DDECLUSAGE_TEXCOORD, 2}, {D3DDECLUSAGE_TEXCOORD, 3},    {D3DDECLUSAGE_TEXCOORD, 4},
        {D3DDECLUSAGE_TEXCOORD, 5}, {D3DDECLUSAGE_TEXCOORD, 6},    {D3DDECLUSAGE_TEXCOORD, 7},
        {D3DDECLUSAGE_POSITION, 1}, {D3DDECLUSAGE_NORMAL, 1},
    };
    if (reg >= 17) return false;
    usage = kUsage[reg][0];
    index = kUsage[reg][1];
    return true;
}

size_t render::ShaderLength(const DWORD* code, size_t max) {
    if (!code) return 0;
    for (size_t i = 1; i < max;) {
        DWORD t = code[i];
        if (t == kEnd) return i + 1;
        DWORD op = t & 0xFFFF;
        if (op == kComment) {
            i += 1 + ((t >> 16) & 0x7FFF);
        } else if (op == kDef) {
            i += 6;  // dest + 4 raw floats (their top bit is arbitrary)
        } else {
            ++i;
            while (i < max && IsParam(code[i])) ++i;
        }
    }
    return 0;
}

std::vector<DWORD> render::ConvertShader8(const DWORD* code, const std::vector<DWORD>& inputs, std::string& note) {
    std::vector<DWORD> out;
    size_t n = ShaderLength(code);
    if (n < 2) {
        note = "no end token";
        return out;
    }
    DWORD version = code[0];
    bool vs = (version & 0xFFFF0000) == 0xFFFE0000, ps = (version & 0xFFFF0000) == 0xFFFF0000;
    if ((!vs && !ps) || (version & 0xFF00) != 0x0100) {
        char b[48];
        snprintf(b, sizeof b, "unsupported version 0x%08lX", version);
        note = b;
        return out;
    }
    if ((version & 0xFF) == 0) version |= 1;  // 1.0 -> 1.1

    // Pass 1: which input registers the code reads; the address register (vs.1.0 rounded a0 differently).
    std::vector<DWORD> regs = vs ? inputs : std::vector<DWORD>{};
    bool usesA0 = false;
    for (size_t i = 1; i + 1 < n;) {
        DWORD op = code[i] & 0xFFFF;
        if (op == kComment) { i += 1 + ((code[i] >> 16) & 0x7FFF); continue; }
        if (op == kDef) { i += 6; continue; }
        for (++i; i < n && IsParam(code[i]); ++i) {
            if (RegType(code[i]) == kRegInput && vs) regs.push_back(RegNumber(code[i]));
            if (RegType(code[i]) == kRegAddress && vs) usesA0 = true;  // (in ps, type 3 = t#)
        }
    }
    std::sort(regs.begin(), regs.end());
    regs.erase(std::unique(regs.begin(), regs.end()), regs.end());

    out.reserve(n + regs.size() * 3);
    out.push_back(version);
    int dcls = 0, swizzles = 0, masks = 0;
    for (DWORD r : regs) {
        BYTE usage, index;
        if (!UsageOfRegister(r, usage, index)) continue;
        out.push_back(kDcl);
        out.push_back(0x80000000 | usage | (static_cast<DWORD>(index) << 16));
        out.push_back(0x80000000 | (kRegInput << 28) | (0xF << 16) | r);
        ++dcls;
    }
    // Pass 2: copy, fixing scalar sources.
    for (size_t i = 1; i < n;) {
        DWORD t = code[i];
        DWORD op = t & 0xFFFF;
        if (t == kEnd) { out.push_back(t); break; }
        if (op == kComment) {
            size_t len = 1 + ((t >> 16) & 0x7FFF);
            out.insert(out.end(), code + i, code + i + len);
            i += len;
            continue;
        }
        if (op == kDef) {
            out.insert(out.end(), code + i, code + i + 6);
            i += 6;
            continue;
        }
        out.push_back(t);
        int param = 0;
        for (++i; i < n && IsParam(code[i]); ++i, ++param) {
            DWORD p = code[i];
            if (vs && param == 0 && RegType(p) == kRegRastOut && RegNumber(p) != 0 && ((p >> 16) & 0xF) != 0xF) {
                p |= 0xF << 16;
                ++masks;
            }
            if (vs && IsScalarOp(op) && param == 1 && ((p >> 16) & 0xFF) == kIdentitySwizzle) {
                p = (p & ~0x00FF0000u) | (kReplicateW << 16);
                ++swizzles;
            }
            out.push_back(p);
        }
    }
    char b[128];
    snprintf(b, sizeof b, "%s %lu.%lu, %d dcl, %d scalar swizzle(s), %d fog/psize mask(s)%s", vs ? "vs" : "ps",
             (version >> 8) & 0xFF, version & 0xFF, dcls, swizzles, masks, usesA0 ? ", uses a0" : "");
    note = b;
    return out;
}

#ifndef DS_DIST
namespace {
using ValidatorCallback = HRESULT(WINAPI*)(const char* file, int line, DWORD_PTR, DWORD_PTR id, const char* message,
                                           void* context);
struct ShaderValidator9 {  // IDirect3DShaderValidator9 (undocumented; layout as in Wine)
    virtual HRESULT __stdcall QueryInterface(REFIID, void**) = 0;
    virtual ULONG __stdcall AddRef() = 0;
    virtual ULONG __stdcall Release() = 0;
    virtual HRESULT __stdcall Begin(ValidatorCallback cb, void* context, DWORD_PTR) = 0;
    virtual HRESULT __stdcall Instruction(const char* file, int line, const DWORD* tokens, DWORD count) = 0;
    virtual HRESULT __stdcall End() = 0;
};
HRESULT WINAPI Collect(const char*, int line, DWORD_PTR, DWORD_PTR id, const char* message, void* context) {
    char b[48];
    snprintf(b, sizeof b, "[instr %d, 0x%lX] ", line, static_cast<unsigned long>(id));
    *static_cast<std::string*>(context) += std::string(b) + (message ? message : "?") + " ";
    return S_OK;
}
}  // namespace

std::string render::ValidateShader9(const std::vector<DWORD>& code) {
    static auto create = reinterpret_cast<ShaderValidator9*(WINAPI*)()>(
        GetProcAddress(LoadLibraryA("d3d9.dll"), "Direct3DShaderValidatorCreate9"));
    ShaderValidator9* v = create ? create() : nullptr;
    if (!v || code.empty()) return "";
    std::string messages;
    v->Begin(&Collect, &messages, 0);
    v->Instruction(nullptr, 0, code.data(), 1);  // version token
    int line = 1;
    for (size_t i = 1; i < code.size();) {
        DWORD t = code[i], op = t & 0xFFFF;
        size_t len;
        if (t == kEnd) {
            v->Instruction(nullptr, line, code.data() + i, 1);
            break;
        }
        if (op == kComment) len = 1 + ((t >> 16) & 0x7FFF);
        else if (op == kDef) len = 6;
        else for (len = 1; i + len < code.size() && IsParam(code[i + len]); ++len) {}
        if (op != kComment) v->Instruction(nullptr, line++, code.data() + i, static_cast<DWORD>(len));
        i += len;
    }
    v->End();
    v->Release();
    return messages;
}
#else
std::string render::ValidateShader9(const std::vector<DWORD>&) { return ""; }
#endif
