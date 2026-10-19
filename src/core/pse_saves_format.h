#pragma once
// Chunk layouts of DesertStormFix's own co-save data (docs/sdk/ds1-plugins.md "Co-save chunks") and the reader for
// the older "<save>.sav.dsfix" text sidecar. Pure code (no game, no loader state) so pse_tests can compile it.
//
// Every chunk: version 1, little endian, strings = u16 length + bytes (no NUL), counts = u32.
//   dsfix.loadout  'KITS'  u32 n, n x {i32 role, str weapon id, i32 reserve, i32 magazine, u8 held}
//                          = weapons made at run time (id -1) in the squad's inventories
//   dsfix.gear     'GEAR'  u32 roles, roles x {i32 role, u32 n, n x {str connector, str model ("" = nothing)}}
//   dsfix.vehicles 'VEHS'  u32 n, n x {str original unit, str unit it was swapped for}
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "pse/pse_api.h"

namespace pse_saves {

constexpr uint32_t kKitsTag = PSE_TAG('K', 'I', 'T', 'S');
constexpr uint32_t kGearTag = PSE_TAG('G', 'E', 'A', 'R');
constexpr uint32_t kVehiclesTag = PSE_TAG('V', 'E', 'H', 'S');
constexpr uint32_t kChunkVersion = 1;

struct SavedWeapon {
    int role = -1;
    std::string id;
    int reserve = 0, magazine = 0;
    bool held = false;
};
struct SavedGearPiece {
    std::string connector, model;   // model "" = nothing on the connector
};
using SavedGear = std::map<int, std::vector<SavedGearPiece>>;    // by role
using SavedVehicles = std::map<std::string, std::string>;        // original unit -> new unit

std::vector<uint8_t> EncodeKits(const std::vector<SavedWeapon>& kits);
std::vector<uint8_t> EncodeGear(const SavedGear& gear);
std::vector<uint8_t> EncodeVehicles(const SavedVehicles& swaps);
// false = damaged chunk (out keeps what was read before the damage)
bool DecodeKits(const uint8_t* data, size_t len, std::vector<SavedWeapon>* out);
bool DecodeGear(const uint8_t* data, size_t len, SavedGear* out);
bool DecodeVehicles(const uint8_t* data, size_t len, SavedVehicles* out);

// The old sidecar (DesertStormFix up to 3.3.0-Alpha): one line each -
//   "role|weapon id|reserve|magazine|held"   spawned weapon
//   "G|role|connector|model"                  gear ("-" = nothing)
//   "V|ORIGINAL|NEW"                          vehicle swap
// Unknown lines are skipped.
struct Sidecar {
    std::vector<SavedWeapon> kits;
    SavedGear gear;
    SavedVehicles vehicles;
};
Sidecar ParseSidecar(const std::string& text);
// A co-save image (the pse file format, docs/sdk/reference/serialization.md) holding the sidecar's data as the
// chunks the plugins write today: what pse::cosave::LoadBytes takes for a save that has only a .dsfix.
std::vector<uint8_t> SidecarToCosave(const Sidecar& s);

// "<dir>\Name.sav" -> "<dir>\Name.pse" (any other name: + ".pse"); "<dir>\Name.sav" -> "<dir>\Name.sav.dsfix".
std::string CoSavePathFor(const std::string& savePath);
std::string SidecarPathFor(const std::string& savePath);

}  // namespace pse_saves
