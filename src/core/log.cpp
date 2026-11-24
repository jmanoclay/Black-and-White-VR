#include "core/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

#include "core/paths.h"

namespace bw::log {

static FILE* g_file = nullptr;
static CRITICAL_SECTION g_lock;
static LARGE_INTEGER g_start, g_freq;

void init() {
    InitializeCriticalSection(&g_lock);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    g_file = _wfopen((paths::bwvr_dir() + L"logs\\bwvr.log").c_str(), L"w");
}

void shutdown() {
    if (g_file) fclose(g_file);
    g_file = nullptr;
}

void write(const char* fmt, ...) {
    if (!g_file) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = double(now.QuadPart - g_start.QuadPart) / double(g_freq.QuadPart);
    EnterCriticalSection(&g_lock);
    fprintf(g_file, "[%9.3f] ", t);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_file, fmt, args);
    va_end(args);
    fputc('\n', g_file);
    fflush(g_file);
    LeaveCriticalSection(&g_lock);
}

}  // namespace bw::log
