#pragma once
// ds2fix.widescreen (external PSE plugin) only: games/ds2/src/core/log.h's dslog::Write on top of pse.log (the plugin's
// lines go to the loader's sinks: DesertStorm2Fix.log in ds2fix dev builds, PSE\Logs). Release builds log nothing, as
// the standalone build. Implementation: ../plugin.cpp.
namespace dslog {
#ifdef DS_DIST
inline void Open() {}
template <class... Args>
inline void Write(const char*, const Args&...) {}
#else
inline void Open() {}
void Write(const char* fmt, ...);
#endif
}  // namespace dslog
