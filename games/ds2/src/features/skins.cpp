// .skin files (Mods folder): a soldier's own textures - one for the whole character or one per skin part -, another body
// model, and for squad members a name / portrait, without touching the textures and models every other soldier shares.
// DS1 has .skin too (repo root src/features/mods.cpp BuildSkins, bodies.cpp); DS2 draws characters differently, so this
// is its own design (modder page: repo root docs/sdk/ds2-mods.md ".skin files"; addresses: PORTING.md "Skins").
//
// How CDS II.exe textures a soldier (2026-10-22; DS1 addresses in brackets):
//  - every soldier (CHLOGrunt, ctor FUN_004835d0 thiscall(scene record, unit)) ends his ctor with the skin apply
//    FUN_00475020 thiscall(soldier, skin entry) ret 4 [FUN_00435be0] - squad members with their CSkins entry (lookup
//    FUN_00451fc0(key, uniform [0x627154], role)), everyone else with null. Soldier +0x24 unit (+4 name, +8 / +0x2B body
//    model per uniform, +0x1CF squad role, -1 none), +0x30 render instance, +0xE24 skin entry. The apply registers the
//    portrait; the entry's texture replaces the instance override (+0x20C, FUN_0055dd60) only when there already is one
//    (0x475087) - soldiers normally have none and draw their body's own TEXL texture(s).
//  - the skin draw FUN_0055f1f0 (thiscall instance) draws each skinned object of instance +0xC8 (next +0xE0, kind +0xC
//    = 2, skin data +0xB4) by FUN_00556290(skin data, texture) at the call 0x55F334: texture = the draw's argument, else
//    the instance override, else (null) the object's own (skin data +4 -> +4). An override covers every part; the
//    parts' own textures belong to the model, i.e. to every soldier with that body.
//  Here: the apply is wrapped (entry detour) - a soldier a .skin matches gets his record (per skin part: texture), his
//  body (instance +0xC8 / +0x1DC = the mod's model, same skeleton) and, for squad members, a private copy of his skin
//  entry with the .skin's names / portrait. The draw call 0x55F334 is hooked (esi = instance, edi = object): parts of a
//  recorded instance get the record's texture; nothing else changes (the override, the shared models stay the game's).
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
constexpr uint32_t kApply = 0x475020;
constexpr uint8_t kApplyEntry[] = {0x8B, 0x44, 0x24, 0x04, 0x53};  // mov eax, [esp+4]; push ebx
constexpr uint32_t kDrawPartCall = 0x55F334, kDrawPart = 0x556290;
constexpr uint32_t kGruntCtor = 0x4835D0;
constexpr uint8_t kGruntCtorEntry[] = {0x8B, 0x44, 0x24, 0x08, 0x83, 0xEC, 0x38};  // mov eax, [esp+8]; sub esp, 0x38
constexpr uint32_t kTextureManager = 0x680064, kAddTexture = 0x5432B0, kLoadPending = 0x543430;
constexpr uint32_t kWorld = 0x68008C, kLoadModel = 0x4E5D20;
constexpr uint32_t kFpuSave = 0x4C12C0, kFpuLevel = 0x4C12A0, kFpuRestore = 0x4C12D0;
constexpr uint32_t kUniform = 0x627154;   // 0 SAS, 1 Delta
constexpr uint32_t kSetModel = 0x55F6D0;  // CAnimInstance vtable 0x5DAE78 slot 7

template <class T>
T& At(const void* p, uint32_t off) { return *reinterpret_cast<T*>(const_cast<uint8_t*>(static_cast<const uint8_t*>(p)) + off); }

std::string Upper(std::string s) {
    CharUpperBuffA(s.data(), static_cast<DWORD>(s.size()));
    return s;
}
std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}
std::string StripExt(std::string s) {
    const size_t slash = s.find_last_of("\\/");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos) {
        const std::string ext = Upper(s.substr(dot));
        if (ext == ".DDS" || ext == ".TGA" || ext == ".PNG" || ext == ".BMP" || ext == ".DDL" || ext == ".EVO")
            s = s.substr(0, dot);
    }
    return s;
}

