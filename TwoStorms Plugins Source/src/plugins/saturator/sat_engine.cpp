#include "sat_engine.h"
#include "../../core/eelrand.h"
#include "../../core/filters.h"
#include "../../core/satcore.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ts {
namespace sat {

// (pre-gain, pre 1, pre 2, post 1, post 2); filter types as the EQ's: 0 bell, 1 low shelf, 2 high shelf,
// 5 high pass 6, 6 high pass 12, 10 low pass 12, 11 low pass 24
const double STYT[NSTY][17] = {
    {0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0},                          // Clean Tube
    {0, -1, 0, 0, 0, -1, 0, 0, 0, 1, 100, 1, 0.7, 2, 8000, -1.5, 0.7},                // Warm Tube
    {2, 0, 2000, 2, 0.8, -1, 0, 0, 0, 1, 90, 1.5, 0.7, 10, 12000, 0, 0.7},            // Hot Tube
    {3, 5, 100, 0, 0.7, -1, 0, 0, 0, -1, 0, 0, 0, 10, 8000, 0, 0.7},                  // Broken Tube
    {0, 2, 6000, 3, 0.7, -1, 0, 0, 0, 2, 6000, -3, 0.7, 1, 100, 0.5, 0.7},            // Clean Tape
    {0, 2, 5000, 5, 0.7, -1, 0, 0, 0, 2, 5000, -5, 0.7, 0, 80, 1.5, 0.8},             // Warm Tape
    {0, 2, 4000, 6, 0.7, -1, 0, 0, 0, 2, 4000, -6, 0.7, 1, 100, 2, 0.7},              // Hot Tape
    {0, 2, 3000, 8, 0.7, -1, 0, 0, 0, 2, 3000, -8, 0.7, 10, 9000, 0, 0.7},            // Extreme Tape
    {0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0},                          // Tape Head
    {0, 1, 120, 4, 0.7, -1, 0, 0, 0, 1, 120, -4, 0.7, -1, 0, 0, 0},                   // Subtle Transformer
    {0, 1, 150, 7, 0.7, -1, 0, 0, 0, 1, 150, -7, 0.7, 2, 8000, -1, 0.7},              // Warm Transformer
    {0, 1, 200, 9, 0.7, -1, 0, 0, 0, 1, 200, -9, 0.7, 2, 6000, -2, 0.7},              // Lush Transformer
    {0, 5, 80, 0, 0.7, 0, 700, -3, 0.7, 0, 3000, 2, 0.8, 10, 8000, 0, 0.7},           // American Amp
    {3, 5, 120, 0, 0.7, 0, 1500, 6, 0.8, 10, 5500, 0, 0.7, 0, 200, 2, 0.8},           // British Amp
    {2, 5, 60, 0, 0.7, -1, 0, 0, 0, 10, 7000, 0, 0.7, -1, 0, 0, 0},                   // Smooth Amp
    {3, 5, 100, 0, 0.7, 0, 900, 4, 0.8, 10, 6500, 0, 0.7, -1, 0, 0, 0},               // Crunch Amp
    {6, 6, 700, 0, 0.7, 0, 800, 8, 0.6, 11, 4500, 0, 0.7, -1, 0, 0, 0},               // Scream Amp
    {0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0},                          // Console Channel
    {0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0},                          // Console Buss
    {0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0},                          // Even Harmonics
    {0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0},                          // Inflator
    {4, 6, 60, 0, 0.7, -1, 0, 0, 0, 10, 5000, 0, 0.7, 0, 400, 3, 0.8},                // Fuzz
    {0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0},                          // Smudge
    {0, -1, 0, 0, 0, -1, 0, 0, 0, 10, 4500, 0, 0.7, -1, 0, 0, 0},                     // Breakdown
    {0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0},                          // Foldback
    {0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0},                          // Sine Fold
    {0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0},                          // Rectify
    {0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0},                          // Destroy
    {0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0},                          // Soft Clip
    {0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0},                          // Hard Clip
};
const char *const STYLE_NAMES[NSTY] = {"Clean Tube", "Warm Tube", "Hot Tube", "Broken Tube", "Clean Tape", "Warm Tape", "Hot Tape",
                                       "Extreme Tape", "Tape Head", "Subtle Transformer", "Warm Transformer", "Lush Transformer",
                                       "American Amp", "British Amp", "Smooth Amp", "Crunch Amp", "Scream Amp", "Console Channel",
                                       "Console Buss", "Even Harmonics", "Inflator", "Fuzz", "Smudge", "Breakdown", "Foldback",
                                       "Sine Fold", "Rectify", "Destroy", "Soft Clip", "Hard Clip"};
// 1 = the style runs without oversampling (Destroy)
static const int STYF[NSTY] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0};
// the oversampling halfband filters: polyphase all-pass pairs (3 + 3 for 1 -> 2, 2 + 2 for 2 -> 4)
static const double OSA[6] = {0.042739, 0.324518, 0.698222, 0.159812, 0.509278, 0.893506};
static const double OSB[4] = {0.048108, 0.436836, 0.193414, 0.778879};
static const double QX[4] = {0.5411961, 1.306563, 0.5411961, 1.306563};
// the modulation LFOs' note values (in beats): Free, 1/32, 1/16, 1/8, 1/4, 1/2, 1 bar, 2 bars, 4 bars, 1/16T, 1/8T, 1/4T, 1/8.
static const double SYNB[16] = {0, 0.125, 0.25, 0.5, 1, 2, 4, 8, 16, 1.0 / 6, 1.0 / 3, 2.0 / 3, 0.75, 0, 0, 0};
static const int MTP[12] = {K_DRIVE, K_MIX, K_LEVEL, K_DYN, K_FB, K_FBF, K_BASS, K_MID, K_TRE, K_PRES, K_DPAN, K_LPAN};
static const double MTS[12] = {100, 100, 24, 100, 100, 5, 12, 12, 12, 12, 100, 100};

