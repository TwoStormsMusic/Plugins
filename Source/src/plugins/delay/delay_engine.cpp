#include "delay_engine.h"
#include "../../core/eelrand.h"
#include "../../core/filters.h"
#include "../../core/satcore.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ts {
namespace delay {

const double YBEATS[17] = {0.000000, 0.083333, 0.125000, 0.166667, 0.250000, 0.375000, 0.333333, 0.500000, 0.750000,
                           0.666667, 1.000000, 1.500000, 1.333333, 2.000000, 3.000000, 4.000000, 8.000000};
const int MTP[12] = {K_TL, K_FB, K_LEVEL, K_PAN, K_CROSS, K_HP, K_LP, K_DRIVE, K_SEMI, K_DIFF, K_WIDTH, K_BG};
static const double MTS[12] = {1, 100, 24, 100, 100, 5, 5, 100, 12, 100, 100, 12};
// the modulation LFOs' note values (in beats): Free, 1/32, 1/16, 1/8, 1/4, 1/2, 1 bar, 2 bars, 4 bars, 1/16T, 1/8T, 1/4T, 1/8.
static const double SYNB[16] = {0, 0.125, 0.25, 0.5, 1, 2, 4, 8, 16, 1.0 / 6, 1.0 / 3, 2.0 / 3, 0.75, 0, 0, 0};
static const double RBASE[8] = {1429, 1721, 2053, 2381, 2749, 3089, 3433, 3779};
static const double YOSC[8] = {-0.000539342, 0.018795537, -0.113860873, 0.595604679, 0.595604679, -0.113860873, 0.018795537, -0.000539342};
// the saturation styles the delay offers (the saturator's that work without memory) and their pre-gains (dB)
static const int YSATI[24] = {0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 19, 20, 21, 24, 25, 26, 28, 29};
static const double YSATG[30] = {0, 0, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 2, 3, 6, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0};

static inline double bq(double x, const double *o, double *z)
{
    double y = o[0] * x + z[0];
    z[0] = o[1] * x - o[3] * y + z[1];
    z[1] = o[2] * x - o[4] * y;
    return y;
}
static inline double lim(double x)
{
    double a = std::fabs(x);
    return a < 1 ? x : (x > 0 ? 1 + 0.5 * ds_th((a - 1) * 2) : -1 - 0.5 * ds_th((a - 1) * 2));
}
static inline double ss3(double t) { return t * t * (3 - 2 * t); }
static inline int64_t pow2At(double n)
{
    int64_t l = 1;
    while (l < n) l *= 2;
    return l;
}
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
    YDLEN = pow2At(8.1 * srate + 16); YDM = YDLEN - 1;
    YSLEN = pow2At(4.5 * srate + 32); YSM = YSLEN - 1;
    YALEN = pow2At(0.1 * srate + 8); YAM = YALEN - 1;
    YDB.assign(NL * 2 * YDLEN, 0.0);
    YSBF.assign(NL * 2 * YSLEN, 0.0);
    YABF.assign(NL * 2 * 4 * YALEN, 0.0);
    // (EEL2: a / binds tighter than a *, so a * b / c is a * (b / c); written out here)
    RLEN = pow2At(3779 * 2.1 * (srate / 48000) + 96); RM = RLEN - 1;
    RPLEN = pow2At(0.21 * srate + 8); RPM = RPLEN - 1;
    RAL = pow2At(0.011 * srate + 8); RAM = RAL - 1;
    RSL = pow2At(0.2 * srate + 16); RSM = RSL - 1;
    RVB.assign(8 * RLEN, 0.0);
    RPB.assign(2 * RPLEN, 0.0);
    RAP.assign(6 * RAL, 0.0);
    RSB.assign(RSL, 0.0);
#define Z(a) memset(a, 0, sizeof a)
    Z(YCUR); Z(YTGT); Z(YLO); Z(YLP); Z(YDX); Z(YDY); Z(YMD); Z(YCZ); Z(YFZ); Z(YFC); Z(YFEN); Z(YON); Z(YMODE); Z(YGK); Z(YTM);
    Z(YFBc); Z(YFBt); Z(YXc); Z(YXt); Z(YLVc); Z(YLVt); Z(YPNc); Z(YPNt); Z(YWDc); Z(YWDt); Z(YNT); Z(YTP); Z(YTL);
    Z(YCHR); Z(YCA); Z(YNZ); Z(YBQ); Z(YHD); Z(YWA); Z(YFA); Z(YBA); Z(YSHON); Z(YSHR); Z(YSHW); Z(YSHP); Z(YSHI); Z(YSD1); Z(YSD2); Z(YSW1);
    Z(YSHB); Z(YSHBI); Z(YSHE); Z(YSX); Z(YSHOFF); Z(YDFON); Z(YDFG); Z(YDFD); Z(YDFM); Z(YDFY); Z(YDFOFF);
    Z(YSTc); Z(YDGc); Z(YDGt); Z(YCPc); Z(YCPt); Z(YSBc); Z(YSBt); Z(YBMC); Z(YBMZ); Z(YBEN); Z(YBCH); Z(YFE); Z(YFG); Z(YOS);
    Z(YRSc); Z(YRSt); Z(YLCH); Z(YRD); Z(YRG); Z(RZ); Z(RVM); Z(YRCH);
    Z(EFFP); Z(MV); Z(MPH); Z(MRA); Z(MRB); Z(MEN); Z(MKA); Z(MKR); Z(MCC); Z(MLS); Z(MSR); Z(MFL);
#undef Z
    YRSH = 0; YRSE = 0; YRSP = 0; YRSI = 0; YRSR = 2; YRSW = 0; YRPD = 0; YRDA = 0.5; YRIG = 0.3; YROG = 1; YRCH[0] = -1;
    y_rl = 0; y_rr = 0; y_rvc = 0; y_rvt = 0; y_rvoff = 100000; y_rsl = 0; y_rsr = 0;
    y_wp = 0; y_tc = 0; y_snap = true; y_frc = 0; y_frt = 0; y_dgc = 1; y_dgt = 1; y_mxc = 0; y_mxt = 0; y_dyc = 1; y_dyt = 1;
    y_den = 0; y_ingc = 1; y_ingt = 1; y_wetabs = 0; y_rgc = 1; y_rgt = 1; y_vl = 0; y_vr = 0;
    for (int j = 0; j < NL - 1; ++j) { y_con[j] = 0; y_lkc[j] = 0; y_lkt[j] = 0; }
    for (int k = 0; k < NL; ++k) { RVON[k] = false; RVBT[k] = false; RVSP[k] = 2; RVBI[k] = 1; RVBW[k] = 0; }
    for (int i = 0; i < NC; ++i) { RVPH[i] = 0; RVW[i] = 0; RVWP[i] = 0; }
    y_hq = true; y_hql = 7; y_bt = 0; y_bps = 0.000005; y_wl = 0; y_wr = 0;
    d_nf = 0; d_beat = 0; d_nvel = 0; d_nheld = 0; d_pb = 0;
    c_ip0 = 0; c_ip1 = 0; c_mi0 = -200; c_mi1 = -200;
    // the JSFX's @block on a new sample rate
    DC_R = 1 - 2 * PI * (8 / srate);
    YKS = 1 - std::exp(-1 / (0.01 * srate));
    YKF = 1 - std::exp(-1 / (0.03 * srate));
    YBR = 0.8 / srate;
    for (int i = 0; i < NC; ++i) {
        YWR[i] = (0.45 + 0.17 * (i & 1) + 0.05 * (i >> 1)) / srate;
        YFR[i] = (7 + 1.3 * (i & 1) + 0.4 * (i >> 1)) / srate;
    }
    YKA = 1 - std::exp(-1 / (0.001 * srate));
    YKR = 1 - std::exp(-1 / (0.12 * srate));
    YAL_ST = 2 * (int)std::ceil(srate / 48000);
    YAL_N = (int)std::floor(0.006 * (srate / YAL_ST));
    YAL_M = (int)std::floor(0.003 * (srate / YAL_ST));
    for (int i = 0; i < NC * 4; ++i) {
        YDFP[i] = 0;   // the JSFX's (i * 0.137) % 1: EEL2's % works on whole numbers, so every phase starts at 0
        YDFI[i] = (0.29 + 0.083 * i) / srate;
    }
    RVI[0] = 0.31 / srate; RVI[1] = 0.43 / srate; RVI[2] = 0.57 / srate; RVI[3] = 0.71 / srate;
    YRMD = 0.0005 * srate;
    for (int i = 0; i < 3; ++i) {
        YRAD[i] = std::floor((i == 0 ? 142 : (i == 1 ? 107 : 379)) * (srate / 48000) + 0.5);
        YRAD[i + 3] = std::floor((i == 0 ? 152 : (i == 1 ? 113 : 401)) * (srate / 48000) + 0.5);
    }
    V.an.clear();
}