// ---- .skin files ----
struct PartTexture {
    int number = 0;      // 1-based skin part, or 0 = by name
    std::string object;  // object name (upper case) when number = 0
    std::string texture;
};
struct Skin {
    std::string file, mod;
    std::vector<std::string> soldiers;  // upper-case unit names; "SQUAD" / "ALL"
    int uniform = -1;                   // -1 any, 0 SAS, 1 Delta
    std::string texture;                // whole character
    std::vector<PartTexture> parts;
    std::string body;                   // model name (no .evo)
    std::unordered_map<std::string, uint32_t> bodyNames;  // the mod body's object names -> EOBJ name hashes
    std::string first, last, portrait;
};
std::vector<Skin> g_skins;

// EOBJ headers of a DS2 model file (version 0x35): "EOBJ", u32 version, model[64], node name[64], u32 name hash ...
// The loaded object keeps the hash at +0xDC (reader FUN_005087b0), so parts can be named by their Blender object name.
void ReadObjectNames(const std::string& path, std::unordered_map<std::string, uint32_t>& out) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const DWORD size = GetFileSize(f, nullptr);
    std::vector<uint8_t> data(size == INVALID_FILE_SIZE ? 0 : size);
    DWORD got = 0;
    if (!data.empty()) ReadFile(f, data.data(), size, &got, nullptr);
    CloseHandle(f);
    for (size_t i = 0; i + 4 + 4 + 64 + 64 + 4 <= got; ++i) {
        if (memcmp(&data[i], "EOBJ", 4) != 0) continue;
        uint32_t version;
        memcpy(&version, &data[i + 4], 4);
        if ((version & 0xFFFF) != 0x35) continue;
        char name[65] = {};
        memcpy(name, &data[i + 8 + 64], 64);
        uint32_t hash;
        memcpy(&hash, &data[i + 8 + 128], 4);
        if (name[0]) out.emplace(Upper(name), hash);
    }
}

