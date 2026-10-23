// The Act's transient shaper (twostorms_act.jsfx, @block and @sample), ported line for line:
// 1 to 3 bands (Linkwitz-Riley crossovers from state-variable filters), attack and sustain per band
// and gain group, optional lookahead. Runs before the Act's gain; costs nothing while off.
#pragma once
#include <vector>

namespace ts {
namespace actp {

enum ShaperParam {
    P_TSON = 43, P_TSATT = 44, P_TSSUS = 45, P_TSX1 = 50, P_TSX2 = 51, P_TSXO1 = 53, P_TSXO2 = 54, P_TSLA = 55, P_TSLAMS = 56,
};

struct ShaperIn {
    bool sw;                 // switched on
    bool xo1, xo2;           // crossovers ticked
    double att[3], sus[3];   // per band, in %
    double x1, x2;           // crossover boxes (Hz)
    bool la;                 // lookahead
    double lams;
};

class Shaper {
public:
    Shaper();
    // @block, after the Act's: nchs = a_nchs; groups from the Act (a_groups(-1, ...))
    void block(const ShaperIn &in, double srate, int nchs, int ng, const int *ga, const int *gn);
    // @sample, over the block, in place on ch[0..nchs)
    void process(double *const *ch, int nchs, int n);
    static int latency(const ShaperIn &in, double srate);

    // for the window: the most each band went up or down in the last block (dB)
    double TSMD[3] = {};
    bool running() const { return run; }

private:
    static constexpr int TSLN = 4096, TSLM = TSLN - 1;
    bool sw = false, on = false, run = false, was = false, fast = false;
    int nb = 1, nbw = 0, ld = 0, lp = 0, ng = 0;
    double warm = 0, mix = 0, sr = 0;
    double kfa = 0, ksa = 0, kfr = 0, ksr = 0, kg = 0, kmix = 0;
    double xc1 = 0, xc2 = 0, fa = 0, fb = 0;
    double TSA[3] = {}, TSU[3] = {}, TSXC[6] = {}, TSBX[3] = {1, 1, 1}, TSBN[3] = {1, 1, 1};
    double TSHS[3] = {};
    int TGA[8] = {}, TGN[8] = {};
    double TSGI[8] = {};
    double TSX[80] = {}, TSF[24] = {}, TSS[24] = {}, TSL[24] = {}, TSH[24] = {}, TSG[24] = {}, TSB[24] = {};
    std::vector<double> LAB;
    double x1a = 0, x1b = 0, x1c = 0, x2a = 0, x2b = 0, x2c = 0;
    // the fast path (one linked stereo pair): its own state, as in the JSFX
    double tl1a = 0, tl1b = 0, tl2a = 0, tl2b = 0, tl3a = 0, tl3b = 0, tl4a = 0, tl4b = 0, tl5a = 0, tl5b = 0;
    double tr1a = 0, tr1b = 0, tr2a = 0, tr2b = 0, tr3a = 0, tr3b = 0, tr4a = 0, tr4b = 0, tr5a = 0, tr5b = 0;
    double tf0 = 0, ts0 = 0, tr0 = 0, th0 = 0, tf1 = 0, ts1 = 0, tr1 = 0, th1 = 0, tf2 = 0, ts2 = 0, tr2 = 0, th2 = 0;
    double tg0 = 1, tg1 = 1, tg2 = 1;
    // the JSFX's band globals persist between samples (and are written to the delay even when unused)
    double b00 = 0, b01 = 0, b10 = 0, b11 = 0, b20 = 0, b21 = 0;
    double bp_ = 0;   // ts_bp
    void xo(double f, int i);
    double tsvf(double x, double *st, double a1, double a2, double a3);
};

} // namespace actp
} // namespace ts