void Engine::reset()
{
    std::fill(YDB.begin(), YDB.end(), 0.0);
    std::fill(YSBF.begin(), YSBF.end(), 0.0);
    std::fill(YABF.begin(), YABF.end(), 0.0);
    std::fill(RVB.begin(), RVB.end(), 0.0);
    std::fill(RPB.begin(), RPB.end(), 0.0);
    std::fill(RAP.begin(), RAP.end(), 0.0);
    std::fill(RSB.begin(), RSB.end(), 0.0);
    memset(YCZ, 0, sizeof YCZ); memset(YFZ, 0, sizeof YFZ); memset(YBMZ, 0, sizeof YBMZ); memset(YOS, 0, sizeof YOS);
    memset(YDX, 0, sizeof YDX); memset(YDY, 0, sizeof YDY); memset(YDFY, 0, sizeof YDFY); memset(RZ, 0, sizeof RZ);
    memset(YFE, 0, sizeof YFE);
    for (int i = 0; i < NC; ++i) { RVPH[i] = 0; RVW[i] = 0; RVWP[i] = 0; }
    y_vl = 0; y_vr = 0;
    y_rl = 0; y_rr = 0; y_wl = 0; y_wr = 0; y_wetabs = 0; y_den = 0;
}

// ======================================================================== reads
// d samples back from the slot about to be written (Catmull-Rom between four neighbours)
double Engine::rd(const double *b, int64_t m, double d) const
{
    double p = (double)y_wp - d, fi = std::floor(p), f = p - fi;
    int64_t i = (int64_t)fi;
    double a = b[(i - 1) & m], g = b[i & m], c = b[(i + 1) & m], e = b[(i + 2) & m];
    return g + 0.5 * f * (c - a + f * (2 * a - 5 * g + 4 * c - e + f * (3 * (g - c) + e - a)));
}
// the same with six neighbours (5th-order Lagrange)
double Engine::rd6(const double *b, int64_t m, double d) const
{
    double p = (double)y_wp - d, fi = std::floor(p), f = p - fi;
    int64_t i = (int64_t)fi;
    double a = f + 2, c = f + 1, e = f - 1, g = f - 2, h = f - 3;
    double t = c * f, u = e * g, v = a * f, w = a * c;
    double x = t * u * h * -0.008333333333;
    double y = v * u * h * 0.041666666667;
    double z = w * e * g * h * -0.083333333333;
    double j = w * f * g * h * 0.083333333333;
    x = x * b[(i - 2) & m] + y * b[(i - 1) & m] + z * b[i & m] + j * b[(i + 1) & m];
    return x + w * f * e * h * -0.041666666667 * b[(i + 2) & m] + w * f * e * g * 0.008333333333 * b[(i + 3) & m];
}

// pitch shifting: where to start the tap about to fade in so it lines up with the tap playing (0 .. ~6 ms further back)
double Engine::align(const double *o, double dn, double dr) const
{
    int64_t pa = (int64_t)std::floor((double)y_wp - dr);
    int bi = 0;
    double bc = -1000000000;
    for (int i = 0; i < YAL_N; ++i) {
        int64_t pb = (int64_t)std::floor((double)y_wp - dn) - (int64_t)i * YAL_ST;
        double c = 0, e = 0.0000001;
        for (int m = 0; m < YAL_M; ++m) {
            double a = o[(pa - (int64_t)m * YAL_ST) & YSM], b = o[(pb - (int64_t)m * YAL_ST) & YSM];
            c += a * b;
            e += b * b;
        }
        c /= std::sqrt(e);
        if (c > bc) { bc = c; bi = i; }
    }
    return (double)bi * YAL_ST;
}