void ParseSkin(const features::ModFileRef& ref) {
    HANDLE f = CreateFileA(ref.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        features::ModLog("[fail] Skins: can't read %s", ref.path.c_str());
        return;
    }
    std::string text;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof buf, &got, nullptr) && got) text.append(buf, got);
    CloseHandle(f);
    Skin s;
    s.mod = ref.mod;
    const size_t slash = ref.path.find_last_of("\\/");
    s.file = slash == std::string::npos ? ref.path : ref.path.substr(slash + 1);
    size_t pos = 0;
    int lineNo = 0;
    bool ok = true;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = Trim(line);
        if (line.empty()) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            features::ModLog("[fail] Skins: %s line %d - \"key = value\" expected: %s", s.file.c_str(), lineNo, line.c_str());
            continue;
        }
        std::string key = Upper(Trim(line.substr(0, eq)));
        const std::string value = Trim(line.substr(eq + 1));
        if (key == "SOLDIER" || key == "SOLDIERS" || key == "UNIT" || key == "UNITS") {
            size_t p = 0;
            while (p <= value.size()) {
                size_t c = value.find(',', p);
                if (c == std::string::npos) c = value.size();
                std::string name = Upper(Trim(value.substr(p, c - p)));
                p = c + 1;
                if (name.empty()) continue;
                if (name == "CONNORS") name = "RAMIREZ";   // Connors is "Ramirez" in the game's tables
                if (name == "RIFLEMAN") name = "BRADLEY";
                if (name == "HEAVY" || name == "HEAVY WEAPONS") name = "RAMIREZ";
                if (name == "ENGINEER" || name == "COMBAT ENGINEER") name = "JONES";
                if (name == "SNIPER") name = "FOLEY";
                if (name == "*" || name == "EVERYONE") name = "ALL";
                s.soldiers.push_back(name);
            }
        } else if (key == "UNIFORM") {
            const std::string u = Upper(value);
            if (u == "SAS") s.uniform = 0;
            else if (u == "DELTA" || u == "DF" || u == "US" || u == "DELTA FORCE") s.uniform = 1;
            else if (u == "ANY" || u == "ALL" || u.empty()) s.uniform = -1;
            else features::ModLog("[fail] Skins: %s - uniform \"%s\" unknown (SAS, Delta or any)", s.file.c_str(), value.c_str());
        } else if (key == "TEXTURE") {
            s.texture = StripExt(value);
        } else if (key.compare(0, 8, "TEXTURE ") == 0) {
            PartTexture pt;
            const std::string which = Trim(key.substr(8));
            char* stop = nullptr;
            const long n = strtol(which.c_str(), &stop, 10);
            if (stop && !*stop && n > 0) pt.number = static_cast<int>(n);
            else pt.object = which;
            pt.texture = StripExt(value);
            s.parts.push_back(pt);
        } else if (key == "BODY" || key == "MODEL") {
            s.body = StripExt(value);
        } else if (key == "FIRST NAME") {
            s.first = Upper(value);
        } else if (key == "LAST NAME") {
            s.last = Upper(value);
        } else if (key == "PORTRAIT") {
            s.portrait = StripExt(value);
        } else if (key == "NAME" || key == "DISPLAY NAME") {
            // ignored: a label for the modder (the soldier's name comes from first / last name)
        } else {
            features::ModLog("[fail] Skins: %s line %d - unknown key \"%s\"", s.file.c_str(), lineNo, key.c_str());
        }
    }
    if (s.soldiers.empty()) {
        features::ModLog("[fail] Skins: %s - no \"soldier = <unit name>\" line, not used", s.file.c_str());
        ok = false;
    }
    if (s.texture.empty() && s.parts.empty() && s.body.empty() && s.first.empty() && s.last.empty() && s.portrait.empty()) {
        features::ModLog("[fail] Skins: %s - nothing to change (texture, texture <part>, body, names, portrait)", s.file.c_str());
        ok = false;
    }
    if (s.first.size() > 0x23 || s.last.size() > 0x23) {
        features::ModLog("[fail] Skins: %s - a name longer than 35 characters is cut", s.file.c_str());
        s.first.resize(std::min<size_t>(s.first.size(), 0x23));
        s.last.resize(std::min<size_t>(s.last.size(), 0x23));
    }
    if (s.portrait.size() > 0x23) {
        features::ModLog("[fail] Skins: %s - portrait name longer than 35 characters, ignored", s.file.c_str());
        s.portrait.clear();
    }
    auto checkTexture = [&](const std::string& t) {
        if (!t.empty() && !features::ModHasTexture(t))
            features::ModLog("[fail] Skins: %s - texture %s isn't in any mod (needs %s.dds / .tga / .png in a mod folder)",
                             s.file.c_str(), t.c_str(), t.c_str());
    };
    checkTexture(s.texture);
    for (const PartTexture& p : s.parts) checkTexture(p.texture);
    checkTexture(s.portrait);
    if (!s.body.empty()) {
        const std::string path = features::ModFilePath((s.body + ".evo").c_str());
        if (path.empty())
            features::ModLog("[fail] Skins: %s - body %s.evo isn't in any mod (a game model of that name is tried)",
                             s.file.c_str(), s.body.c_str());
        else
            ReadObjectNames(path, s.bodyNames);
    }
    for (const PartTexture& p : s.parts)
        if (p.number == 0 && !s.bodyNames.count(p.object))
            features::ModLog("[fail] Skins: %s - \"texture %s\": no object of that name in %s (use the part number, or a "
                             "name from the body's .evo)", s.file.c_str(), p.object.c_str(),
                             s.body.empty() ? "a body (no body = ...)" : (s.body + ".evo").c_str());
    if (!ok) return;
    std::string who;
    for (const std::string& n : s.soldiers) who += (who.empty() ? "" : ", ") + n;
    features::ModLog("Skins: %s (%s) - soldier %s%s%s%s%s%s%s", s.file.c_str(), s.mod.c_str(), who.c_str(),
                     s.uniform == 0 ? " (SAS uniform)" : s.uniform == 1 ? " (Delta uniform)" : "",
                     s.texture.empty() ? "" : (", texture " + s.texture).c_str(),
                     s.parts.empty() ? "" : (", " + std::to_string(s.parts.size()) + " part texture(s)").c_str(),
                     s.body.empty() ? "" : (", body " + s.body).c_str(),
                     s.first.empty() && s.last.empty() ? "" : (", name " + s.first + " " + s.last).c_str(),
                     s.portrait.empty() ? "" : (", portrait " + s.portrait).c_str());
    g_skins.push_back(std::move(s));
}

