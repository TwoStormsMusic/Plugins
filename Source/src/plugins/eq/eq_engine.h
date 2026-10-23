// TwoStorms EQ's sound: up to 16 bands (bell, shelves, notch, band pass, high and low pass at 6 to 96 dB/oct), each
// with its own frequency, gain and Q, a channel (stereo, left, right, mid, side), an optional gain-dependent Q and
// optional dynamics (compress or expand, from the main input or the sidechain). Matched filters that glide; zero
// latency by default, or linear phase / natural phase (one FIR, partitioned FFT convolution) or lookahead. Auto gain
// (curve or program), solo, delta, input, dry/wet; Set dynamic thresholds. Ported line for line from
// twostorms_eq.jsfx.
#pragma once
#include "../../core/abmatch.h"
#include "../../core/act.h"
#include "../../core/plugin.h"
#include "../../gui/analyzer.h"
#include <atomic>
#include <vector>

namespace ts {
namespace peq {

enum {
    P_SCV = 43, P_SEL = 44, P_DRNG = 46, P_AGM = 47, P_SCT = 48, V_RAT = 49, P_IN = 50, P_MIX = 51, P_AUTOG = 52, P_DTP = 53,
    P_LIN = 54, P_LA = 55, P_LAMS = 56, P_AN = 57, P_AB = 58, P_DELTA = 59, V_HLD = 252, P_TGTM = 253, P_NBOX = 254, V_KNE = 255,
};
constexpr int EB0 = 60, EBN = 12, NB = 16;
// a band's parameters: EB0 + band * EBN + K_*
enum { K_ON = 0, K_TYPE, K_FREQ, K_GAIN, K_Q, K_PLACE, K_DYN, K_DSRC, K_THR, K_RNG, K_ATT, K_REL };
inline int ep(int b, int k) { return EB0 + b * EBN + k; }
constexpr int GS_TTIME = 71;   // the Gain Stage's Set row: the listening time

// the packed band settings: K_DYN = mode (0 off, 1 compress, 2 expand) + 3 x detector (0 peak, 1 RMS) + 6 x ratio step
// (0..10000: the ratio is 100^(n/10000), 10000 = inf) + 60006 x hold (0.5 ms steps); K_DSRC = trigger (0 main, 1 sidechain)
// + 2 x (Q mode 0..3 + 4 x knee in 0.1 dB)
inline int pkDm(double v) { return (int)std::floor(v + 0.5) % 3; }
inline double pkRt(double v) { return std::pow(100.0, ((int64_t)std::floor(std::floor(v + 0.5) / 6) % 10001) / 10000.0); }
inline int pkDet(double v) { return (int)((int64_t)std::floor(std::floor(v + 0.5) / 3) % 2); }
inline double pkHold(double v) { return std::floor(std::floor(v + 0.5) / 60006) * 0.5; }
inline int pkSrc(double v) { return (int)std::floor(v + 0.5) % 2; }
inline int pkQm(double v) { return (int)((int64_t)std::floor(std::floor(v + 0.5) / 2) % 4); }
inline double pkKn(double v) { return std::floor(std::floor(v + 0.5) / 8) * 0.1; }
// the latency (samples) of phase mode lin (0 zero latency, 1..4 linear, 5..7 natural) and lookahead
int latency(int lin, bool la, double lams, double sr);

// what the window reads
struct Display {
    std::atomic<float> edg[NB];     // each dynamic band's gain change now (dB)
    std::atomic<double> agdb{0};    // what Auto gain does now (dB)
    std::atomic<double> mi0{-200}, mi1{-200};
    std::atomic<int> side{0};
    std::atomic<int> solo{-1};      // written by the window
    std::atomic<int> scok{0};       // written by the window: a dynamic band listens to the sidechain
    AnFeed an;
    Display() { for (auto &e : edg) e.store(0); }
};

class EngineHost {
public:
    virtual ~EngineHost() {}
    virtual double pv(int id) const = 0;
    virtual double abv(int id) const = 0;
    virtual void setAudio(int id, double v) = 0;   // Set dynamic thresholds
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
    void post(const double *const *sc, double *const *out, int n);
    // the Act's Set command (the window's button, or the Gain Stage's Set row)
    void tgStart() { if (e_nd > 0) { A.tgs = 1; e_tgk = -1; } }
    int dynCount() const { return e_nd; }

private:
    static constexpr int ECS = 40, EZN = 32, E_TK = 16;
    EngineHost &H;
    Act &A;
    UiLink &U;
    Display &V;
    double srate = 44100;
    bool nonceDrawn = false, idDrawn = false, anw = false;
    std::vector<double> br0, br1, bq0, bq1;