// ======================================================================== the loop
// what happens to a repeat on its way round: pitch / reverse, diffusion, filters, character, saturation
double Engine::proc(int i, int k, double x)
{
    if (truth(YSHON[k])) {
        double *o = YSBF.data() + (int64_t)i * YSLEN;
        o[y_wp & YSM] = x;
        if (YSW1[k] >= 1) x = rdq(o, YSM, YSD1[k] + YSX[i * 2]);
        else x = rdq(o, YSM, YSD1[k] + YSX[i * 2]) * YSW1[k] + rdq(o, YSM, YSD2[k] + YSX[i * 2 + 1]) * (1 - YSW1[k]);
    }
    if (truth(YDFON[k])) {
        double g = YDFG[k];
        for (int j = 0; j < 4; ++j) {
            int n = i * 4 + j;
            double *o = YABF.data() + (int64_t)n * YALEN;
            double v;
            if (YDFM[n] > 0) {
                // two of the four all-passes move slowly; the all-pass interpolator keeps the level flat
                double t = YDFP[n] + YDFI[n];
                if (t >= 1) t -= 1;
                YDFP[n] = t;
                double pp = (double)y_wp - (YDFD[n] + YDFM[n] * (std::fabs(2 * t - 1) * 2 - 1));
                double ii = std::floor(pp), ff = pp - ii;
                int64_t ix = (int64_t)ii;
                double a = ff / (2 - ff);
                v = a * o[(ix + 1) & YAM] + o[ix & YAM] - a * YDFY[n];
                YDFY[n] = v;
            } else {
                v = o[(y_wp - (int64_t)YDFD[n]) & YAM];
            }
            double w = x + g * v;
            o[y_wp & YAM] = w;
            x = v - g * w;
        }
    }
    for (int j = 0; j < 3; ++j)
        if (truth(YFEN[k * 3 + j])) x = bq(x, YFC + (k * 3 + j) * 5, YFZ + (i * 3 + j) * 2);
    int ch = (int)YCHR[k];
    if (ch > 0) {
        double *z = YCZ + i * 4;
        x += (eel_rand(2) - 1) * YNZ[k];
        z[0] += YCA[k] * (x - z[0]);
        x = z[0];
        if (ch == 1 || ch == 2) { z[1] += YCA[k] * (x - z[1]); x = z[1]; }
        if (ch == 1 && truth(YBEN[k])) x = bq(x, YBMC + k * 5, YBMZ + i * 2);
        if (ch == 3) {
            if ((z[2] -= 1) <= 0) { z[2] = YHD[k]; z[3] = std::floor(x * YBQ[k] + 0.5) / YBQ[k]; }
            x = z[3];
        }
    }
    int st = (int)YSTc[k];
    if (y_hq) {
        // saturation at twice the rate (half-band filters, 7 samples of delay that the delay time takes off)
        double *z = YOS + i * 64;
        int nn = (int)(y_wp & 15);
        z[nn] = x;
        if (YSBc[k] > 0.0005) {
            z[16 + nn] = x * YDGc[k];
            double e = 0;
            for (int t = 0; t < 8; ++t) e += YOSC[t] * z[16 + ((nn - t) & 15)];
            z[32 + nn] = ds_core(st, e);
            z[48 + nn] = ds_core(st, z[16 + ((nn - 3) & 15)]);
            double y = 0.5 * z[48 + ((nn - 4) & 15)];
            for (int t = 0; t < 8; ++t) y += 0.5 * YOSC[t] * z[32 + ((nn - t) & 15)];
            y *= YCPc[k];
            x = z[(nn - 7) & 15];
            x += (y - x) * YSBc[k];
        } else {
            x = z[(nn - 7) & 15];
        }
    } else if (YSBc[k] > 0.0005) {
        double y = ds_core(st, x * YDGc[k]) * YCPc[k];
        x += (y - x) * YSBc[k];
    }
    double y = x - YDX[i] + DC_R * YDY[i];
    YDX[i] = x;
    YDY[i] = y;
    y = lim(y);
    return std::fabs(y) < 0.00000000000000000001 ? 0 : y;
}

// the shared reverb: pre-delay, three input all-passes per side, an 8-line feedback network (Hadamard mix, damping,
// slow modulation of four lines), and a shimmer
void Engine::rv(double il, double ir)
{
    int64_t p = y_wp & RPM;
    RPB[p] = il;
    RPB[RPLEN + p] = ir;
    int64_t q = (y_wp - (int64_t)YRPD) & RPM;
    il = RPB[q];
    ir = RPB[RPLEN + q];
    for (int i = 0; i < 3; ++i) {
        double *o = RAP.data() + i * RAL;
        int64_t d = (int64_t)YRAD[i];
        double b = o[(y_wp - d) & RAM], a = il + 0.6 * b;
        o[y_wp & RAM] = a;
        il = b - 0.6 * a;
        o = RAP.data() + (3 + i) * RAL;
        d = (int64_t)YRAD[3 + i];
        b = o[(y_wp - d) & RAM];
        a = ir + 0.6 * b;
        o[y_wp & RAM] = a;
        ir = b - 0.6 * a;
    }
    double s = 0;
    if (YRSH > 0.0003) {
        RSB[y_wp & RSM] = (y_rl + y_rr) * 0.5;
        double t = YRSP + YRSI;
        if (t >= 1) t -= 1;
        YRSP = t;
        double a = t + 0.5;
        if (a >= 1) a -= 1;
        double f = t < 0.5 ? 2 * t : 2 - 2 * t;
        f = f * f * (3 - 2 * f);
        double m = YRSR;
        double d = 4 + (m < 1 ? (1 - m) * YRSW * t : (m - 1) * YRSW * (1 - t));
        double c = 4 + (m < 1 ? (1 - m) * YRSW * a : (m - 1) * YRSW * (1 - a));
        s = rd(RSB.data(), RSM, d) * f + rd(RSB.data(), RSM, c) * (1 - f);
        s = lim(s) * YRSH;
        YRSE += (std::fabs(y_rl) + std::fabs(y_rr) - YRSE) * 0.002;
        if (YRSE > 0.5) s *= 0.5 / YRSE;
    }
    double ie = (il + s) * YRIG + 0.000000000000000001, io = (ir + s) * YRIG + 0.000000000000000001;
    double o[8];
    for (int l = 0; l < 8; ++l) {
        const double *b = RVB.data() + l * RLEN;
        double v;
        if ((l & 1) == 0) {
            int mi = l / 2;
            double t = RVM[mi] + RVI[mi];
            if (t >= 1) t -= 1;
            RVM[mi] = t;
            double qq = (double)y_wp - (YRD[l] + YRMD * (std::fabs(2 * t - 1) * 2 - 1));
            double fi = std::floor(qq), f = qq - fi;
            int64_t ix = (int64_t)fi;
            double a = b[ix & RM];
            v = a + (b[(ix + 1) & RM] - a) * f;
        } else {
            v = b[(y_wp - (int64_t)YRD[l]) & RM];
        }
        RZ[l] += YRDA * (v - RZ[l]);
        o[l] = RZ[l] * YRG[l];
    }
    y_rl = (o[0] + o[3] - o[4] - o[7]) * YROG;
    y_rr = (o[1] + o[2] - o[5] - o[6]) * YROG;
    double a = o[0] + o[1], b = o[0] - o[1], c = o[2] + o[3], d = o[2] - o[3];
    double d0 = o[4] + o[5], d1 = o[4] - o[5], d2 = o[6] + o[7], d3 = o[6] - o[7];
    double d4 = a + c, d5 = b + d, d6 = a - c, d7 = b - d;   // lines 0..3 after two stages
    a = d0 + d2; b = d1 + d3; c = d0 - d2; d = d1 - d3;      // lines 4..7 after two stages
    int64_t w = y_wp & RM;
    RVB[0 * RLEN + w] = (d4 + a) * 0.35355339 + ie;
    RVB[1 * RLEN + w] = (d5 + b) * 0.35355339 + io;
    RVB[2 * RLEN + w] = (d6 + c) * 0.35355339 + ie;
    RVB[3 * RLEN + w] = (d7 + d) * 0.35355339 + io;
    RVB[4 * RLEN + w] = (d4 - a) * 0.35355339 + ie;
    RVB[5 * RLEN + w] = (d5 - b) * 0.35355339 + io;
    RVB[6 * RLEN + w] = (d6 - c) * 0.35355339 + ie;
    RVB[7 * RLEN + w] = (d7 - d) * 0.35355339 + io;
}

