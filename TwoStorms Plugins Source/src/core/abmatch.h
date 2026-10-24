// The A/B set playing, the window's held Swap / Bypass and level matching: the same code in every TwoStorms
// plug-in with A/B sets (the JSFX's c_uiok ... c_trk block and its after-the-Act part), ported line for line.
#pragma once
#include "eel.h"
#include <atomic>
#include <cmath>

namespace ts {

// written by the window, read by the audio thread
struct UiLink {
    std::atomic<double> beat{0};   // c_uibeat: the hold buttons count while this is fresh
    std::atomic<int> hold{0};      // c_uihold: 1 = Bypass held/latched, 2 = Swap
    std::atomic<double> last{0};   // g_last: the window is open (meters, level matching, history run)
};

class AbMatch {
public:
    bool uiok = false, bph = false, swh = false, lat = false, side = false, uion = false;
    double LMI[2] = {}, LMO[2] = {}, LMV[2] = {};

    // @block: latched = P_AB >= 0.5
    void block(const UiLink &U, double now, bool latched, int samplesblock, double srate)
    {
        uiok = now - U.beat.load(std::memory_order_relaxed) < 0.5;
        int hold = U.hold.load(std::memory_order_relaxed);
        bph = uiok && hold == 1;
        swh = uiok && hold == 2;
        lat = latched;
        side = swh ? !lat : lat;
        uion = now - U.last.load(std::memory_order_relaxed) < 0.5;
        // level matching (about 1.5 s, only while the window is open), per set: how much louder the plug-in makes
        // things (not for 0.3 s after the playing set changes: it is still settling)
        int sd = side ? 1 : 0;
        if (sd != lside) { lside = sd; lmskip = 0.3 * srate; }
        if (lmskip > 0) { lmskip -= samplesblock; ei = 0; }
        if (samplesblock != lbs || srate != lbsr) { lbs = samplesblock; lbsr = srate; klm = 1 - std::exp(-samplesblock / (1.5 * srate)); }
        if (ei > 0.0000001 * samplesblock) {
            LMI[sd] += klm * (ei / samplesblock - LMI[sd]);
            LMO[sd] += klm * (eo / samplesblock - LMO[sd]);
            LMV[sd] = 1;
        }
        ei = 0;
        eo = 0;
        // held Bypass: the raw input at the playing set's loudness; held Swap: the other set at this one's
        bpg = (truth(LMV[sd]) && LMI[sd] > 0) ? std::sqrt(LMO[sd] / LMI[sd]) : 1;
        int la = lat ? 1 : 0;
        trt = (swh && truth(LMV[0]) && truth(LMV[1]) && LMI[0] > 0 && LMI[1] > 0 && LMO[0] > 0 && LMO[1] > 0)
                  ? std::sqrt((LMO[la] / LMI[la]) / (LMO[!la] / LMI[!la]))
                  : 1;
        bps = 1 / (0.01 * srate);
        trk = 1 - std::exp(-1 / (0.01 * srate));
    }
    // after the built-in Act, per sample: r = the input as level matching measures it, s = the output (in place),
    // q = what Bypass plays (the input, delayed like the audio)
    inline void post(double r0, double r1, double &s0, double &s1, double q0, double q1)
    {
        if (uion) { ei += r0 * r0 + r1 * r1; eo += s0 * s0 + s1 * s1; }
        if (bpv > 0 || bph || ne(tr, 1) || ne(trt, 1)) {
            bpv = emax(0, emin(1, bpv + (bph ? bps : -bps)));
            tr += (trt - tr) * trk;
            if (std::fabs(tr - trt) < 0.00001) tr = trt;
            s0 = s0 * tr * (1 - bpv) + q0 * bpg * bpv;
            s1 = s1 * tr * (1 - bpv) + q1 * bpg * bpv;
        }
    }

private:
    int lside = 0, lbs = -1;
    double lmskip = 0, lbsr = -1, klm = 0, ei = 0, eo = 0;
    double bpg = 1, trt = 1, tr = 1, bpv = 0, bps = 0, trk = 0;
};

} // namespace ts
