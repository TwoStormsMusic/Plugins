// TwoStorms Multiband's sound: one to four bands split by Linkwitz-Riley crossovers (zero latency, in phase and flat
// when added back) or linear-phase FIR crossovers (the bands add back to exactly the input, delayed). Per band: on,
// solo, compress or expand, threshold, ratio, knee, attack, hold, release, range, makeup (by hand or auto) and a
// Character (Clean VCA, Opto, FET, Vari-mu or Custom: envelope, release mode, curve, feedback, saturation). For every
// band: peak / RMS / hybrid detection, mono / stereo / mid-side / left-right with a stereo link, main input or
// sidechain, input, dry/wet, delta, optional lookahead; Set thresholds. Ported line for line from
// twostorms_multiband.jsfx.
#pragma once
#include "../../core/abmatch.h"
#include "../../core/act.h"
#include "../../core/plugin.h"
#include "../../gui/analyzer.h"
#include <atomic>
#include <vector>

namespace ts {
namespace mb {

enum {
    P_X1 = 50,   // crossover i (0..2): frequency P_X1 + 2i, used (ticked) P_X1 + 2i + 1
    P_XM = 56, P_IN = 57, P_MIX = 58, P_DET = 59, P_RMST = 60, P_CMODE = 61, P_SLINK = 62, P_LKL = 63, P_SC = 64,
    P_SCT = 65, P_LA = 66, P_LAMS = 67, P_DELTA = 68, P_TTYPE = 70, P_TTIME = 71, P_AB = 80, P_SEL = 82, P_BLEND = 84,
    P_AN = 86,   // the window's analyzer (not in the JSFX)
    P_TGR0 = 170,   // each band's Target GR (170..173)
};
constexpr int MBB = 90, MBK = 20, NB = 4;
// band b (counted from the lowest band in use): parameter MBB + b * MBK + K_*
enum {
    K_ON = 0, K_SOLO, K_MODE, K_THR, K_RAT, K_KNEE, K_ATT, K_REL, K_RNG, K_MK, K_AMK, K_HLD,
    K_CHAR, K_BASE, K_ENV, K_RELM, K_CURVE, K_FB, K_SAT, K_SATT,
};
inline int bp(int b, int k) { return MBB + b * MBK + k; }
// the latency (samples): linear phase 3 x its block, or the lookahead
int latency(bool lin, bool la, double lams, double sr);

// what the window reads
struct Display {
    std::atomic<float> bgr[NB];     // each band's largest gain change in the last block (dB, signed)
    std::atomic<float> blv[NB];     // each band's loudest detector level in the last block (dB)
    std::atomic<float> bmka[NB];    // each band's auto makeup (dB)
    std::atomic<double> mi0{-200}, mi1{-200};
    std::atomic<int> side{0};
    AnFeed an;
    Display()
    {
        for (int b = 0; b < NB; ++b) { bgr[b].store(0); blv[b].store(-200); bmka[b].store(0); }
    }
};

class EngineHost {
public:
    virtual ~EngineHost() {}
    virtual double pv(int id) const = 0;
    virtual double abv(int id) const = 0;
    virtual void setAudio(int id, double v) = 0;   // Set thresholds
};

class Engine {
public:
    Engine(EngineHost &h, Act &a, UiLink &u, Display &d) : H(h), A(a), U(u), V(d) {}
    AbMatch ab;

    void init(double sr);
    void reset();
    void block(int samplesblock);
    void afterAct();
    void samples(const double *const *in, const double *const *sc, double *const *out, int n);
    void post(double *const *out, int n);
    // the Act's Set command (the window's button, or the Gain Stage's Set row): starts when a band is on
    void tgStart() { if (m_anyon) A.tgs = 1; }
    // what the Gain Stage's Set row shows: the bands in use and on
    int bandsUsed() const { return m_nb; }
    bool bandOn(int b) const { return b < m_nb && BON[b]; }

private:
    EngineHost &H;
    Act &A;
    UiLink &U;
    Display &V;
    double srate = 44100;
    bool nonceDrawn = false, idDrawn = false;
    std::vector<double> br0, br1, bx0, bx1, bq0, bq1;

