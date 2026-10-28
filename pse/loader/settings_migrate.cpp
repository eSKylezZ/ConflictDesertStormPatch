// One-time copy of a host's old settings key into its pse.settings key (loader.h MigrateRegistryValues).
// DesertStormFix: HKLM\SOFTWARE\Pivotal Games\Conflict Desert Storm\Enhancements -> HKCU\Software\PSE\ds1\Plugins\dsfix.
#include <windows.h>

#include <cwctype>
#include <set>
#include <vector>

#include "internal.h"

namespace pse {

MigrationResult MigrateRegistryValues(const std::vector<RegistrySource>& sources, HKEY__* dstRoot,
                                      const std::string& dstPath, const char* marker) {
    MigrationResult r;
    if (!dstRoot || dstPath.empty() || !marker || !*marker) return r;
    HKEY dst;
    if (RegCreateKeyExW(dstRoot, detail::Widen(dstPath).c_str(), 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                        nullptr, &dst, nullptr) != ERROR_SUCCESS)
        return r;
    const std::wstring markerW = detail::Widen(marker);
    if (RegQueryValueExW(dst, markerW.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        RegCloseKey(dst);
        r.ok = true;
        return r;
    }
    r.ran = true;
    std::set<std::wstring> seen;   // names handled by an earlier source (case-insensitive like the registry)
    auto lower = [](std::wstring s) {
        for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
        return s;
    };
    bool allOk = true;
    for (size_t i = 0; i < sources.size(); ++i) {
        const RegistrySource& s = sources[i];
        HKEY src = nullptr;
        if (!s.root || RegOpenKeyExW(s.root, detail::Widen(s.path).c_str(), 0, KEY_READ | s.extraAccess, &src) !=
                           ERROR_SUCCESS)
            continue;
        ++r.sourcesFound;
        DWORD count = 0, maxName = 0, maxData = 0;
        RegQueryInfoKeyW(src, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &count, &maxName, &maxData, nullptr,
                         nullptr);
        std::vector<wchar_t> name(maxName + 2);
        std::vector<BYTE> data(maxData + 2);
        for (DWORD k = 0;; ++k) {
            DWORD nameLen = static_cast<DWORD>(name.size()), dataLen = static_cast<DWORD>(data.size()), type = 0;
            LSTATUS e = RegEnumValueW(src, k, name.data(), &nameLen, nullptr, &type, data.data(), &dataLen);
            if (e == ERROR_MORE_DATA) {   // grew since RegQueryInfoKey
                name.resize(name.size() * 2 + 256);
                data.resize(data.size() * 2 + 4096);
                --k;
                continue;
            }
            if (e != ERROR_SUCCESS) break;
            const std::wstring n(name.data(), nameLen);
            if (n.empty()) continue;   // the key's default value isn't a setting
            const std::wstring key = lower(n);
            if (key == lower(markerW) || !seen.insert(key).second) continue;
            const std::string label = detail::Narrow(n);
            if (RegQueryValueExW(dst, n.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                r.kept.push_back(label);
                continue;
            }
            if (RegSetValueExW(dst, n.c_str(), 0, type, data.data(), dataLen) == ERROR_SUCCESS) {
                r.copied.push_back(label + " (source " + std::to_string(i + 1) + ")");
            } else {
                r.failed.push_back(label);
                allOk = false;
            }
        }
        RegCloseKey(src);
    }
    if (allOk) {
        const DWORD one = 1;
        allOk = RegSetValueExW(dst, markerW.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof one) ==
                ERROR_SUCCESS;
    }
    RegCloseKey(dst);
    r.ok = allOk;
    return r;
}

}  // namespace pse