static inline double bq(double x, const double *o, double *z)
{
    double y = o[0] * x + z[0];
    z[0] = o[1] * x - o[3] * y + z[1];
    z[1] = o[2] * x - o[4] * y;
    return y;
}
static inline double chain(double x, const double *o, int n, double *z)
{
    for (int k = 0; k < n; ++k) x = bq(x, o + k * 5, z + k * 2);
    return x;
}
// the half-band stages (3 and 2 all-passes per path)
static inline void ups3(double x, double *z, const double *c, double *o)
{
    double y = x, w;
    w = y - c[0] * z[0]; y = c[0] * w + z[0]; z[0] = w;
    w = y - c[1] * z[1]; y = c[1] * w + z[1]; z[1] = w;
    w = y - c[2] * z[2]; y = c[2] * w + z[2]; z[2] = w;
    o[0] = y;
    y = x;
    w = y - c[3] * z[3]; y = c[3] * w + z[3]; z[3] = w;
    w = y - c[4] * z[4]; y = c[4] * w + z[4]; z[4] = w;
    w = y - c[5] * z[5]; y = c[5] * w + z[5]; z[5] = w;
    o[1] = y;
}
static inline double dns3(double x0, double x1, double *z, const double *c)
{
    double y = x0, w, y0;
    w = y - c[0] * z[0]; y = c[0] * w + z[0]; z[0] = w;
    w = y - c[1] * z[1]; y = c[1] * w + z[1]; z[1] = w;
    w = y - c[2] * z[2]; y = c[2] * w + z[2]; z[2] = w;
    y0 = y;
    y = z[6];
    w = y - c[3] * z[3]; y = c[3] * w + z[3]; z[3] = w;
    w = y - c[4] * z[4]; y = c[4] * w + z[4]; z[4] = w;
    w = y - c[5] * z[5]; y = c[5] * w + z[5]; z[5] = w;
    z[6] = x1;
    return 0.5 * (y0 + y);
}
static inline void ups2(double x, double *z, const double *c, double *o)
{
    double y = x, w;
    w = y - c[0] * z[0]; y = c[0] * w + z[0]; z[0] = w;
    w = y - c[1] * z[1]; y = c[1] * w + z[1]; z[1] = w;
    o[0] = y;
    y = x;
    w = y - c[2] * z[2]; y = c[2] * w + z[2]; z[2] = w;
    w = y - c[3] * z[3]; y = c[3] * w + z[3]; z[3] = w;
    o[1] = y;
}
static inline double dns2(double x0, double x1, double *z, const double *c)
{
    double y = x0, w, y0;
    w = y - c[0] * z[0]; y = c[0] * w + z[0]; z[0] = w;
    w = y - c[1] * z[1]; y = c[1] * w + z[1]; z[1] = w;
    y0 = y;
    y = z[4];
    w = y - c[2] * z[2]; y = c[2] * w + z[2]; z[2] = w;
    w = y - c[3] * z[3]; y = c[3] * w + z[3]; z[3] = w;
    z[4] = x1;
    return 0.5 * (y0 + y);
}
static inline double dk(double f, double r) { return 1 - std::exp(-2 * PI * (f / r)); }
static inline double dkf(double c, double osf) { return 1 - std::exp(std::log(1 - c) / osf); }
static inline double dss(double x) { return x < -1 ? -1 : (x > 1 ? 1 : x * 1.5 - x * x * x * 0.5); }
// the loudness compensation for style st at drive d (0..100)
static double cmp(int st, double d)
{
    int i = (int)emin(19, std::floor(d / 5));
    double f = d / 5 - i;
    const double *o = SAT_CMPT[st] + i;
    return o[0] + (o[1] - o[0]) * f;
}

// ======================================================================== setup
void Engine::init(double sr)
{
    srate = sr;
    if (!nonceDrawn) { eel_rand(1000000); nonceDrawn = true; }   // the instance nonce (the JSFX's first rand in @init)
#define Z(a) memset(a, 0, sizeof a)
    Z(BSZ); Z(PCO); Z(PNS); Z(QNS); Z(BST); Z(BOS); Z(TON); Z(BON); Z(BMS); Z(FBON); Z(TCH);
    Z(DGc); Z(DGi); Z(PGc); Z(PGi); Z(DRVP); Z(MXc); Z(MXi); Z(GDc); Z(GDi); Z(FBc); Z(FBi); Z(OGc); Z(OGt);
    Z(EF); Z(ES); Z(FBK); Z(GDB); Z(EO); Z(DDL); Z(BSG); Z(UPO);
    Z(XL); Z(XH); Z(XA); Z(XFc); Z(XFt); Z(XFd); Z(ZXL); Z(ZXH); Z(ZA); Z(DRW);
    Z(EFFP); Z(MV); Z(MPH); Z(MRA); Z(MRB); Z(MEN); Z(MKA); Z(MKR); Z(MCC); Z(MLS); Z(MSR); Z(MSB); Z(MFL);
    Z(CCH); Z(CBS);
#undef Z
    for (int b = 0; b < NB; ++b) { BSZ[b * 2 * BSTR + ZSH] = 1; BSZ[b * 2 * BSTR + BSTR + ZSH] = 1; }   // breakdown's flip-flop starts at +1
    d_nf = 0; d_beat = 0; d_tempo = 120; d_nvel = 0; d_nheld = 0; d_pb = 0;
    TH_K1 = TH_K2 = TH_K3 = TH_G3 = SM_L = BD_K = 0;
    d_nb = 3; d_slope = 1; d_os = 0; d_osf = 1; d_tc = 0; d_dint = 0; d_dp = 0; d_rp = 0;
    d_snap = true; d_del = false; d_csr = 0; d_cdp = 0; d_dd = 0; d_ingc = 0; d_ingt = 1;
    DC_R = D_KAE = D_KRE = D_KS = 0;
    c_ip0 = 0; c_ip1 = 0; c_mi0 = -200; c_mi1 = -200;
    uisolo = -1;
    V.an.clear();
}

void Engine::reset()
{
    memset(BSZ, 0, sizeof BSZ);
    for (int b = 0; b < NB; ++b) { BSZ[b * 2 * BSTR + ZSH] = 1; BSZ[b * 2 * BSTR + BSTR + ZSH] = 1; }
    memset(DDL, 0, sizeof DDL);
    memset(DRW, 0, sizeof DRW);
    xzero();
}