    // the bands
    double XC[9] = {}, XF[3] = {}, XS[72] = {}, SLR[36] = {};
    double BA[8] = {}, BD[8] = {}, BAU[8] = {}, BG[8] = {}, BGU[8] = {}, BGI[8] = {};
    bool BON[NB] = {}, BSOL[NB] = {}, BEXP[NB] = {}, BENVS[NB] = {}, BSATON[NB] = {}, BSATT[NB] = {}, BMODE2[NB] = {}, BFB[NB] = {};
    double BTHR[NB] = {}, BSL[NB] = {}, BKN[NB] = {}, BRNG[NB] = {}, BSGN[NB] = {}, BMK[NB] = {}, BAA[NB] = {}, BAR[NB] = {};
    double BRR[NB] = {}, BARF[NB] = {}, BARS[NB] = {}, BASA[NB] = {}, BDR[NB] = {}, BFBK[NB] = {};
    int BCV[NB] = {}, BRELM[NB] = {};
    double MST[128] = {}, BHT[NB] = {}, BYS[NB] = {}, BYN[NB] = {}, BMKA[NB] = {}, BGR[NB] = {}, BGRD[NB] = {}, BLV[NB] = {};
    double BSD[NB] = {}, BSB[NB] = {}, BSC[NB] = {}, BSG[NB] = {}, SATS[32] = {};
    std::vector<double> ZLA;
    // linear phase
    std::vector<double> LDS, LHT, LAC, LFD, LTM, LFFP, LIB, LPO, LPX, DLY, DLR, GRG;
    // Set thresholds
    std::vector<double> TGLV;
    double TGH[640] = {}, TGMX[NB] = {};

    // per block
    int m_nb = 1, m_nbw = -1, m_linw = -1, m_lb = 1024, m_lat = 3072, m_ld = 0, c_ld = 0, m_gd = 0, m_mode = 1, m_detm = 0, m_tk = 4;
    double m_srw = -1, m_x[3] = {-1, -1, -1};
    bool m_lin = false, m_lrb = true, c_ext = false, c_del = false, m_mono = false, m_ms = false, m_lkl = false, m_one = false;
    bool m_pwr = false, c_la = false, m_anyon = false, m_anysolo = false, e_first = true, anw = false;
    double m_lrbt = 1, c_lin = -1000000, c_ing = 1, c_mix = 1, m_sct = 1, m_lk = 1, c_latms = 0, m_blend = 0.5, m_rmst = 20;
    double m_rk = 0, m_tw = 0, m_tki = 0.25, m_cr = 12000, m_ska = 0, m_skr = 0, e_dck = 0;
    double c_dm = 0, c_wmt = 1, c_zk = 0, c_ings = 1, c_dms = 0, c_wms = 1;
    double c_mi0 = -200, c_mi1 = -200, c_ip0 = 0, c_ip1 = 0, c_dlv = -200, c_vdot = 0;
    // per sample
    int m_tc = 0, m_zp = 0, m_dp = 0, m_gp = 0;
    double m_rd0 = 0, m_rd1 = 0, m_dx0 = 0, m_dx1 = 0;
    // linear phase
    int m_lcur = 0, m_lpos = 0, m_lbank = 0;
    bool m_lfirst = true, m_lnew = false, m_lxf = false;
    // Set thresholds
    int c_tgn = 0, c_tc = 0, c_tms = 48, c_tgdur = 5000, c_tgi = 0, c_tgb = -1, c_tit = 0, c_spos = 0, c_shs = 0, c_srm = 0;
    bool c_ttype = false, c_sexp = false, c_s2m = false;
    double c_sy = 0, c_sh = 0, c_s3 = 0, c_s4 = 0, c_s5 = 0, c_ssum = 0, c_scnt = 0, c_ssl = 0, c_skn = 0, c_srg = 0, c_stg = 0;
    double c_saa = 0, c_sar = 0, c_srr = 0, c_sarf = 0, c_sars = 0, c_sasa = 0, c_tlo = -60, c_thi = 0, c_st = -30;
    int c_scv = 0;

    double cpv(int p) const { return ab.swh ? H.abv(p) : H.pv(p); }
    void xoSet(int i, double f);
    void split(double x, double *st, double *out) const;
    void det(int b, double *st, double v);
    double dbOf(double *st);
    double curve(int cv, double o, double kn) const;
    double gain(int b, double *st, double lv);
    double sat(int b, double x, double *st);
    void firBuild();
    void linOut(int bank, double *out);
    void linRun();
    void linClear();
    void simBegin();
    void simRun(int n, int b);
    double simResult();
    void simBand(int b);
    void tgBlock();
};

} // namespace mb
} // namespace ts
