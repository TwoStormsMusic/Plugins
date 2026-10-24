// EEL2's rand(x): one Mersenne Twister for the whole process with a fixed seed (0x4141f00d), as in REAPER, where
// every JSFX draws from the same generator. Ported from WDL's nseel-cfunc.c so ported plug-ins draw the same numbers.
#pragma once

namespace ts {
double eel_rand(double x);   // 0 <= result < floor(x) (x < 1: 1)
}
