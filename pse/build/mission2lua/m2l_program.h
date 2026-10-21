#pragma once
// mission2lua (C++ port): the DLL as the Python generator sees it (docs/internal/mission-translation.md "C++ port").
//   Dll       = missionflow.Dll    (pefile's memory-mapped image, .text range, capstone decode cache)
//   Function  = missionflow.Function (recursive descent, basic blocks, jump tables incl. byte-indexed ones)
//   Program   = mission2lua.Program  (relocations, imports, exports, every reachable function, address-taken ones)
// Every rule follows the Python line by line - the output must be byte-identical (the gate, tools/m2l_gate.py).
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "m2l_decode.h"

namespace pse::m2l {

struct Section {
    std::string name;            // up to 8 characters, NULs cut
    uint32_t va = 0, vsize = 0, rawPtr = 0, rawSize = 0;
    std::vector<uint8_t> data;   // pefile's get_data(): raw bytes (PointerToRawData_adj .. PointerToRawData + SizeOfRawData)
};

struct Import {
    uint32_t iat = 0;            // absolute va of the IAT slot
    std::string dll;
    std::string name;            // empty = by ordinal
    uint32_t ordinal = 0;
};

class Dll {
public:
    bool Parse(const std::vector<uint8_t>& file, std::string* error);

    uint32_t base = 0, sizeOfImage = 0, entryRva = 0;
    uint32_t textLo = 0, textHi = 0;
    std::vector<uint8_t> img;    // pefile.get_memory_mapped_image()
    std::vector<Section> sections;
    std::vector<Import> imports;
    std::map<std::string, uint32_t> exports;   // name -> absolute va
    std::set<uint32_t> relocs;                 // absolute va of every type-3 (HIGHLOW) relocation

    bool InText(uint32_t va) const { return va >= textLo && va < textHi; }
    bool U32(uint32_t va, uint32_t* out) const;
    bool U8(uint32_t va, uint8_t* out) const;
    const Insn* At(uint32_t va);   // decoded instruction or nullptr (cached)

private:
    std::map<uint32_t, std::unique_ptr<Insn>> cache_;
    std::set<uint32_t> bad_;
};

struct Table {
    std::map<uint32_t, uint32_t> values;   // case index -> target
    uint32_t tableVa = 0;                  // jmp [reg*4 + table]
    bool byteTable = false;
};

struct Block {
    uint32_t start = 0;
    std::vector<const Insn*> insns;
    std::vector<uint32_t> succ;
    enum Kind { Fall, Ret, Jmp, Cond, Switch, TailCall, End } kind = Fall;
    std::unique_ptr<Table> table;
};

class Function {
public:
    Function(Dll& dll, uint32_t entry);
    uint32_t entry;
    std::map<uint32_t, Block> blocks;   // sorted by address (Python: sorted(f.blocks))

private:
    std::vector<uint32_t> TableTargets(const Insn& i);       // base shapes, then the register-bound one
    std::vector<uint32_t> TableTargetsBase(const Insn& i);   // missionflow.Function._table_targets
    std::vector<uint32_t> RegisterBound(const Insn& i);      // mission2lua._Function (VERSION 2)
    std::vector<const Insn*> Back(uint32_t va, size_t n);
    Dll& d_;
    std::unique_ptr<Table> last_;
};

class Program {
public:
    bool Load(const std::vector<uint8_t>& file, const std::string& name, std::string* error);

    Dll dll;
    std::string name;
    uint32_t entry = 0;
    std::set<uint32_t> taken;                               // address-taken functions (thunks)
    std::map<uint32_t, std::unique_ptr<Function>> functions;
    std::vector<uint8_t> raw;
};

}  // namespace pse::m2l