// one sample: the input in, the wet signal out (y_wl, y_wr)
void Engine::run(double il, double ir)
{
    y_frc += (y_frt - y_frc) * YKF;
    for (int k = 0; k < NL; ++k) {
        if (truth(YON[k])) {
            if (truth(YSHON[k])) {
                double j = YSHR[k];
                if (truth(YSHB[k])) {
                    // reverse in chunks that start on the beat: the last chunk, backwards, with a short crossfade at the seams
                    double ph = y_bt * YSHBI[k];
                    ph -= std::floor(ph);
                    double e = YSHE[k], sp = 1 + std::fabs(j);
                    YSD1[k] = 4 + sp * YSHW[k] * ph;
                    YSD2[k] = 4 + sp * YSHW[k] * (ph + 1);
                    if (ph < e) { double t = ph / e; YSW1[k] = t * t * (3 - 2 * t); }
                    else YSW1[k] = 1;
                } else {
                    double ph0 = YSHP[k], ph = ph0 + YSHI[k];
                    bool w1 = ph >= 1;
                    if (w1) ph -= 1;
                    YSHP[k] = ph;
                    bool w2 = ph0 < 0.5 && ph >= 0.5;
                    double p2 = ph + 0.5;
                    if (p2 >= 1) p2 -= 1;
                    double a = ph < 0.5 ? 2 * ph : 2 - 2 * ph;
                    YSW1[k] = a * a * (3 - 2 * a);
                    YSD1[k] = 4 + (j < 1 ? (1 - j) * YSHW[k] * ph : (j - 1) * YSHW[k] * (1 - ph));
                    YSD2[k] = 4 + (j < 1 ? (1 - j) * YSHW[k] * p2 : (j - 1) * YSHW[k] * (1 - p2));
                    if (y_hq && j > 0 && (w1 || w2)) {
                        for (int c = 0; c < 2; ++c) {
                            int i = k * 2 + c;
                            const double *o = YSBF.data() + (int64_t)i * YSLEN;
                            if (w1) YSX[i * 2] = align(o, YSD1[k], YSD2[k] + YSX[i * 2 + 1]);
                            if (w2) YSX[i * 2 + 1] = align(o, YSD2[k], YSD1[k] + YSX[i * 2]);
                        }
                    }
                }
            }
            YFBc[k] += (YFBt[k] - YFBc[k]) * YKS; YXc[k] += (YXt[k] - YXc[k]) * YKS;
            YLVc[k] += (YLVt[k] - YLVc[k]) * YKS; YPNc[k] += (YPNt[k] - YPNc[k]) * YKS; YWDc[k] += (YWDt[k] - YWDc[k]) * YKS;
            YDGc[k] += (YDGt[k] - YDGc[k]) * YKS; YCPc[k] += (YCPt[k] - YCPc[k]) * YKS; YSBc[k] += (YSBt[k] - YSBc[k]) * YKS;
            YRSc[k] += (YRSt[k] - YRSc[k]) * YKS;
            int n = (int)YNT[k];
            for (int c = 0; c < 2; ++c) {
                int i = k * 2 + c;
                const double *o = YDB.data() + (int64_t)i * YDLEN;
                YCUR[i] += (YTGT[i] - YCUR[i]) * YGK[k];
                double d = YCUR[i];
                double *m = YMD + i * 8;
                if (YWA[k] > 0) {
                    m[0] += YWR[i];
                    if (m[0] >= 1) { m[0] -= 1; m[1] = m[2]; m[2] = eel_rand(2) - 1; }
                    double t = m[0];
                    d += YWA[k] * (m[1] + (m[2] - m[1]) * (t * t * (3 - 2 * t)));
                }
                if (YFA[k] > 0) {
                    m[3] += YFR[i];
                    if (m[3] >= 1) { m[3] -= 1; m[4] = m[5]; m[5] = eel_rand(2) - 1; }
                    double t = m[3];
                    d += YFA[k] * (m[4] + (m[5] - m[4]) * (t * t * (3 - 2 * t)));
                }
                if (YBA[k] > 0) {
                    m[6] += YBR;
                    if (m[6] >= 1) m[6] -= 1;
                    double t = m[6] < 0.5 ? m[6] : m[6] - 1;
                    t = 4 * t * (1 - 2 * std::fabs(t));   // a sine, near enough
                    d += YBA[k] * t;
                }
                d = emax(4, emin((double)(YDLEN - 8), d));
                double v = 0;
                double s = 0;
                if (RVON[k]) {
                    // reversed (the CLAP's own): the loop runs forwards; what comes out is the last chunk (the delay
                    // time, or a note value on the beat) played backwards, from its newest end, the chunk before
                    // fading out over its first 15 ms; so every repeat is reversed. (No taps while reversing.)
                    double sp = RVSP[k], wmax = (double)(YDLEN - 64) / (1.45 * sp), ph;
                    if (RVBT[k]) {
                        ph = y_bt * RVBI[k];
                        ph -= std::floor(ph);
                        RVW[i] = RVWP[i] = emin(wmax, RVBW[k]);
                    } else {
                        if (RVW[i] <= 0) { RVW[i] = RVWP[i] = emin(wmax, d); RVPH[i] = 0; }
                        ph = RVPH[i] + 1 / RVW[i];
                        if (ph >= 1) { ph -= 1; RVWP[i] = RVW[i]; RVW[i] = emin(wmax, d); }
                        RVPH[i] = ph;
                    }
                    double W = RVW[i], e = emin(0.45, RVE * srate / W);
                    s = rdq(o, YDM, 4 + sp * W * ph);
                    if (ph < e) {
                        double t = ph / e;
                        t = t * t * (3 - 2 * t);
                        s = s * t + rdq(o, YDM, emin((double)(YDLEN - 8), 4 + sp * (W * ph + RVWP[i]))) * (1 - t);
                    }
                    YLO[i] = s;
                    YLP[i] = rdq(o, YDM, emax(4, d - y_hql));
                } else {
                    for (int j = 0; j < n - 1; ++j) v += rdq(o, YDM, emax(4, d * YTP[k * 4 + j] - y_hql)) * YTL[k * 4 + j];
                    s = rdq(o, YDM, emax(4, d - y_hql));
                    YLO[i] = v + s;
                    YLP[i] = s;
                }
            }
        } else {
            YLO[k * 2] = 0; YLO[k * 2 + 1] = 0; YLP[k * 2] = 0; YLP[k * 2 + 1] = 0;
        }
    }
    // what goes into the lines, and round again
    for (int k = 0; k < NL; ++k) {
        double *oL = YDB.data() + (int64_t)(k * 2) * YDLEN, *oR = YDB.data() + (int64_t)(k * 2 + 1) * YDLEN;
        if (truth(YON[k])) {
            double il2 = il, ir2 = ir;
            // in series: the repeats of the line before it (while it plays) instead of the input
            if (k > 0 && y_con[k - 1] == 1 && truth(YON[k - 1])) { il2 = YLO[(k - 1) * 2]; ir2 = YLO[(k - 1) * 2 + 1]; }
            int j = (int)YMODE[k];
            double sl, sr;
            if (j == 0) { sl = il2; sr = ir2; }
            else {
                double m = (il2 + ir2) * 0.5;
                sl = j == 1 ? m : 0;
                sr = j == 1 ? 0 : m;
            }
            sl *= 1 - y_frc;
            sr *= 1 - y_frc;
            double f = YFBc[k] + (1 - YFBc[k]) * y_frc;
            double xx = YXc[k];
            double pl = YLP[k * 2], pr = YLP[k * 2 + 1];
            double ml = pl * (1 - xx) + pr * xx, mr = pr * (1 - xx) + pl * xx;
            // the leveler: when the feedback itself gets near full scale it is turned down, smoothly, rather than clipped
            double fe = emax(std::fabs(ml), std::fabs(mr)) * f;
            YFE[k] += (fe > YFE[k] ? YKA : YKR) * (fe - YFE[k]);
            double fg = YFE[k] > 0.9 ? 0.9 / YFE[k] : 1;
            YFG[k] = fg;
            f *= fg;
            // linked: some of its repeats come from the line next to it, by the Link amount (from both neighbours: at
            // most all of them, shared)
            double w0 = (k > 0 && y_con[k - 1] == 2 && truth(YON[k - 1])) ? y_lkc[k - 1] : 0;
            double w1 = (k < NL - 1 && y_con[k] == 2 && truth(YON[k + 1])) ? y_lkc[k] : 0;
            if (w0 + w1 > 1) { double sc = 1 / (w0 + w1); w0 *= sc; w1 *= sc; }
            if (w0 > 0 || w1 > 0) {
                double el = 0, er = 0;
                for (int q = 0; q < 2; ++q) {
                    int fo = q == 0 ? k - 1 : k + 1;
                    double w = q == 0 ? w0 : w1;
                    if (w <= 0) continue;
                    double a = (YFBc[fo] + (1 - YFBc[fo]) * y_frc) * YFG[fo], x2 = YXc[fo];
                    double ql = YLP[fo * 2], qr = YLP[fo * 2 + 1];
                    double ol = ql * (1 - x2) + qr * x2, orr = qr * (1 - x2) + ql * x2;
                    el += w * a * ol;
                    er += w * a * orr;
                }
                sl += (1 - w0 - w1) * f * ml + el;
                sr += (1 - w0 - w1) * f * mr + er;
            } else {
                sl += f * ml;
                sr += f * mr;
            }
            // everything on the way in (input and repeats alike): pitch, diffusion, filters, character, saturation
            double x = proc(k * 2, k, sl);
            sl = x + (sl - x) * y_frc;
            x = proc(k * 2 + 1, k, sr);
            sr = x + (sr - x) * y_frc;
            oL[y_wp & YDM] = lim(sl);
            oR[y_wp & YDM] = lim(sr);
        } else {
            oL[y_wp & YDM] = 0;
            oR[y_wp & YDM] = 0;
        }
    }
    // the wet signal: each line with its width, pan and level (and what it sends to the reverb)
    y_wl = 0; y_wr = 0; y_rsl = 0; y_rsr = 0;
    for (int k = 0; k < NL; ++k) {
        if (!truth(YON[k])) continue;
        double m = (YLO[k * 2] + YLO[k * 2 + 1]) * 0.5, s = (YLO[k * 2] - YLO[k * 2 + 1]) * 0.5 * YWDc[k];
        double a = YPNc[k], lv = YLVc[k];
        double pl = (m + s) * lv * (a > 0 ? 1 - a : 1);
        double pr = (m - s) * lv * (a < 0 ? 1 + a : 1);
        y_wl += pl;
        y_wr += pr;
        y_rsl += pl * YRSc[k];
        y_rsr += pr * YRSc[k];
    }
    // the shared reverb (kept apart: it ducks by its own amount)
    y_rvc += (y_rvt - y_rvc) * YKS;
    y_vl = 0; y_vr = 0;
    if (y_rvc > 0.0003 || y_rvt > 0.0003) {
        rv(y_rsl, y_rsr);
        y_vl = y_rl * y_rvc;
        y_vr = y_rr * y_rvc;
    }
    y_wetabs = emax(std::fabs(y_wl + y_vl), std::fabs(y_wr + y_vr));
    y_bt += y_bps;
    y_wp = (y_wp + 1) & 1073741823;
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
            MSR[s] = emin(2, (int)std::floor(cpv(mp(s, M_OPT)) + 0.5) % 4);
            MFL[d_nf++] = s;
        }
        double v = mval(s);
        MV[s] = v;
        int tb = (int)std::floor(cpv(mp(s, M_TLINE)) + 0.5), tp = (int)std::floor(cpv(mp(s, M_TPAR)) + 0.5);
        double amt = cpv(mp(s, M_AMT)) * 0.01 * v;
        if (tb == 0) {
            tp = std::min(tp, 4);
            if (tp == 0) EFFP[P_WET] = clampd(EFFP[P_WET] + amt * 24, -60, 12);
            if (tp == 1) EFFP[P_IN] = clampd(EFFP[P_IN] + amt * 12, -24, 24);
            if (tp == 2) EFFP[P_DUCK] = clampd(EFFP[P_DUCK] + amt * 100, 0, 100);
            if (tp == 3) { if (amt >= 0.5) EFFP[P_FREEZE] = 1; }
            if (tp == 4) EFFP[P_RVB] = clampd(EFFP[P_RVB] + amt * 100, 0, 100);
        } else {
            int b0 = tb > NL ? 0 : tb - 1, b1 = tb > NL ? NL - 1 : tb - 1;
            tp = iclamp(tp, 0, 11);
            for (int b = b0; b <= b1; ++b) {
                int p = ep(b, MTP[tp]);
                if (tp == 0) YTM[b] *= epow(2, amt);
                else if (tp == 5 || tp == 6) EFFP[p] = clampd(EFFP[p] * epow(2, amt * 5), H.plo(p), H.phi(p));
                else EFFP[p] = clampd(EFFP[p] + amt * MTS[tp], H.plo(p), H.phi(p));
            }
        }
    }
}

