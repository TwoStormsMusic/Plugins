#include "eelrand.h"
#include <atomic>
#include <cmath>
#include <cstdint>

namespace ts {

static std::atomic_flag s_lock = ATOMIC_FLAG_INIT;

static uint32_t genrand_int32()
{
    enum { N = 624, M = 397 };
    static const uint32_t mag01[2] = {0x0u, 0x9908b0dfu};
    static uint32_t mt[N];
    static uint32_t idx = 0;
    uint32_t y;
    uint32_t mti = idx;
    if (!mti) {
        uint32_t s = 0x4141f00d;
        mt[0] = s;
        for (mti = 1; mti < N; mti++) mt[mti] = 1812433253u * (mt[mti - 1] ^ (mt[mti - 1] >> 30)) + mti;
        idx = N;
    }
    if (mti >= N) {
        int kk;
        idx = 1;
        for (kk = 0; kk < N - M; kk++) {
            y = (mt[kk] & 0x80000000u) | (mt[kk + 1] & 0x7fffffffu);
            mt[kk] = mt[kk + M] ^ (y >> 1) ^ mag01[y & 1];
        }
        for (; kk < N - 1; kk++) {
            y = (mt[kk] & 0x80000000u) | (mt[kk + 1] & 0x7fffffffu);
            mt[kk] = mt[kk + (M - N)] ^ (y >> 1) ^ mag01[y & 1];
        }
        y = (mt[N - 1] & 0x80000000u) | (mt[0] & 0x7fffffffu);
        mt[N - 1] = mt[M - 1] ^ (y >> 1) ^ mag01[y & 1];
        mti = 0;
    } else {
        idx++;
    }
    y = mt[mti];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= (y >> 18);
    return y;
}

double eel_rand(double f)
{
    double x = std::floor(f);
    if (x < 1.0) x = 1.0;
    while (s_lock.test_and_set(std::memory_order_acquire)) {}
    uint32_t r = genrand_int32();
    s_lock.clear(std::memory_order_release);
    return (double)r * (1.0 / (double)0xFFFFFFFF) * x;
}

} // namespace ts