void ReadSkins() {
    g_skins.clear();
    for (const features::ModFileRef& ref : features::ModFilesWithExtension(".SKIN")) ParseSkin(ref);
    if (!g_skins.empty()) features::ModLog("[ok]   Skins: %zu .skin file(s)", g_skins.size());
}

// ---- matching ----
const char* UnitName(const uint8_t* unit) {
    const char* n = unit ? At<const char*>(unit, 4) : nullptr;
    return n ? n : "";
}

const Skin* Match(const uint8_t* unit) {
    if (!unit) return nullptr;
    const std::string name = Upper(UnitName(unit));
    const bool squad = At<int8_t>(unit, 0x1CF) >= 0;
    const int uniform = *reinterpret_cast<const int*>(kUniform);
    const Skin* found = nullptr;
    for (const Skin& s : g_skins) {   // later files win (name order, like the mods)
        if (s.uniform >= 0 && s.uniform != uniform) continue;
        for (const std::string& want : s.soldiers)
            if (want == "ALL" || (want == "SQUAD" && squad) || want == name || (squad && name == want + "_TRAINING"))
                found = &s;
    }
    return found;
}

// ---- game calls ----
void* AddTexture(const std::string& name, uint32_t flags = 0x11) {
    void* mgr = *reinterpret_cast<void**>(kTextureManager);
    if (!mgr || name.empty()) return nullptr;
    using Add = void*(__thiscall*)(void*, const char*, uint32_t);
    return reinterpret_cast<Add>(kAddTexture)(mgr, name.c_str(), flags);
}

uint8_t* LoadModel(const std::string& name) {
    void* world = *reinterpret_cast<void**>(kWorld);
    if (!world) return nullptr;
    const uint32_t saved = reinterpret_cast<uint32_t(__cdecl*)()>(kFpuSave)();
    reinterpret_cast<void(__cdecl*)()>(kFpuLevel)();
    using Load = uint8_t*(__thiscall*)(void*, int, const char*, int, int);
    uint8_t* model = reinterpret_cast<Load>(kLoadModel)(world, 4, name.c_str(), 1, 0);
    reinterpret_cast<void(__cdecl*)(uint32_t)>(kFpuRestore)(saved);
    return model;
}

int KeyCount(const uint8_t* model) {   // skeleton keys (instance +0x1C0 / +0x1C4 come from it)
    const uint8_t* skel = model ? At<const uint8_t*>(model, 0xEC) : nullptr;
    return skel ? At<int>(skel, 0) : -1;
}
void* PartOwnTexture(const uint8_t* object) {   // skin data +4 -> +4
    const uint8_t* sd = At<const uint8_t*>(object, 0xB4);
    const uint8_t* part = sd ? At<const uint8_t*>(sd, 4) : nullptr;
    return part ? At<void*>(part, 4) : nullptr;
}
const char* TextureName(const void* tex) {
    const char* n = tex ? At<const char*>(tex, 8) : nullptr;
    return n ? n : "";
}

// A body's own part textures, taken the first time the model is seen (an override change nulls them on the shared
// model, FUN_0055dd70). Keyed by model address + its first object's name hash (a later level may reuse the address).
struct OwnTextures {
    uint32_t hash = 0;
    std::vector<void*> parts;  // per object in model order (null = none / "Default")
};
std::unordered_map<const uint8_t*, OwnTextures> g_own;