// ======================================================================== settings
// one line's settings -> what the engine uses (every 16 samples)
void Engine::lprep(int k)
{
    int o = EB0 + k * EBN;
    YON[k] = EFFP[o + K_ON] >= 0.5;
    YMODE[k] = std::floor(EFFP[o + K_MODE] + 0.5);
    // times: free in ms, or a note value at the host tempo
    for (int c = 0; c < 2; ++c) {
        int u = (c == 1 && EFFP[o + K_LINK] >= 0.5) ? 0 : c;
        int s = (int)std::floor(EFFP[o + (u ? K_SR : K_SL)] + 0.5);
        double ms = s > 0 ? YBEATS[iclamp(s, 0, 16)] * (60000 / y_tempo) : EFFP[o + (u ? K_TR : K_TL)];
        ms = clampd(ms * YTM[k], 1, 8000);
        YTGT[k * 2 + c] = ms * 0.001 * srate;
    }
    YGK[k] = 1 - std::exp(-1 / (emax(1, EFFP[o + K_GLIDE]) * 0.001 * srate));
    // feedback, cross, level, pan, width
    bool chp = YMODE[k] > 0;
    YFBt[k] = clampd(EFFP[o + K_FB], 0, 120) * 0.01;
    YXt[k] = chp ? 1 : clampd(EFFP[o + K_CROSS], 0, 100) * 0.01;
    double x = EFFP[o + K_LEVEL];
    YLVt[k] = x <= -59.9 ? 0 : epow(10, x / 20);
    YPNt[k] = clampd(EFFP[o + K_PAN], -100, 100) * 0.01;
    YWDt[k] = clampd(EFFP[o + K_WIDTH], 0, 200) * 0.01;
    // taps: where they sit between 0 and the delay, and how loud
    double n = 1 + clampd(std::floor(EFFP[o + K_TAPS] + 0.5), 0, 3);
    YNT[k] = n;
    double e = std::floor(EFFP[o + K_TPAT] + 0.5);
    e = e == 0 ? 1 : (e == 1 ? 2 : (e == 2 ? 0.5 : 0.6));
    double tl = clampd(EFFP[o + K_TLVL], 0, 100) * 0.01;
    for (int j = 0; j < 4; ++j) {
        YTP[k * 4 + j] = epow((j + 1) / n, e);
        YTL[k * 4 + j] = epow(tl, n - 1 - j);
    }
    // character and age
    double ty = std::floor(EFFP[o + K_CHAR] + 0.5);
    YCHR[k] = ty;
    double ag = clampd(EFFP[o + K_AGE], 0, 100) * 0.01;
    double f = ty == 1 ? 15000 * epow(0.22, ag) : (ty == 2 ? 9000 * epow(0.3, ag) : 16000 * epow(0.4, ag));
    YCA[k] = 1 - std::exp(-2 * PI * (emin(f, srate * 0.45) / srate));
    YNZ[k] = ty == 1 ? ag * ag * 0.0012 : (ty == 2 ? ag * ag * 0.004 : (ty == 3 ? ag * ag * 0.003 : 0));
    YBQ[k] = epow(2, 15 - 10 * ag);
    YHD[k] = 1 + std::floor(ag * 5);
    YWA[k] = clampd(EFFP[o + K_WOW], 0, 100) * 0.01 * 0.003 * srate;
    YFA[k] = clampd(EFFP[o + K_FLUT], 0, 100) * 0.01 * 0.00012 * srate;
    YBA[k] = ty == 2 ? 0.00035 * ag * srate : 0;
    // the tape's head bump: a low bell that grows with age
    YBEN[k] = ty == 1;
    if (ty == 1 && (ag != YBCH[k * 2] || y_snap)) { YBCH[k * 2] = ag; flt::coef(0, 70, 1.2 + 2.3 * ag, 0.8, 0, YBMC + k * 5, srate); }
    YRSt[k] = clampd(EFFP[o + K_RSND], 0, 100) * 0.01;
    // saturation
    double dv = clampd(EFFP[o + K_DRIVE], 0, 100);
    int st = YSATI[(int)clampd(std::floor(EFFP[o + K_SAT] + 0.5), 0, 23)];
    YSTc[k] = st;
    YDGt[k] = epow(10, (-6 + 0.5 * dv + YSATG[st]) / 20);
    YCPt[k] = epow(10, cmp(st, dv) / 20);
    YSBt[k] = emin(1, dv / 5);
    // diffusion
    x = clampd(EFFP[o + K_DIFF], 0, 100);
    bool on = x >= 0.5;
    if (on) {
        if (YDFOFF[k] > 300) std::fill(YABF.begin() + (int64_t)(k * 2 * 4) * YALEN, YABF.begin() + (int64_t)(k * 2 * 4 + 8) * YALEN, 0.0);
        YDFOFF[k] = 0;
    } else {
        YDFOFF[k] = emin(100000, YDFOFF[k] + 1);
    }
    YDFON[k] = on;
    YDFG[k] = 0.65 * x * 0.01;
    double sz = clampd(EFFP[o + K_DSZ], 5, 80) * 0.001 * srate;
    for (int c = 0; c < 2; ++c)
        for (int j = 0; j < 4; ++j) {
            int m = (k * 2 + c) * 4 + j;
            YDFD[m] = emax(8, std::floor(sz * (j == 0 ? 0.29 : (j == 1 ? 0.43 : (j == 2 ? 0.71 : 1))) * (1 + 0.073 * c) + 0.5));
            YDFM[m] = (j & 1) ? emin(0.025 * YDFD[m], 0.0004 * srate) : 0;
        }
    // pitch and reverse
    int sh = (int)std::floor(EFFP[o + K_SH] + 0.5);
    // reverse (and reverse + pitch): on the way out (see run); the loop's shifter is for Pitch only
    bool rev = (sh & 2) != 0;
    if (rev && !RVON[k])
        for (int c = 0; c < 2; ++c) { RVW[k * 2 + c] = 0; RVPH[k * 2 + c] = 0; }
    RVON[k] = rev;
    RVSP[k] = 1 + ((sh & 1) ? epow(2, EFFP[o + K_SEMI] / 12) : 1);
    {
        int sy2 = (int)std::floor(EFFP[o + K_SHSY] + 0.5);
        RVBT[k] = rev && sy2 > 0;
        if (RVBT[k]) {
            double cb = YBEATS[iclamp(sy2, 0, 16)];
            RVBI[k] = 1 / cb;
            RVBW[k] = cb * (60 / y_tempo) * srate;
        }
    }
    double r = sh == 1 ? epow(2, EFFP[o + K_SEMI] / 12) : 1;
    double w = clampd(EFFP[o + K_SHSZ], 20, 300) * 0.001 * srate;
    on = sh == 1 && ne(r, 1);
    if (on) {
        if (YSHOFF[k] > 300) {
            std::fill(YSBF.begin() + (int64_t)(k * 2) * YSLEN, YSBF.begin() + (int64_t)(k * 2 + 2) * YSLEN, 0.0);
            for (int j = 0; j < 4; ++j) YSX[k * 4 + j] = 0;
        }
        YSHOFF[k] = 0;
    } else {
        YSHOFF[k] = emin(100000, YSHOFF[k] + 1);
    }
    YSHON[k] = on;
    YSHR[k] = r;
    YSHI[k] = 1 / w;
    // reverse in chunks that follow the beat (a note value for the window), else free windows
    int sy = (int)std::floor(EFFP[o + K_SHSY] + 0.5);
    YSHB[k] = 0;   // (the JSFX's reverse on the beat, in the loop: the reverse is on the way out now)
    (void)sy;
    if (truth(YSHB[k])) {
        double cb = YBEATS[iclamp(sy, 0, 16)];
        double cs = emin(cb * (60 / y_tempo) * srate, (YSLEN - 600) / ((1 + std::fabs(r)) * 1.05));
        YSHW[k] = cs;
        YSHBI[k] = 1 / (cs * (y_tempo / (60 * srate)));
        YSHE[k] = emin(0.45, 0.012 * (srate / cs));
    } else {
        YSHW[k] = w;
    }
    if (!y_hq || r < 0 || truth(YSHB[k]))
        for (int j = 0; j < 4; ++j) YSX[k * 4 + j] = 0;
    // the three filters in the loop: high pass, low pass, bell (rebuilt only when they change)
    double *lm = YLCH + k * 12;
    f = EFFP[o + K_HP];
    YFEN[k * 3] = f > 20.5;
    if (truth(YFEN[k * 3]) && (f != lm[0] || y_snap)) { lm[0] = f; flt::coef(6, f, 0, 0.7071, 0, YFC + (k * 3) * 5, srate); }
    f = EFFP[o + K_LP];
    YFEN[k * 3 + 1] = f < 19500;
    if (truth(YFEN[k * 3 + 1]) && (f != lm[1] || y_snap)) { lm[1] = f; flt::coef(10, f, 0, 0.7071, 0, YFC + (k * 3 + 1) * 5, srate); }
    f = EFFP[o + K_BF];
    double g = EFFP[o + K_BG], q = EFFP[o + K_BQ];
    YFEN[k * 3 + 2] = std::fabs(g) >= 0.05;
    if (truth(YFEN[k * 3 + 2]) && (f != lm[2] || g != lm[3] || q != lm[4] || y_snap)) {
        lm[2] = f; lm[3] = g; lm[4] = q;
        flt::coef(0, f, g, q, 0, YFC + (k * 3 + 2) * 5, srate);
    }
}

