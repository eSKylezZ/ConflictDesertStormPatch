// Small string helpers shared by the loader services and the host link library (pse/hostlink): no loader state.
#include <windows.h>

#include <cstdio>

#include "internal.h"

namespace pse::detail {

std::string VFormat(const char* fmt, va_list args) {
    if (!fmt) return {};
    char small[512];
    va_list copy;
    va_copy(copy, args);
    int n = std::vsnprintf(small, sizeof small, fmt, copy);
    va_end(copy);
    if (n < 0) return fmt;   // bad format: show it as it is
    if (static_cast<size_t>(n) < sizeof small) return std::string(small, static_cast<size_t>(n));
    std::string big(static_cast<size_t>(n) + 1, '\0');
    std::vsnprintf(big.data(), big.size(), fmt, args);
    big.resize(static_cast<size_t>(n));
    return big;
}

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace pse::detail