const OwnTextures& ModelOwnTextures(const uint8_t* model) {
    OwnTextures& o = g_own[model];
    if (o.hash == At<uint32_t>(model, 0xDC) && !o.parts.empty()) return o;
    o.hash = At<uint32_t>(model, 0xDC);
    o.parts.clear();
    int n = 0;
    for (const uint8_t* obj = model; obj && n < 4096; obj = At<const uint8_t*>(obj, 0xE0), ++n) {
        void* t = At<int>(obj, 0xC) == 2 ? PartOwnTexture(obj) : nullptr;
        if (t && _stricmp(TextureName(t), "Default") == 0) t = nullptr;
        o.parts.push_back(t);
    }
    return o;
}

// ---- per-instance records (read by the draw hook) ----
struct Record {
    const uint8_t* model = nullptr;     // the full-detail model the parts belong to
    void* whole = nullptr;              // texture for every part (and LOD models)
    std::vector<std::pair<const uint8_t*, void*>> parts;   // object -> texture
    const Skin* skin = nullptr;
};
std::unordered_map<const uint8_t*, Record> g_records;
std::unordered_set<void*> g_textureTried;   // textures the draw hook made the game load (once each)
std::unordered_set<std::string> g_applied;  // report lines written ("skin|unit")

// Squad members: a private copy of the skin entry with the .skin's names / portrait (the game's entry is shared by
// every soldier with that skin key). Name records = the fNames / sNames layout: char[0x24] + u32 hash.
struct EntryCopy {
    uint8_t entry[0x5C];
    uint8_t first[0x28], last[0x28];
};
std::unordered_map<std::string, std::unique_ptr<EntryCopy>> g_entries;   // "file|entry address"
std::vector<std::unique_ptr<EntryCopy>> g_oldEntries;   // kept after a reload: soldiers in play may still point at them

uint8_t* EntryFor(const Skin& s, uint8_t* entry) {
    if (!entry || (s.first.empty() && s.last.empty() && s.portrait.empty())) return entry;
    char key[300];
    snprintf(key, sizeof key, "%s|%p", s.file.c_str(), static_cast<void*>(entry));
    auto& copy = g_entries[key];
    if (!copy) {
        copy = std::make_unique<EntryCopy>();
        memcpy(copy->entry, entry, sizeof copy->entry);
        if (!s.first.empty()) {
            memset(copy->first, 0, sizeof copy->first);
            memcpy(copy->first, s.first.c_str(), std::min<size_t>(s.first.size(), 0x23));
            At<uint8_t*>(copy->entry, 0x50) = copy->first;
        }
        if (!s.last.empty()) {
            memset(copy->last, 0, sizeof copy->last);
            memcpy(copy->last, s.last.c_str(), std::min<size_t>(s.last.size(), 0x23));
            At<uint8_t*>(copy->entry, 0x54) = copy->last;
        }
        if (!s.portrait.empty()) {
            memset(copy->entry + 0x27, 0, 0x25);
            memcpy(copy->entry + 0x27, s.portrait.c_str(), std::min<size_t>(s.portrait.size(), 0x24));
            At<void*>(copy->entry, 0x4C) = nullptr;
        }
    }
    return copy->entry;
}

