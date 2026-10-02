#include "core/log.h"

#ifndef DS_DIST

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <share.h>

namespace {
FILE* g_file = nullptr;
}

void dslog::Open() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    while (n > 0 && path[n - 1] != '\\') --n;
    path[n] = '\0';
    strcat_s(path, "DesertStormFix.log");
    g_file = _fsopen(path, "w", _SH_DENYWR);
}

void dslog::Write(const char* fmt, ...) {
    static LARGE_INTEGER start, freq;
    if (!start.QuadPart) QueryPerformanceCounter(&start), QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double s = double(now.QuadPart - start.QuadPart) / double(freq.QuadPart);
    char buf[1100];
    const int n = snprintf(buf, sizeof buf, "[%4d:%06.3f] ", static_cast<int>(s / 60), s - 60 * static_cast<int>(s / 60));
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf + n, sizeof buf - n, fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    if (g_file) {
        fprintf(g_file, "%s\n", buf);
        fflush(g_file);
    }
}

#endif  // DS_DIST
