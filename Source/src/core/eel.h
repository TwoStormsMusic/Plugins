// Helpers that reproduce EEL2 (JSFX) semantics exactly, so ported code computes the same numbers.
//   ==  is |a - b| <  0.00001         (eq)
//   !=  is |a - b| >= 0.00001         (ne)
//   a value is true when |a| >= 0.00001 (truth)
//   === and !== are exact (plain C++ == and !=)
#pragma once
#include <cmath>
#include <cstdint>

namespace ts {

constexpr double EEL_CLOSE = 0.00001;
constexpr double PI = 3.141592653589793;

inline bool eq(double a, double b) { return std::fabs(a - b) < EEL_CLOSE; }
inline bool ne(double a, double b) { return std::fabs(a - b) >= EEL_CLOSE; }
inline bool truth(double a) { return std::fabs(a) >= EEL_CLOSE; }
inline double sqr(double a) { return a * a; }
// EEL2 min/max (no NaN games needed here)
inline double emin(double a, double b) { return a < b ? a : b; }
inline double emax(double a, double b) { return a > b ? a : b; }
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
// floor(x + 0.5), the JSFX way of rounding an enum slider
inline int iround(double v) { return (int)std::floor(v + 0.5); }
inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
// x ^ y in EEL2 (an invalid result, like a negative number to a fractional power, is 0 there)
inline double epow(double a, double b)
{
    double r = std::pow(a, b);
    return r != r ? 0 : r;
}
// denormal guard used throughout the JSFX: abs(x) < 1e-21 ? x = 0
inline void flush21(double &x) { if (std::fabs(x) < 0.000000000000000000001) x = 0; }

} // namespace ts