void Dress(uint8_t* soldier, const Skin& s) {
    uint8_t* inst = At<uint8_t*>(soldier, 0x30);
    const uint8_t* unit = At<const uint8_t*>(soldier, 0x24);
    if (!inst || !At<uint8_t*>(inst, 0xC8)) return;
    Record rec;
    rec.skin = &s;
    std::string bodyNote;
    if (!s.body.empty()) {
        uint8_t* full = At<uint8_t*>(inst, 0x1DC) ? At<uint8_t*>(inst, 0x1DC) : At<uint8_t*>(inst, 0xC8);
        uint8_t* model = LoadModel(s.body);
        const int keys = At<int>(inst, 0x1C4);   // skeleton keys of his own body
        if (!model) {
            features::ModLog("[fail] Skins: %s - body %s didn't load as a character model", s.file.c_str(), s.body.c_str());
        } else if (model == full) {
            bodyNote = "body " + s.body;
        } else if (KeyCount(model) != keys) {
            features::ModLog("[fail] Skins: %s - %s already exists on his own body; body %s has %d skeleton keys (his %d), "
                             "so it can only be given when he is created", s.file.c_str(), UnitName(unit), s.body.c_str(),
                             KeyCount(model), keys);
        } else if (At<uint32_t>(At<void*>(inst, 0), 0x1C) != kSetModel) {
            features::ModLog("[fail] Skins: %s - %s's render instance isn't a CAnimInstance (vtable %p) - body not changed",
                             s.file.c_str(), UnitName(unit), At<void*>(inst, 0));
        } else {
            // CAnimInstance::SetModel (vtable +0x1C, the LOD switch's call): re-sizes the pose arrays when the key count
            // differs (a body with extra skin objects), keeps the animation layers. +0x1DC = the model the LOD switch
            // returns to.
            if (At<uint8_t*>(inst, 0xC8) == full)
                reinterpret_cast<void(__thiscall*)(void*, void*)>(kSetModel)(inst, model);
            At<uint8_t*>(inst, 0x1DC) = model;
            bodyNote = "body " + s.body + " (switched)";
        }
    }
    rec.model = At<uint8_t*>(inst, 0x1DC) ? At<uint8_t*>(inst, 0x1DC) : At<uint8_t*>(inst, 0xC8);
    rec.whole = s.texture.empty() ? nullptr : AddTexture(s.texture);
    // parts: explicit textures by number / name, else the body's own (a mod body keeps its textures), else whole
    const bool ownBody = !bodyNote.empty();
    const OwnTextures* own = ownBody ? &ModelOwnTextures(rec.model) : nullptr;
    int index = 0, number = 0, explicitHits = 0, ownHits = 0;
    for (const uint8_t* obj = rec.model; obj && index < 4096; obj = At<const uint8_t*>(obj, 0xE0), ++index) {
        if (At<int>(obj, 0xC) != 2) continue;
        ++number;
        void* tex = nullptr;
        for (const PartTexture& p : s.parts) {
            if (p.number == number) tex = AddTexture(p.texture);
            else if (p.number == 0) {
                auto it = s.bodyNames.find(p.object);
                if (it != s.bodyNames.end() && it->second == At<uint32_t>(obj, 0xDC)) tex = AddTexture(p.texture);
            }
        }
        if (tex) ++explicitHits;
        if (!tex && !rec.whole && own && index < static_cast<int>(own->parts.size()) && own->parts[index]) {
            tex = own->parts[index];
            ++ownHits;
        }
        if (tex) rec.parts.emplace_back(obj, tex);
    }
    for (const PartTexture& p : s.parts)
        if (p.number > number) {
            const std::string k = s.file + "|part" + std::to_string(p.number);
            if (g_applied.insert(k).second)
                features::ModLog("[fail] Skins: %s - \"texture %d\": the body has %d skin part(s)", s.file.c_str(), p.number, number);
        }
    dslog::Write("[dev] Skins: %s now model %p full %p keys %d, entry %p, %zu part texture(s), whole %p", UnitName(unit),
                 At<void*>(inst, 0xC8), At<void*>(inst, 0x1DC), At<int>(inst, 0x1C4), At<void*>(soldier, 0xE24),
                 rec.parts.size(), rec.whole);
#ifndef DS_DIST
    if (const uint8_t* e = At<const uint8_t*>(soldier, 0xE24)) {   // the name getters 0x4756F0 / 0x475710 read these
        const char* first = At<const char*>(e, 0x50);
        const char* last = At<const char*>(e, 0x54);
        dslog::Write("[dev] Skins: %s entry names \"%s\" \"%s\", portrait \"%.36s\" texture %p", UnitName(unit),
                     first ? first : "-", last ? last : "-", reinterpret_cast<const char*>(e + 0x27), At<void*>(e, 0x4C));
    }
#endif
    if (rec.whole || !rec.parts.empty()) g_records[inst] = rec;
    else g_records.erase(inst);
    const std::string line = s.file + "|" + UnitName(unit);
    if (g_applied.insert(line).second)
        features::ModLog("Skins: %s applied to %s (%d skin part(s)%s%s%s%s)", s.file.c_str(), UnitName(unit), number,
                         rec.whole ? (", texture " + s.texture).c_str() : "",
                         explicitHits ? (", " + std::to_string(explicitHits) + " part texture(s)").c_str() : "",
                         ownHits ? (", " + std::to_string(ownHits) + " of the body's own").c_str() : "",
                         bodyNote.empty() ? "" : (", " + bodyNote).c_str());
}

