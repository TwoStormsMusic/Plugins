#include "mb_engine.h"
#include "../../core/eelrand.h"
#include "../../core/fft.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ts {
namespace mb {

static const double XK2 = 2 * std::sqrt(2.0);

static int lbFor(double sr) { return sr <= 50000 ? 1024 : (sr <= 100000 ? 2048 : 4096); }

int latency(bool lin, bool la, double lams, double sr)
{
    int lat = 3 * lbFor(sr);
    int ld = la ? (int)emax(1, emin(2000, std::floor(lams * 0.001 * sr + 0.5))) : 0;
    ld = lin ? std::min(ld, lat) : std::min(ld, 2047);
    return lin ? lat : ld;
}

// ln cosh(u) without overflow, and tanh (the saturation)
static inline double e_lcosh(double u)
{
    u = std::fabs(u);
    return u + std::log(1 + std::exp(-2 * u)) - 0.6931471805599453;
}
static inline double e_tanh(double u) { return u > 20 ? 1 : (u < -20 ? -1 : 1 - 2 / (std::exp(2 * u) + 1)); }

// ======================================================================== setup
void Engine::init(double sr)
{
    srate = sr;
    if (!nonceDrawn) { eel_rand(1000000); nonceDrawn = true; }   // the instance nonce (the JSFX's first rand in @init)
#define Z(a) memset(a, 0, sizeof a)
    Z(XC); Z(XF); Z(XS); Z(SLR); Z(BA); Z(BD); Z(BAU); Z(BG); Z(BGU); Z(BGI);
    Z(BON); Z(BSOL); Z(BEXP); Z(BENVS); Z(BSATON); Z(BSATT); Z(BMODE2); Z(BFB);
    Z(BTHR); Z(BSL); Z(BKN); Z(BRNG); Z(BSGN); Z(BMK); Z(BAA); Z(BAR); Z(BRR); Z(BARF); Z(BARS); Z(BASA); Z(BDR); Z(BFBK);
    Z(BCV); Z(BRELM); Z(MST); Z(BHT); Z(BYS); Z(BYN); Z(BMKA); Z(BGR); Z(BGRD);
    Z(BSD); Z(BSB); Z(BSC); Z(BSG); Z(SATS); Z(TGH); Z(TGMX);
#undef Z
    for (double &v : BLV) v = -200;
    ZLA.assign(10 * 2048, 0.0);
    LDS.assign(32768, 0.0); LHT.assign(16384, 0.0); LAC.assign(16384, 0.0); LFD.assign(65536, 0.0); LTM.assign(16384, 0.0);
    LFFP.assign(6 * 65536, 0.0); LIB.assign(16384, 0.0); LPO.assign(6 * 4096, 0.0); LPX.assign(6 * 4096, 0.0);
    DLY.assign(2 * 16384, 0.0); DLR.assign(2 * 16384, 0.0); GRG.assign(8 * 16384, 0.0);
    TGLV.assign(4 * 30000, 0.0);
    m_nb = 1; m_nbw = -1; m_linw = -1; m_srw = -1; m_lb = 1024; m_lat = 3072; m_ld = 0; c_ld = 0; m_gd = 0;
    m_x[0] = m_x[1] = m_x[2] = -1;
    m_lin = false; m_lrb = true; m_lrbt = 1; e_first = true; anw = false;
    c_lin = -1000000; c_ing = 1; c_ings = 1; c_dms = 0; c_wms = 1;
    c_mi0 = -200; c_mi1 = -200; c_ip0 = 0; c_ip1 = 0; c_dlv = -200; c_vdot = 0;
    m_tc = 0; m_zp = 0; m_dp = 0; m_gp = 0; m_rd0 = m_rd1 = m_dx0 = m_dx1 = 0;
    m_lcur = 0; m_lpos = 0; m_lbank = 0; m_lfirst = true; m_lnew = false; m_lxf = false;
    c_tgn = 0; c_tc = 0; c_tgb = -1; c_tgi = 0;
    V.an.clear();
}

void Engine::reset()
{
    memset(XS, 0, sizeof XS); memset(SLR, 0, sizeof SLR); memset(MST, 0, sizeof MST); memset(SATS, 0, sizeof SATS);
    std::fill(ZLA.begin(), ZLA.end(), 0.0);
    linClear();
    m_lrb = true; m_lrbt = 1;
    for (int i = 0; i < 8; ++i) { BG[i] = 1; BGI[i] = 0; }
}

// ======================================================================== the crossovers
// crossover i's coefficients for f Hz (Q = 1 / sqrt 2, so two in a row make a Linkwitz-Riley 24 dB/oct low pass)
void Engine::xoSet(int i, double f)
{
    double *c = XC + i * 3, g = std::tan(PI * (emin(f, 0.45 * srate) / srate));
    c[0] = 1 / (1 + g * (g + std::sqrt(2.0)));
    c[1] = g * c[0];
    c[2] = g * c[1];
}

// one state-variable filter step (topology-preserving), coefficients c[0..2]: returns the low pass, bp the band pass
static inline double svf(double x, double *st, const double *c, double &bp)
{
    double v3 = x - st[1];
    double v1 = c[0] * st[0] + c[1] * v3;
    double v2 = st[1] + c[1] * st[0] + c[2] * v3;
    st[0] = 2 * v1 - st[0];
    st[1] = 2 * v2 - st[1];
    bp = v1;
    return v2;
}

// split x into the bands in use, lowest first, into out[0 .. m_nb - 1] (st: 9 filters x 2). Each crossover: the LR4 low
// pass, and above it the allpass minus that low pass (the allpass comes from the first filter's band pass, so the high
// side needs no filters of its own). The bands below a crossover go through its allpass too, so all stay in phase and
// add back to the input with a flat level.
void Engine::split(double x, double *st, double *out) const
{
    if (m_nb == 1) { out[0] = x; return; }
    double b1, bq, l1 = svf(x, st, XC, b1), lo = svf(l1, st + 2, XC, bq), hi = x - XK2 * b1 - lo;
    if (m_nb == 2) { out[0] = lo; out[1] = hi; return; }
    l1 = svf(hi, st + 4, XC + 3, b1);
    double m1 = svf(l1, st + 6, XC + 3, bq), h2 = hi - XK2 * b1 - m1;
    svf(lo, st + 8, XC + 3, bq);
    lo -= XK2 * bq;
    if (m_nb == 3) { out[0] = lo; out[1] = m1; out[2] = h2; return; }
    l1 = svf(h2, st + 10, XC + 6, b1);
    out[2] = svf(l1, st + 12, XC + 6, bq);
    out[3] = h2 - XK2 * b1 - out[2];
    svf(lo, st + 14, XC + 6, bq);
    out[0] = lo - XK2 * bq;
    svf(m1, st + 16, XC + 6, bq);
    out[1] = m1 - XK2 * bq;
}

// ======================================================================== the detector and the gain computer
// the detector, every sample: the peak envelope (and its highest point since the last tick, st[7]) and the mean square
inline void Engine::det(int b, double *st, double v)
{
    double a = std::fabs(v);
    st[0] = BENVS[b] ? (a > st[0] ? a : st[0] * BDR[b]) : a;
    if (st[0] > st[7]) st[7] = st[0];
    if (m_detm > 0) st[1] += m_rk * (v * v - st[1]);
}

// ... and at each tick its level in dB (Peak: the highest point since the last tick, so no peak slips between ticks)
double Engine::dbOf(double *st)
{
    double lp = std::log(emax(st[7], 0.000000001)) * 8.685889638065035;
    st[7] = 0;
    if (m_detm == 0) return lp;
    double lr = std::log(emax(std::sqrt(emax(st[1], 0)) * 1.4142135623730951, 0.000000001)) * 8.685889638065035;
    return m_detm == 1 ? lr : lp + m_blend * (lr - lp);
}

// how far the gain moves (dB, before the ratio) at o dB past the threshold, with knee kn. Curve 0: standard (quadratic
// knee); 1: soft, the ratio grows in over the first 12 dB (vari-mu like); 2: progressive over the first 18 dB (opto like)
double Engine::curve(int cv, double o, double kn) const
{
    if (cv < 0.5) return (o + o < -kn) ? 0 : ((o + o > kn) ? o : sqr(o + kn * 0.5) / (2 * kn));
    double x = o + kn * 0.5;
    return x <= 0 ? 0 : (cv < 1.5 ? x - 12 * (1 - std::exp(-x / 12)) : (x < 18 ? x * (x / 36) : x - 9));
}

// band b's gain computer and ballistics for one detector channel (state st) at level lv: the gain change in dB.
// Compress moves the gain past the threshold, Expand under it (a conventional expander: (ratio - 1) dB per dB);
// Range caps it, its sign says which way (negative cuts, positive boosts). The state counts how far it has moved.
// Attack: moving away from 0 (Compress) or back towards 0, opening (Expand); Release the other way: classic,
// program-dependent (slower after long, deep moves) or 2-stage (a fast and a slow stage, the deeper one counts).
double Engine::gain(int b, double *st, double lv)
{
    double o = BEXP[b] ? BTHR[b] - lv : lv - BTHR[b];
    double t = o < -0.5 * BKN[b] ? 0 : emin(BRNG[b], BSL[b] * curve(BCV[b], o, BKN[b]));
    double d = st[2];
    st[5] += (t > st[5] ? m_ska : m_skr) * (t - st[5]);
    if (BEXP[b] ? t < d : t > d) {
        st[8] = BHT[b];
        d = t + BAA[b] * (d - t);
        if (BMODE2[b]) { st[3] = t + BAA[b] * (st[3] - t); st[4] = t + BASA[b] * (st[4] - t); d = emax(st[3], st[4]); }
    } else if (st[8] > 0) {
        // Hold: the gain stays where it got to for the hold time, then releases
        st[8] -= 1;
    } else if (BRELM[b] == 1) {
        d = t + st[6] * (d - t);   // program-dependent: the coefficient is worked out once per block
    } else if (BMODE2[b]) {
        st[3] = t + BARF[b] * (st[3] - t);
        st[4] = t + BARS[b] * (st[4] - t);
        d = emax(st[3], st[4]);
    } else {
        d = t + BAR[b] * (d - t);
    }
    if (!BMODE2[b]) { st[3] = d; st[4] = d; }
    if (d < 0.00001) d = 0;
    st[2] = d;
    return d * BSGN[b];
}

// saturation: tanh (Soft) or a biased tanh (Warm: even harmonics, DC removed); first-order antiderivative
// anti-aliasing instead of oversampling, so it adds no latency. State: previous input, previous antiderivative,
// DC follower, started?
inline double Engine::sat(int b, double x, double *st)
{
    double sd = BSD[b], sb = BSB[b], sc = BSC[b];
    double F = e_lcosh(sd * (x + sb)) / (sd * sd) - sc * x;
    if (!truth(st[3])) { st[0] = x; st[1] = F; st[3] = 1; }
    double dx = x - st[0];
    double y = std::fabs(dx) > 0.00001 ? (F - st[1]) / dx : e_tanh(sd * ((x + st[0]) * 0.5 + sb)) / sd - sc;
    st[1] = F;
    st[0] = x;
    y /= BSG[b];
    if (BSATT[b]) { st[2] += e_dck * (y - st[2]); y -= st[2]; }
    return y;
}

// ======================================================================== linear phase
// Each crossover's low pass as a zero-phase FIR with the Linkwitz-Riley magnitude, by partitioned FFT convolution. The
// bands are the differences of the low passes (and the delayed input minus the top one), so they add back to exactly
// the input, delayed. The filters for the crossovers in use go into the bank not playing (the first time: the one
// playing).
void Engine::firBuild()
{
    int nn = 4 * m_lb, nh = nn / 2;
    int bank = m_lfirst ? m_lbank : 1 - m_lbank;
    double *lds = LDS.data(), *lht = LHT.data();
    for (int f = 0; f < m_nb - 1; ++f) {
        double fc = XF[f];
        for (int k = 0; k <= nh; ++k) {
            double u = k * ((srate / nn) / fc);
            u *= u;
            lds[2 * k] = 1 / (1 + u * u);
            lds[2 * k + 1] = 0;
        }
        for (int k = 1; k < nh; ++k) { lds[2 * (nn - k)] = lds[2 * k]; lds[2 * (nn - k) + 1] = 0; }
        fft::inverse(lds, nn);
        // centre it at tap nn / 2, taper the ends (Tukey, 25 % each side)
        for (int n = 0; n < nn; ++n) {
            int k = n + nh;
            if (k >= nn) k -= nn;
            double u = (double)n / nn;
            u = emin(u, 1 - u);
            double w = u < 0.25 ? 0.5 - 0.5 * std::cos(4 * PI * u) : 1;
            lht[n] = lds[2 * k] / nn * w;
        }
        // its 4 partitions as spectra
        for (int p = 0; p < 4; ++p) {
            double *z = LFFP.data() + (bank * 3 + f) * 65536 + p * 16384;
            for (int i = 0; i < 2 * m_lb; ++i) { z[2 * i] = i < m_lb ? lht[p * m_lb + i] : 0; z[2 * i + 1] = 0; }
            fft::forward(z, 2 * m_lb);
        }
    }
    if (m_lfirst) { m_lfirst = false; m_lnew = false; }
    else m_lnew = true;
}

// one filter bank's low-pass outputs for the block (the low passes in use, both channels) into out. The filters are
// real, so one complex convolution does both channels: left in the real parts, right in the imaginary ones.
void Engine::linOut(int bank, double *out)
{
    int m = 2 * m_lb;
    double im = 1.0 / m;
    double *acc = LAC.data(), *tm = LTM.data();
    for (int k = 0; k < m_nb - 1; ++k) {
        for (int p = 0; p < 4; ++p) {
            double *d = p ? tm : acc;
            memcpy(d, LFD.data() + ((m_lcur - p) & 3) * 16384, 2 * m * sizeof(double));
            const double *h = LFFP.data() + (bank * 3 + k) * 65536 + p * 16384;
            for (int i = 0; i < m; ++i) {
                double ar = d[2 * i], ai = d[2 * i + 1], br = h[2 * i], bi = h[2 * i + 1];
                d[2 * i] = ar * br - ai * bi;
                d[2 * i + 1] = ar * bi + ai * br;
            }
            if (p)
                for (int i = 0; i < 2 * m; ++i) acc[i] += tm[i];
        }
        fft::inverse(acc, m);
        for (int i = 0; i < m_lb; ++i) {
            out[k * 4096 + i] = acc[2 * (m_lb + i)] * im;
            out[(3 + k) * 4096 + i] = acc[2 * (m_lb + i) + 1] * im;
        }
    }
}

// a block is in: its spectrum, then the low passes for the next block (after a filter change, the old ones too, to
// fade across)
void Engine::linRun()
{
    int m = 2 * m_lb;
    double *dst = LFD.data() + m_lcur * 16384;
    for (int i = 0; i < m; ++i) { dst[2 * i] = LIB[i]; dst[2 * i + 1] = LIB[8192 + i]; }
    fft::forward(dst, m);
    if (m_lnew) {
        linOut(m_lbank, LPX.data());
        m_lbank = 1 - m_lbank;
        m_lxf = true;
        m_lnew = false;
    } else {
        m_lxf = false;
    }
    linOut(m_lbank, LPO.data());
    m_lcur = (m_lcur + 1) & 3;
    memcpy(LIB.data(), LIB.data() + m_lb, m_lb * sizeof(double));
    memcpy(LIB.data() + 8192, LIB.data() + 8192 + m_lb, m_lb * sizeof(double));
}

void Engine::linClear()
{
    std::fill(LIB.begin(), LIB.end(), 0.0);
    std::fill(LPO.begin(), LPO.end(), 0.0);
    std::fill(LPX.begin(), LPX.end(), 0.0);
    std::fill(LFD.begin(), LFD.end(), 0.0);
    std::fill(DLY.begin(), DLY.end(), 0.0);
    std::fill(DLR.begin(), DLR.end(), 0.0);
    std::fill(GRG.begin(), GRG.end(), 0.0);
    m_lcur = 0; m_lpos = 0; m_lfirst = true; m_lnew = false; m_lxf = false; m_dp = 0; m_gp = 0;
}

// ======================================================================== Set thresholds
// Per band, the threshold that gives the band its target gain change on what was recorded (its detector's level each
// millisecond while listening). A simulated band - its curve, ratio, knee, range, attack, hold and release, run at
// 1 kHz over the recording - and a bisection on the threshold (16 steps, a few blocks each). Peak: the change the band
// stays under 95% of the playing time; Average: its mean. Compress bands count the playing time (above -60 dBFS),
// expand bands everything above -90 dBFS (they work on the quiet parts). A target beyond the band's range aims for
// just under the range. (The JSFX ran the simulation on the curve of whichever band the audio thread worked out last;
// here each band's own.)
void Engine::simBegin()
{
    c_sy = 0; c_sh = 0; c_s3 = 0; c_s4 = 0; c_s5 = 0; c_ssum = 0; c_scnt = 0; c_spos = 0;
    memset(TGH, 0, sizeof TGH);
}

// one millisecond of the band, as gain() moves it (classic, program-dependent or 2-stage release)
void Engine::simRun(int n, int b)
{
    const double *base = TGLV.data() + b * 30000;
    int cnt = std::min(n, c_tgn - c_spos);
    for (int q = 0; q < cnt; ++q) {
        double lv = base[c_spos];
        if (std::fabs(lv) < 500) {
            double o = c_sexp ? c_st - lv : lv - c_st;
            double t = o < -0.5 * c_skn ? 0 : emin(c_srg, c_ssl * curve(c_scv, o, c_skn));
            c_s5 += (t > c_s5 ? 0.0033277870 : 0.0009995002) * (t - c_s5);
            if (c_sexp ? t < c_sy : t > c_sy) {
                c_sh = c_shs;
                c_sy = t + c_saa * (c_sy - t);
                if (c_s2m) { c_s3 = t + c_saa * (c_s3 - t); c_s4 = t + c_sasa * (c_s4 - t); c_sy = emax(c_s3, c_s4); }
            } else if (c_sh > 0) {
                c_sh -= 1;
            } else if (c_srm == 1) {
                c_sy = t + std::exp(-c_srr / (0.3 + 1.4 * emin(1, c_s5 * 0.125))) * (c_sy - t);
            } else if (c_s2m) {
                c_s3 = t + c_sarf * (c_s3 - t);
                c_s4 = t + c_sars * (c_s4 - t);
                c_sy = emax(c_s3, c_s4);
            } else {
                c_sy = t + c_sar * (c_sy - t);
            }
            if (!c_s2m) { c_s3 = c_sy; c_s4 = c_sy; }
            if (lv > (c_sexp ? -90 : -60)) {
                c_ssum += c_sy;
                c_scnt += 1;
                TGH[(int)emin(600, std::floor(c_sy * 10))] += 1;
            }
        }
        c_spos += 1;
    }
}

double Engine::simResult()
{
    if (c_scnt <= 0) return -1;
    if (c_ttype) return c_ssum / c_scnt;
    double acc = 0, lim = 0.95 * c_scnt;
    int i = 0;
    while (i <= 600) {
        acc += TGH[i];
        if (acc >= lim) break;
        i += 1;
    }
    return i * 0.1;
}

// set up band b's simulation (its current engine values and times)
void Engine::simBand(int b)
{
    c_tgb = b;
    c_scv = BCV[b];
    c_sexp = BEXP[b]; c_ssl = BSL[b]; c_skn = BKN[b]; c_srg = BRNG[b];
    c_stg = emin(H.pv(P_TGR0 + b), 0.95 * BRNG[b]);
    // the band's own ballistics, per millisecond (as block() works them out per tick)
    c_srm = BRELM[b]; c_s2m = BMODE2[b];
    c_saa = epow(BAA[b], 0.001 * m_cr); c_sar = epow(BAR[b], 0.001 * m_cr); c_srr = BRR[b] * 0.001 * m_cr;
    c_sarf = epow(BARF[b], 0.001 * m_cr); c_sars = epow(BARS[b], 0.001 * m_cr); c_sasa = epow(BASA[b], 0.001 * m_cr);
    c_shs = (int)std::floor(BHT[b] / (0.001 * m_cr) + 0.5);
    c_tlo = -60; c_thi = 0; c_tit = 0; c_st = -30;
    simBegin();
}

// listen, then set every band's threshold. A.tgs: 0 idle, 1 waiting for signal, 2 listening, 3 working
void Engine::tgBlock()
{
    int &c_tgs = A.tgs;
    if (c_tgs > 0 && !m_anyon) c_tgs = 0;
    if (c_tgs == 1) {
        // wait for signal at the detector (-50 dBFS, as Auto waits)
        if (c_dlv > -50) {
            c_tgs = 2; c_tgn = 0; c_tc = 0;
            c_tms = (int)emax(1, std::floor(srate * 0.001 + 0.5));
            for (int i = 0; i < NB; ++i) TGMX[i] = BEXP[i] ? 1000 : -1000;
            int t = (int)std::floor(H.pv(P_TTIME) + 0.5);
            c_tgdur = 1000 * (t <= 0 ? 1 : (t == 1 ? 2 : (t == 2 ? 3 : (t == 3 ? 5 : (t == 4 ? 10 : (t == 5 ? 20 : 30))))));
        }
    } else if (c_tgs == 2) {
        if (c_tgn >= c_tgdur) { c_tgs = 3; c_tgi = 0; c_tgb = -1; c_ttype = H.pv(P_TTYPE) >= 0.5; }
    } else if (c_tgs == 3) {
        // band by band: a bisection step takes a few blocks of simulation
        if (c_tgb < 0) {
            while (c_tgi < m_nb && !BON[c_tgi]) c_tgi += 1;
            if (c_tgi >= m_nb) c_tgs = 0;
            else simBand(c_tgi);
        } else {
            simRun(4000, c_tgb);
            if (c_spos >= c_tgn) {
                double r = simResult();
                if (r < 0) {
                    c_tgb = -1;
                    c_tgi += 1;
                } else {
                    if (c_sexp ? r < c_stg : r > c_stg) c_tlo = c_st;
                    else c_thi = c_st;
                    c_tit += 1;
                    if (c_tit >= 16) {
                        H.setAudio(bp(c_tgb, K_THR), emax(-60, emin(0, std::floor((c_tlo + c_thi) * 5 + 0.5) / 10)));
                        c_tgb = -1;
                        c_tgi += 1;
                    } else {
                        c_st = (c_tlo + c_thi) * 0.5;
                        simBegin();
                    }
                }
            }
        }
    }
    A.tgel = c_tgs == 2 ? c_tgn * 0.001 : 0;
    A.tgav = m_anyon ? 1 : 0;
}

// ======================================================================== per block
void Engine::block(int samplesblock)
{
    ab.block(U, A.now(), H.pv(P_AB) >= 0.5, samplesblock, srate);
    V.side.store(ab.side ? 1 : 0, std::memory_order_relaxed);

    // the bands: one, plus one per ticked crossover; the ticked frequencies sorted (whichever box holds which) and kept
    // at least a third of an octave apart
    m_nb = 1;
    for (int i = 0; i < 3; ++i)
        if (cpv(P_X1 + 2 * i + 1) >= 0.5) { XF[m_nb - 1] = cpv(P_X1 + 2 * i); m_nb += 1; }
    if (m_nb == 3 && XF[0] > XF[1]) std::swap(XF[0], XF[1]);
    if (m_nb == 4)
        for (int r = 0; r < 2; ++r)
            for (int i = 0; i < 2; ++i)
                if (XF[i] > XF[i + 1]) std::swap(XF[i], XF[i + 1]);
    for (int i = 1; i < m_nb - 1; ++i) XF[i] = emax(XF[i], XF[i - 1] * 1.26);
    for (int i = 0; i < m_nb - 1; ++i) XF[i] = emin(XF[i], 0.45 * srate);
    // linear phase (its block grows with the sample rate, so the filters keep their length in time)
    m_lin = H.pv(P_XM) >= 0.5;
    m_lb = lbFor(srate);
    m_lat = 3 * m_lb;
    // a change of the band count, the mode or the sample rate starts the filters again (a crossover moving does not)
    if (m_nb != m_nbw || (int)m_lin != m_linw || srate != m_srw) {
        m_nbw = m_nb; m_linw = m_lin ? 1 : 0; m_srw = srate;
        memset(XS, 0, sizeof XS); memset(MST, 0, sizeof MST); std::fill(ZLA.begin(), ZLA.end(), 0.0);
        memset(BYS, 0, sizeof BYS); memset(BYN, 0, sizeof BYN); memset(BMKA, 0, sizeof BMKA);
        memset(SLR, 0, sizeof SLR);
        linClear();
        m_x[0] = m_x[1] = m_x[2] = -1;
        m_lrb = true;
        m_lrbt = 1;
        for (int i = 0; i < 8; ++i) { BG[i] = 1; BGI[i] = 0; }
    }
    for (int i = 0; i < m_nb - 1; ++i)
        if (m_x[i] != XF[i]) { xoSet(i, XF[i]); m_lrb = true; m_x[i] = XF[i]; }
    // linear phase: new filters at most every 40 ms while a crossover moves
    m_lrbt += samplesblock / srate;
    if (m_lin && m_lrb && m_lrbt >= 0.04) {
        if (m_nb > 1) firBuild();
        m_lrb = false;
        m_lrbt = 0;
    }

    // settings for this block (from the other set while Swap is held)
    double t = cpv(P_IN);
    if (t != c_lin) { c_lin = t; c_ing = epow(10, t / 20); }
    c_mix = cpv(P_MIX) * 0.01;
    c_ext = cpv(P_SC) >= 0.5;
    m_sct = epow(10, cpv(P_SCT) / 20);
    c_del = H.pv(P_DELTA) >= 0.5;
    m_mode = (int)emax(0, emin(3, std::floor(cpv(P_CMODE) + 0.5)));
    m_mono = m_mode == 0;
    m_ms = m_mode == 2;
    m_lk = m_mono ? 1 : cpv(P_SLINK) * 0.01;
    m_lkl = cpv(P_LKL) >= 0.5;
    // one detector and one gain per band for both channels: Mono, or full link without Mid/Side
    m_one = m_mono || (m_lk >= 0.999 && !m_ms);
    m_pwr = m_mono || m_lkl;
    // lookahead: the latched set's (a held Swap keeps the delay, so the latency never jumps)
    c_la = H.pv(P_LA) >= 0.5;
    m_ld = c_la ? (int)emax(1, emin(2000, std::floor(H.pv(P_LAMS) * 0.001 * srate + 0.5))) : 0;
    m_ld = m_lin ? std::min(m_ld, m_lat) : std::min(m_ld, 2047);
    c_ld = m_lin ? m_lat : m_ld;
    c_latms = c_ld / srate * 1000;
    m_gd = m_lat - m_ld;   // linear phase: how long the gains wait to meet the audio

    m_detm = (int)emax(0, emin(2, std::floor(cpv(P_DET) + 0.5)));
    m_blend = cpv(P_BLEND) * 0.01;
    m_rmst = emax(0.01, cpv(P_RMST));
    m_rk = 1 - std::exp(-1 / (m_rmst * 0.001 * srate));
    m_tw = m_detm == 1 ? m_rmst : (m_detm == 2 ? m_rmst * m_blend : 0);
    // the gain computers run every m_tk samples (a 12 kHz control rate at 48 kHz); their time constants are per tick
    m_tk = srate <= 50000 ? 4 : (srate <= 100000 ? 8 : 16);
    m_tki = 1.0 / m_tk;
    m_cr = srate / m_tk;
    m_ska = 1 - std::exp(-1 / (0.3 * m_cr));
    m_skr = 1 - std::exp(-1 / (1 * m_cr));
    e_dck = 1 - std::exp(-2 * PI * (8 / srate));

    // each band
    m_anyon = false;
    m_anysolo = false;
    for (int b = 0; b < NB; ++b) {
        if (b < m_nb) {
            BON[b] = cpv(bp(b, K_ON)) >= 0.5;
            m_anyon |= BON[b];
            // the band's Character: curve, release, feedback and saturation (voicings inspired by the circuits, not emulations)
            int ch = (int)emax(0, emin(4, std::floor(cpv(bp(b, K_CHAR)) + 0.5)));
            int cv = (int)emax(0, emin(2, std::floor(cpv(bp(b, K_CURVE)) + 0.5)));
            int rm = (int)emax(0, emin(2, std::floor(cpv(bp(b, K_RELM)) + 0.5)));
            double fb = cpv(bp(b, K_FB)) * 0.01, sa = cpv(bp(b, K_SAT)) * 0.01;
            bool st = cpv(bp(b, K_SATT)) >= 0.5;
            if (ch == 1) { cv = 0; rm = 1; fb = 0; sa = 0; }
            if (ch == 2) { cv = 2; rm = 2; fb = 0.5; sa = 0.15; st = false; }
            if (ch == 3) { cv = 0; rm = 1; fb = 0.6; sa = 0.35; st = false; }
            if (ch == 4) { cv = 1; rm = 1; fb = 0.4; sa = 0.3; st = true; }
            if (c_la) fb = 0;   // a feedback detector can't look ahead
            BCV[b] = cv; BRELM[b] = rm; BFBK[b] = fb;
            BENVS[b] = ch > 0 ? true : cpv(bp(b, K_ENV)) >= 0.5;
            // (a band that is off is left alone: the JSFX still ran its Character's saturation)
            BSATON[b] = BON[b] && sa > 0.001;
            BSD[b] = 1 + 5 * sa;
            BSB[b] = st ? 0.2 / BSD[b] : 0;
            BSATT[b] = st;
            BSC[b] = e_tanh(BSD[b] * BSB[b]) / BSD[b];
            BSG[b] = 1 - sqr(e_tanh(BSD[b] * BSB[b]));
            if (!BSATON[b]) memset(SATS + b * 8, 0, 8 * sizeof(double));
            BSOL[b] = H.pv(bp(b, K_SOLO)) >= 0.5;
            m_anysolo |= BSOL[b];
            BEXP[b] = cpv(bp(b, K_MODE)) >= 0.5;
            BTHR[b] = cpv(bp(b, K_THR));
            double r = cpv(bp(b, K_RAT));
            BSL[b] = BEXP[b] ? (r >= 99.5 ? 1000 : r - 1) : (r >= 99.5 ? 1 : 1 - 1 / emax(1, r));
            BKN[b] = emax(0.0001, cpv(bp(b, K_KNEE)));
            r = cpv(bp(b, K_RNG));
            BRNG[b] = std::fabs(r);
            BSGN[b] = r < 0 ? -1 : 1;
            // makeup: by hand, or auto (the band's average gain change while there's signal, about 2 s, turned around)
            if (BYN[b] > 0) BMKA[b] += (1 - std::exp(-samplesblock / (2 * srate))) * (-BYS[b] / BYN[b] - BMKA[b]);
            BYS[b] = 0;
            BYN[b] = 0;
            BMK[b] = BON[b] ? epow(10, (cpv(bp(b, K_AMK)) >= 0.5 ? BMKA[b] : cpv(bp(b, K_MK))) / 20) : 1;
            BHT[b] = std::floor(cpv(bp(b, K_HLD)) * 0.001 * m_cr + 0.5);
            // the labelled attack and release are what you get: the RMS window is taken off them
            double att = cpv(bp(b, K_ATT)), rel = cpv(bp(b, K_REL));
            if (m_tw > 0) {
                att = std::sqrt(emax(0.0001, att * att - m_tw * m_tw));
                rel = std::sqrt(emax(1, rel * rel - m_tw * m_tw));
            }
            // the peak envelope falls with the release (at most 40 ms), the gain releases over the rest of it
            double td = BENVS[b] ? emax(1, emin(40, 0.4 * rel)) : 0;
            BDR[b] = BENVS[b] ? std::exp(-1 / (td * 0.001 * srate)) : 0;
            rel = emax(1, BENVS[b] ? emax(0.5 * rel, rel - td) : rel);
            BAA[b] = std::exp(-1 / (emax(BENVS[b] ? 0.05 : 0.01, att) * 0.001 * m_cr));
            BAR[b] = std::exp(-1 / (rel * 0.001 * m_cr));
            BRR[b] = 1 / (rel * 0.001 * m_cr);
            BARF[b] = std::exp(-1 / (emax(1, rel * 0.25) * 0.001 * m_cr));
            BARS[b] = std::exp(-1 / (rel * 2 * 0.001 * m_cr));
            BASA[b] = std::exp(-1 / (emax(1, rel * 0.5) * 0.001 * m_cr));
            BMODE2[b] = BRELM[b] == 2 && !BEXP[b];
            // feedback (a Compress band that cuts): the detector hears the gain change; the slope is raised so the
            // static curve stays the one set
            BFB[b] = BFBK[b] > 0 && !BEXP[b] && BSGN[b] < 0;
            if (BFB[b]) {
                BSL[b] = BSL[b] / emax(0.05, 1 - BFBK[b] * BSL[b]);
                BSL[b] = emin(BSL[b], 0.9 * (((1 + BAA[b]) / emax(0.000001, 1 - BAA[b])) / BFBK[b]));
            }
        } else {
            BON[b] = false;
            BSOL[b] = false;
        }
        // program-dependent release: how slow, from how deep and long the band has been working (once per block)
        if (b < m_nb && BRELM[b] == 1) {
            double *s = MST + b * 32;
            s[6] = std::exp(-BRR[b] / (0.3 + 1.4 * emin(1, s[5] * 0.125)));
            s += 16;
            s[6] = std::exp(-BRR[b] / (0.3 + 1.4 * emin(1, s[5] * 0.125)));
        }
        // the window: each band's largest gain change and loudest level in the last block
        BGRD[b] = BGR[b];
        BGR[b] = 0;
    }
    // keep the filter and follower states out of the denormal range
    for (double &v : XS) flush21(v);
    {
        double s = 0;
        for (double v : SLR) s += std::fabs(v);
        if (s < 0.000000000001) memset(SLR, 0, sizeof SLR);
    }
    for (double &v : MST) flush21(v);
    for (double &v : BGI) flush21(v);
    for (double &v : SATS) flush21(v);

    c_dm = 1 - c_mix;
    c_wmt = c_mix;
    c_zk = 1 - std::exp(-1 / (0.01 * srate));
    if (e_first) { c_ings = c_ing; c_dms = c_dm; c_wms = c_wmt; e_first = false; }

    // meters for the window: input peak (after Input), loudest level the detector saw
    double dur = samplesblock / srate;
    c_mi0 = emax(c_ip0 > 0.0000000001 ? 20 * std::log10(c_ip0) : -200, c_mi0 - 20 * dur);
    c_mi1 = emax(c_ip1 > 0.0000000001 ? 20 * std::log10(c_ip1) : -200, c_mi1 - 20 * dur);
    c_ip0 = 0;
    c_ip1 = 0;
    c_dlv = c_vdot > 0.0000000001 ? 20 * std::log10(c_vdot) : -200;
    c_vdot = 0;
    anw = ab.uion;
    V.mi0.store(c_mi0, std::memory_order_relaxed);
    V.mi1.store(c_mi1, std::memory_order_relaxed);
    if (ab.uion)
        for (int b = 0; b < NB; ++b) {
            V.bgr[b].store((float)BGRD[b], std::memory_order_relaxed);
            V.blv[b].store((float)BLV[b], std::memory_order_relaxed);
            V.bmka[b].store((float)BMKA[b], std::memory_order_relaxed);
        }
    for (double &v : BLV) v = -200;

    // "Set thresholds": listen, then set every band's threshold
    tgBlock();
}

void Engine::afterAct()
{
    if (!idDrawn && A.myid != 0) { eel_rand(899999); idDrawn = true; }
}

// ======================================================================== per sample
void Engine::samples(const double *const *in, const double *const *sc, double *const *out, int n)
{
    if ((int)br0.size() < n) {
        br0.resize(n); br1.resize(n); bx0.resize(n); bx1.resize(n); bq0.resize(n); bq1.resize(n);
    }
    const bool uion = ab.uion;
    const int tgs = A.tgs;
    double *const o0 = out[0], *const o1 = out[1];
    for (int i = 0; i < n; ++i) {
        // Input, dry and wet gains glide (no zipper noise when they move)
        c_ings += c_zk * (c_ing - c_ings);
        c_dms += c_zk * (c_dm - c_dms);
        c_wms += c_zk * (c_wmt - c_wms);
        double c_r0 = in[0][i], c_r1 = in[1][i];
        double c_x0 = c_r0 * c_ings, c_x1 = c_r1 * c_ings;
        // the detector's signal: the input or the sidechain (with its trim)
        double c_d0, c_d1;
        if (c_ext) {
            c_d0 = (sc && sc[0] ? sc[0][i] : 0) * m_sct;
            c_d1 = (sc && sc[1] ? sc[1][i] : 0) * m_sct;
        } else {
            c_d0 = c_x0;
            c_d1 = c_x1;
        }
        double t = emax(std::fabs(c_d0), std::fabs(c_d1));
        if (t > c_vdot) c_vdot = t;

        // ---- the bands: for the detector (always the zero-latency split, so it hears every sample as it arrives),
        // and for the audio (the same split, or in linear phase the convolution) ----
        if (!m_lin || !c_ext) {
            split(c_x0, SLR, BA);
            split(c_x1, SLR + 18, BA + 4);
        }
        const double *m_pd = BA;
        if (c_ext) {
            split(c_d0, XS + 36, BD);
            split(c_d1, XS + 54, BD + 4);
            m_pd = BD;
        }
        if (m_lin) {
            // the convolution's input; the input and raw input delayed by the latency
            LIB[m_lb + m_lpos] = c_x0;
            LIB[8192 + m_lb + m_lpos] = c_x1;
            DLY[m_dp] = c_x0; DLY[16384 + m_dp] = c_x1; DLR[m_dp] = c_r0; DLR[16384 + m_dp] = c_r1;
            int q = (m_dp - m_lat) & 16383;
            m_dx0 = DLY[q]; m_dx1 = DLY[16384 + q]; m_rd0 = DLR[q]; m_rd1 = DLR[16384 + q];
            m_dp = (m_dp + 1) & 16383;
            // the bands from the low passes (crossfaded from the old filters over a block after a change)
            double u = m_lxf ? 0.5 - 0.5 * std::cos(PI * ((double)m_lpos / m_lb)) : 0;
            for (int c = 0; c < 2; ++c) {
                double p0 = 0;
                for (int k = 0; k < m_nb - 1; ++k) {
                    int o = (c * 3 + k) * 4096 + m_lpos;
                    double p1 = m_lxf ? LPX[o] + (LPO[o] - LPX[o]) * u : LPO[o];
                    BAU[c * 4 + k] = p1 - p0;
                    p0 = p1;
                }
                BAU[c * 4 + m_nb - 1] = (c ? m_dx1 : m_dx0) - p0;
            }
            m_lpos += 1;
            if (m_lpos >= m_lb) { linRun(); m_lpos = 0; }
        }

        // ---- each band's detector (every sample), then every m_tk samples its gain computer and ballistics; the gains
        // glide between those ticks ----
        for (int b = 0; b < m_nb; ++b) {
            if (!BON[b]) continue;
            double v0 = m_pd[b], v1 = m_pd[4 + b];
            if (m_ms) { double tt = (v0 + v1) * 0.5; v1 = (v0 - v1) * 0.5; v0 = tt; }
            double *s0 = MST + b * 32;
            if (m_one) {
                det(b, s0, m_pwr ? std::sqrt(0.5 * (v0 * v0 + v1 * v1)) : emax(std::fabs(v0), std::fabs(v1)));
            } else {
                det(b, s0, v0);
                det(b, s0 + 16, v1);
            }
        }
        if ((m_tc -= 1) <= 0) {
            m_tc = m_tk;
            for (int b = 0; b < m_nb; ++b) {
                double *s0 = MST + b * 32, *s1 = s0 + 16;
                if (BON[b]) {
                    double y0, y1, lv;
                    if (m_one) {
                        double l0 = dbOf(s0);
                        if (BFB[b]) l0 -= BFBK[b] * s0[2];
                        y0 = gain(b, s0, l0);
                        y1 = y0;
                        lv = l0;
                    } else {
                        double l0 = dbOf(s0), l1 = dbOf(s1);
                        if (BFB[b]) { l0 -= BFBK[b] * s0[2]; l1 -= BFBK[b] * s1[2]; }
                        // the stereo link: the louder side (Max) or the power average
                        double ref = m_lkl ? emax(l0, l1) + 10 * std::log10(0.5 * (1 + epow(10, -std::fabs(l0 - l1) * 0.1))) : emax(l0, l1);
                        l0 += m_lk * (ref - l0);
                        l1 += m_lk * (ref - l1);
                        y0 = gain(b, s0, l0);
                        y1 = gain(b, s1, l1);
                        lv = emax(l0, l1);
                    }
                    BGI[b] = ((ne(y0, 0) ? std::exp(y0 * 0.11512925464970229) : 1) - BG[b]) * m_tki;
                    BGI[4 + b] = ((ne(y1, 0) ? std::exp(y1 * 0.11512925464970229) : 1) - BG[4 + b]) * m_tki;
                    if (uion) {
                        if (std::fabs(y0) > std::fabs(BGR[b])) BGR[b] = y0;
                        if (std::fabs(y1) > std::fabs(BGR[b])) BGR[b] = y1;
                        if (lv > BLV[b]) BLV[b] = lv;
                    }
                    if (lv > -60) { BYS[b] += (y0 + y1) * 0.5; BYN[b] += 1; }
                    // "Set thresholds" is listening: each millisecond's loudest level (Compress) or quietest (Expand)
                    if (tgs == 2) {
                        if (BEXP[b] ? lv < TGMX[b] : lv > TGMX[b]) TGMX[b] = lv;
                    }
                } else {
                    BG[b] = 1; BG[4 + b] = 1; BGI[b] = 0; BGI[4 + b] = 0;
                }
            }
        }
        for (int k = 0; k < 8; ++k) BG[k] += BGI[k];
        if (tgs == 2) {
            if ((c_tc += 1) >= c_tms) {
                if (c_tgn < 30000) {
                    for (int b = 0; b < NB; ++b) { TGLV[b * 30000 + c_tgn] = TGMX[b]; TGMX[b] = BEXP[b] ? 1000 : -1000; }
                    c_tgn += 1;
                }
                c_tc = 0;
            }
        }

        // ---- the audio and the gains meet: zero latency, the bands wait for the lookahead; linear phase, the gains
        // wait for the audio (less the lookahead) ----
        const double *m_pa, *m_pg;
        if (m_lin) {
            for (int k = 0; k < 8; ++k) GRG[k * 16384 + m_gp] = BG[k];
            int q = (m_gp - m_gd) & 16383;
            for (int k = 0; k < 8; ++k) BGU[k] = GRG[k * 16384 + q];
            m_gp = (m_gp + 1) & 16383;
            m_pa = BAU;
            m_pg = BGU;
        } else {
            if (m_ld > 0) {
                int q = (m_zp - m_ld) & 2047;
                double *z = ZLA.data();
                for (int k = 0; k < m_nb; ++k) {
                    z[k * 2048 + m_zp] = BA[k];
                    BAU[k] = z[k * 2048 + q];
                    z[(4 + k) * 2048 + m_zp] = BA[4 + k];
                    BAU[4 + k] = z[(4 + k) * 2048 + q];
                }
                z[8 * 2048 + m_zp] = c_r0;
                z[9 * 2048 + m_zp] = c_r1;
                m_rd0 = z[8 * 2048 + q];
                m_rd1 = z[9 * 2048 + q];
                m_zp = (m_zp + 1) & 2047;
                m_pa = BAU;
            } else {
                m_rd0 = c_r0;
                m_rd1 = c_r1;
                m_pa = BA;
            }
            m_pg = BG;
        }

        // ---- the bands back together: each at its gain (Mid/Side: on its mid and side) and makeup; solo picks bands.
        // Dry is the bands untouched (the same phase as the wet, so dry/wet never combs); Delta is what the gains took
        // away ----
        double w0 = 0, w1 = 0, e0 = 0, e1 = 0, s0 = 0, s1 = 0;
        for (int b = 0; b < m_nb; ++b) {
            double a0 = m_pa[b], a1 = m_pa[4 + b], y0, y1;
            if (m_ms) {
                double tt = (a0 + a1) * 0.5 * m_pg[b], uu = (a0 - a1) * 0.5 * m_pg[4 + b];
                y0 = tt + uu;
                y1 = tt - uu;
            } else {
                y0 = a0 * m_pg[b];
                y1 = a1 * m_pg[4 + b];
            }
            if (!m_anysolo || BSOL[b]) {
                e0 += a0 - y0;
                e1 += a1 - y1;
                // the band's saturation (its Character), after the gain
                if (BSATON[b]) { y0 = sat(b, y0, SATS + b * 8); y1 = sat(b, y1, SATS + b * 8 + 4); }
                w0 += y0 * BMK[b];
                w1 += y1 * BMK[b];
            }
            s0 += a0;
            s1 += a1;
        }
        if (c_del) { o0[i] = e0; o1[i] = e1; }
        else if (m_anysolo) { o0[i] = w0; o1[i] = w1; }
        else { o0[i] = s0 * c_dms + w0 * c_wms; o1[i] = s1 * c_dms + w1 * c_wms; }
        br0[i] = c_r0; br1[i] = c_r1; bx0[i] = c_x0; bx1[i] = c_x1; bq0[i] = m_rd0; bq1[i] = m_rd1;
    }
}

// ---------- after the built-in Act: meters and level matching (only for the window), held Bypass / Swap, analyzer ----------
void Engine::post(double *const *out, int n)
{
    for (int i = 0; i < n; ++i) {
        if (ab.uion) {
            double a = std::fabs(bx0[i]);
            if (a > c_ip0) c_ip0 = a;
            a = std::fabs(bx1[i]);
            if (a > c_ip1) c_ip1 = a;
        }
        ab.post(br0[i], br1[i], out[0][i], out[1][i], bq0[i], bq1[i]);
        if (anw) V.an.put((bx0[i] + bx1[i]) * 0.5, (out[0][i] + out[1][i]) * 0.5);
    }
    if (anw) V.an.publish();
}

} // namespace mb
} // namespace ts
