#pragma once

// GreatEscapeFix.log next to the exe, rewritten every launch. Diagnostics only - not a settings file. Also gets every
// loader / layer / plugin line (tgefix.cpp's log sink). Compiled out of distribution builds (DS_DIST): those leave the
// lines to the loader's PSE\Logs\loader.log.
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
