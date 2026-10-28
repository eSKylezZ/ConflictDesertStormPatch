#pragma once

// DesertStormFix.log next to the exe, rewritten every launch. Diagnostics only - not a settings file.
// Compiled out of distribution builds (DS_DIST).
namespace dslog {
#ifdef DS_DIST
inline void Open() {}
template <class... Args>
inline void Write(const char*, const Args&...) {}
#else
void Open();
void Write(const char* fmt, ...);
#endif
}  // namespace dslog
