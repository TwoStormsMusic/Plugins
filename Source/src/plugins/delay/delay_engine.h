// TwoStorms Delay's sound: three delay lines (A, B and C), each with its own left and right time (free or at the
// tempo), feedback (with a leveler), ping-pong or stereo with cross feedback, up to four taps, level, pan and width.
// In each loop: pitch shift, diffusion, high pass / low pass / bell, a character (tape, BBD, lo-fi) with wow and
// flutter, and saturation (24 styles, at twice the rate in High quality); reverse plays each repeat backwards (on the
// way out, so every repeat is reversed). Next to each other, two lines run in parallel, in series (the first's repeats
// into the second) or linked (their repeats cross by the Link amount); ducking (the repeats and the reverb each by its
// own amount), freeze, a shared reverb with shimmer, dry and wet levels, and eight modulation slots (LFO, envelope
// follower, MIDI). Ported line for line from twostorms_delay.jsfx (two lines); Line C, the routing per pair, the
// reverse, the levels and the ducking split are the CLAP's own.
#pragma once
#include "../../core/abmatch.h"
#include "../../core/act.h"
#include "../../core/plugin.h"
#include "../../gui/analyzer.h"
#include <atomic>
#include <vector>

namespace ts {
namespace delay {

enum {
    P_SEL = 43, P_C23 = 44, P_C12 = 45, P_IN = 46, P_DRY = 47, P_LK23 = 48, P_AB = 49, P_DUCK = 50, P_DTHR = 51, P_DATT = 52,
    P_DREL = 53, P_DSRC = 54, P_FREEZE = 55, P_MSEL = 56, P_MODOPEN = 57, P_LK12 = 58, P_AN = 59, P_RVB = 60, P_RDEC = 61,
    P_RSIZ = 62, P_RDMP = 63, P_RSHM = 64, P_RSHP = 65, P_RPRE = 66, P_HQ = 67, P_GRP = 68, P_WET = 69, P_RDUCK = 254,
};
// P_C12 / P_C23: how A and B, and B and C, connect: 0 parallel, 1 series (the first's repeats into the second), 2 linked
constexpr int EB0 = 70, EBN = 40, NL = 3, MB0 = 190, MN = 8, NMOD = 8, NPAR = 255;
// a line's parameters: EB0 + line * EBN + K_*
enum {
    K_ON = 0, K_TL, K_SL, K_TR, K_SR, K_LINK, K_FB, K_CROSS, K_MODE, K_LEVEL, K_PAN, K_WIDTH, K_CHAR, K_AGE, K_WOW, K_FLUT, K_GLIDE,
    K_DRIVE, K_SAT, K_HP, K_LP, K_BF, K_BG, K_BQ, K_DIFF, K_DSZ, K_SH, K_SEMI, K_SHSZ, K_TAPS, K_TPAT, K_TLVL, K_SHSY, K_RSND,
    K_COUNT
};
// a modulation slot's parameters: MB0 + slot * MN + M_*
enum { M_TYPE = 0, M_TLINE, M_TPAR, M_AMT, M_RATE, M_RATE2, M_OPT, M_SYNC };
inline int ep(int b, int k) { return EB0 + b * EBN + k; }
inline int mp(int s, int k) { return MB0 + s * MN + k; }

extern const double YBEATS[17];   // a note value's length in beats (the sync settings)
extern const int MTP[12];         // the line parameter each modulation target moves

// what the window reads
struct Display {
    std::atomic<float> eff[NPAR];   // the settings as the modulation has them now
    std::atomic<float> mv[NMOD];    // each slot's value now
    std::atomic<double> tempo{120};
    std::atomic<double> mi0{-200}, mi1{-200};   // input peaks (after Input), dB
    std::atomic<int> side{0};
    AnFeed an;
    Display()
    {
        for (auto &e : eff) e.store(0);
        for (auto &m : mv) m.store(0);
    }
};

class EngineHost {
public:
    virtual ~EngineHost() {}
    virtual double pv(int id) const = 0;
    virtual double abv(int id) const = 0;
    virtual double plo(int id) const = 0;
    virtual double phi(int id) const = 0;
};

class Engine {
public:
    Engine(EngineHost &h, Act &a, UiLink &u, Display &d) : H(h), A(a), U(u), V(d) {}
    AbMatch ab;

    void init(double sr);
    void reset();
    // @block, the delay's part (before the Act's)
    void block(int samplesblock, const Transport &tr, const std::vector<MidiEv> &midi);
    void afterAct();
    // @sample, the delay's part (before the Act); sc: the sidechain (or null)
    void samples(const double *const *in, const double *const *sc, double *const *out, int n);
    // after the Act: held Bypass / Swap, input meters, the analyzer's rings
    void post(double *const *out, int n);

private:
    EngineHost &H;
    Act &A;
    UiLink &U;
    Display &V;
    double srate = 44100;
    bool nonceDrawn = false, idDrawn = false, anw = false;
    std::vector<double> br0, br1;

