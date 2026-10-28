#pragma once
// D3D8 shader bytecode -> D3D9 (token level, no D3DX). See shader8.cpp.
#include <windows.h>

#include <string>
#include <vector>

namespace render {

// D3D8 vertex declaration register (D3DVSDE_*) -> D3D9 usage / index (the fixed D3D8 mapping). False above v16.
bool UsageOfRegister(DWORD reg, BYTE& usage, BYTE& index);

// Converts one shader. `inputs` = the v registers the D3D8 declaration feeds (vertex shaders only; registers the code
// reads are declared too). Returns an empty vector for bytecode it doesn't understand; `note` gets a short summary
// of what was changed (or why it failed).
std::vector<DWORD> ConvertShader8(const DWORD* code, const std::vector<DWORD>& inputs, std::string& note);

// Number of DWORDs in D3D8 bytecode up to and including the end token (0 if none within `max`).
size_t ShaderLength(const DWORD* code, size_t max = 65536);

}  // namespace render

namespace render {
// Dev builds: runs D3D9 bytecode through d3d9.dll's shader validator and returns its messages ("" = valid or
// validator unavailable). Release builds return "".
std::string ValidateShader9(const std::vector<DWORD>& code);
}  // namespace render
