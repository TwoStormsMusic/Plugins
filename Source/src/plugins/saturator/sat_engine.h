// TwoStorms Saturator's sound: up to six bands (Linkwitz-Riley crossovers at 12, 24 or 48 dB per octave, each with
// its all-pass for the bands below), each with one of 30 saturation styles (tube, tape, transformer, amp, console,
// harmonics, wild, digital) at its drive, run at 1x, 2x or 4x the sample rate, with dynamics before the shaper,
// feedback through a band pass, tone (bass, mid, treble, presence), level, mix, stereo or mid/side, and drive / level
// balance; eight modulation slots (LFO, envelope follower, MIDI) and Delta (hear only what is added). Ported line
// for line from twostorms_saturator.jsfx.
#pragma once
#include "../../core/abmatch.h"
#include "../../core/act.h"
#include "../../core/plugin.h"
#include "../../gui/analyzer.h"
#include <atomic>
#include <vector>

namespace ts {
namespace sat {

enum {
    P_SEL = 43, P_NB = 45, P_SLOPE = 46, P_OS = 47, P_IN = 48, P_GDRV = 49, P_GDYN = 50, P_MIX = 51,
    P_AN = 53, P_AB = 54, P_DELTA = 55, P_MSEL = 56, P_MODOPEN = 57, P_XF = 60,
};
constexpr int EB0 = 70, EBN = 16, NB = 6, MB0 = 170, MN = 8, NMOD = 8, NPAR = 234, NSTY = 30;
// a band's parameters: EB0 + band * EBN + K_*
enum { K_ON = 0, K_MUTE, K_STYLE, K_DRIVE, K_MIX, K_LEVEL, K_DYN, K_FB, K_FBF, K_BASS, K_MID, K_TRE, K_PRES, K_DPAN, K_LPAN, K_CH };
// a modulation slot's parameters: MB0 + slot * MN + M_*
enum { M_TYPE = 0, M_TBAND, M_TPAR, M_AMT, M_RATE, M_RATE2, M_OPT, M_SYNC };
inline int ep(int b, int k) { return EB0 + b * EBN + k; }
inline int mp(int s, int k) { return MB0 + s * MN + k; }

// per style: the pre-gain (dB), then up to 2 pre and 2 post filters (type, frequency, gain, Q; type -1 = none)
extern const double STYT[NSTY][17];
extern const char *const STYLE_NAMES[NSTY];
// the latency (whole samples) of oversampling setting os (0 off, 1 2x, 2 4x)
inline int osLatency(int os) { return os <= 0 ? 0 : (os == 1 ? 3 : 5); }

// what the window reads
struct Display {
    std::atomic<float> eff[NPAR];   // the settings as the modulation has them now
    std::atomic<float> mv[NMOD];    // each slot's value now
    std::atomic<float> ef[NB], eo[NB];   // each band's level in and out (linear)
    std::atomic<double> mi0{-200}, mi1{-200};   // input peaks (after Input), dB
    std::atomic<int> side{0};
    std::atomic<int> solo{-1};      // the window's solo (written by the window)
    AnFeed an;
    Display()
    {
        for (auto &e : eff) e.store(0);
        for (auto &m : mv) m.store(0);
        for (int b = 0; b < NB; ++b) { ef[b].store(0); eo[b].store(0); }
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
    // @block, the saturator's part (before the Act's)
    void block(int samplesblock, const Transport &tr, const std::vector<MidiEv> &midi);
    void afterAct();
    // @sample, the saturator's part (before the Act); sc: the sidechain (or null)
    void samples(const double *const *in, const double *const *sc, double *const *out, int n);
    // after the Act: held Bypass / Swap, input meters, the analyzer's rings
    void post(double *const *out, int n);

private:
    static constexpr int BSTR = 96;
    enum { ZPRE = 0, ZPOST = 8, ZTONE = 16, ZDC = 24, ZFB = 26, ZSH = 32, ZUA = 48, ZUB = 54, ZDA = 58, ZDB = 65 };
    EngineHost &H;
    Act &A;
    UiLink &U;
    Display &V;
    double srate = 44100;
    bool nonceDrawn = false, idDrawn = false, anw = false;
    std::vector<double> br0, br1, bq0, bq1;

    // per band and channel: the states
    double BSZ[NB * 2 * BSTR] = {};
    // per band: pre (4 sections), style post (4), tone (4 slots), 5 numbers each
    double PCO[NB * 60] = {};
    int PNS[NB] = {}, QNS[NB] = {}, BST[NB] = {}, BOS[NB] = {};
    bool TON[NB * 4] = {}, BON[NB] = {}, BMS[NB] = {}, FBON[NB] = {};
    double TCH[NB * 8] = {};
    double DGc[NB * 2] = {}, DGi[NB * 2] = {}, PGc[NB * 2] = {}, PGi[NB * 2] = {}, DRVP[NB * 2] = {};
    double MXc[NB] = {}, MXi[NB] = {}, GDc[NB] = {}, GDi[NB] = {}, FBc[NB] = {}, FBi[NB] = {}, OGc[NB] = {}, OGt[NB] = {};
    double EF[NB] = {}, ES[NB] = {}, FBK[NB * 4] = {}, GDB[NB] = {}, EO[NB] = {};
    double DDL[NB * 2 * 16] = {}, BSG[16] = {}, UPO[8] = {};
    // the crossovers: per crossover 4 low-pass and 4 high-pass sections, 2 all-pass sections (5 numbers each)
    double XL[5][4][5] = {}, XH[5][4][5] = {}, XA[5][2][5] = {};
    double XFc[5] = {}, XFt[5] = {}, XFd[5] = {};
    // their states: [channel][crossover][section][2] and the all-passes [channel][band][crossover][section][2]
    double ZXL[2][5][4][2] = {}, ZXH[2][5][4][2] = {}, ZA[2][6][5][2][2] = {};
    double DRW[256] = {};
    // modulation
    double EFFP[256] = {}, MV[NMOD] = {}, MPH[NMOD] = {}, MRA[NMOD] = {}, MRB[NMOD] = {}, MEN[NMOD] = {}, MKA[NMOD] = {}, MKR[NMOD] = {};
    double MCC[130] = {}, MLS[NMOD] = {};
    int MSR[NMOD] = {}, MSB[NMOD] = {}, MFL[NMOD] = {};
    int d_nf = 0;
    double d_beat = 0, d_tempo = 120, d_nvel = 0, d_nheld = 0, d_pb = 0;
    // the shaper's rate-dependent constants
    double CCH[16] = {}, CBS[16] = {};
    double TH_K1 = 0, TH_K2 = 0, TH_K3 = 0, TH_G3 = 0, SM_L = 0, BD_K = 0;
    // the rest
    int d_nb = 3, d_slope = 1, d_os = 0, d_osf = 1, d_tc = 0, d_dint = 0;
    int64_t d_dp = 0, d_rp = 0;
    bool d_snap = true, d_del = false;
    double d_csr = 0, d_cdp = 0, d_dd = 0, d_ingc = 0, d_ingt = 1;
    double DC_R = 0, D_KAE = 0, D_KRE = 0, D_KS = 0;
    double d_o0 = 0, d_o1 = 0;
    double c_ip0 = 0, c_ip1 = 0, c_mi0 = -200, c_mi1 = -200;
    int uisolo = -1;

    double cpv(int p) const { return ab.swh ? H.abv(p) : H.pv(p); }
    void rates();
    void xdes(int j, double f, int slope);
    void xzero();
    void xflush();
    void split(double xl, double xr);
    double shape(int st, double u, double *sz);
    void filt(int b);
    double chan(int b, int c, double x);
    void band(int b);
    double mval(int s);
    void modeval();
    void bprep(int b);
    void tick();
};

} // namespace sat
} // namespace ts
