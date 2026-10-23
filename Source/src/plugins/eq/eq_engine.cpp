#include "eq_engine.h"
#include "../../core/eelrand.h"
#include "../../core/fft.h"
#include "../../core/filters.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace ts {
namespace peq {

using flt::coefBw;
using flt::cutHp;
using flt::cutNp;
using flt::cutS;

// linear and natural phase: the block size per mode (the filter is 8 blocks long)
static const int LBS[8] = {0, 256, 512, 1024, 2048, 256, 512, 1024};

int latency(int lin, bool la, double lams, double sr)
{
    lin = iclamp(lin, 0, 7);
    if (lin) return lin >= 5 ? LBS[lin] : 5 * LBS[lin];
    if (la) return (int)emax(1, emin(8000, std::floor(lams * 0.001 * sr + 0.5)));
    return 0;
}

// the detector's filter (and the solo filter): 0 band pass, 1 low pass, 2 high pass
static void edet(int kind, double f, double q, double *o, double srate)
{
    double w = 2 * PI * (emin(f, srate * 0.49) / srate), cw = std::cos(w), sw = std::sin(w), al = sw / (2 * q), a0 = 1 + al;
    if (kind == 0) { o[0] = al / a0; o[1] = 0; o[2] = -al / a0; }
    else if (kind == 1) { o[0] = (1 - cw) * (0.5 / a0); o[1] = (1 - cw) / a0; o[2] = o[0]; }
    else { o[0] = (1 + cw) * (0.5 / a0); o[1] = -(1 + cw) / a0; o[2] = o[0]; }
    o[3] = -2 * (cw / a0);
    o[4] = (1 - al) / a0;
}

// ======================================================================== setup
void Engine::init(double sr)
{
    srate = sr;
    if (!nonceDrawn) { eel_rand(1000000); nonceDrawn = true; }   // the instance nonce (the JSFX's first rand in @init)
#define Z(a) memset(a, 0, sizeof a)
    Z(ECO); Z(EZS); Z(ETMP); Z(ENS); Z(EPL); Z(EACT); Z(EDL); Z(EML); Z(ETY); Z(EQM); Z(EDS); Z(EDMD);
    Z(EAC); Z(ELV); Z(ESM); Z(EDY); Z(EDDT); Z(ESF); Z(ESG); Z(ESQ); Z(EBG); Z(EDC); Z(EDZ); Z(EENV);
    Z(EKA); Z(EKR); Z(EKD); Z(EDG); Z(EHT); Z(EHC); Z(EDTH); Z(EDRG); Z(EDSL); Z(EDKN);
    Z(G); Z(ZL); Z(ZR); Z(LN); Z(LP); Z(ESO); Z(ESZ); Z(DSG); Z(TGHI); Z(TGMX); Z(TGMN); Z(KWC); Z(KWZ); Z(AMR);
    Z(LBM); Z(LBNF); Z(LBO); Z(LBI);
#undef Z
    for (double &v : ECP) v = -99999;   // "not seen yet"
    TGLV.assign(NB * 30000, 0.0);
    DLA.assign(32768, 0.0); DLB.assign(32768, 0.0); DRA.assign(32768, 0.0); DRB.assign(32768, 0.0);
    LDS.assign(32768, 0.0); LFD.assign(131072, 0.0); LFF.assign(524288, 0.0); LAC.assign(8192, 0.0);
    LRES.assign(4 * 8200, 0.0); LIB.assign(8192, 0.0); LOB.assign(4096, 0.0); LOB2.assign(4096, 0.0);
    LGC.assign(65536, 0.0); LGL.assign(4 * 8200, 0.0); LHT.assign(16384, 0.0);
    // auto gain (curve): 64 log-spaced frequencies (pink noise: equal weight per octave)
    for (int i = 0; i < 64; ++i) {
        double t = 2 * PI * (emin(20 * epow(1000, i / 63.0), srate * 0.49) / srate);
        AGCW[i] = std::cos(t);
        AGC2[i] = std::cos(2 * t);
    }
    seenOk = false;
    e_ing = 1; e_dm = 0; e_wm = 1; e_ag = 1; e_agt = 1; e_agdb = 0; e_agdbs = 0; e_agdbp = 0; e_pi = 0; e_po = 0;
    e_ksm = 0.05; e_ksm_sr = 0; e_kp = 0; e_agk = 0; e_csr = 0; e_lrbt = 0;
    e_agon = e_agm = e_del = e_nat = e_la = e_agmv = e_agp = false;
    e_snap = e_rescan = e_dirty = e_anyf = e_hasms = e_haslr = e_lrb = false;
    e_lnew = false; e_lfirst = true; e_lnox = false; e_lxf = false; e_tgsig = false;
    e_lin = 0; e_linp = -1; e_lb = 0; e_ld = 0; e_dl = 0; e_na = 0; e_nd = 0; e_nm = 0; e_tc = 0; e_flc = 0;
    e_sol = -1; e_sold = 0; e_lcur = 0; e_lpos = 0; e_lbank = 0; e_lmode = 0; e_tgk = -1; e_sside = -1; e_dp = 0; e_sct = 1;
    e_tgn = 0; e_tc2 = 0; e_tms = 48; e_tgdur = 5000; e_tgi = 0;
    c_ip0 = 0; c_ip1 = 0; c_mi0 = -200; c_mi1 = -200;
    uisolo = -1;
    V.an.clear();
}

void Engine::reset()
{
    memset(EZS, 0, sizeof EZS); memset(EDZ, 0, sizeof EDZ); memset(ZL, 0, sizeof ZL); memset(ZR, 0, sizeof ZR);
    memset(ESZ, 0, sizeof ESZ); memset(KWZ, 0, sizeof KWZ); memset(EENV, 0, sizeof EENV);
    lclear();
}

// ======================================================================== the bands
void Engine::pub(int b)
{
    for (int k = 0; k < 5; ++k) G[b][k] = ECO[b * ECS + k];
    LN[b] = ENS[b];
    LP[b] = EPL[b];
}

void Engine::zero(int b)
{
    memset(EZS + b * EZN, 0, EZN * sizeof(double));
    EDZ[b * 2] = 0; EDZ[b * 2 + 1] = 0;
    ZL[b][0] = ZL[b][1] = ZR[b][0] = ZR[b][1] = 0;
}

// u through ns sections (coefficients at o, states at z: 4 per section, the left channel's first)
double Engine::filt(double u, const double *o, int ns, double *z)
{
    int k = 0;
    for (int i = 0; i < ns; ++i) {
        double y = o[k] * u + z[0];
        z[0] = o[k + 1] * u - o[k + 3] * y + z[1];
        z[1] = o[k + 2] * u - o[k + 4] * y;
        u = y;
        k += 5;
        z += 4;
    }
    return u;
}

// a band that is not a plain stereo one: left, right, mid or side, every section through the arrays
void Engine::slow(int b, double &l, double &r)
{
    int ns = ENS[b], pl = EPL[b];
    const double *o = ECO + b * ECS;
    double *z = EZS + b * EZN;
    if (pl == 1) l = filt(l, o, ns, z);
    else if (pl == 2) r = filt(r, o, ns, z + 2);
    else if (pl == 3) {
        double u = (l + r) * 0.5, v = filt(u, o, ns, z) - u;
        l += v;
        r += v;
    } else {
        double u = (l - r) * 0.5, v = filt(u, o, ns, z) - u;
        l += v;
        r -= v;
    }
}

// the sections after the first of a stereo band (the steeper cuts)
void Engine::xsec(int b, double &l, double &r)
{
    const double *o = ECO + b * ECS + 5;
    double *z = EZS + b * EZN + 4;
    l = filt(l, o, ENS[b] - 1, z);
    r = filt(r, o, ENS[b] - 1, z + 2);
}

// the bands that move, every E_TK samples: gliding settings, the dynamic gain, then new coefficients when they changed
void Engine::tick()
{
    for (int i = 0; i < e_nm; ++i) {
        int b = EML[i], bb = b * 12, ty = ETY[b];
        bool ch = false;
        if (ESM[b]) {
            double tf = ECP[bb + 2], tq = ECP[bb + 4], tg = ELV[b] ? 0 : ECP[bb + 3];
            double x = std::log(tf / ESF[b]);
            if (std::fabs(x) < 0.0001) ESF[b] = tf;
            else ESF[b] *= std::exp(x * e_ksm);
            x = std::log(tq / ESQ[b]);
            if (std::fabs(x) < 0.0001) ESQ[b] = tq;
            else ESQ[b] *= std::exp(x * e_ksm);
            x = tg - ESG[b];
            if (std::fabs(x) < 0.002) ESG[b] = tg;
            else ESG[b] += x * e_ksm;
            if (eq(ESF[b], tf) && eq(ESQ[b], tq) && eq(ESG[b], tg)) { ESM[b] = false; e_rescan = true; }
            ch = true;
        }
        if (EDY[b]) {
            double lv = EDDT[b] ? 10 * std::log10(EENV[b] + 0.00000000000000000001) + 3.0103 : 20 * std::log10(EENV[b] + 0.0000000001);
            double ov = EDMD[b] == 1 ? lv - EDTH[b] : EDTH[b] - lv;
            double kn = EDKN[b];
            double e = kn > 0.01 ? (ov <= -kn * 0.5 ? 0 : (ov >= kn * 0.5 ? ov : sqr(ov + kn * 0.5) / (2 * kn))) : emax(0, ov);
            double r = EDSL[b] * e, rg = EDRG[b];
            double tgt = rg >= 0 ? emin(rg, r) : emax(rg, -r);
            // Compress: attack as the gain moves away from 0, then (Hold) it stays a while before it releases back.
            // Expand, as a conventional expander or gate: attack as it opens (back towards 0), hold, then release as it closes
            double k;
            if (EDMD[b] == 2 ? std::fabs(tgt) <= std::fabs(EDG[b]) : std::fabs(tgt) >= std::fabs(EDG[b])) { k = EKA[b]; EHC[b] = EHT[b]; }
            else if (EHC[b] > 0) { EHC[b] -= 1; k = 0; }
            else k = EKR[b];
            EDG[b] += (tgt - EDG[b]) * k;
        } else if (ne(EDG[b], 0)) {
            if (std::fabs(EDG[b]) < 0.002) { EDG[b] = 0; ch = true; e_rescan = true; }
            else { EDG[b] -= EDG[b] * e_ksm; ch = true; }
        }
        double ge = ESG[b] + EDG[b];
        if (ch || std::fabs(ge - EBG[b]) > 0.01) {
            EBG[b] = ge;
            ENS[b] = coefBw(ty, ESF[b], ge, ESQ[b], EQM[b], ECO + b * ECS, srate);
            pub(b);
        }
        if (ELV[b] && !ESM[b] && eq(EDG[b], 0)) {
            EAC[b] = false; ELV[b] = false; ENS[b] = 0;
            pub(b);
            zero(b);
            e_rescan = true;
        }
    }
}

// the bands looked at again: their settings -> what runs (all: as if every band were new, e.g. a new sample rate)
void Engine::scan(bool all)
{
    e_sct = 1;   // (the sidechain trim is gone: Set dynamic thresholds does that job)
    e_na = 0; e_nd = 0; e_nm = 0; e_hasms = false; e_haslr = false; e_dirty = true;
    for (int b = 0; b < NB; ++b) {
        int bb = b * 12;
        bool ch = all;
        for (int k = 0; k < 12; ++k) {
            double v = cpv(EB0 + bb + k);
            if (v != ECP[bb + k]) { ECP[bb + k] = v; ch = true; }
        }
        if (ch) e_dirty = true;
        bool on = ECP[bb] >= 0.5;
        int ty = (int)std::floor(ECP[bb + 1] + 0.5);
        double f = ECP[bb + 2], g = ECP[bb + 3], q = ECP[bb + 4];
        int pl = (int)std::floor(ECP[bb + 5] + 0.5);
        double dv = std::floor(ECP[bb + 6] + 0.5), sv = std::floor(ECP[bb + 7] + 0.5);
        int dm0 = pkDm(dv), qm = pkQm(sv);
        bool dy = on && !e_lin && ty <= 2 && dm0 > 0;
        bool gt = ty <= 2;   // has a gain
        bool ba = false;     // rebuild this band's coefficients now
        if (all && EAC[b]) EAC[b] = false;
        if (on) {
            if (!EAC[b]) {
                // a band switching on: a bell or shelf grows in from flat
                EAC[b] = true; ELV[b] = false;
                ESF[b] = f; ESQ[b] = q; ESG[b] = (gt && !e_lin && !all) ? 0 : g;
                ESM[b] = gt && !e_lin && !all;
                EDG[b] = 0; EENV[b] = 0; ETY[b] = ty; EPL[b] = pl;
                zero(b);
                ba = true;
            } else if (ty != ETY[b] || pl != EPL[b]) {
                if (pl != EPL[b]) zero(b);
                ESF[b] = f; ESQ[b] = q; ESG[b] = g; ESM[b] = false; ELV[b] = false;
                ETY[b] = ty; EPL[b] = pl;
                ba = true;
            } else if (ch && (ESF[b] != f || ESQ[b] != q || ESG[b] != g || ELV[b])) {
                ELV[b] = false;
                if (e_lin) { ESF[b] = f; ESQ[b] = q; ESG[b] = g; ESM[b] = false; ba = true; }
                else ESM[b] = true;
            }
            if (EQM[b] != qm) { EQM[b] = qm; ba = true; }
            // the dynamics
            EDY[b] = dy;
            if (dy) {
                if (ch) {
                    EDMD[b] = dm0; EDTH[b] = ECP[bb + 8]; EDRG[b] = ECP[bb + 9];
                    double rt = pkRt(dv);
                    EDSL[b] = dm0 == 2 ? (rt >= 99.5 ? 1000 : rt - 1) : (rt >= 99.5 ? 1 : 1 - 1 / rt);
                    EDKN[b] = pkKn(sv);
                    bool dt = pkDet(dv) >= 1;
                    if (dt != EDDT[b]) { EDDT[b] = dt; EENV[b] = 0; }
                    EDS[b] = (pkSrc(sv) ? 4 : 0) + (pl == 1 ? 1 : (pl == 2 ? 2 : (pl == 4 ? 3 : 0)));
                    edet(ty == 1 ? 1 : (ty == 2 ? 2 : 0), f, ty == 0 ? emax(0.4, emin(10, q)) : 0.7071068, EDC + b * 5, srate);
                    // the attack and release can't be faster than the band's own period lets the detector see
                    double at = emax(emax(0.1, ECP[bb + 10]), 500 / f), rl = emax(emax(1, ECP[bb + 11]), 2000 / f);
                    EKA[b] = 1 - std::exp(-E_TK / (at * 0.001 * srate));
                    EKR[b] = 1 - std::exp(-E_TK / (rl * 0.001 * srate));
                    EHT[b] = std::floor(pkHold(dv) * 0.001 * (srate / E_TK) + 0.5);
                    EKD[b] = EDDT[b] ? 1 - std::exp(-1 / (emax(0.01, emin(0.06, 4 / f)) * srate))
                                     : 1 - std::exp(-1 / (emax(0.0003, emin(0.03, 3 / f)) * srate));
                }
                EDL[e_nd++] = b;
            }
            if (ba) {
                EBG[b] = ESG[b] + EDG[b];
                ENS[b] = coefBw(ty, ESF[b], EBG[b], ESQ[b], qm, ECO + b * ECS, srate);
                pub(b);
            }
            // for the curve, auto gain and the linear / natural filters: the static settings
            if (!gt || std::fabs(g) >= 0.001) {
                EACT[e_na++] = b;
                if (pl >= 3) e_hasms = true;
                if (pl == 1 || pl == 2) e_haslr = true;
            }
            if (ESM[b] || dy || ne(EDG[b], 0)) EML[e_nm++] = b;
        } else {
            // off: a bell or shelf glides back to flat first
            if (EAC[b]) {
                if (ETY[b] <= 2 && !e_lin && (std::fabs(ESG[b]) > 0.002 || std::fabs(EDG[b]) > 0.002)) {
                    ELV[b] = true; ESM[b] = true; EDY[b] = false;
                    EML[e_nm++] = b;
                } else {
                    EAC[b] = false; ELV[b] = false; ESM[b] = false; EDG[b] = 0; ENS[b] = 0; EENV[b] = 0; EDY[b] = false;
                    pub(b);
                    zero(b);
                }
            } else {
                EENV[b] = 0;
                EDY[b] = false;
            }
        }
    }
    e_anyf = e_na > 0 || e_nm > 0;
}

// ======================================================================== linear and natural phase
// the bands' summed analog response as one FIR, run by partitioned FFT convolution. Linear phase: a symmetric FIR (zero
// phase, delayed). Natural phase: the minimum-phase FIR with the same magnitude, i.e. the phase the analog filters
// have, with only the block as latency.
void Engine::lclear()
{
    std::fill(LIB.begin(), LIB.end(), 0.0);
    std::fill(LOB.begin(), LOB.end(), 0.0);
    std::fill(LOB2.begin(), LOB2.end(), 0.0);
    std::fill(LFD.begin(), LFD.end(), 0.0);
    std::fill(DLA.begin(), DLA.end(), 0.0);
    std::fill(DLB.begin(), DLB.end(), 0.0);
    std::fill(DRA.begin(), DRA.end(), 0.0);
    std::fill(DRB.begin(), DRB.end(), 0.0);
    e_lcur = 0; e_lpos = 0; e_lfirst = true; e_lnew = false; e_lxf = false;
}

// band b's analog prototype from its static settings: type, centre frequency, A, Q^2, then for a cut high pass?,
// order, 1/Q^2 per pole pair
void Engine::amPrep(int b, double *r)
{
    int bb = b * 12, ty = (int)std::floor(ECP[bb + 1] + 0.5);
    double f = emin(ECP[bb + 2], srate * 0.49), g = ECP[bb + 3], q = emax(0.05, ECP[bb + 4]);
    q = flt::qe(ty, g, q, pkQm(ECP[bb + 7]));
    r[0] = ty; r[1] = f; r[2] = epow(10, g / 40); r[3] = q * q;
    if (ty >= 5) {
        // a Butterworth cut: high pass?, its order, then 1 / Q^2 of each pole pair
        int np = cutNp(cutS(ty));
        r[4] = cutHp(ty) ? 1 : 0;
        r[5] = np;
        for (int i = 0; i < np / 2; ++i) {
            double c = std::cos(PI * ((2 * i + 1 + (np & 1)) / (2.0 * np)));
            r[6 + i] = 4 * c * c;
        }
    }
}

// the analog prototype's power response |H|^2 at fx (Hz)
double Engine::am(const double *r, double fx) const
{
    int ty = (int)r[0];
    double x2 = fx / r[1], A = r[2], qq = r[3], p;
    x2 *= x2;
    if (ty == 0) p = (sqr(1 - x2) + x2 * A * (A / qq)) / (sqr(1 - x2) + x2 / (A * A * qq));
    else if (ty == 1) p = A * A * ((sqr(A - x2) + x2 * (A / qq)) / (sqr(1 - A * x2) + x2 * (A / qq)));
    else if (ty == 2) p = A * A * ((sqr(1 - A * x2) + x2 * (A / qq)) / (sqr(A - x2) + x2 * (A / qq)));
    else if (ty == 3) p = sqr(1 - x2) / (sqr(1 - x2) + x2 / qq);
    else if (ty == 4) p = (x2 / qq) / (sqr(1 - x2) + x2 / qq);
    else {
        int np = (int)r[5];
        bool hp = truth(r[4]);
        p = (np & 1) ? (hp ? x2 : 1) / (1 + x2) : 1;
        for (int i = 0; i < np / 2; ++i) p *= (hp ? x2 * x2 : 1) / (sqr(1 - x2) + x2 * r[6 + i]);
    }
    return emax(p, 0.000000001);
}

// one filter's impulse response in LHT (nn taps) -> its 8 partitions as spectra in a filter bank
void Engine::firStore(int bk, int f, int lb, int nn)
{
    (void)nn;
    for (int p = 0; p < 8; ++p) {
        double *z = LFF.data() + bk * 262144 + (f * 8 + p) * 8192;
        for (int i = 0; i < 2 * lb; ++i) {
            z[2 * i] = i < lb ? LHT[p * lb + i] : 0;
            z[2 * i + 1] = 0;
        }
        fft::forward(z, 2 * lb);
    }
}

// the minimum-phase spectrum with the log magnitude o (natural log of the amplitude, bins 0..nn/2) -> dst (nn bins)
void Engine::minphase(const double *o, double *dst, int nn)
{
    int nh = nn / 2;
    double *L = LDS.data();
    for (int k = 0; k <= nh; ++k) { L[2 * k] = o[k]; L[2 * k + 1] = 0; }
    for (int k = 1; k < nh; ++k) { L[2 * (nn - k)] = o[k]; L[2 * (nn - k) + 1] = 0; }
    fft::inverse(L, nn);
    // the real cepstrum, folded onto the causal side
    for (int n = 0; n < nn; ++n) {
        double c = L[2 * n] / nn;
        if (n == 0 || n == nh) L[2 * n] = c;
        else if (n < nh) L[2 * n] = 2 * c;
        else L[2 * n] = 0;
        L[2 * n + 1] = 0;
    }
    fft::forward(L, nn);
    for (int k = 0; k < nn; ++k) {
        double m = std::exp(L[2 * k]), ph = L[2 * k + 1];
        dst[2 * k] = m * std::cos(ph);
        dst[2 * k + 1] = m * std::sin(ph);
    }
}

void Engine::firBuild()
{
    int lb = LBS[e_lin], nn = 8 * lb, nh = nn / 2;
    double *L = LDS.data();
    e_lmode = (e_haslr && e_hasms) ? 2 : (e_hasms ? 1 : 0);
    int nf = e_lmode == 2 ? 4 : 2;
    int bk = e_lfirst ? e_lbank : 1 - e_lbank;
    LBM[bk] = e_lmode;
    LBNF[bk] = nf;
    LBO[bk * 4] = 0; LBI[bk * 4] = 0;
    if (e_lmode == 2) {
        LBO[bk * 4 + 1] = 0; LBI[bk * 4 + 1] = 1; LBO[bk * 4 + 2] = 1; LBI[bk * 4 + 2] = 0; LBO[bk * 4 + 3] = 1; LBI[bk * 4 + 3] = 1;
    } else {
        LBO[bk * 4 + 1] = 1; LBI[bk * 4 + 1] = 1;
    }
    e_lnox = !e_lfirst && LBM[e_lbank] != e_lmode;
    for (int i = 0; i < e_na; ++i) amPrep(EACT[i], AMR + i * 16);
    if (e_nat) {
        // ---- natural phase: the log magnitude of each group of bands, made minimum phase, then combined ----
        // groups (log amplitude): 0 stereo + left, 1 stereo + right, 2 mid, 3 side (mode 1: 0 = stereo + mid, 1 = stereo + side)
        std::fill(LGL.begin(), LGL.end(), 0.0);
        for (int k = 0; k <= nh; ++k) {
            double fx = k * (srate / nn);
            for (int i = 0; i < e_na; ++i) {
                int pl = EPL[EACT[i]];
                double ln = 0.5 * std::log(am(AMR + i * 16, fx));
                if (e_lmode == 1) {
                    if (pl == 0 || pl == 3) LGL[k] += ln;
                    if (pl == 0 || pl == 4) LGL[8200 + k] += ln;
                } else {
                    if (pl == 0 || pl == 1) LGL[k] += ln;
                    if (pl == 0 || pl == 2) LGL[8200 + k] += ln;
                    if (pl == 3) LGL[16400 + k] += ln;
                    if (pl == 4) LGL[24600 + k] += ln;
                }
            }
        }
        // a group can't be deeper than -50 dB: the log of a response that goes to zero at DC would need an endless filter
        for (double &v : LGL) v = emax(v, -5.756);
        double *C = LGC.data();
        minphase(LGL.data(), C, nn);
        minphase(LGL.data() + 8200, C + 2 * nn, nn);
        if (e_lmode == 2) { minphase(LGL.data() + 16400, C + 4 * nn, nn); minphase(LGL.data() + 24600, C + 6 * nn, nn); }
        for (int f = 0; f < nf; ++f) {
            for (int k = 0; k < nn; ++k) {
                if (e_lmode == 2) {
                    // out = Mb(Hm, Hs) * diag(HL, HR); Mb = [[p, q], [q, p]], p = (Hm + Hs) / 2, q = (Hm - Hs) / 2
                    const double *hl = C + 2 * k, *hr = C + 2 * nn + 2 * k, *hm = C + 4 * nn + 2 * k, *hs = C + 6 * nn + 2 * k;
                    double pr = (hm[0] + hs[0]) * 0.5, pq = (hm[1] + hs[1]) * 0.5, ar = (hm[0] - hs[0]) * 0.5, ai = (hm[1] - hs[1]) * 0.5;
                    double br, bi, a, t;
                    if (f == 0) { br = hl[0]; bi = hl[1]; a = pr; t = pq; }
                    else if (f == 1) { br = hr[0]; bi = hr[1]; a = ar; t = ai; }
                    else if (f == 2) { br = hl[0]; bi = hl[1]; a = ar; t = ai; }
                    else { br = hr[0]; bi = hr[1]; a = pr; t = pq; }
                    L[2 * k] = a * br - t * bi;
                    L[2 * k + 1] = a * bi + t * br;
                } else {
                    const double *o = C + f * 2 * nn + 2 * k;
                    L[2 * k] = o[0];
                    L[2 * k + 1] = o[1];
                }
            }
            fft::inverse(L, nn);
            for (int n = 0; n < nn; ++n) {
                double u = (double)n / nn;
                double w = u < 0.75 ? 1 : 0.5 + 0.5 * std::cos(PI * ((u - 0.75) / 0.25));
                LHT[n] = L[2 * n] / nn * w;
            }
            firStore(bk, f, lb, nn);
        }
    } else {
        // ---- linear phase: the response at every bin, as a 2x2 matrix (rows: output channel, columns: input channel) ----
        for (int k = 0; k <= nh; ++k) {
            double fx = k * (srate / nn);
            double m00 = 1, m01 = 0, m10 = 0, m11 = 1;
            for (int i = 0; i < e_na; ++i) {
                int b = EACT[i], pl = EPL[b];
                double a = std::sqrt(am(AMR + i * 16, fx));
                if (pl == 0) { m00 *= a; m01 *= a; m10 *= a; m11 *= a; }
                else if (pl == 1) { m00 *= a; m01 *= a; }
                else if (pl == 2) { m10 *= a; m11 *= a; }
                else if (e_lmode == 1) {
                    if (pl == 3) { m00 *= a; m01 *= a; }
                    else { m10 *= a; m11 *= a; }
                } else {
                    // mid / side in the left-right basis: Mb = [[p, q], [q, p]]
                    double p = (a + 1) * 0.5, t = pl == 3 ? (a - 1) * 0.5 : (1 - a) * 0.5;
                    double n0 = p * m00 + t * m10, u = p * m01 + t * m11, w = t * m00 + p * m10, z = t * m01 + p * m11;
                    m00 = n0; m01 = u; m10 = w; m11 = z;
                }
            }
            LRES[k] = m00;
            if (e_lmode == 2) { LRES[8200 + k] = m01; LRES[16400 + k] = m10; LRES[24600 + k] = m11; }
            else LRES[8200 + k] = m11;
        }
        for (int f = 0; f < nf; ++f) {
            // zero-phase spectrum (natural order) -> impulse response
            const double *o = LRES.data() + f * 8200;
            for (int k = 0; k <= nh; ++k) { L[2 * k] = o[k]; L[2 * k + 1] = 0; }
            for (int k = 1; k < nh; ++k) { L[2 * (nn - k)] = o[k]; L[2 * (nn - k) + 1] = 0; }
            fft::inverse(L, nn);
            // centre it at tap nn/2, taper the ends (Tukey, 25 % each side)
            for (int n = 0; n < nn; ++n) {
                int k = n + nh;
                if (k >= nn) k -= nn;
                double u = (double)n / nn;
                u = emin(u, 1 - u);
                double w = u < 0.25 ? 0.5 - 0.5 * std::cos(4 * PI * u) : 1;
                LHT[n] = L[2 * k] / nn * w;
            }
            firStore(bk, f, lb, nn);
        }
    }
    if (e_lfirst) { e_lfirst = false; e_lnew = false; }
    else e_lnew = true;
}

// one filter bank's output for the current block into dst (2 x 2048)
void Engine::linOut(int bk, double *dst)
{
    int m = 2 * e_lb;
    double *acc = LAC.data();
    for (int o = 0; o < 2; ++o) {
        std::fill(acc, acc + 2 * m, 0.0);
        for (int f = 0; f < LBNF[bk]; ++f) {
            if (LBO[bk * 4 + f] != o) continue;
            for (int p = 0; p < 8; ++p) {
                const double *src = LFD.data() + LBI[bk * 4 + f] * 65536 + ((e_lcur - p) & 7) * 8192;
                const double *fl = LFF.data() + bk * 262144 + (f * 8 + p) * 8192;
                for (int k = 0; k < 2 * m; k += 2) {
                    double ar = src[k], ai = src[k + 1], br = fl[k], bi = fl[k + 1];
                    acc[k] += ar * br - ai * bi;
                    acc[k + 1] += ar * bi + ai * br;
                }
            }
        }
        fft::inverse(acc, m);
        for (int k = 0; k < e_lb; ++k) dst[o * 2048 + k] = acc[2 * (e_lb + k)] / m;
    }
}

// one block of the convolution (the inputs are in LIB: the previous block and this one, per channel)
void Engine::linRun()
{
    int m = 2 * e_lb;
    for (int ch = 0; ch < 2; ++ch) {
        double *dst = LFD.data() + ch * 65536 + e_lcur * 8192;
        for (int i = 0; i < m; ++i) { dst[2 * i] = LIB[ch * 4096 + i]; dst[2 * i + 1] = 0; }
        fft::forward(dst, m);
    }
    if (e_lnew) {
        // a new filter is ready: run the old bank too and fade over this block
        if (e_lnox) {
            e_lbank = 1 - e_lbank;
            e_lxf = false;
            linOut(e_lbank, LOB.data());
        } else {
            linOut(e_lbank, LOB2.data());
            e_lbank = 1 - e_lbank;
            linOut(e_lbank, LOB.data());
            e_lxf = true;
        }
        e_lnew = false;
    } else {
        e_lxf = false;
        linOut(e_lbank, LOB.data());
    }
    e_lcur = (e_lcur + 1) & 7;
    // the newest block becomes the old one
    memmove(LIB.data(), LIB.data() + e_lb, e_lb * sizeof(double));
    memmove(LIB.data() + 4096, LIB.data() + 4096 + e_lb, e_lb * sizeof(double));
}

// ======================================================================== Set dynamic thresholds
// listen, then set every dynamic band's threshold: a compress band reaches its full range, and an expand band is back
// to 0, on the loudest P_DTP % of the playing time. A.tgs: 0 idle, 1 waiting for signal, 2 listening, 3 working (one
// band per block)
void Engine::tgBlock()
{
    int &c_tgs = A.tgs;
    if (c_tgs > 0 && (e_lin || e_nd == 0 || (e_tgk >= 0 && !EDY[e_tgk]))) c_tgs = 0;
    if (c_tgs == 1) {
        // waiting for signal at what triggers the band(s): -50 dBFS, as Auto waits
        if (e_tgsig) {
            c_tgs = 2; e_tgn = 0; e_tc2 = 0;
            for (int b = 0; b < NB; ++b) { TGMX[b] = 0; TGMN[b] = 1000000; }
            e_tms = (int)emax(1, std::floor(srate * 0.001 + 0.5));
            int t = (int)std::floor(H.pv(P_TGTM) + 0.5);
            e_tgdur = 1000 * (t <= 0 ? 1 : (t == 1 ? 2 : (t == 2 ? 3 : (t == 3 ? 5 : (t == 4 ? 10 : (t == 5 ? 20 : 30))))));
        }
    } else if (c_tgs == 2) {
        if (e_tgn >= e_tgdur) { c_tgs = 3; e_tgi = 0; }
    } else if (c_tgs == 3) {
        // one band per block: the level its detector passes P_DTP % of the playing time (the loudest moments), from a
        // histogram in 0.1 dB steps from -60 to +40 dBFS (silence under -60 dB doesn't count)
        if (e_tgi < e_nd) {
            int k = EDL[e_tgi];
            const double *lvb = TGLV.data() + k * 30000;
            memset(TGHI, 0, sizeof TGHI);
            int n = 0;
            for (int i = 0; i < e_tgn; ++i) {
                double t = lvb[i];
                if (t > -60) { TGHI[(int)emin(999, std::floor((t + 60) * 10))] += 1; n += 1; }
            }
            if (n > 0) {
                double lim = n * emax(0.001, emin(0.5, H.pv(P_DTP) * 0.01)), acc = 0;
                int i = 999;
                for (;;) {
                    acc += TGHI[i];
                    if (acc < lim && i > 0) i -= 1;
                    else break;
                }
                double lv = -60 + i * 0.1;   // the bin's low edge, and the threshold rounds down: the band surely gets there
                double kn = EDKN[k], th;
                if (EDMD[k] == 1) {
                    // Compress: the threshold where those moments just reach the full range (through the knee if need be)
                    double ov = std::fabs(EDRG[k]) / emax(0.001, EDSL[k]);
                    if (kn > 0.01 && ov < kn * 0.5) ov = std::sqrt(2 * kn * ov) - kn * 0.5;
                    th = lv - ov;
                } else {
                    // Expand: the threshold just under those moments, so the band is back to 0 on them (full range under it)
                    th = lv - kn * 0.5;
                }
                H.setAudio(ep(k, K_THR), emax(-60, emin(0, std::floor(th * 10 + 0.0001) / 10)));
            }
            e_tgi += 1;
        } else {
            c_tgs = 0;
        }
    }
    A.tgel = c_tgs == 2 ? e_tgn * 0.001 : 0;
    A.tgav = e_nd > 0 ? 1 : 0;
    e_tgsig = false;
}

// ======================================================================== per block / per sample
void Engine::block(int samplesblock)
{
    ab.block(U, A.now(), H.pv(P_AB) >= 0.5, samplesblock, srate);
    V.side.store(ab.side ? 1 : 0, std::memory_order_relaxed);
    uisolo = V.solo.load(std::memory_order_relaxed);
    e_ing = epow(10, cpv(P_IN) / 20);
    double mix = cpv(P_MIX) * 0.01;
    e_dm = 1 - mix;
    e_wm = mix;
    e_agon = cpv(P_AUTOG) >= 0.5;
    e_agm = cpv(P_AGM) >= 0.5;
    e_del = H.pv(P_DELTA) >= 0.5;
    // latency: linear phase (block + half the filter), natural phase (the block) or lookahead; the latched set's, so a
    // held Swap never changes it
    e_lin = (int)emax(0, emin(7, std::floor(H.pv(P_LIN) + 0.5)));
    e_nat = e_lin >= 5;
    e_la = !e_lin && H.pv(P_LA) >= 0.5;
    e_lb = e_lin ? LBS[e_lin] : 0;
    e_ld = e_la ? (int)emax(1, emin(8000, std::floor(H.pv(P_LAMS) * 0.001 * srate + 0.5))) : 0;
    e_dl = e_lin ? (e_nat ? e_lb : 5 * e_lb) : e_ld;
    if (e_lin != e_linp) { e_linp = e_lin; lclear(); e_lrb = true; e_lrbt = 1; e_lxf = false; e_snap = true; }
    if (srate != e_csr || e_ksm_sr != srate) {
        e_ksm_sr = srate;
        e_ksm = 1 - std::exp(-E_TK / (0.006 * srate));
        e_kp = 1 - std::exp(-1 / (0.4 * srate));
        // the K weighting for the program auto gain: a 38 Hz high pass and a +4 dB shelf at 1.7 kHz
        coefBw(6, 38, 0, 0.5, 0, KWC, srate);
        coefBw(2, 1681, 4, 0.7071068, 0, KWC + 5, srate);
        e_snap = true;
    }
    // the bands: looked at again only when a setting changed (the window, the host, a held Swap, the rate) or a
    // glide ended; otherwise the lists from last time stand
    bool all = srate != e_csr || e_snap;
    e_csr = srate;
    e_snap = false;
    e_dirty = false;
    bool chg = !seenOk;
    for (int i = 43; i < 256; ++i) {
        double v = cpv(i);
        if (!seenOk || v != seen[i]) { seen[i] = v; chg = true; }
    }
    seenOk = true;
    int side = ab.side ? 1 : 0;
    if (all || chg || side != e_sside || e_rescan) {
        e_sside = side;
        e_rescan = false;
        scan(all);
    }
    // flush the filters' tiny leftovers (denormals), every 8th block (the JSFX's flush of the first sections' states
    // never fires: its `x != 0` is EEL2's fuzzy compare, true only from 0.00001 up; so they aren't flushed here either)
    if (++e_flc >= 8) {
        e_flc = 0;
        for (double &v : EZS) flush21(v);
        for (int i = 0; i < e_nd; ++i) {
            double *z = EDZ + EDL[i] * 2;
            flush21(z[0]);
            flush21(z[1]);
            if (EENV[EDL[i]] < 0.000000000000000000001) EENV[EDL[i]] = 0;
        }
        if (e_pi < 0.000000000000000000001) e_pi = 0;
        if (e_po < 0.000000000000000000001) e_po = 0;
        for (double &v : KWZ) flush21(v);
    }
    tgBlock();
    // auto gain (curve): the average power of the static curve (equal weight per octave)
    if (e_dirty) {
        for (int i = 0; i < e_na; ++i) {
            int bb = EACT[i] * 12;
            int n2 = coefBw((int)std::floor(ECP[bb + 1] + 0.5), ECP[bb + 2], ECP[bb + 3], ECP[bb + 4], pkQm(ECP[bb + 7]), ETMP + i * ECS, srate);
            ETMP[NB * ECS + (i & 15)] = n2;
        }
        double ap = 0;
        for (int k = 0; k < 64; ++k) {
            double pw = 1;
            for (int i = 0; i < e_na; ++i) {
                int b = EACT[i], n2 = (int)ETMP[NB * ECS + (i & 15)];
                if (n2 > 0) {
                    double m2 = flt::mag2(ETMP + i * ECS, n2, AGCW[k], AGC2[k]);
                    pw *= EPL[b] == 0 ? m2 : std::sqrt(m2);
                }
            }
            ap += pw;
        }
        e_agdbs = emax(-24, emin(24, -10 * std::log10(emax(0.000000001, ap / 64))));
        e_lrb = true;
    }
    // auto gain (program): how much louder (K-weighted) the filters make what is playing, measured
    e_agp = e_agon && e_agm && !e_del;
    if (e_agp) {
        if (e_pi > 0.00000001 && e_po > 0.000000000001) {
            double t2 = emax(-24, emin(24, -10 * std::log10(e_po / e_pi)));
            if (!e_agmv) { e_agdbp = t2; e_agmv = true; }
            else e_agdbp += (t2 - e_agdbp) * (1 - std::exp(-samplesblock / (1.0 * srate)));
        }
    } else {
        e_agmv = false;
    }
    e_agdb = (e_agp && e_agmv) ? e_agdbp : e_agdbs;
    e_agt = e_agon ? epow(10, e_agdb / 20) : 1;
    e_agk = 1 - std::exp(-1 / (0.03 * srate));
    // linear and natural phase: build the filter when something changed (not more often than every 40 ms)
    e_lrbt += samplesblock / srate;
    if (e_lin && e_lrb && e_lrbt >= 0.04) { firBuild(); e_lrb = false; e_lrbt = 0; }
    if (!e_lin) e_lrb = false;
    // solo: the soloed band's own filter (2 sections for steeper skirts)
    e_sol = (ab.uiok && uisolo >= 0 && uisolo < NB && ECP[uisolo * 12] >= 0.5) ? uisolo : -1;
    if (e_sol >= 0) {
        int bb = e_sol * 12, ty = (int)std::floor(ECP[bb + 1] + 0.5);
        edet(ty >= 5 ? (cutHp(ty) ? 1 : 2) : (ty == 1 ? 1 : (ty == 2 ? 2 : 0)), ECP[bb + 2],
             (ty == 1 || ty == 2 || ty >= 5) ? 0.7071068 : emax(0.4, emin(12, ECP[bb + 4])), ESO, srate);
        memcpy(ESO + 5, ESO, 5 * sizeof(double));
        if (e_sol != e_sold) { e_sold = e_sol; memset(ESZ, 0, sizeof ESZ); }
    }
    anw = ab.uion && (H.pv(P_AN) >= 0.5 || (H.pv(P_SCV) >= 0.5 && V.scok.load(std::memory_order_relaxed)));
    // meters for the window: input peak (after Input)
    double dur = samplesblock / srate;
    c_mi0 = emax(c_ip0 > 0.0000000001 ? 20 * std::log10(c_ip0) : -200, c_mi0 - 20 * dur);
    c_mi1 = emax(c_ip1 > 0.0000000001 ? 20 * std::log10(c_ip1) : -200, c_mi1 - 20 * dur);
    c_ip0 = 0; c_ip1 = 0;
    V.mi0.store(c_mi0, std::memory_order_relaxed);
    V.mi1.store(c_mi1, std::memory_order_relaxed);
    if (ab.uion) {
        for (int b = 0; b < NB; ++b) V.edg[b].store((float)EDG[b], std::memory_order_relaxed);
        V.agdb.store(e_agdb, std::memory_order_relaxed);
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
        double x0 = r0 * e_ing, x1 = r1 * e_ing;
        // the dynamic bands' detectors (they hear the input without the delay)
        if (e_nd > 0) {
            DSG[0] = (x0 + x1) * 0.5; DSG[1] = x0; DSG[2] = x1; DSG[3] = (x0 - x1) * 0.5;
            double e2 = s2 * e_sct, e3 = s3 * e_sct;
            DSG[4] = (e2 + e3) * 0.5; DSG[5] = e2; DSG[6] = e3; DSG[7] = (e2 - e3) * 0.5;
            for (int j = 0; j < e_nd; ++j) {
                int b = EDL[j];
                const double *o = EDC + b * 5;
                double *z = EDZ + b * 2;
                double u = DSG[EDS[b]];
                double y = o[0] * u + z[0];
                z[0] = o[1] * u - o[3] * y + z[1];
                z[1] = o[2] * u - o[4] * y;
                if (EDDT[b]) EENV[b] += EKD[b] * (y * y - EENV[b]);
                else {
                    y = std::fabs(y);
                    if (y > EENV[b]) EENV[b] = y;
                    else EENV[b] += EKD[b] * (y - EENV[b]);
                }
            }
        }
        // Set dynamic thresholds: waiting for signal, then each dynamic band's detector level per millisecond
        if (A.tgs == 1) {
            if (emax(std::fabs(x0), std::fabs(x1)) >= 0.00316 || emax(std::fabs(s2), std::fabs(s3)) * e_sct >= 0.00316) e_tgsig = true;
        }
        if (A.tgs == 2) {
            for (int j = 0; j < e_nd; ++j) {
                int b = EDL[j];
                if (EENV[b] > TGMX[b]) TGMX[b] = EENV[b];
                if (EENV[b] < TGMN[b]) TGMN[b] = EENV[b];
            }
            if (++e_tc2 >= e_tms) {
                if (e_tgn < 30000) {
                    for (int j = 0; j < e_nd; ++j) {
                        int b = EDL[j];
                        double x = EDMD[b] == 2 ? TGMN[b] : TGMX[b];
                        TGLV[b * 30000 + e_tgn] = EDDT[b] ? 10 * std::log10(x + 0.00000000000000000001) + 3.0103 : 20 * std::log10(x + 0.0000000001);
                        TGMX[b] = 0;
                        TGMN[b] = 1000000;
                    }
                    e_tgn += 1;
                }
                e_tc2 = 0;
            }
        }
        // the bands that move (gliding settings, dynamics) update their filters every E_TK samples
        if (e_nm > 0) {
            if (++e_tc >= E_TK) { e_tc = 0; tick(); }
        } else {
            e_tc = 0;
        }
        // the audio waits (lookahead, or the linear / natural phase filter's delay)
        double d0, d1, q0, q1;
        if (e_dl > 0) {
            DLA[e_dp] = x0; DLB[e_dp] = x1; DRA[e_dp] = r0; DRB[e_dp] = r1;
            int64_t ix = (e_dp - e_dl) & 32767;
            d0 = DLA[ix]; d1 = DLB[ix]; q0 = DRA[ix]; q1 = DRB[ix];
            e_dp = (e_dp + 1) & 32767;
        } else {
            d0 = x0; d1 = x1; q0 = r0; q1 = r1;
        }
        e_ag += (e_agt - e_ag) * e_agk;
        double o0, o1;
        if (e_sol >= 0) {
            o0 = filt(d0, ESO, 2, ESZ);
            o1 = filt(d1, ESO, 2, ESZ + 2);
        } else {
            double l, r;
            if (e_lin) {
                double u, v;
                if (e_lmode == 1) { u = (x0 + x1) * 0.5; v = (x0 - x1) * 0.5; }
                else { u = x0; v = x1; }
                LIB[e_lb + e_lpos] = u;
                LIB[4096 + e_lb + e_lpos] = v;
                l = LOB[e_lpos];
                r = LOB[2048 + e_lpos];
                if (e_lxf) {
                    // crossfade from the old filter to the new one over the block
                    double t = 0.5 - 0.5 * std::cos(PI * ((double)e_lpos / e_lb));
                    l = LOB2[e_lpos] + (l - LOB2[e_lpos]) * t;
                    r = LOB2[2048 + e_lpos] + (r - LOB2[2048 + e_lpos]) * t;
                }
                if (e_lmode == 1) { u = l; l = u + r; r = u - r; }
                e_lpos += 1;
                if (e_lpos >= e_lb) { linRun(); e_lpos = 0; }
            } else {
                l = d0;
                r = d1;
                // no band in use: nothing to filter
                if (e_anyf) {
                    for (int b = 0; b < NB; ++b) {
                        if (LN[b] <= 0) continue;
                        if (LP[b] == 0) {
                            const double *g = G[b];
                            double y = g[0] * l + ZL[b][0];
                            ZL[b][0] = g[1] * l - g[3] * y + ZL[b][1];
                            ZL[b][1] = g[2] * l - g[4] * y;
                            l = y;
                            y = g[0] * r + ZR[b][0];
                            ZR[b][0] = g[1] * r - g[3] * y + ZR[b][1];
                            ZR[b][1] = g[2] * r - g[4] * y;
                            r = y;
                            if (LN[b] > 1) xsec(b, l, r);
                        } else {
                            slow(b, l, r);
                        }
                    }
                }
            }
            // program auto gain: K-weighted power of what goes in and of what the filters make of it
            if (e_agp) {
                double p1 = filt(d0, KWC, 2, KWZ), p2 = filt(d1, KWC, 2, KWZ + 8);
                double p3 = filt(l, KWC, 2, KWZ + 16), p4 = filt(r, KWC, 2, KWZ + 24);
                e_pi += e_kp * (p1 * p1 + p2 * p2 - e_pi);
                e_po += e_kp * (p3 * p3 + p4 * p4 - e_po);
            }
            double y0 = l * e_ag, y1 = r * e_ag;
            if (e_del) { o0 = y0 - d0; o1 = y1 - d1; }
            else { o0 = d0 * e_dm + y0 * e_wm; o1 = d1 * e_dm + y1 * e_wm; }
        }
        out[0][i] = o0;
        out[1][i] = o1;
        br0[i] = r0; br1[i] = r1; bq0[i] = q0; bq1[i] = q1;
        if (ab.uion) {
            double t = std::fabs(x0);
            if (t > c_ip0) c_ip0 = t;
            t = std::fabs(x1);
            if (t > c_ip1) c_ip1 = t;
        }
    }
}

void Engine::post(const double *const *sc, double *const *out, int n)
{
    for (int i = 0; i < n; ++i) {
        ab.post(br0[i], br1[i], out[0][i], out[1][i], bq0[i], bq1[i]);
        if (anw) V.an.put((br0[i] + br1[i]) * 0.5, (out[0][i] + out[1][i]) * 0.5, sc ? (sc[0][i] + sc[1][i]) * 0.5 * e_sct : 0);
    }
    if (anw) V.an.publish();
}

} // namespace peq
} // namespace ts