// everything that changes slowly, every 16 samples
void Engine::tick()
{
    for (int i = 0; i < EB0 + NL * EBN; ++i) EFFP[i] = cpv(i);
    for (int i = MB0; i < MB0 + NMOD * MN; ++i) EFFP[i] = cpv(i);
    for (int k = 0; k < NL; ++k) YTM[k] = 1;
    d_beat += 16 * (y_tempo / (60 * srate));
    modeval();
    // how the lines next to each other connect, and the link amounts (glide)
    y_con[0] = (int)clampd(std::floor(EFFP[P_C12] + 0.5), 0, 2);
    y_con[1] = (int)clampd(std::floor(EFFP[P_C23] + 0.5), 0, 2);
    y_lkt[0] = clampd(EFFP[P_LK12], 0, 100) * 0.01;
    y_lkt[1] = clampd(EFFP[P_LK23], 0, 100) * 0.01;
    for (int j = 0; j < NL - 1; ++j) y_lkc[j] += (y_lkt[j] - y_lkc[j]) * 0.3;
    y_frt = EFFP[P_FREEZE] >= 0.5;
    y_hq = EFFP[P_HQ] >= 0.5;
    y_hql = y_hq ? 7 : 0;
    // the shared reverb
    y_rvt = clampd(EFFP[P_RVB], 0, 100) * 0.01;
    double x = clampd(EFFP[P_RDEC], 0.2, 10), t = clampd(EFFP[P_RSIZ], 50, 200);
    if (x != YRCH[0] || t != YRCH[1] || y_snap) {
        YRCH[0] = x; YRCH[1] = t;
        for (int i = 0; i < 8; ++i) {
            YRD[i] = emax(16, std::floor(RBASE[i] * t * 0.01 * (srate / 48000) + 0.5));
            YRG[i] = epow(10, -3 * (YRD[i] / (srate * x * 1.13)));
        }
        YROG = 1.65 / std::sqrt(x);
    }
    YRDA = 1 - std::exp(-2 * PI * (emin(clampd(EFFP[P_RDMP], 1000, 20000), srate * 0.45) / srate));
    YRPD = std::floor(clampd(EFFP[P_RPRE], 0, 200) * 0.001 * srate);
    int e = (int)std::floor(EFFP[P_RSHP] + 0.5);
    YRSR = e == 0 ? 2 : (e == 1 ? 1.498307 : (e == 2 ? 2.996614 : 0.5));
    // 100% stays under the point where the loop would start to grow (lower with longer decay and smaller size)
    YRSH = clampd(EFFP[P_RSHM], 0, 100) * 0.01 * emin(1, 0.35 * epow(x, -0.42) * t * 0.01) * (e == 3 ? 0.8 : 1);
    YRSW = 0.06 * srate;
    YRSI = 1 / YRSW;
    if (y_rvt > 0.0003 || y_rvc > 0.0003) {
        if (y_rvoff > 600) {
            std::fill(RVB.begin(), RVB.end(), 0.0);
            std::fill(RPB.begin(), RPB.end(), 0.0);
            std::fill(RAP.begin(), RAP.end(), 0.0);
            std::fill(RSB.begin(), RSB.end(), 0.0);
            for (int i = 0; i < 8; ++i) RZ[i] = 0;
            y_rl = 0; y_rr = 0;
        }
        y_rvoff = 0;
    } else {
        y_rvoff = emin(1000000, y_rvoff + 1);
    }
    for (int k = 0; k < NL; ++k) lprep(k);
    // input, and the dry and wet levels (the CLAP's: the JSFX had a dry/wet balance)
    y_ingt = epow(10, clampd(EFFP[P_IN], -24, 24) / 20);
    x = EFFP[P_WET];
    y_mxt = x <= -59.9 ? 0 : epow(10, emin(12, x) / 20);
    x = EFFP[P_DRY];
    y_dyt = x <= -59.9 ? 0 : epow(10, emin(12, x) / 20);
    // the first time (or a new sample rate): start at the targets
    if (y_snap) {
        for (int i = 0; i < NC; ++i) YCUR[i] = YTGT[i];
        for (int k = 0; k < NL; ++k) {
            YFBc[k] = YFBt[k]; YXc[k] = YXt[k]; YLVc[k] = YLVt[k]; YPNc[k] = YPNt[k]; YWDc[k] = YWDt[k];
            YDGc[k] = YDGt[k]; YCPc[k] = YCPt[k]; YSBc[k] = YSBt[k]; YSHP[k] = 0; YRSc[k] = YRSt[k];
        }
        y_rvc = y_rvt; y_ingc = y_ingt; y_mxc = y_mxt; y_dyc = y_dyt; y_frc = y_frt;
        for (int j = 0; j < NL - 1; ++j) y_lkc[j] = y_lkt[j];
    }
    y_snap = false;
}