// ======================================================================== the crossovers
// crossover j at f: Linkwitz-Riley 12 / 24 / 48 dB per octave, with its all-pass for the bands below
void Engine::xdes(int j, double f, int slope)
{
    double w = 2 * PI * (emin(f, srate * 0.45) / srate);
    if (slope == 0) {
        double t = std::tan(w * 0.5), a1 = (t - 1) / (t + 1);
        for (int i = 0; i < 2; ++i) {
            double *lp = XL[j][i], *hp = XH[j][i];
            lp[0] = t / (1 + t); lp[1] = lp[0]; lp[2] = 0; lp[3] = a1; lp[4] = 0;
            hp[0] = 1 / (1 + t); hp[1] = -hp[0]; hp[2] = 0; hp[3] = a1; hp[4] = 0;
        }
        double *ap = XA[j][0];
        ap[0] = a1; ap[1] = 1; ap[2] = 0; ap[3] = a1; ap[4] = 0;
    } else {
        int n = slope == 1 ? 2 : 4;
        double cw = std::cos(w);
        for (int i = 0; i < n; ++i) {
            double q = slope == 1 ? 0.7071068 : QX[i];
            double al = std::sin(w) / (2 * q), a0 = 1 + al;
            double *lp = XL[j][i], *hp = XH[j][i];
            lp[0] = (1 - cw) * (0.5 / a0); lp[1] = (1 - cw) / a0; lp[2] = lp[0]; lp[3] = -2 * (cw / a0); lp[4] = (1 - al) / a0;
            hp[0] = (1 + cw) * (0.5 / a0); hp[1] = -(1 + cw) / a0; hp[2] = hp[0]; hp[3] = lp[3]; hp[4] = lp[4];
        }
        n = slope == 1 ? 1 : 2;
        for (int i = 0; i < n; ++i) {
            double q = slope == 1 ? 0.7071068 : QX[i];
            double al = std::sin(w) / (2 * q), a0 = 1 + al;
            double *ap = XA[j][i];
            ap[0] = (1 - al) / a0; ap[1] = -2 * (cw / a0); ap[2] = 1; ap[3] = ap[1]; ap[4] = ap[0];
        }
    }
}

void Engine::xzero()
{
    memset(ZXL, 0, sizeof ZXL);
    memset(ZXH, 0, sizeof ZXH);
    memset(ZA, 0, sizeof ZA);
}

void Engine::xflush()
{
    double *a = &ZXL[0][0][0][0], *h = &ZXH[0][0][0][0], *p = &ZA[0][0][0][0][0];
    for (int i = 0; i < 2 * 5 * 4 * 2; ++i) { flush21(a[i]); flush21(h[i]); }
    for (int i = 0; i < 2 * 6 * 5 * 2 * 2; ++i) flush21(p[i]);
}

// the input into the bands (BSG[0..5] left, BSG[8..13] right): low part off at each crossover, the rest goes on;
// each band below a later crossover goes through that crossover's all-pass, so the bands add up flat
void Engine::split(double xl, double xr)
{
    double bl[6] = {}, br[6] = {};
    const int ns = d_slope == 2 ? 4 : 2;
    for (int j = 0; j < d_nb - 1; ++j) {
        double t = xl;
        for (int k = 0; k < ns; ++k) t = bq(t, XL[j][k], ZXL[0][j][k]);
        bl[j] = t;
        t = xl;
        for (int k = 0; k < ns; ++k) t = bq(t, XH[j][k], ZXH[0][j][k]);
        xl = t;
        t = xr;
        for (int k = 0; k < ns; ++k) t = bq(t, XL[j][k], ZXL[1][j][k]);
        br[j] = t;
        t = xr;
        for (int k = 0; k < ns; ++k) t = bq(t, XH[j][k], ZXH[1][j][k]);
        xr = t;
    }
    bl[d_nb - 1] = xl;
    br[d_nb - 1] = xr;
    for (int j = 1; j < d_nb - 1; ++j) {
        for (int i = 0; i < j; ++i) {
            bl[i] = bq(bl[i], XA[j][0], ZA[0][i][j][0]);
            if (d_slope == 2) bl[i] = bq(bl[i], XA[j][1], ZA[0][i][j][1]);
        }
        for (int i = 0; i < j; ++i) {
            br[i] = bq(br[i], XA[j][0], ZA[1][i][j][0]);
            if (d_slope == 2) br[i] = bq(br[i], XA[j][1], ZA[1][i][j][1]);
        }
    }
    for (int b = 0; b < 6; ++b) { BSG[b] = bl[b]; BSG[8 + b] = br[b]; }
}

// ======================================================================== the bands
// the stateful styles' rate-dependent constants (the shaper runs at srate * oversampling)
void Engine::rates()
{
    double r = srate * d_osf;
    CCH[0] = dk(120, r); CCH[1] = dk(r, r); CCH[2] = dk(5, r); CCH[3] = dk(80, r); CCH[4] = dkf(0.512, d_osf); CCH[5] = dk(320, r); CCH[6] = dk(20000, r);
    CBS[0] = dk(220, r); CBS[1] = dk(r, r); CBS[2] = dk(5, r); CBS[3] = dk(100, r); CBS[4] = dkf(0.513, d_osf); CBS[5] = dk(320, r); CBS[6] = dk(20000, r); CBS[7] = dk(8, r);
    TH_K2 = 2 * std::sin(3680 * (PI / r));
    TH_K3 = -1.4 * TH_K2;
    TH_K1 = 5.0 / 7;
    // the JSFX's -10 ^ (...): EEL2 reads it as (-10) ^ (...), a negative number to a fractional power, which it
    // makes 0; so the tape head's third path is silent there, and here
    TH_G3 = epow(-10, (r < 88200 ? 1.4 : 2.0) * 3 / 20) * TH_K1;
    SM_L = 0.16 / d_osf;
    BD_K = 0.002 / d_osf;
}

