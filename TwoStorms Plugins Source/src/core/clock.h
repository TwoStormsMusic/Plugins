// A machine-wide monotonic clock in seconds (the JSFX's time_precise()). Every process on the
// machine reads the same clock, so heartbeats in shared memory compare across processes.
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace ts {

inline double now_sec()
{
#ifdef _WIN32
    static double inv = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1.0 / (double)f.QuadPart; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * inv;
#else
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#endif
}

} // namespace ts
