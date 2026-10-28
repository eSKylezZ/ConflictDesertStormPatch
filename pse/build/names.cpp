// File names for the manifests (optional): the archives store only name hashes. Names are recovered like the
// ModKit's archive.py does: words from the game's exe / DLLs and from the archives' own meshes / text files (texture
// and model names, table rows), each tried as it is and with every extension the game uses; a word whose hash is in a
// manifest names that entry - if its extension agrees with the content (a .DDS name on DDS data ...).
#include <windows.h>

#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pse_build.h"

namespace pse::build {

namespace {
const char* const kExts[] = {"DDS", "EVO", "RFX", "PNG", "TGA", "BMP", "IMG", "FAS", "SPL", "ENV", "TXT", "INI",
                             "PSF", "SBK", "PRS", "PRB", "AED", "DYN", "PI2", "PAX", "PAD", "OCT", "RDA", "RMD",
                             "QTD", "SKL", "DAT", "CSV", "FNT", "WAV", "NAV", "MAP", "GZ",  "TEXT", "SCH", "KEY",
                             "DDL", "LOC"};

bool Printable(uint8_t c) { return c >= 0x21 && c <= 0x7E; }
bool NameChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}
bool Separator(char c) {
    return c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '=' || c == ',' || c == ';' || c == '(' || c == ')' ||
           c == '<' || c == '>' || c == '|';
}

// One printable run -> its tokens (split at separators, path folders and leading @ / # dropped).
void Tokens(const char* s, size_t n, std::unordered_set<std::string>* words) {
    size_t i = 0;
    while (i < n) {
        while (i < n && Separator(s[i])) ++i;
        size_t j = i;
        while (j < n && !Separator(s[j])) ++j;
        std::string tok(s + i, j - i);
        i = j;
        const size_t slash = tok.find_last_of("\\/");
        if (slash != std::string::npos) tok = tok.substr(slash + 1);
        while (!tok.empty() && (tok.front() == '@' || tok.front() == '#')) tok.erase(tok.begin());
        while (!tok.empty() && (tok.back() == '@' || tok.back() == '#')) tok.pop_back();
        if (tok.size() < 2 || tok.size() > 64) continue;
        bool ok = true;
        for (char& c : tok) {
            if (!NameChar(c)) {
                ok = false;
                break;
            }
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        }
        if (ok) words->insert(std::move(tok));
    }
}
}  // namespace

const char* ContentKind(const uint8_t* b, uint32_t size) {
    if (size < 4) return "BIN";
    if (!std::memcmp(b, "DDS ", 4)) return "DDS";
    if (!std::memcmp(b, "EOBJ", 4)) return "EVO";
    if (!std::memcmp(b, "\x89PNG", 4)) return "PNG";
    if (b[0] == 'B' && b[1] == 'M') return "BMP";
    if (!std::memcmp(b, "REFX", 4) || (b[0] == '/' && b[1] == '/')) return "RFX";
    if (size >= 32 && !std::memcmp(b, "\x02\x00\x00\x00", 4)) {
        uint32_t n, bounds;
        std::memcpy(&n, b + 4, 4);
        std::memcpy(&bounds, b + 8, 4);
        if (n > 0 && n < 256 && bounds < 64) return "PRB";
    }
    return "BIN";
}

void CollectWords(const uint8_t* data, size_t size, std::unordered_set<std::string>* words) {
    // ASCII runs of >= 3 printable characters.
    size_t i = 0;
    while (i < size) {
        if (!Printable(data[i])) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < size && Printable(data[j])) ++j;
        if (j - i >= 3) Tokens(reinterpret_cast<const char*>(data + i), j - i, words);
        i = j;
    }
    // UTF-16LE runs (text tables).
    std::string run;
    for (size_t k = 0; k + 1 < size; k += 2) {
        if (Printable(data[k]) && data[k + 1] == 0) {
            run += static_cast<char>(data[k]);
            continue;
        }
        if (run.size() >= 3) Tokens(run.data(), run.size(), words);
        run.clear();
    }
    if (run.size() >= 3) Tokens(run.data(), run.size(), words);
}

void RecoverNames(const std::wstring& gameDir, std::unordered_set<std::string>* words,
                  const std::unordered_map<uint32_t, const char*>& wanted, uint32_t (*hash)(const char*),
                  std::unordered_map<uint32_t, std::string>* out, const std::function<bool(uint32_t, uint32_t)>& tick) {
    // The game's code names many files too (FrontEnd.dll, mission DLLs, the exe).
    for (const std::wstring& dir : {gameDir, OriginalsDir(gameDir)})   // mission DLLs may have been moved
    for (const wchar_t* pattern : {L"*.exe", L"*.dll"}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.nFileSizeHigh || fd.nFileSizeLow > (32u << 20) || _wcsnicmp(fd.cFileName, L"unins", 5) == 0) continue;
            HANDLE f = CreateFileW((dir + L"\\" + fd.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
            if (f == INVALID_HANDLE_VALUE) continue;
            std::vector<uint8_t> data(fd.nFileSizeLow);
            DWORD got = 0;
            if (ReadFile(f, data.data(), fd.nFileSizeLow, &got, nullptr) && got == fd.nFileSizeLow)
                CollectWords(data.data(), data.size(), words);
            CloseHandle(f);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    auto offer = [&](const std::string& name, const char* ext) {
        const uint32_t h = hash(name.c_str());
        auto it = wanted.find(h);
        if (it == wanted.end() || out->count(h)) return;
        // The content must agree with the extension (an unknown kind accepts any name).
        if (std::strcmp(it->second, "BIN") != 0 && std::strcmp(it->second, ext) != 0) return;
        out->emplace(h, name);
    };
    std::unordered_set<std::string> stems;
    for (const std::string& w : *words) {
        const size_t dot = w.rfind('.');
        if (dot != std::string::npos) {
            const std::string ext = w.substr(dot + 1);
            for (const char* e : kExts)
                if (ext == e) {
                    offer(w, e);
                    break;
                }
            stems.insert(w.substr(0, dot));
        } else {
            stems.insert(w);
        }
    }
    // (the slow part: every stem with every extension) - tick(done, total) every 4096 stems; false = stop
    uint32_t n = 0;
    const uint32_t total = static_cast<uint32_t>(stems.size());
    for (const std::string& stem : stems) {
        if ((++n & 4095) == 0 && tick && !tick(n, total)) return;
        if (stem.empty()) continue;
        for (const char* e : kExts) offer(stem + "." + e, e);
    }
}

}  // namespace pse::build