// one sample of style st at the shaper's rate; sz = this band and channel's shaper state (16 numbers)
double Engine::shape(int st, double u, double *sz)
{
    double y, c;
    if (st == 8) {
        double x1 = emax(-1, emin(1, u));
        sz[0] += sz[1] * TH_K2;
        sz[2] = sz[0] * TH_K1 + sz[1] - x1;
        sz[1] += sz[2] * TH_K3;
        y = emax(-1, emin(1, (emax(-1, emin(1, sz[2])) * TH_G3 + dss(sz[0] * 1.516) + dss(sz[1] * 1.516)) * 0.4956));
    } else if (st == 17) {
        // console channel: a feedback network around a rational saturator
        double x1 = 2.6 * ds_th(u / 2.6) * 0.3981;
        double s0 = (sz[0] += (x1 * 0.4 - sz[0]) * CCH[0]);
        double s1 = (sz[1] += (0.0002135622 - sz[1]) * CCH[1]);
        double x2 = s0 + 1.4142135622 - s1;
        double x3 = x2 * x2 * 0.5 - 1;
        double x4 = (0.735 * x3 - sz[0]) * 5.8 + x1;
        double lo3 = (sz[2] += ((sz[8] - sz[9]) - sz[2]) * CCH[2]);
        double hi3 = (sz[8] - sz[9]) - lo3;
        double x5 = x4 - 0.01 - hi3 * 0.15 - lo3 * 0.410706;
        double a = x5 - sz[7] * 0.4 - sz[9] * 0.6;
        double s3 = (sz[3] += (hi3 - sz[3]) * CCH[3]);
        double x6 = a + 1.3 * s3;
        sz[8] = x6;
        sz[9] = (sz[4] += ((hi3 * 0.09 + ds_th(x6) + 0.008) - sz[4]) * CCH[4]);
        sz[5] += (sz[9] - sz[5]) * CCH[5];
        sz[7] = (sz[6] += ((sz[9] - sz[5]) - sz[6]) * CCH[6]);
        y = (0.34 * sz[7] + sz[9]) * 1.9;
    } else if (st == 18) {
        // console buss (no crosstalk): even harmonics from a squarer, a filtered feedback network
        double x3 = 2.6 * ds_th(u / 2.6) * 0.3981;
        double s0 = (sz[0] += (x3 * 0.45 - sz[0]) * CBS[0]);
        double s1 = (sz[1] += (0.0002135622 - sz[1]) * CBS[1]);
        double x4 = s0 + 1.4142135622 - s1;
        double x5 = x4 * x4 * 0.5 - 1;
        double x6 = (0.74 * x5 - sz[0]) * 5.8 + x3;
        double d89 = sz[8] - sz[9];
        double hi3 = d89 - (sz[2] += (d89 - sz[2]) * CBS[2]);
        double a = x6 - 0.01 - 0.2 * hi3 - 0.411795 * sz[2] - 0.4 * sz[10] - 0.6 * sz[9];
        double s3 = (sz[3] += (hi3 - sz[3]) * CBS[3]);
        double x7 = a + 0.26 * s3;
        sz[8] = x7;
        sz[4] += ((0.18 * hi3 + ds_th(x7) + 0.008) - sz[4]) * CBS[4];
        sz[5] += (sz[4] - sz[5]) * CBS[5];
        sz[9] = sz[4];
        sz[10] = (sz[6] += ((sz[4] - sz[5]) - sz[6]) * CBS[6]);
        double x8 = (0.34 * sz[10] + sz[9]) * 3.9;
        sz[7] += (x8 - sz[7]) * CBS[7];
        y = (x8 - sz[7]) * 0.5;
    } else if (st == 22) {
        // smudge: the shaped signal can only move so fast, then is smoothed again
        c = ds_th(u);
        sz[0] += emax(-SM_L, emin(SM_L, c - sz[0]));
        sz[1] += (sz[0] - sz[1]) * 0.6;
        y = sz[1];
    } else if (st == 23) {
        // breakdown: the distorted signal plus a square sub octave that follows its level
        c = ds_th(u);
        double pos = u > 0.02 ? 1 : (u < -0.02 ? 0 : sz[1]);
        if (truth(pos) && !truth(sz[1])) sz[0] = -sz[0];
        sz[1] = pos;
        sz[2] += (std::fabs(c) - sz[2]) * BD_K * 10;
        y = 0.55 * c + 0.65 * sz[0] * sz[2];
    } else if (st == 27) {
        // destroy: fewer bits and a lower sample rate as the drive goes up
        sz[0] -= 1;
        if (sz[0] <= 0) {
            double hold = 1 + std::floor(d_cdp / 12.5);
            double q = epow(2, 13 - 0.1 * d_cdp);
            sz[0] = hold;
            sz[1] = std::floor(emax(-1, emin(1, u)) * q + 0.5) / q;
        }
        y = sz[1];
    } else {
        y = ds_core(st, u);
    }
    return y;
}

// a band's style filters and tone: rebuild them
void Engine::filt(int b)
{
    int st = BST[b];
    const double *o = STYT[st];
    double *p = PCO + b * 60;
    int n = 0;
    for (int i = 0; i < 2; ++i) {
        double t = o[1 + i * 4];
        if (t >= 0) n += flt::coef((int)t, o[2 + i * 4], o[3 + i * 4], o[4 + i * 4], 0, p + n * 5, srate);
    }
    PNS[b] = n;
    n = 0;
    for (int i = 2; i < 4; ++i) {
        double t = o[1 + i * 4];
        if (t >= 0) n += flt::coef((int)t, o[2 + i * 4], o[3 + i * 4], o[4 + i * 4], 0, p + 20 + n * 5, srate);
    }
    QNS[b] = n;
    // tone: bass, mid, treble, presence (a slot with no gain does nothing)
    for (int k = 0; k < 4; ++k) {
        double g = EFFP[ep(b, K_BASS + k)];
        int tn = k == 0 ? flt::coef(1, 150, g, 0.7, 0, p + 40, srate)
                        : (k == 1 ? flt::coef(0, 900, g, 0.7, 0, p + 45, srate)
                                  : (k == 2 ? flt::coef(2, 4000, g, 0.7, 0, p + 50, srate) : flt::coef(0, 9000, g, 0.8, 0, p + 55, srate)));
        TON[b * 4 + k] = tn > 0;
    }
}

