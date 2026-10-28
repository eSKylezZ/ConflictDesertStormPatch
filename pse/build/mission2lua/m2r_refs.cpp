// mission2lua_readable (C++ port): the scene's editor IDs (refs_from_env) through the records layer's scene reader, and
// single files out of the install's archives (the ModKit's archive.GameFiles).
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>

#include "mission2lua.h"
#include "placed.h"

namespace pse::m2l {

namespace {

uint32_t Ds1Hash(const std::string& name) {   // FUN_004b9a10 over the upper-cased bare name
    std::string base = name;
    const size_t cut = base.find_last_of("\\/");
    if (cut != std::string::npos) base = base.substr(cut + 1);
    uint32_t x = 1;
    for (unsigned char c : base) {
        if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 32);
        for (int b = 0; b < 8; ++b) {
            const uint32_t in = ((c >> b) ^ x ^ (x >> 1) ^ (x >> 21) ^ (x >> 31)) & 1;
            x = x << 1 | in;
        }
    }
    return x;
}

std::wstring LowerW(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

}  // namespace

bool SceneRefsFromEnv(const std::string& envBytes, SceneRefs* out) {
    std::vector<std::string> regions;
    if (!pse::records::SceneRefNames(envBytes, &out->objects, &out->waypoints, &regions)) return false;
    out->regions.clear();
    for (size_t k = 0; k < regions.size(); ++k) out->regions[static_cast<int64_t>(k)] = regions[k];
    return true;
}

bool ReadArchiveFile(const std::wstring& gameDir, const std::string& name, std::string* out) {
    // games.game_files(folder, (".dat",)): PSE\Originals then the game folder (the game folder's copy wins), sorted by
    // lower-case name; archive.list_archives leaves out unins*
    std::map<std::wstring, std::wstring> files;
    for (const std::wstring& dir : {gameDir + L"\\PSE\\Originals", gameDir}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring low = LowerW(fd.cFileName);
            if (low.size() < 4 || low.compare(low.size() - 4, 4, L".dat") != 0) continue;
            if (low.compare(0, 5, L"unins") == 0) continue;
            files[low] = dir + L"\\" + fd.cFileName;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    const uint32_t want = Ds1Hash(name);
    for (auto& [low, path] : files) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        uint8_t first[12];
        DWORD got = 0;
        bool found = false;
        uint32_t off = 0, size = 0;
        if (ReadFile(f, first, 12, &got, nullptr) && got == 12) {
            uint32_t head;
            std::memcpy(&head, first + 4, 4);
            head = std::max<uint32_t>(12, std::min<uint32_t>(head, 1u << 22));
            std::vector<uint8_t> dir(head);
            SetFilePointer(f, 0, nullptr, FILE_BEGIN);
            if (ReadFile(f, dir.data(), head, &got, nullptr)) {
                dir.resize(got);
                uint32_t firstOff = 0;
                if (dir.size() >= 12) std::memcpy(&firstOff, dir.data() + 4, 4);
                for (size_t p = 0; p + 12 <= std::min<size_t>(firstOff, dir.size()); p += 12) {
                    uint32_t e[3];
                    std::memcpy(e, dir.data() + p, 12);
                    if (!e[0] && !e[1] && !e[2]) break;
                    if (e[0] == want) {
                        off = e[1];
                        size = e[2];
                        found = true;
                        break;
                    }
                }
            }
        }
        if (found) {
            out->assign(size, '\0');
            SetFilePointer(f, static_cast<LONG>(off), nullptr, FILE_BEGIN);
            DWORD n = 0;
            ReadFile(f, out->data(), size, &n, nullptr);
            out->resize(n);
            CloseHandle(f);
            return true;
        }
        CloseHandle(f);
    }
    return false;
}

std::string SceneTableLua(const std::string& envBytes) {
    SceneRefs r;
    if (!SceneRefsFromEnv(envBytes, &r)) return {};
    std::vector<std::string> cams;
    pse::records::SceneCameraNames(envBytes, &cams);
    auto table = [](const char* name, const std::map<std::string, int64_t>& m) {
        std::string out = std::string("  ") + name + " = {\n";
        for (auto& [k, v] : m) out += "    [\"" + k + "\"] = " + std::to_string(v) + ",\n";
        return out + "  },\n";
    };
    auto invert = [](const std::map<int64_t, std::string>& m) {
        std::map<std::string, int64_t> o;
        for (auto& [v, k] : m) o.emplace(k, v);
        return o;
    };
    std::map<std::string, int64_t> cameras;   // build.py scene_ids: set_view_camera(n) = the scene's n-th camera
    for (size_t k = 0; k < cams.size(); ++k) {
        std::set<std::string> fresh;
        const std::string nm = cams[k].empty() ? "CAMERA_" + std::to_string(k) : cams[k];
        cameras.emplace(pse::records::RefIdent(nm, &fresh), static_cast<int64_t>(k));
    }
    return "return {\n" + table("objects", invert(r.objects)) + table("waypoints", invert(r.waypoints)) +
           table("regions", invert(r.regions)) + table("cameras", cameras) + "}\n";
}

std::function<bool(const std::string&, SceneRefs*)> SceneRefsFromGame(const std::wstring& gameDir) {
    return [gameDir](const std::string& scene, SceneRefs* refs) {
        std::string data;
        if (!ReadArchiveFile(gameDir, scene, &data)) return false;
        return SceneRefsFromEnv(data, refs);
    };
}

const char* ReadableBuild() { return "1+cpp.1"; }

ReadableCache EnsureReadableCached(const std::wstring& dllPath, const std::wstring& dir, const std::wstring& gameDir,
                                   bool force) {
    ReadableCache r;
    std::vector<uint8_t> raw;
    if (!ReadFileBytes(dllPath, &raw)) {
        r.error = "can't read the DLL";
        return r;
    }
    r.sha256 = Sha256Hex(raw);
    const size_t cut = dllPath.find_last_of(L"\\/");
    const std::wstring file = cut == std::wstring::npos ? dllPath : dllPath.substr(cut + 1);
    const std::wstring stem = file.substr(0, file.find_last_of(L'.'));
    std::string name;
    for (wchar_t c : file) name += c < 128 ? static_cast<char>(c) : '?';
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring lua = dir + L"\\" + stem + L".lua", stampPath = dir + L"\\" + stem + L".stamp";
    r.luaPath = lua;
    const std::string want = "dll " + r.sha256 + " " + std::to_string(raw.size()) + "\nreadable " + ReadableBuild() + "\n";
    std::vector<uint8_t> stamp, text;
    if (!force && ReadFileBytes(stampPath, &stamp) && ReadFileBytes(lua, &text)) {
        const std::string st(stamp.begin(), stamp.end());
        if (st.compare(0, want.size(), want) == 0) {
            const size_t sc = st.find("scene ", want.size());
            if (sc != std::string::npos) r.scene = st.substr(sc + 6, st.find('\n', sc) - sc - 6);
            r.bundle.assign(text.begin(), text.end());
            r.ok = true;
            return r;
        }
    }
    const DWORD t0 = GetTickCount();
    ReadableParts parts;
    if (!TranslateReadable(raw, name, "", SceneRefsFromGame(gameDir), &parts, &r.error)) return r;
    r.ms = static_cast<double>(GetTickCount() - t0);
    r.bundle = parts.bundle;
    r.scene = parts.scene;
    r.generated = true;
    auto write = [](const std::wstring& path, const std::string& data) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD n = 0;
        const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &n, nullptr) && n == data.size();
        CloseHandle(h);
        return ok;
    };
    // the stamp last: a half-written .lua never counts as up to date
    r.ok = true;
    if (!write(lua, r.bundle) || !write(stampPath, want + "scene " + r.scene + "\n")) r.error = "cache not written";
    return r;
}

}  // namespace pse::m2l
