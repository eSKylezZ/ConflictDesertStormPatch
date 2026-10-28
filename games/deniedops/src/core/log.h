#pragma once

// DeniedOpsFix.log next to the exe, rewritten every launch (diagnostics only, not a settings file). Gets the loader /
// layer / plugin lines too (deniedopsfix.cpp's log sink). Compiled out of distribution builds (DS_DIST): those leave
// the lines to the loader's PSE\Logs\loader.log.
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