// one band, one channel: pre filters, drive (with feedback), the shaper at the oversampled rate, DC, feedback tap,
// post filters, tone, level
double Engine::chan(int b, int c, double x)
{
    double *z = BSZ + (b * 2 + c) * BSTR;
    const double *p = PCO + b * 60;
    int n = PNS[b];
    if (n > 0) x = chain(x, p, n, z + ZPRE);
    double u = x * DGc[b * 2 + c] * GDc[b];
    double fb = FBc[b];
    if (fb > 0) u += fb * z[ZFB + 2];
    int st = BST[b], os = BOS[b];
    double *sz = z + ZSH;
    d_cdp = DRVP[b * 2 + c];
    double y;
    if (os == 1) {
        y = shape(st, u, sz);
    } else if (os == 2) {
        ups3(u, z + ZUA, OSA, UPO);
        double y0 = shape(st, UPO[0], sz), y1 = shape(st, UPO[1], sz);
        y = dns3(y0, y1, z + ZDA, OSA);
    } else {
        ups3(u, z + ZUA, OSA, UPO);
        double a0 = UPO[0], a1 = UPO[1];
        ups2(a0, z + ZUB, OSB, UPO);
        double y0 = shape(st, UPO[0], sz), y1 = shape(st, UPO[1], sz);
        double r0 = dns2(y0, y1, z + ZDB, OSB);
        ups2(a1, z + ZUB, OSB, UPO);
        y0 = shape(st, UPO[0], sz);
        y1 = shape(st, UPO[1], sz);
        double r1 = dns2(y0, y1, z + ZDB, OSB);
        y = dns3(r0, r1, z + ZDA, OSA);
    }
    // DC
    double v1 = y;
    y = y - z[ZDC] + DC_R * z[ZDC + 1];
    z[ZDC] = v1;
    z[ZDC + 1] = y;
    // the feedback tap: a band pass at the feedback frequency (running while there is feedback)
    if (FBON[b]) {
        int k = b * 4;
        double v3 = y - z[ZFB + 1];
        double w1 = FBK[k] * z[ZFB] + FBK[k + 1] * v3;
        double w2 = z[ZFB + 1] + FBK[k + 1] * z[ZFB] + FBK[k + 2] * v3;
        z[ZFB] = 2 * w1 - z[ZFB];
        z[ZFB + 1] = 2 * w2 - z[ZFB + 1];
        z[ZFB + 2] = w1 * FBK[k + 3];
    }
    n = QNS[b];
    if (n > 0) y = chain(y, p + 20, n, z + ZPOST);
    int k = b * 4;
    if (TON[k]) y = bq(y, p + 40, z + ZTONE);
    if (TON[k + 1]) y = bq(y, p + 45, z + ZTONE + 2);
    if (TON[k + 2]) y = bq(y, p + 50, z + ZTONE + 4);
    if (TON[k + 3]) y = bq(y, p + 55, z + ZTONE + 6);
    return y * PGc[b * 2 + c];
}

// one band: the dry (late by the oversampling delay), the wet, their mix; added to d_o0 / d_o1
void Engine::band(int b)
{
    double l = BSG[b], r = BSG[8 + b];
    int k = b * 2;
    DGc[k] += DGi[k]; DGc[k + 1] += DGi[k + 1]; PGc[k] += PGi[k]; PGc[k + 1] += PGi[k + 1];
    MXc[b] += MXi[b]; GDc[b] += GDi[b]; FBc[b] += FBi[b];
    OGc[b] += (OGt[b] - OGc[b]) * 0.004;
    double e = emax(std::fabs(l), std::fabs(r));
    EF[b] += (e > EF[b] ? D_KAE : D_KRE) * (e - EF[b]);
    double og = OGc[b];
    if (og > 0.0001 || OGt[b] > 0) {
        // the dry, late by d_dd
        int64_t di = (int64_t)std::floor(d_dd);
        double fr = d_dd - di, dl, dr;
        double *p = DDL + k * 16, *q = p + 16;
        p[d_dp & 15] = l;
        q[d_dp & 15] = r;
        if (eq(fr, 0)) {
            dl = p[(d_dp - di) & 15];
            dr = q[(d_dp - di) & 15];
        } else {
            dl = p[(d_dp - di) & 15] * (1 - fr) + p[(d_dp - di - 1) & 15] * fr;
            dr = q[(d_dp - di) & 15] * (1 - fr) + q[(d_dp - di - 1) & 15] * fr;
        }
        if (BON[b]) {
            double xa, xb, wl, wr;
            if (BMS[b]) { xa = (l + r) * 0.5; xb = (l - r) * 0.5; }
            else { xa = l; xb = r; }
            double wa = chan(b, 0, xa), wb = chan(b, 1, xb);
            if (BMS[b]) { wl = wa + wb; wr = wa - wb; }
            else { wl = wa; wr = wb; }
            l = dl + MXc[b] * (wl - dl);
            r = dr + MXc[b] * (wr - dr);
        } else {
            l = dl;
            r = dr;
        }
        l *= og;
        r *= og;
        if (ab.uion) {
            e = emax(std::fabs(l), std::fabs(r));
            EO[b] += (e > EO[b] ? D_KAE : D_KRE) * (e - EO[b]);
        }
        if (d_slope == 0 && (b & 1)) { d_o0 -= l; d_o1 -= r; }
        else { d_o0 += l; d_o1 += r; }
    } else {
        EO[b] = 0;
    }
}