// ======================================================================== per block / per sample
void Engine::block(int samplesblock, const Transport &tr, const std::vector<MidiEv> &midi)
{
    ab.block(U, A.now(), H.pv(P_AB) >= 0.5, samplesblock, srate);
    V.side.store(ab.side ? 1 : 0, std::memory_order_relaxed);
    y_tempo = tr.tempo > 0 ? tr.tempo : 120;
    y_bps = y_tempo / (60 * srate);
    if (tr.playing) {
        y_bt = tr.beat;
        if (std::fabs(tr.beat - d_beat) > 0.25) d_beat = tr.beat;
        else d_beat += (tr.beat - d_beat) * 0.1;
    }
    V.tempo.store(y_tempo, std::memory_order_relaxed);
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
    // ducking: how fast, and from where
    y_dka = 1 - std::exp(-1 / (emax(0.5, cpv(P_DATT)) * 0.001 * srate));
    y_dkr = 1 - std::exp(-1 / (emax(5, cpv(P_DREL)) * 0.001 * srate));
    y_dsrc = cpv(P_DSRC) >= 0.5;
    y_dthr = cpv(P_DTHR);
    y_dam = cpv(P_DUCK) * 0.01;
    y_rdam = cpv(P_RDUCK) * 0.01;
    // flush the tiny leftovers (denormals; the followers' levels the JSFX way, sign and all)
    for (int i = 0; i < NC * 6; ++i) flush21(YFZ[i]);
    for (int i = 0; i < NC * 4; ++i) flush21(YCZ[i]);
    for (int i = 0; i < NMOD; ++i)
        if (MEN[i] < 0.000000000000000000001) MEN[i] = 0;
    if (y_den < 0.000000000000000000001) y_den = 0;
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
    }
}