    // the bands
    double ECO[NB * ECS] = {}, EZS[NB * EZN] = {}, ECP[NB * 12] = {}, ETMP[NB * ECS + 24] = {};
    int ENS[NB] = {}, EPL[NB] = {}, EACT[NB] = {}, EDL[NB] = {}, EML[NB] = {}, ETY[NB] = {}, EQM[NB] = {}, EDS[NB] = {}, EDMD[NB] = {};
    bool EAC[NB] = {}, ELV[NB] = {}, ESM[NB] = {}, EDY[NB] = {}, EDDT[NB] = {};
    double ESF[NB] = {}, ESG[NB] = {}, ESQ[NB] = {}, EBG[NB] = {}, EDC[NB * 5] = {}, EDZ[NB * 2] = {}, EENV[NB] = {};
    double EKA[NB] = {}, EKR[NB] = {}, EKD[NB] = {}, EDG[NB] = {}, EHT[NB] = {}, EHC[NB] = {};
    double EDTH[NB] = {}, EDRG[NB] = {}, EDSL[NB] = {}, EDKN[NB] = {};
    // the published first section of each band (the JSFX's plain variables) and its states
    double G[NB][5] = {}, ZL[NB][2] = {}, ZR[NB][2] = {};
    int LN[NB] = {}, LP[NB] = {};
    double ESO[24] = {}, ESZ[16] = {}, DSG[8] = {};
    // Set dynamic thresholds
    std::vector<double> TGLV;
    double TGHI[1000] = {}, TGMX[NB] = {}, TGMN[NB] = {};
    // the delay (lookahead, linear-phase latency)
    std::vector<double> DLA, DLB, DRA, DRB;
    double AGCW[64] = {}, AGC2[64] = {}, KWC[10] = {}, KWZ[32] = {};
    // linear and natural phase
    std::vector<double> LDS, LFD, LFF, LAC, LRES, LIB, LOB, LOB2, LGC, LGL, LHT;
    double AMR[NB * 16] = {};
    int LBM[2] = {}, LBNF[2] = {}, LBO[8] = {}, LBI[8] = {};
    double seen[256];
    bool seenOk = false;

    double e_ing = 1, e_dm = 0, e_wm = 1, e_ag = 1, e_agt = 1, e_agdb = 0, e_agdbs = 0, e_agdbp = 0, e_pi = 0, e_po = 0;
    double e_ksm = 0.05, e_ksm_sr = 0, e_kp = 0, e_agk = 0, e_csr = 0, e_lrbt = 0;
    bool e_agon = false, e_agm = false, e_del = false, e_nat = false, e_la = false, e_agmv = false, e_agp = false;
    bool e_snap = false, e_rescan = false, e_dirty = false, e_anyf = false, e_hasms = false, e_haslr = false, e_lrb = false;
    bool e_lnew = false, e_lfirst = true, e_lnox = false, e_lxf = false, e_tgsig = false;
    int e_lin = 0, e_linp = -1, e_lb = 0, e_ld = 0, e_dl = 0, e_na = 0, e_nd = 0, e_nm = 0, e_tc = 0, e_flc = 0;
    int e_sol = -1, e_sold = 0, e_lcur = 0, e_lpos = 0, e_lbank = 0, e_lmode = 0, e_tgk = -1, e_sside = -1;
    int64_t e_dp = 0;
    double e_sct = 1;
    int e_tgn = 0, e_tc2 = 0, e_tms = 48, e_tgdur = 5000, e_tgi = 0;
    double c_ip0 = 0, c_ip1 = 0, c_mi0 = -200, c_mi1 = -200;
    int uisolo = -1;

    double cpv(int p) const { return ab.swh ? H.abv(p) : H.pv(p); }
    void pub(int b);
    void zero(int b);
    double filt(double u, const double *o, int ns, double *z);
    void slow(int b, double &l, double &r);
    void xsec(int b, double &l, double &r);
    void tick();
    void lclear();
    void amPrep(int b, double *r);
    double am(const double *r, double fx) const;
    void firStore(int bk, int f, int lb, int nn);
    void minphase(const double *o, double *dst, int nn);
    void firBuild();
    void linOut(int bk, double *dst);
    void linRun();
    void scan(bool all);
    void tgBlock();
};

} // namespace peq
} // namespace ts