    // buffers (sizes are powers of two)
    int64_t YDLEN = 1, YDM = 0, YSLEN = 1, YSM = 0, YALEN = 1, YAM = 0;
    int64_t RLEN = 1, RM = 0, RPLEN = 1, RPM = 0, RAL = 1, RAM = 0, RSL = 1, RSM = 0;
    std::vector<double> YDB, YSBF, YABF, RVB, RPB, RAP, RSB;
    // per channel (line * 2 + side) and per line
    static constexpr int NC = 2 * NL;
    double YCUR[NC] = {}, YTGT[NC] = {}, YLO[NC] = {}, YLP[NC] = {}, YDX[NC] = {}, YDY[NC] = {}, YMD[NC * 8] = {}, YWR[NC] = {}, YFR[NC] = {};
    double YCZ[NC * 4] = {}, YFZ[NC * 6] = {}, YFC[NL * 15] = {}, YFEN[NL * 3] = {};
    double YON[NL] = {}, YMODE[NL] = {}, YGK[NL] = {}, YTM[NL] = {};
    double YFBc[NL] = {}, YFBt[NL] = {}, YXc[NL] = {}, YXt[NL] = {}, YLVc[NL] = {}, YLVt[NL] = {}, YPNc[NL] = {}, YPNt[NL] = {}, YWDc[NL] = {}, YWDt[NL] = {};
    double YNT[NL] = {}, YTP[NL * 4] = {}, YTL[NL * 4] = {};
    double YCHR[NL] = {}, YCA[NL] = {}, YNZ[NL] = {}, YBQ[NL] = {}, YHD[NL] = {}, YWA[NL] = {}, YFA[NL] = {}, YBA[NL] = {};
    double YSHON[NL] = {}, YSHR[NL] = {}, YSHW[NL] = {}, YSHP[NL] = {}, YSHI[NL] = {}, YSD1[NL] = {}, YSD2[NL] = {}, YSW1[NL] = {};
    double YSHB[NL] = {}, YSHBI[NL] = {}, YSHE[NL] = {}, YSX[NC * 2] = {}, YSHOFF[NL] = {};
    double YDFON[NL] = {}, YDFG[NL] = {}, YDFD[NC * 4] = {}, YDFM[NC * 4] = {}, YDFP[NC * 4] = {}, YDFI[NC * 4] = {}, YDFOFF[NL] = {}, YDFY[NC * 4] = {};
    double YSTc[NL] = {}, YDGc[NL] = {}, YDGt[NL] = {}, YCPc[NL] = {}, YCPt[NL] = {}, YSBc[NL] = {}, YSBt[NL] = {};
    double YBMC[NL * 5] = {}, YBMZ[NC * 2] = {}, YBEN[NL] = {}, YBCH[NL * 2] = {};
    double YFE[NL] = {}, YFG[NL] = {};
    double YOS[NC * 64] = {};
    double YRSc[NL] = {}, YRSt[NL] = {};
    double YLCH[NL * 12] = {};
    // reverse (the CLAP's own): each repeat backwards, on the way out. Per line: on, the speed (1 + the pitch
    // ratio), chunks on the beat; per channel: the phase in its chunk and the chunk's length (samples)
    bool RVON[NL] = {}, RVBT[NL] = {};
    double RVSP[NL] = {}, RVBI[NL] = {}, RVBW[NL] = {}, RVPH[NC] = {}, RVW[NC] = {}, RVWP[NC] = {}, RVE = 0.015;
    // routing (the CLAP's own): 0 parallel, 1 series, 2 linked, for A-B and B-C; the link amounts
    int y_con[NL - 1] = {};
    double y_lkc[NL - 1] = {}, y_lkt[NL - 1] = {};
    // the reverb
    double YRD[8] = {}, YRG[8] = {}, RZ[8] = {}, RVM[4] = {}, RVI[4] = {}, YRAD[6] = {}, YRCH[4] = {};
    double YRSH = 0, YRSE = 0, YRSP = 0, YRSI = 0, YRSR = 2, YRSW = 0, YRPD = 0, YRDA = 0.5, YRIG = 0.3, YROG = 1, YRMD = 0;
    double y_rl = 0, y_rr = 0, y_rvc = 0, y_rvt = 0, y_rvoff = 100000, y_rsl = 0, y_rsr = 0;
    // modulation
    double EFFP[256] = {}, MV[NMOD] = {}, MPH[NMOD] = {}, MRA[NMOD] = {}, MRB[NMOD] = {}, MEN[NMOD] = {}, MKA[NMOD] = {}, MKR[NMOD] = {};
    double MCC[130] = {}, MLS[NMOD] = {}, MSR[NMOD] = {};
    int MFL[NMOD] = {};
    int d_nf = 0;
    double d_beat = 0, d_nvel = 0, d_nheld = 0, d_pb = 0;
    // the rest
    int64_t y_wp = 0;
    int y_tc = 0;
    bool y_snap = true, y_hq = true, y_dsrc = false;
    double y_frc = 0, y_frt = 0, y_dgc = 1, y_dgt = 1, y_mxc = 0, y_mxt = 0, y_dyc = 1, y_dyt = 1, y_den = 0, y_ingc = 1, y_ingt = 1;
    double y_wetabs = 0, y_hql = 7, y_bt = 0, y_bps = 0.000005, y_wl = 0, y_wr = 0, y_tempo = 120;
    double y_dka = 0, y_dkr = 0, y_dthr = -30, y_dam = 0, y_rdam = 0, y_rgc = 1, y_rgt = 1, y_vl = 0, y_vr = 0;
    double DC_R = 0, YKS = 0, YKF = 0, YBR = 0, YKA = 0, YKR = 0;
    int YAL_ST = 2, YAL_N = 1, YAL_M = 1;
    double c_ip0 = 0, c_ip1 = 0, c_mi0 = -200, c_mi1 = -200;

    double cpv(int p) const { return ab.swh ? H.abv(p) : H.pv(p); }
    double rd(const double *b, int64_t m, double d) const;
    double rd6(const double *b, int64_t m, double d) const;
    double rdq(const double *b, int64_t m, double d) const { return y_hq ? rd6(b, m, d) : rd(b, m, d); }
    double align(const double *o, double dn, double dr) const;
    double proc(int i, int k, double x);
    void rv(double il, double ir);
    void run(double il, double ir);
    double mval(int s);
    void modeval();
    void lprep(int k);
    void tick();
};

} // namespace delay
} // namespace ts