// ======================================================================== modulation
// slot s's value for this tick: -1..1 (LFO) or 0..1 (follower, MIDI)
double Engine::mval(int s)
{
    int ty = (int)std::floor(cpv(mp(s, M_TYPE)) + 0.5);
    double v = 0;
    if (ty == 1) {
        int sh = (int)std::floor(cpv(mp(s, M_OPT)) + 0.5) % 6;
        int o = (int)std::floor(cpv(mp(s, M_SYNC)) + 0.5);
        double ph;
        if (o > 0) {
            double cyc = SYNB[o & 15];
            ph = d_beat / cyc;
            ph -= std::floor(ph);
        } else {
            MPH[s] += cpv(mp(s, M_RATE)) * (16 / srate);
            MPH[s] -= std::floor(MPH[s]);
            ph = MPH[s];
        }
        if (sh == 0) v = std::sin(2 * PI * ph);
        if (sh == 1) v = ph < 0.25 ? 4 * ph : (ph < 0.75 ? 2 - 4 * ph : 4 * ph - 4);
        if (sh == 2) v = 2 * ph - 1;
        if (sh == 3) v = 1 - 2 * ph;
        if (sh == 4) { MLS[s] += ((ph < 0.5 ? 1 : -1) - MLS[s]) * 0.5; v = MLS[s]; }
        if (sh == 5) {
            if (ph < MRB[s]) { MRA[s] = MEN[s]; MEN[s] = eel_rand(2) - 1; }
            MRB[s] = ph;
            double t = 0.5 - 0.5 * std::cos(PI * ph);
            v = MRA[s] + (MEN[s] - MRA[s]) * t;
        }
    }
    if (ty == 2) {
        int o = (int)std::floor(cpv(mp(s, M_OPT)) + 0.5);
        double rt = 24 * (1 + (o / 4) % 3);
        v = clampd((20 * std::log10(MEN[s] + 0.000001) + rt) / rt, 0, 1);
    }
    if (ty == 3) {
        int o = (int)std::floor(cpv(mp(s, M_OPT)) + 0.5);
        v = o < 128 ? MCC[o < 0 ? 0 : o] : (o == 128 ? d_nvel : (d_pb + 1) * 0.5);
    }
    return v;
}

// every slot, on top of the settings in EFFP
void Engine::modeval()
{
    d_nf = 0;
    for (int s = 0; s < NMOD; ++s) {
        int ty = (int)std::floor(cpv(mp(s, M_TYPE)) + 0.5);
        if (ty <= 0) { MV[s] = 0; continue; }
        if (ty == 2) {
            MKA[s] = 1 - std::exp(-1 / (emax(0.05, cpv(mp(s, M_RATE))) * 0.001 * srate));
            MKR[s] = 1 - std::exp(-1 / (emax(0.5, cpv(mp(s, M_RATE2))) * 0.001 * srate));
            MSR[s] = (int)std::floor(cpv(mp(s, M_OPT)) + 0.5) % 4;
            double tbv = cpv(mp(s, M_TBAND));
            MSB[s] = (int)emax(0, emin(NB - 1, (tbv >= 1 && tbv <= 6) ? std::floor(tbv + 0.5) - 1 : 0));
            MFL[d_nf++] = s;
        }
        double v = mval(s);
        MV[s] = v;
        int tb = (int)std::floor(cpv(mp(s, M_TBAND)) + 0.5), tp = (int)std::floor(cpv(mp(s, M_TPAR)) + 0.5);
        double amt = cpv(mp(s, M_AMT)) * 0.01 * v;
        if (tb == 0) {
            tp = std::min(tp, 3);
            // (Input here only moves the window's marker: the JSFX reads the input gain from the setting itself)
            int p = tp == 0 ? P_GDRV : (tp == 1 ? P_MIX : (tp == 2 ? P_IN : P_GDYN));
            double sp = tp == 2 ? 12 : 100;
            EFFP[p] = clampd(EFFP[p] + amt * sp, H.plo(p), H.phi(p));
        } else {
            int b0 = tb == 7 ? 0 : tb - 1, b1 = tb == 7 ? d_nb - 1 : tb - 1;
            tp = iclamp(tp, 0, 11);
            for (int b = b0; b <= b1; ++b) {
                if (b < 0 || b >= NB) continue;
                int p = ep(b, MTP[tp]);
                if (tp == 5) EFFP[p] = clampd(EFFP[p] * epow(2, amt * 5), H.plo(p), H.phi(p));
                else EFFP[p] = clampd(EFFP[p] + amt * MTS[tp], H.plo(p), H.phi(p));
            }
        }
    }
}