using ApplyFn = void(__thiscall*)(void*, void*);
ApplyFn g_apply = nullptr;

void __fastcall ApplyHook(uint8_t* soldier, void*, uint8_t* entry) {
    const Skin* s = nullptr;
    __try {
        s = Match(At<const uint8_t*>(soldier, 0x24));
        if (s) entry = EntryFor(*s, entry);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s = nullptr;
    }
    g_apply(soldier, entry);
    __try {
        if (s) Dress(soldier, *s);
        else if (uint8_t* inst = At<uint8_t*>(soldier, 0x30)) g_records.erase(inst);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dslog::Write("[fail] Skins: dressing soldier %p faulted", static_cast<void*>(soldier));
    }
}

// Bodies: the soldier's render instance is made in the HLO base ctor (FUN_004cf170 -> FUN_004cdef0(model name = scene
// record +0x20 (char[0x44]), loaded model = record +0x64, ...): null model = load by name). The CHLOGrunt ctor
// FUN_004835d0 thiscall(record, unit) ret 8 is wrapped: for a soldier whose .skin has a body the record names / holds
// the body for the call (restored after), so his instance is built on it from the start - any key count, also a body
// with extra skin objects (CAnimInstance::SetModel 0x55F6D0 only switches between models with the same object / key
// counts).
using GruntCtorFn = void*(__thiscall*)(void*, uint8_t*, uint8_t*);
GruntCtorFn g_gruntCtor = nullptr;

