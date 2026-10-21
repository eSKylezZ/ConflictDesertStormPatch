// mission2lua (C++ port): the missions cache - <game>\PSE\Cache\missions\<stem>.lua + <stem>.stamp
// (docs/internal/asset-store.md "Missions cache", DESIGN.md §12).
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <functional>

#include "mission2lua.h"

namespace pse::m2l {

std::function<std::string(const std::string&, const std::string&)> g_luaCheck;

namespace {

std::wstring WidenUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string NarrowUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool WriteAll(const std::wstring& path, const std::string& data) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const bool ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &put, nullptr) && put == data.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::string Stamp(const std::string& sha, size_t size) {
    return "dll " + sha + " " + std::to_string(size) + "\ntranslator " + TranslatorBuild() + "\n";
}

std::wstring FileName(const std::wstring& path) {
    const size_t s = path.find_last_of(L"\\/");
    return s == std::wstring::npos ? path : path.substr(s + 1);
}

}  // namespace

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    const bool ok = out->empty() || (ReadFile(f, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) && got == out->size());
    CloseHandle(f);
    return ok;
}

std::vector<std::wstring> MissionDlls(const std::wstring& gameDir) {
    std::vector<std::wstring> out;
    // the game folder, then <game>\PSE\Originals (pse_build OriginalsDir: DLLs moved there once processed); a name in
    // both counts once - the game folder's copy (sorted first below, then unique by file name)
    for (const std::wstring& dir : {gameDir, gameDir + L"\\PSE\\Originals"})
    for (const wchar_t* pat : {L"Mission*.dll", L"Training*.dll"}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\" + pat).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(dir + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    {   // one entry per file name, the game folder's first
        auto fileName = [](const std::wstring& p) { return p.substr(p.find_last_of(L"\\/") + 1); };
        std::vector<std::wstring> unique;
        for (const std::wstring& p : out) {
            bool dup = false;
            for (const std::wstring& u : unique) dup |= _wcsicmp(fileName(u).c_str(), fileName(p).c_str()) == 0;
            if (!dup) unique.push_back(p);
        }
        out.swap(unique);
    }
    // Python: sorted by os.path.normcase (lower-case full path)
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {   // by file name
        const wchar_t* x = a.c_str() + a.find_last_of(L"\\/") + 1;
        const wchar_t* y = b.c_str() + b.find_last_of(L"\\/") + 1;
        return _wcsicmp(x, y) < 0;
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }),
              out.end());
    return out;
}

std::wstring DefaultMissionsDir(const std::wstring& gameDir) {
    auto writable = [](const std::wstring& dir) {
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        const std::wstring probe = dir + L"\\.pse-write-test";
        HANDLE f = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        CloseHandle(f);
        return true;
    };
    std::wstring dir = gameDir;
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    const std::wstring cache = dir + L"\\PSE\\Cache";
    if (writable(cache)) {
        SHCreateDirectoryExW(nullptr, (cache + L"\\missions").c_str(), nullptr);
        return cache + L"\\missions";
    }
    std::wstring lower = dir;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
    const std::string h = Sha256Hex(std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(lower.data()),
                                                         reinterpret_cast<const uint8_t*>(lower.data() + lower.size())));
    wchar_t* local = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) base = local;
    CoTaskMemFree(local);
    const std::wstring out = base + L"\\PSE\\ds1-" + WidenUtf8(h.substr(0, 8)) + L"\\missions";
    SHCreateDirectoryExW(nullptr, out.c_str(), nullptr);
    return out;
}

CacheResult EnsureCached(const std::wstring& dllPath, const std::wstring& dir, bool force) {
    CacheResult r;
    std::vector<uint8_t> dll;
    if (!ReadFileBytes(dllPath, &dll)) {
        r.error = "can't read " + NarrowUtf8(dllPath);
        return r;
    }
    r.sha256 = Sha256Hex(dll);
    const std::wstring file = FileName(dllPath);
    const size_t dot = file.find_last_of(L'.');
    const std::wstring stem = dot == std::wstring::npos ? file : file.substr(0, dot);
    r.luaPath = dir + L"\\" + stem + L".lua";
    const std::wstring stampPath = dir + L"\\" + stem + L".stamp";
    const std::string want = Stamp(r.sha256, dll.size());
    if (!force) {
        std::vector<uint8_t> have;
        if (ReadFileBytes(stampPath, &have) && std::string(have.begin(), have.end()) == want &&
            GetFileAttributesW(r.luaPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            r.ok = true;
            return r;
        }
    }
    std::string lua;
    if (!Translate(dll, NarrowUtf8(file), &lua, &r.stats, &r.error)) return r;
    if (g_luaCheck) {
        const std::string err = g_luaCheck(lua, "=" + NarrowUtf8(stem));
        if (!err.empty()) {
            r.error = "generated Lua doesn't compile: " + err;
            return r;
        }
    }
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    DeleteFileW(stampPath.c_str());   // stamp last: a half-written pair never counts as valid
    if (!WriteAll(r.luaPath, lua) || !WriteAll(stampPath, want)) {
        r.error = "can't write " + NarrowUtf8(r.luaPath);
        return r;
    }
    r.ok = r.generated = true;
    return r;
}

}  // namespace pse::m2l