// ======================================================================== settings
// one band's settings -> gains, filters, dynamics, feedback (every 16 samples)
void Engine::bprep(int b)
{
    int k = b * EBN + EB0;
    bool on = EFFP[k + K_ON] >= 0.5, mute = EFFP[k + K_MUTE] >= 0.5;
    int st = (int)clampd(std::floor(EFFP[k + K_STYLE] + 0.5), 0, NSTY - 1);
    // a new style: clear this band's states, rebuild its filters
    if (st != BST[b] || d_snap) {
        BST[b] = st;
        memset(BSZ + b * 2 * BSTR, 0, 2 * BSTR * sizeof(double));
        BSZ[b * 2 * BSTR + ZSH] = 1;
        BSZ[b * 2 * BSTR + BSTR + ZSH] = 1;
        BOS[b] = STYF[st] ? 1 : d_osf;
        TCH[b * 8 + 7] = 1;
    }
    for (int c = 0; c < 4; ++c)
        if (EFFP[k + K_BASS + c] != TCH[b * 8 + c]) { TCH[b * 8 + c] = EFFP[k + K_BASS + c]; TCH[b * 8 + 7] = 1; }
    if (truth(TCH[b * 8 + 7])) { filt(b); TCH[b * 8 + 7] = 0; }
    BON[b] = on;
    BMS[b] = EFFP[k + K_CH] >= 0.5;
    OGt[b] = (mute || (uisolo >= 0 && ab.uiok && uisolo != b)) ? 0 : 1;
    double d = clampd(EFFP[k + K_DRIVE] + EFFP[P_GDRV], 0, 100);
    double mix = clampd(EFFP[k + K_MIX], 0, 100) * 0.0001 * clampd(EFFP[P_MIX], 0, 100);
    double lvdb = EFFP[k + K_LEVEL], lv = lvdb <= -59.9 ? 0 : epow(10, lvdb / 20);
    double dpan = EFFP[k + K_DPAN] * 0.01, lpan = EFFP[k + K_LPAN] * 0.01;
    for (int c = 0; c < 2; ++c) {
        double dv = d * (c == 0 ? 1 - emax(0, dpan) : 1 - emax(0, -dpan));
        DRVP[b * 2 + c] = dv;
        double tg = epow(10, (-6 + 0.5 * dv + STYT[st][0]) / 20);
        DGi[b * 2 + c] = (tg - DGc[b * 2 + c]) / 16;
        tg = epow(10, cmp(st, dv) / 20) * lv * (c == 0 ? 1 - emax(0, lpan) : 1 - emax(0, -lpan));
        PGi[b * 2 + c] = (tg - PGc[b * 2 + c]) / 16;
    }
    MXi[b] = (mix - MXc[b]) / 16;
    // the band's level (fast, and a slow average of it) -> dynamics
    double lvf = EF[b];
    ES[b] += (lvf - ES[b]) * D_KS;
    double lvs = 20 * std::log10(ES[b] + 0.000000001);
    double dy = clampd(EFFP[k + K_DYN] + EFFP[P_GDYN], -100, 100) * 0.01;
    double gdd = 0;
    if (ne(dy, 0) && lvs > -70) {
        double dv = clampd(20 * std::log10(lvf + 0.000000001) - lvs, -24, 24);
        if (dy > 0) gdd = clampd(-dy * 0.8 * dv, -18, 9);
        else gdd = clampd(dv < 0 ? -dy * 1.6 * dv : -dy * 0.3 * dv, -40, 6);
    }
    GDB[b] = gdd;
    GDi[b] = (epow(10, gdd / 20) - GDc[b]) / 16;
    // feedback: how much (only while there is signal), and the band pass's frequency
    double fb = clampd(EFFP[k + K_FB], 0, 100) * 0.011;
    double gate = emin(1, lvf * 3000);
    FBi[b] = (fb * gate - FBc[b]) / 16;
    // the band pass runs while there is feedback; when it stops, its state is cleared for the next time
    FBON[b] = fb > 0 || FBc[b] > 0.0000001;
    if (!FBON[b]) {
        double *a1 = BSZ + b * 2 * BSTR;
        a1[ZFB] = 0; a1[ZFB + 1] = 0; a1[ZFB + 2] = 0;
        a1 += BSTR;
        a1[ZFB] = 0; a1[ZFB + 1] = 0; a1[ZFB + 2] = 0;
    }
    double f = EFFP[k + K_FBF];
    if (f != TCH[b * 8 + 4] || d_snap) {
        TCH[b * 8 + 4] = f;
        double g = std::tan(PI * (emin(f, srate * 0.45) / srate)), kk = 1.0 / 3;
        double a1 = 1 / (1 + g * (g + kk)), a2 = g * a1, a3 = g * a2;
        FBK[b * 4] = a1; FBK[b * 4 + 1] = a2; FBK[b * 4 + 2] = a3; FBK[b * 4 + 3] = kk;
    }
    // the first time (or after a restart): start at the targets
    if (d_snap) {
        for (int c = 0; c < 2; ++c) {
            DGc[b * 2 + c] += DGi[b * 2 + c] * 16; DGi[b * 2 + c] = 0;
            PGc[b * 2 + c] += PGi[b * 2 + c] * 16; PGi[b * 2 + c] = 0;
        }
        MXc[b] += MXi[b] * 16; MXi[b] = 0;
        GDc[b] += GDi[b] * 16; GDi[b] = 0;
        FBc[b] += FBi[b] * 16; FBi[b] = 0;
        OGc[b] = OGt[b];
    }
}

// everything that changes slowly, every 16 samples: settings -> gains, filters, crossovers
void Engine::tick()
{
    // the settings (this set, or the other while Swap is held), then the modulation on top
    for (int i = 0; i < EB0 + NB * EBN; ++i) EFFP[i] = cpv(i);
    for (int i = MB0; i < MB0 + NMOD * MN; ++i) EFFP[i] = cpv(i);
    d_beat += 16 * (d_tempo / (60 * srate));
    modeval();
    // crossovers: sorted, at least a quarter octave apart, gliding
    double pf = 20;
    for (int i = 0; i < d_nb - 1; ++i) {
        double f = clampd(EFFP[P_XF + i], 20, emin(20000, srate * 0.45));
        f = emax(f, pf * 1.19);
        pf = f;
        XFt[i] = f;
        if (d_snap || XFc[i] <= 0) XFc[i] = f;
        else XFc[i] += (f - XFc[i]) * 0.35;
        if (d_snap || std::fabs(XFc[i] - XFd[i]) > 0.0004 * XFc[i]) { xdes(i, XFc[i], d_slope); XFd[i] = XFc[i]; }
    }
    for (int b = 0; b < d_nb; ++b) bprep(b);
    d_snap = false;
}

