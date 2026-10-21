#pragma once
// Base forms (DESIGN.md §13.3, docs/sdk/data-files.md "Records"): a game's data tables as records with namespaced IDs
// ("ds1:weapon/us_wpn_m16a2") in PSE data files, the tables made again from records, and mods' record edits merged
// field by field. The C++ twin of the ModKit's conflict_ds_tools/forms.py - same records, same tables, same report
// lines (scripts/records_gate.py).
//
//   Schema (pse/records/<game>/schema.lua): tables = { { file, type, columns = { { name, kind, none?, ref? }, ... },
//     rows_per_key?, owner? }, ... }; kind int / fixed2 / yesno / text / ref; ref = a type or a list of types.
#include <map>
#include <string>
#include <vector>

#include "data.h"

namespace pse::records {

// file name as stored in the archives (upper case, "WEAPS.TXT") -> its bytes
using TableFiles = std::map<std::string, std::string>;
// record type -> its data file
using Forms = std::map<std::string, pse::data::Document>;

struct Edits {
    std::string mod, file;
    pse::data::Document doc;   // root list = edit tables
};

// The records of every table the schema names that `tables` has. false + *error for a table the schema can't read.
bool Generate(const TableFiles& tables, const pse::data::Value& schema, Forms* out, std::string* error);
// The tables again (byte for byte for unchanged records).
TableFiles Tables(const Forms& forms, const pse::data::Value& schema);
// Applies the edits in order (field by field, the later mod wins); *report gets one line per new / deleted record,
// conflict and problem ("[fail] ...").
void Merge(Forms* forms, const std::vector<Edits>& edits, const pse::data::Value& schema, std::vector<std::string>* report);

std::string RecordId(const std::string& type, const std::string& key);   // "ds1:<type>/<ASCII lower key>"

}  // namespace pse::records