void Engine::afterAct()
{
    if (!idDrawn && A.myid != 0) { eel_rand(899999); idDrawn = true; }
}

void Engine::samples(const double *const *in, const double *const *sc, double *const *out, int n)
{
    if ((int)br0.size() < n) { br0.resize(n); br1.resize(n); }
    for (int i = 0; i < n; ++i) {
        double r0 = in[0][i], r1 = in[1][i];
        double s2 = sc ? sc[0][i] : 0, s3 = sc ? sc[1][i] : 0;
        br0[i] = r0; br1[i] = r1;
        y_ingc += (y_ingt - y_ingc) * 0.002;
        double x0 = r0 * y_ingc, x1 = r1 * y_ingc;
        // the modulation followers (the wet signal for source "Wet")
        for (int f = 0; f < d_nf; ++f) {
            int s = MFL[f];
            double v = MSR[s] == 0 ? emax(std::fabs(x0), std::fabs(x1)) : (MSR[s] == 1 ? y_wetabs : emax(std::fabs(s2), std::fabs(s3)));
            MEN[s] += (v > MEN[s] ? MKA[s] : MKR[s]) * (v - MEN[s]);
        }
        if (++y_tc >= 16) { y_tc = 0; tick(); }
        run(x0, x1);
        // ducking: the louder the source, the quieter the repeats
        double v = y_dsrc ? emax(std::fabs(s2), std::fabs(s3)) : emax(std::fabs(x0), std::fabs(x1));
        y_den += (v > y_den ? y_dka : y_dkr) * (v - y_den);
        double over = clampd((20 * std::log10(y_den + 0.000000001) - y_dthr) / 10, 0, 1);
        y_dgt = epow(10, -(y_dam * 40 * over) / 20);
        y_rgt = epow(10, -(y_rdam * 40 * over) / 20);
        y_dgc += (y_dgt - y_dgc) * 0.002;
        y_rgc += (y_rgt - y_rgc) * 0.002;
        y_mxc += (y_mxt - y_mxc) * 0.002;
        y_dyc += (y_dyt - y_dyc) * 0.002;
        out[0][i] = x0 * y_dyc + (y_wl * y_dgc + y_vl * y_rgc) * y_mxc;
        out[1][i] = x1 * y_dyc + (y_wr * y_dgc + y_vr * y_rgc) * y_mxc;
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
        ab.post(br0[i], br1[i], out[0][i], out[1][i], br0[i], br1[i]);
        if (anw) V.an.put((br0[i] + br1[i]) * 0.5, (out[0][i] + out[1][i]) * 0.5);
    }
    if (anw) V.an.publish();
}

} // namespace delay
} // namespace ts