void* __fastcall GruntCtorHook(void* self, void*, uint8_t* record, uint8_t* unit) {
    uint8_t* body = nullptr;
    char savedName[0x44];
    uint8_t* savedModel = nullptr;
    __try {
        const Skin* s = record ? Match(unit) : nullptr;
        if (s && !s->body.empty()) {
            body = LoadModel(s->body);
            if (body) {
                memcpy(savedName, record + 0x20, sizeof savedName);
                savedModel = At<uint8_t*>(record, 0x64);
                memset(record + 0x20, 0, sizeof savedName);
                memcpy(record + 0x20, s->body.c_str(), std::min<size_t>(s->body.size(), sizeof savedName - 1));
                At<uint8_t*>(record, 0x64) = body;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        body = nullptr;
    }
    void* result = g_gruntCtor(self, record, unit);
    if (body) {
        memcpy(record + 0x20, savedName, sizeof savedName);
        At<uint8_t*>(record, 0x64) = savedModel;
    }
    return result;
}

// The draw call 0x55F334: esi = instance, edi = object (captured by the stub), ecx = skin data, one stack argument.
const uint8_t* g_drawInst = nullptr;
const uint8_t* g_drawObj = nullptr;

void* Loaded(void* tex) {
    if (!tex || At<void*>(tex, 0x10)) return tex;
    if (g_textureTried.insert(tex).second) {   // registered after the level's own batch (a mid-level body / skin)
        if (void* mgr = *reinterpret_cast<void**>(kTextureManager)) {
            const uint32_t flags = At<uint32_t>(tex, 0x2C);
            At<uint32_t>(tex, 0x2C) = flags & ~0x10u;   // 0x10 = not found in the level archive (the batch's 2nd pass)
            At<int>(mgr, 0x10) = 1;   // "something pending" (a model's TEXL textures don't always set it)
            reinterpret_cast<void(__thiscall*)(void*, void*)>(kLoadPending)(mgr, nullptr);
            dslog::Write("[dev] Skins: texture %s flags %08X -> %08X, refs %d, loaded %p", TextureName(tex), flags,
                         At<uint32_t>(tex, 0x2C), At<int>(tex, 0x24), At<void*>(tex, 0x10));
        }
        if (!At<void*>(tex, 0x10))
            features::ModLog("[fail] Skins: texture %s didn't load (missing, or not a texture file)", TextureName(tex));
    }
    return At<void*>(tex, 0x10) ? tex : nullptr;
}

void __fastcall DrawPart(void* skinData, void*, void* tex) {
    if (!tex && !g_records.empty()) {
        auto it = g_records.find(g_drawInst);
        if (it != g_records.end()) {
            const Record& r = it->second;
            void* want = nullptr;
            if (At<const uint8_t*>(g_drawInst, 0xC8) == r.model) {
                for (const auto& [obj, t] : r.parts)
                    if (obj == g_drawObj) want = t;
            }
            if (!want) want = r.whole;
            if (want) want = Loaded(want);
            if (want) tex = want;
#ifndef DS_DIST
            static std::unordered_set<const uint8_t*> seen;
            if (seen.insert(g_drawObj).second)
                dslog::Write("[dev] Skins: drew instance %p object %p with %s", g_drawInst, g_drawObj,
                             want ? TextureName(want) : "the game's texture");
#endif
        }
    }
    reinterpret_cast<void(__thiscall*)(void*, void*)>(kDrawPart)(skinData, tex);
}

__declspec(naked) void DrawPartStub() {
    __asm {
        mov g_drawInst, esi
        mov g_drawObj, edi
        jmp DrawPart
    }
}

template <class Fn>
bool HookEntry(uint32_t entry, const void* fn, Fn& next, const uint8_t* prologue, size_t len) {
#ifdef DS2_PSE_HOST
    return patch::HookFunction(entry, fn, reinterpret_cast<void**>(&next), prologue, len);
#else
    if (!patch::Matches(entry, prologue, len)) return false;
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    memcpy(tramp, prologue, len);
    tramp[len] = 0xE9;
    const int32_t rel = static_cast<int32_t>(entry + len - (reinterpret_cast<uint32_t>(tramp) + len + 5));
    memcpy(tramp + len + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, len + 5);
    next = reinterpret_cast<Fn>(tramp);
    return patch::WriteJump(entry, fn);
#endif
}

bool g_hooked = false;

void InstallHooks() {
    if (g_hooked || g_skins.empty()) return;
    g_hooked = true;
    if (!HookEntry(kApply, reinterpret_cast<const void*>(&ApplyHook), g_apply, kApplyEntry, sizeof kApplyEntry)) {
        features::ModLog("[fail] Skins: skin apply at 0x%08X not recognised - .skin files not used", kApply);
        return;
    }
    if (!HookEntry(kGruntCtor, reinterpret_cast<const void*>(&GruntCtorHook), g_gruntCtor, kGruntCtorEntry,
                   sizeof kGruntCtorEntry))
        features::ModLog("[fail] Skins: soldier set-up at 0x%08X not recognised - bodies only by model switch (same "
                         "skeleton keys)", kGruntCtor);
    if (!patch::HookCall(kDrawPartCall, reinterpret_cast<const void*>(&DrawPartStub), kDrawPart))
        features::ModLog("[fail] Skins: skin draw at 0x%08X not recognised - .skin textures not shown", kDrawPartCall);
    dslog::Write("[ok]   Skins: skin apply + skin draw hooked");
}
}  // namespace

void features::ApplySkins() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!settings::Get().featMods) return;
    ReadSkins();
    InstallHooks();
}

void features::ReloadSkins() {
    if (!settings::Get().featMods) return;
    g_records.clear();   // the records point into g_skins
    for (auto& [key, copy] : g_entries) g_oldEntries.push_back(std::move(copy));   // soldiers in play keep theirs
    g_entries.clear();
    ReadSkins();
    InstallHooks();
}