// ======================================================================== per block / per sample
void Engine::block(int samplesblock, const Transport &tr, const std::vector<MidiEv> &midi)
{
    ab.block(U, A.now(), H.pv(P_AB) >= 0.5, samplesblock, srate);
    V.side.store(ab.side ? 1 : 0, std::memory_order_relaxed);
    uisolo = V.solo.load(std::memory_order_relaxed);
    if (srate != d_csr) {
        d_csr = srate; d_snap = true; d_os = -1;
        DC_R = 1 - 2 * PI * (8 / srate);
        D_KAE = 1 - std::exp(-1 / (0.003 * srate));
        D_KRE = 1 - std::exp(-1 / (0.06 * srate));
        D_KS = 1 - std::exp(-16 / (1.5 * srate));
    }
    d_ingt = epow(10, cpv(P_IN) / 20);
    if (d_snap) d_ingc = d_ingt;
    d_del = H.pv(P_DELTA) >= 0.5;
    // oversampling (the latched set's: a held Swap never changes the latency)
    int osn = (int)emax(0, emin(2, std::floor(H.pv(P_OS) + 0.5)));
    if (osn != d_os) {
        d_os = osn;
        d_osf = d_os == 0 ? 1 : (d_os == 1 ? 2 : 4);
        rates();
        d_dd = d_os == 0 ? 0 : (d_os == 1 ? 3.2115 : 4.5117);
        d_dint = (int)std::floor(d_dd + 0.5);
        memset(BSZ, 0, sizeof BSZ);
        for (int b = 0; b < NB; ++b) { BSZ[b * 2 * BSTR + ZSH] = 1; BSZ[b * 2 * BSTR + BSTR + ZSH] = 1; }
        d_snap = true;
    }
    int nbn = (int)emax(1, emin(NB, std::floor(cpv(P_NB) + 0.5))), sln = (int)emax(0, emin(2, std::floor(cpv(P_SLOPE) + 0.5)));
    if (nbn != d_nb || sln != d_slope) {
        d_nb = nbn;
        d_slope = sln;
        xzero();
        d_snap = true;
    }
    // tempo for the synced LFOs
    d_tempo = tr.tempo > 0 ? tr.tempo : 120;
    if (tr.playing) {
        if (std::fabs(tr.beat - d_beat) > 0.25) d_beat = tr.beat;
        else d_beat += (tr.beat - d_beat) * 0.1;
    }
    // MIDI: control changes and notes for the MIDI sources (everything passes through: the plug-in does that)
    for (const MidiEv &m : midi) {
        int st = m.d[0] & 240, d2 = m.len > 1 ? m.d[1] & 127 : 0, d3 = m.len > 2 ? m.d[2] & 127 : 0;
        if (st == 176) MCC[d2] = d3 / 127.0;
        if (st == 144 && d3 > 0) { d_nvel = d3 / 127.0; d_nheld += 1; }
        if (st == 128 || (st == 144 && d3 == 0)) {
            d_nheld = emax(0, d_nheld - 1);
            if (d_nheld == 0) d_nvel = 0;
        }
        if (st == 224) d_pb = (d2 + d3 * 128 - 8192) / 8192.0;
    }
    // flush the filters' tiny leftovers (denormals; the followers' levels the JSFX way, sign and all)
    for (double &v : BSZ) flush21(v);
    xflush();
    for (int b = 0; b < NB; ++b) {
        if (EF[b] < 0.000000000000000000001) EF[b] = 0;
        if (ES[b] < 0.000000000000000000001) ES[b] = 0;
        if (EO[b] < 0.000000000000000000001) EO[b] = 0;
    }
    for (int i = 0; i < NMOD; ++i)
        if (MEN[i] < 0.000000000000000000001) MEN[i] = 0;
    anw = ab.uion && H.pv(P_AN) >= 0.5;
    // meters for the window: input peak (after Input)
    double dur = samplesblock / srate;
    c_mi0 = emax(c_ip0 > 0.0000000001 ? 20 * std::log10(c_ip0) : -200, c_mi0 - 20 * dur);
    c_mi1 = emax(c_ip1 > 0.0000000001 ? 20 * std::log10(c_ip1) : -200, c_mi1 - 20 * dur);
    c_ip0 = 0; c_ip1 = 0;
    V.mi0.store(c_mi0, std::memory_order_relaxed);
    V.mi1.store(c_mi1, std::memory_order_relaxed);
    if (ab.uion) {
        for (int i = 0; i < NPAR; ++i) V.eff[i].store((float)EFFP[i], std::memory_order_relaxed);
        for (int s = 0; s < NMOD; ++s) V.mv[s].store((float)MV[s], std::memory_order_relaxed);
        for (int b = 0; b < NB; ++b) {
            V.ef[b].store((float)(b < d_nb ? EF[b] : 0), std::memory_order_relaxed);
            V.eo[b].store((float)(b < d_nb ? EO[b] : 0), std::memory_order_relaxed);
        }
    }
}

void Engine::afterAct()
{
    if (!idDrawn && A.myid != 0) { eel_rand(899999); idDrawn = true; }
}

void Engine::samples(const double *const *in, const double *const *sc, double *const *out, int n)
{
    if ((int)br0.size() < n) { br0.resize(n); br1.resize(n); bq0.resize(n); bq1.resize(n); }
    for (int i = 0; i < n; ++i) {
        double r0 = in[0][i], r1 = in[1][i];
        double s2 = sc ? sc[0][i] : 0, s3 = sc ? sc[1][i] : 0;
        d_ingc += (d_ingt - d_ingc) * 0.002;
        double x0 = r0 * d_ingc, x1 = r1 * d_ingc;
        // the modulation followers
        for (int f = 0; f < d_nf; ++f) {
            int s = MFL[f];
            double v = MSR[s] == 0 ? emax(std::fabs(x0), std::fabs(x1)) : (MSR[s] == 1 ? EF[MSB[s]] : emax(std::fabs(s2), std::fabs(s3)));
            MEN[s] += (v > MEN[s] ? MKA[s] : MKR[s]) * (v - MEN[s]);
        }
        if (++d_tc >= 16) { d_tc = 0; tick(); }
        // the raw input, as late as the oversampled bands (whole samples for Bypass, interpolated for Delta)
        DRW[d_rp & 63] = r0; DRW[64 + (d_rp & 63)] = r1; DRW[128 + (d_rp & 63)] = x0; DRW[192 + (d_rp & 63)] = x1;
        double q0 = DRW[(d_rp - d_dint) & 63], q1 = DRW[64 + ((d_rp - d_dint) & 63)];
        double dd0 = 0, dd1 = 0;
        if (d_del) {
            int64_t di = (int64_t)std::floor(d_dd);
            double fr = d_dd - di;
            dd0 = DRW[128 + ((d_rp - di) & 63)] * (1 - fr) + DRW[128 + ((d_rp - di - 1) & 63)] * fr;
            dd1 = DRW[192 + ((d_rp - di) & 63)] * (1 - fr) + DRW[192 + ((d_rp - di - 1) & 63)] * fr;
        }
        d_rp += 1;
        split(x0, x1);
        d_o0 = 0;
        d_o1 = 0;
        for (int b = 0; b < d_nb; ++b) band(b);
        d_dp += 1;
        if (d_del) { out[0][i] = d_o0 - dd0; out[1][i] = d_o1 - dd1; }
        else { out[0][i] = d_o0; out[1][i] = d_o1; }
        br0[i] = r0; br1[i] = r1; bq0[i] = q0; bq1[i] = q1;
        if (ab.uion) {
            double t = std::fabs(x0);
            if (t > c_ip0) c_ip0 = t;
            t = std::fabs(x1);
            if (t > c_ip1) c_ip1 = t;
        }
    }
}

void Engine::post(double *const *out, int n)
{
    for (int i = 0; i < n; ++i) {
        ab.post(br0[i], br1[i], out[0][i], out[1][i], bq0[i], bq1[i]);
        if (anw) V.an.put((br0[i] + br1[i]) * 0.5, (out[0][i] + out[1][i]) * 0.5);
    }
    if (anw) V.an.publish();
}

} // namespace sat
} // namespace ts
