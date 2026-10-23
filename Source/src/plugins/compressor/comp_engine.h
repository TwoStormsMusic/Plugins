// TwoStorms Compressor: the engine (detector, gain computer, ballistics, lookahead, saturation),
// ported from the JSFX's @block and @sample with the same arithmetic in the same order, so it
// produces the same samples. Variable names follow the JSFX (c_* compressor, e_* engine).
#pragma once
#include "../../core/act.h"
#include <atomic>
#include <vector>

namespace ts {
namespace comp {

// parameter numbers (= JSFX slider numbers)
enum {
    P_THR = 50, P_RAT = 51, P_KNEE = 52, P_ATT = 53, P_HLD = 54, P_REL = 55, P_RNG = 56, P_DET = 57,
    P_CMODE = 58, P_SLINK = 59, P_IN = 60, P_MIX = 61, P_AMK = 62, P_DELTA = 64, P_SC = 65,
    P_LISTEN = 66, P_LA = 67, P_LAMS = 68, P_TGR = 69, P_TTYPE = 70, P_TTIME = 71, P_AB = 73,
    P_R1 = 74,   // trigger ranges r = 0..3: P_R1 + 3r on, + 3r + 1 low (Hz), + 3r + 2 high (Hz)
    P_CUIST = 86, P_RMST = 87, P_ENV = 88, P_RELM = 89, P_FB = 90, P_BLEND = 92, P_LKL = 93, P_RCMB = 94,
    P_SCHP = 95, P_SCHS = 96, P_SCTL = 97, P_SCT = 98, P_AMKM = 99, P_CURVE = 100, P_CHAR = 101,
    P_SAT = 102, P_SATT = 103, P_BASE = 104,
};

// what the engine needs from the plug-in
class EngineHost {
public:
    virtual ~EngineHost() {}
    virtual double pv(int id) const = 0;        // slider(p)
    virtual double abv(int id) const = 0;       // ABSET[p]: the other A/B set
    virtual void setFromAudio(int id, double v) = 0;
};

// written by the window (main thread), read by the audio thread
struct UiLink {
    std::atomic<double> beat{0};   // c_uibeat: the hold buttons count while this is fresh
    std::atomic<int> hold{0};      // c_uihold: 1 = Bypass held/latched, 2 = Swap
    std::atomic<double> last{0};   // g_last: the window is open (meters, level matching, analyzer run)
};

// written by the audio thread, read by the window
struct Display {
    std::atomic<double> mi0{-200}, mi1{-200};   // input peak meters (after Input), dB
    std::atomic<double> ugr{0};                 // gain reduction, dB (negative)
    std::atomic<double> dlv{-200};              // loudest detector level, dB
    std::atomic<double> latms{0};
    std::atomic<int> ld{0};                     // lookahead in samples (the latency)
    std::atomic<int> side{0};                   // the set playing now (0 A, 1 B)
    std::atomic<double> mkdb{0};
    static constexpr int HN = 1024, AN = 32768;
    std::atomic<float> hin[HN], hout[HN], hgr[HN];   // history: 10 ms per entry
    std::atomic<int> hp{0};
    std::atomic<float> ani[AN], ano[AN];             // analyzer input / output
    std::atomic<int> ap{0};
    Display();
};

struct Biquad {
    double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;      // bq_one state
    double z1a = 0, z1b = 0, z2a = 0, z2b = 0;   // the trigger ranges' two channels
    void set(double f, double q, int hp, double srate);
    double one(double x)
    {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    double runA(double u)
    {
        double t = b0 * u + z1a;
        z1a = b1 * u - a1 * t + z2a;
        z2a = b2 * u - a2 * t;
        return t;
    }
    double runB(double u)
    {
        double t = b0 * u + z1b;
        z1b = b1 * u - a1 * t + z2b;
        z2b = b2 * u - a2 * t;
        return t;
    }
    void flushAB();
    void zeroAB() { z1a = z1b = z2a = z2b = 0; }
};

// one detector / gain channel (the JSFX's e_c0 / e_c1 namespaces)
struct Chan {
    // detector
    double d1 = 0, d2 = 0, d3 = 0, pe = 0, ms = 0, lv = 0, lp = 0, lr = 0, lin = 0;
    // gain
    double y = 0, yf = 0, ys = 0, sa = 0, k = 0;
    double hold = 0;
    // lookahead rings
    int64_t n = 0, dqh = 0, dqt = 0;
    double sum = 0;
    std::vector<double> tgt, mn, md;
    std::vector<int64_t> q;
    Chan();
    void la_reset();
};

struct Sat { double xp = 0, fp = 0, dcs = 0; bool ini = false; };

class Engine {
public:
    Engine(EngineHost &h, Act &act, UiLink &ui, Display &view);
    void init(double srate);
    void reset();

    // the compressor's part of @block
    void block(int nframes);
    // the compressor's part of @sample, over the block: in[0..1] main, sc[0..1] sidechain (may be null);
    // writes out[0..1] (may alias in)
    void samples(const double *const *in, const double *const *sc, double *const *out, int nframes);
    // after the built-in Act: meters, level matching, held Bypass / Swap, analyzer
    void post(double *const *out, int nframes);

    int latency() const { return c_ld; }
    // the latency the current settings ask for (main thread, for the host)
    static int latencyFor(double la, double lams, double srate);

    double c_mkdb = 0;     // auto makeup's memory (saved with the project)
    bool satFix = true;    // false = exactly the JSFX 1.0 (for the comparison tests)

private:
    EngineHost &H;
    Act &A;
    UiLink &U;
    Display &V;
    double srate = 44100;

    double c_pv(int p) const { return c_swh ? H.abv(p) : H.pv(p); }

    // block settings
    bool c_uiok = false, c_bph = false, c_swh = false, c_lat = false, c_side = false;
    double c_thr = 0, c_rat = 0, c_kn = 0, c_sl = 0;
    int c_mode = 0;
    bool c_two = true;
    double c_lk = 0, c_lin = -1000000, c_ing = 1, c_mix = 1;
    bool c_ext = false, c_lis = false, c_del = false, c_la = false;
    int c_ld = 0;
    double c_latms = 0;
    int c_nr = 0;
    bool c_ron[4] = {}, c_rhp[4] = {}, c_rlp[4] = {};
    double c_rfl[4] = {}, c_rfh[4] = {}, c_rsr[4] = {};
    Biquad c_rh1[4], c_rh2[4], c_rl1[4], c_rl2[4];
    // engine
    int e_chr = 1, e_cv = 0, e_relm = 1;
    double e_fbk = 0, e_sat = 0;
    bool e_satt = false, e_la = false;
    int e_ld = 0;
    bool e_envs = true;
    int e_detm = 0;
    double e_blend = 0, e_rmst = 20, e_rk = 0, e_tw = 0, e_td = 0, e_dr = 0, e_relc = 0;
    double e_aa = 0, e_aau = 0, e_ar = 0, e_rrel = 0, e_arf = 0, e_ars = 0, e_asa = 0, e_ska = 0, e_skr = 0;
    double e_hs = 0, e_sl = 0, e_nrg = -200, c_nrg = -200;
    bool e_mono = false;
    double e_lk = 1;
    bool e_lkl = false, e_rcb = false;
    double e_hpf = 20;
    bool e_hpon = false;
    int e_hps = 1;
    double e_tilt = 0;
    bool e_tlon = false;
    double e_sct = 1, e_lhpf = -1, e_lhps = -1, e_lhsr = 0, e_hpg = 0;
    Biquad e_h0a, e_h1a, e_h0b, e_h1b;
    double e_lp0 = 0, e_lp1 = 0, e_tl0 = 0, e_tl1 = 0, e_tg = 0, e_tgl = 1, e_tgh = 1;
    double e_sd = 1, e_sdi2 = 1, e_sb = 0, e_sc = 0, e_sg = 1, e_dck = 0.001;
    Sat e_s0, e_s1;
    double e_lsd = 0, e_lsb = 0;
    int e_laA = 0;
    double e_lla = -1, e_lld = -1, e_llaA = -1, e_lmono = -1;
    int e_ldA = 0, e_ldB = 0;
    double c_xf = 1, e_xfk = 0;
    bool e_amk = false, e_amm = false, e_pwr = false, e_one = false;
    Chan e_c0, e_c1;
    double e_l0 = 0, e_l1 = 0;
    // makeup / mix
    bool c_amk = false;
    int c_lbs = 0;
    double c_lbsr = 0, c_kmk = 0, c_klm = 0;
    double c_ys = 0;
    double c_yn = 0;
    double e_mkt = 0, c_dm = 0, c_wmt = 1, c_zk = 0.002;
    bool e_first = true;
    double c_ings = 1, c_dms = 0, c_wms = 1;
    bool c_uion = false, c_vneed = false;
    // level matching
    double LMI[2] = {}, LMO[2] = {}, LMV[2] = {};
    int c_lside = -1;
    double c_lmskip = 0, c_ei = 0, c_eo = 0, c_bpg = 1, c_trt = 1, c_tr = 1, c_bps = 0, c_trk = 0, c_bpv = 0;
    // window meters
    double c_mi0 = -200, c_mi1 = -200, c_ip0 = 0, c_ip1 = 0, c_ugr = 0, c_dlv = -200, c_vdot = 0;
    double c_y0 = 0, c_y1 = 0, c_k0 = 1, c_k1 = 1;
    double c_hbi = -200, c_hbo = -200, c_hbg = 0, c_ht = 0;
    int c_hp = 0, c_ap = 0;
    bool c_anw = false;
    // Set threshold
    int c_tgn = 0, c_tc = 0, c_tms = 48, c_tit = 0;
    double c_tm = -200, c_tgdur = 0, c_tlo = -60, c_thi = 0, c_st = -30, c_tgt = 4;
    bool c_ttype = false;
    double c_saa = 0, c_sar = 0, c_shs = 0, c_sy = 0, c_sh = 0, c_ssum = 0;
    int c_scnt = 0, c_spos = 0;
    std::vector<double> TGB;
    int TGH[640];
    // lookahead line (raw input)
    std::vector<double> LAB0, LAB1;
    int c_lp = 0;
    // per-sample values the post stage needs
    std::vector<double> bq0, bq1, br0, br1, bx0, bx1;

    // functions
    double ch_det(Chan &c, double d, double v);
    double ch_lin(Chan &c, double d, double v);
    double la_proc(Chan &c, double t);
    double ch_gain(Chan &c, double lvl);
    double sat_run(Sat &s, double x);
    void c_sim_begin();
    void c_sim_run(int n);
    double c_sim_result();
};

// shared by the engine and the window (the curve)
double e_curve(double o, double kn, double sl, int cv);
double rs_soft(double g, double nrg);

} // namespace comp
} // namespace ts
