#include "comp_engine.h"
#include "../../core/eel.h"
#include <cmath>
#include <cstring>

namespace ts {
namespace comp {

Display::Display()
{
    for (int i = 0; i < HN; ++i) { hin[i].store(-200); hout[i].store(-200); hgr[i].store(0); }
    for (int i = 0; i < AN; ++i) { ani[i].store(0); ano[i].store(0); }
}

// RBJ biquad, high-pass (hp = 1) or low-pass
void Biquad::set(double f, double q, int hp, double srate)
{
    double w = 2 * PI * emin(f, srate * 0.49) / srate, cw = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
    if (hp) { b0 = (1 + cw) * 0.5 / a0; b1 = -(1 + cw) / a0; }
    else { b0 = (1 - cw) * 0.5 / a0; b1 = (1 - cw) / a0; }
    b2 = b0;
    a1 = -2 * cw / a0;
    a2 = (1 - al) / a0;
}

void Biquad::flushAB() { flush21(z1a); flush21(z1b); flush21(z2a); flush21(z2b); }

Chan::Chan()
{
    tgt.assign(8192, 0.0);
    mn.assign(8192, 0.0);
    md.assign(8192, 0.0);
    q.assign(8192, 0);
}

void Chan::la_reset()
{
    std::fill(tgt.begin(), tgt.end(), 0.0);
    std::fill(q.begin(), q.end(), 0);
    std::fill(mn.begin(), mn.end(), 0.0);
    std::fill(md.begin(), md.end(), 0.0);
    n = 0; dqh = 0; dqt = 0; sum = 0;
}

// ln cosh(u) without overflow, and tanh
static inline double e_lcosh(double u) { u = std::fabs(u); return u + std::log(1 + std::exp(-2 * u)) - 0.6931471805599453; }
static inline double e_tanh(double u) { return u > 20 ? 1 : (u < -20 ? -1 : 1 - 2 / (std::exp(2 * u) + 1)); }

// static gain reduction (dB, 0 or less) at o dB over the threshold.
// cv 0: standard (quadratic knee); 1: soft, the ratio grows in over the first 12 dB (vari-mu like);
// 2: progressive, the ratio grows in over the first 18 dB (opto like)
double e_curve(double o, double kn, double sl, int cv)
{
    if (cv < 0.5) {
        return (o + o < -kn) ? 0 : ((o + o > kn) ? sl * o : sl * sqr(o + kn * 0.5) / (2 * kn));
    }
    double x = o + kn * 0.5;
    return x <= 0 ? 0 : (cv < 1.5 ? sl * (x - 12 * (1 - std::exp(-x / 12))) : (x < 18 ? sl * x * x / 36 : sl * (x - 9)));
}

// Range (nrg = the most gain reduction, negative; -200 = no limit): a soft corner instead of a hard clamp
double rs_soft(double g, double nrg)
{
    if (nrg < -199) return g;
    if (nrg > -0.01) return 0;
    double r0 = nrg * 0.7;
    if (g >= r0) return g;
    double w = nrg - r0;
    return r0 + w * (1 - std::exp(-(g - r0) / w));
}

Engine::Engine(EngineHost &h, Act &act, UiLink &ui, Display &view) : H(h), A(act), U(ui), V(view)
{
    TGB.assign(60000, 0.0);
    LAB0.assign(8192, 0.0);
    LAB1.assign(8192, 0.0);
    memset(TGH, 0, sizeof TGH);
}

void Engine::init(double sr)
{
    srate = sr;
}

void Engine::reset()
{
    // clear what holds audio (delay lines, filters, followers); settings are worked out again
    std::fill(LAB0.begin(), LAB0.end(), 0.0);
    std::fill(LAB1.begin(), LAB1.end(), 0.0);
    for (int r = 0; r < 4; ++r) { c_rh1[r].zeroAB(); c_rh2[r].zeroAB(); c_rl1[r].zeroAB(); c_rl2[r].zeroAB(); }
    Biquad *hs[4] = {&e_h0a, &e_h1a, &e_h0b, &e_h1b};
    for (Biquad *b : hs) { b->z1 = 0; b->z2 = 0; }
    e_lp0 = e_lp1 = e_tl0 = e_tl1 = 0;
    Chan *cs[2] = {&e_c0, &e_c1};
    for (Chan *c : cs) {
        c->d1 = c->d2 = c->d3 = c->pe = c->ms = c->lv = c->lp = c->lr = c->lin = 0;
        c->y = c->yf = c->ys = c->sa = c->k = 0;
        c->hold = 0;
    }
    e_s0 = Sat();
    e_s1 = Sat();
    e_lla = -1;   // the lookahead rings start over
    c_xf = 1;
    e_ldA = e_ldB = 0;
    e_first = true;
    c_bpv = 0;
    c_tr = 1;
}

int Engine::latencyFor(double la, double lams, double sr)
{
    return la >= 0.5 ? (int)emax(1, emin(8000, std::floor(lams * 0.001 * sr + 0.5))) : 0;
}

// ---- one detector channel: peak envelope (with intersample peaks), RMS, or both; the level in dB ----
double Engine::ch_det(Chan &c, double d, double v)
{
    double p = v;
    if (c_nr == 0) { double m = std::fabs((9 * (c.d1 + c.d2) - c.d3 - d) * 0.0625); if (m > p) p = m; }
    c.d3 = c.d2; c.d2 = c.d1; c.d1 = d;
    c.pe = e_envs ? (p > c.pe ? p : c.pe * e_dr) : p;
    if (c.pe < 0.0000000001) c.pe = 0;
    double r = 0;
    if (e_detm > 0) {
        c.ms += e_rk * (v * v - c.ms);
        if (c.ms < 0.00000000000000000001) c.ms = 0;
        r = std::sqrt(c.ms) * 1.4142135623730951;   // a sine reads the same as in Peak
    }
    c.lp = std::log(emax(c.pe, 0.000000001)) * 8.685889638065035;
    if (e_detm == 0) c.lv = c.lp;
    else {
        c.lr = std::log(emax(r, 0.000000001)) * 8.685889638065035;
        c.lv = e_detm == 1 ? c.lr : c.lp + e_blend * (c.lr - c.lp);
    }
    return c.lv;
}

// ---- the same detector, linear (no log) ----
double Engine::ch_lin(Chan &c, double d, double v)
{
    double p = v;
    if (c_nr == 0) { double m = std::fabs((9 * (c.d1 + c.d2) - c.d3 - d) * 0.0625); if (m > p) p = m; }
    c.d3 = c.d2; c.d2 = c.d1; c.d1 = d;
    c.pe = e_envs ? (p > c.pe ? p : c.pe * e_dr) : p;
    if (c.pe < 0.0000000001) c.pe = 0;
    if (e_detm > 0) {
        c.ms += e_rk * (v * v - c.ms);
        if (c.ms < 0.00000000000000000001) c.ms = 0;
        c.lin = std::sqrt(c.ms) * 1.4142135623730951;
    } else {
        c.lin = c.pe;
    }
    if (c.lin < 0.000000001) c.lin = 0.000000001;
    return c.lin;
}

// ---- lookahead: the gain lands exactly on the peak ----
// The minimum of the targets over the next e_laA samples, delayed so that a ramp of e_laA samples
// (the attack) ends when the peak leaves the delay line (e_ld samples).
double Engine::la_proc(Chan &c, double t)
{
    c.tgt[c.n & 8191] = t;
    while (c.dqt > c.dqh && c.tgt[c.q[(c.dqt - 1) & 8191] & 8191] >= t) c.dqt -= 1;
    c.q[c.dqt & 8191] = c.n;
    c.dqt += 1;
    while (c.q[c.dqh & 8191] < c.n - e_laA) c.dqh += 1;
    double m = c.tgt[c.q[c.dqh & 8191] & 8191];
    c.mn[c.n & 8191] = m;
    double m2 = c.mn[(c.n - (e_ld - e_laA)) & 8191];
    c.sum += m2 - c.md[(c.n - e_laA - 1) & 8191];
    c.md[c.n & 8191] = m2;
    c.n += 1;
    return c.sum / (e_laA + 1);
}

// ---- one gain channel: gain computer, lookahead, attack / hold / release ----
// Dual-stage: a fast stage (quick release) and a slow stage that only builds up under sustained
// compression; the deeper of the two applies.
double Engine::ch_gain(Chan &c, double lvl)
{
    double g = lvl - c_thr < -0.5 * c_kn ? 0 : rs_soft(e_curve(lvl - c_thr, c_kn, e_sl, e_cv), e_nrg);
    if (e_la) g = la_proc(c, g);
    c.sa += (g < c.sa ? e_ska : e_skr) * (g - c.sa);
    if (g < 0 && g <= c.y) {
        c.y = g + e_aa * (c.y - g);
        if (e_relm == 2) { c.yf = g + e_aa * (c.yf - g); c.ys = g + e_asa * (c.ys - g); c.y = emin(c.yf, c.ys); }
        c.hold = e_hs;
    } else if (c.y < 0) {
        if (c.hold > 0) c.hold -= 1;
        else {
            if (e_relm == 0) c.y = g + e_ar * (c.y - g);
            else if (e_relm == 1) {
                double m = 0.3 + 1.4 * emin(1, -c.sa * 0.125);
                c.y = g + std::exp(-e_rrel / m) * (c.y - g);
            } else {
                c.yf = g + e_arf * (c.yf - g);
                c.ys = g + e_ars * (c.ys - g);
                c.y = emin(c.yf, c.ys);
            }
            if (c.y > -0.0001) { c.y = 0; c.yf = 0; c.ys = 0; }
        }
    }
    if (e_relm != 2) { c.yf = c.y; c.ys = c.y; }
    c.k = c.y < -0.00001 ? std::exp(c.y * 0.11512925464970229) : 1;
    if (c_amk && lvl > -60) { c_ys += c.y; c_yn += 1; }
    return c.y;
}

// ---- saturation: tanh (Soft) or a biased tanh (Warm: even harmonics, DC removed); first-order
// antiderivative anti-aliasing instead of oversampling, so it adds no latency ----
double Engine::sat_run(Sat &s, double x)
{
    double F = e_lcosh(e_sd * (x + e_sb)) * e_sdi2 - e_sc * x;
    if (!s.ini) { s.xp = x; s.fp = F; s.ini = true; }
    double dx = x - s.xp;
    double y = std::fabs(dx) > 0.00001 ? (F - s.fp) / dx : e_tanh(e_sd * ((x + s.xp) * 0.5 + e_sb)) / e_sd - e_sc;
    s.fp = F;
    s.xp = x;
    y /= e_sg;
    if (e_satt) { s.dcs += e_dck * (y - s.dcs); y -= s.dcs; }
    return y;
}

// "Set threshold": one pass of the simulated compressor over the recorded levels, at threshold c_st
void Engine::c_sim_begin()
{
    c_sy = 0; c_sh = 0; c_ssum = 0; c_scnt = 0; c_spos = 0;
    memset(TGH, 0, sizeof TGH);
}

void Engine::c_sim_run(int n)
{
    int cnt = std::min(n, c_tgn - c_spos);
    for (int i = 0; i < cnt; ++i) {
        double lv = TGB[c_spos];
        double o = lv - c_st;
        double g = rs_soft(e_curve(o, c_kn, c_sl, e_cv), c_nrg);
        if (g < 0 && g <= c_sy) { c_sy = g + c_saa * (c_sy - g); c_sh = c_shs; }
        else if (c_sh > 0) c_sh -= 1;
        else c_sy = g + c_sar * (c_sy - g);
        if (lv > -60) {
            c_ssum -= c_sy;
            c_scnt += 1;
            int b = (int)emin(600, std::floor(-c_sy * 10));
            TGH[b] += 1;
        }
        c_spos += 1;
    }
}

// the gain reduction that pass gave: average, or the level 95% of the playing time stays under
double Engine::c_sim_result()
{
    if (c_scnt <= 0) return -1;
    if (c_ttype) return c_ssum / c_scnt;
    double acc = 0, lim = 0.95 * c_scnt;
    bool f = false;
    int i = 0;
    while (i <= 600 && !f) { acc += TGH[i]; if (acc >= lim) f = true; else i += 1; }
    return i * 0.1;
}

// ====================================================================== @block
void Engine::block(int samplesblock)
{
    const double a_now = A.now();
    // the window's hold buttons (1 = Bypass, 2 = Swap), while its heartbeat is fresh
    c_uiok = a_now - U.beat.load(std::memory_order_relaxed) < 0.5;
    int uihold = U.hold.load(std::memory_order_relaxed);
    c_bph = c_uiok && uihold == 1;
    c_swh = c_uiok && uihold == 2;
    c_lat = H.pv(P_AB) >= 0.5;
    c_side = c_swh ? !c_lat : c_lat;

    // settings for this block (from the other set while Swap is held)
    c_thr = c_pv(P_THR);
    c_rat = c_pv(P_RAT);
    c_kn = emax(0.0001, c_pv(P_KNEE));
    c_sl = c_rat >= 99.5 ? -1 : 1 / emax(1, c_rat) - 1;
    c_mode = iclamp((int)std::floor(c_pv(P_CMODE) + 0.5), 0, 3);
    c_two = c_mode != 0;
    c_lk = c_pv(P_SLINK) * 0.01;
    double a_t = c_pv(P_IN);
    if (a_t != c_lin) { c_lin = a_t; c_ing = epow(10, a_t / 20); }
    c_mix = c_pv(P_MIX) * 0.01;
    c_ext = c_pv(P_SC) >= 0.5;
    c_lis = H.pv(P_LISTEN) >= 0.5;
    c_del = H.pv(P_DELTA) >= 0.5;
    // lookahead: the latched set's (a held Swap keeps the delay, so the latency never jumps)
    c_la = H.pv(P_LA) >= 0.5;
    c_ld = c_la ? (int)emax(1, emin(8000, std::floor(H.pv(P_LAMS) * 0.001 * srate + 0.5))) : 0;
    c_latms = c_ld / srate * 1000;
    c_nr = 0;
    for (int r = 0; r < 4; ++r) {
        int pb = P_R1 + 3 * r;
        c_ron[r] = c_pv(pb) >= 0.5;
        c_nr += c_ron[r];
        if (c_ron[r]) {
            double a_f1 = c_pv(pb + 1), a_f2 = c_pv(pb + 2);
            if (a_f1 > a_f2) { double t = a_f1; a_f1 = a_f2; a_f2 = t; }
            c_rhp[r] = a_f1 > 20.5;
            c_rlp[r] = a_f2 < emin(19500, srate * 0.45);
            if (a_f1 != c_rfl[r] || a_f2 != c_rfh[r] || srate != c_rsr[r]) {
                c_rh1[r].set(a_f1, 0.5411961, 1, srate);
                c_rh2[r].set(a_f1, 1.306563, 1, srate);
                c_rl1[r].set(a_f2, 0.5411961, 0, srate);
                c_rl2[r].set(a_f2, 1.306563, 0, srate);
                c_rfl[r] = a_f1; c_rfh[r] = a_f2; c_rsr[r] = srate;
            }
            c_rh1[r].flushAB(); c_rh2[r].flushAB(); c_rl1[r].flushAB(); c_rl2[r].flushAB();
        } else {
            c_rh1[r].zeroAB(); c_rh2[r].zeroAB(); c_rl1[r].zeroAB(); c_rl2[r].zeroAB();
        }
    }
    flush21(e_c0.ms);
    flush21(e_c1.ms);
    // ---------- engine settings; worked out once per block ----------
    c_two = true;
    e_chr = iclamp((int)std::floor(c_pv(P_CHAR) + 0.5), 0, 4);
    e_cv = iclamp((int)std::floor(c_pv(P_CURVE) + 0.5), 0, 2);
    e_relm = iclamp((int)std::floor(c_pv(P_RELM) + 0.5), 0, 2);
    e_fbk = c_pv(P_FB) * 0.01;
    e_sat = c_pv(P_SAT) * 0.01;
    e_satt = c_pv(P_SATT) >= 0.5;
    // a Character decides curve, release, feedback and saturation (voicings, not emulations)
    if (e_chr == 1) { e_cv = 0; e_relm = 1; e_fbk = 0; e_sat = 0; }
    if (e_chr == 2) { e_cv = 2; e_relm = 2; e_fbk = 0.5; e_sat = 0.15; e_satt = false; }
    if (e_chr == 3) { e_cv = 0; e_relm = 1; e_fbk = 0.6; e_sat = 0.35; e_satt = false; }
    if (e_chr == 4) { e_cv = 1; e_relm = 1; e_fbk = 0.4; e_sat = 0.3; e_satt = true; }
    if (c_la) e_fbk = 0;   // a feedback detector can't look ahead
    e_la = c_la;
    e_ld = c_ld;
    e_envs = e_chr > 0 ? true : c_pv(P_ENV) >= 0.5;   // only Custom may use the raw envelope
    e_detm = iclamp((int)std::floor(c_pv(P_DET) + 0.5), 0, 2);
    e_blend = c_pv(P_BLEND) * 0.01;
    e_rmst = emax(0.01, c_pv(P_RMST));
    e_rk = 1 - std::exp(-1 / (e_rmst * 0.001 * srate));
    // the labelled attack and release are what you get: the RMS window and the envelope's own
    // release are taken off them
    double a_att = c_pv(P_ATT), a_rel = c_pv(P_REL);
    e_tw = e_detm == 1 ? e_rmst : (e_detm == 2 ? e_rmst * e_blend : 0);
    if (e_tw > 0) {
        a_att = std::sqrt(emax(0.0001, a_att * a_att - e_tw * e_tw));
        a_rel = std::sqrt(emax(1, a_rel * a_rel - e_tw * e_tw));
    }
    e_td = e_envs ? emax(1, emin(40, 0.4 * c_pv(P_REL))) : 0;
    e_dr = e_envs ? std::exp(-1 / (e_td * 0.001 * srate)) : 0;
    e_relc = emax(1, e_envs ? emax(0.5 * a_rel, a_rel - e_td) : a_rel);
    e_aa = std::exp(-1 / (emax(e_envs ? 0.05 : 0.01, a_att) * 0.001 * srate));
    e_aau = e_aa;
    if (c_la) e_aa = 0;   // with lookahead the ramp is the attack
    e_ar = std::exp(-1 / (e_relc * 0.001 * srate));
    e_rrel = 1 / (e_relc * 0.001 * srate);
    e_arf = std::exp(-1 / (emax(1, e_relc * 0.25) * 0.001 * srate));
    e_ars = std::exp(-1 / (e_relc * 2 * 0.001 * srate));
    e_asa = std::exp(-1 / (emax(1, e_relc * 0.5) * 0.001 * srate));
    e_ska = 1 - std::exp(-1 / (0.3 * srate));
    e_skr = 1 - std::exp(-1 / (1 * srate));
    e_hs = std::floor(c_pv(P_HLD) * 0.001 * srate);
    // feedback: the detector sees the level after the gain; the slope is raised so the static curve
    // stays the one shown
    e_sl = c_sl;
    if (e_fbk > 0) {
        e_sl = c_sl / (1 + e_fbk * c_sl);
        a_t = -0.9 * (1 + e_aau) / (1 - e_aau) / e_fbk;   // keeps the loop stable at very fast attacks
        if (e_sl < a_t) e_sl = a_t;
    }
    e_nrg = c_pv(P_RNG) >= 59.95 ? -200 : -c_pv(P_RNG);
    c_nrg = e_nrg;
    e_mono = c_mode == 0;
    e_lk = e_mono ? 1 : c_lk;
    e_lkl = c_pv(P_LKL) >= 0.5;
    e_rcb = c_pv(P_RCMB) >= 0.5;
    // sidechain filters: high-pass (6, 12 or 24 dB/octave), tilt, trim (external sidechain only)
    e_hpf = c_pv(P_SCHP);
    e_hpon = e_hpf > 20.5;
    e_hps = iclamp((int)std::floor(c_pv(P_SCHS) + 0.5), 0, 2);
    e_tilt = c_pv(P_SCTL);
    e_tlon = std::fabs(e_tilt) > 0.05;
    e_sct = epow(10, c_pv(P_SCT) / 20);
    if (e_hpf != e_lhpf || e_hps != e_lhps || srate != e_lhsr) {
        e_lhpf = e_hpf; e_lhps = e_hps; e_lhsr = srate;
        e_hpg = 1 - std::exp(-2 * PI * e_hpf / srate);
        double a_q1 = e_hps == 2 ? 0.5411961 : 0.7071068, a_q2 = 1.306563;
        e_h0a.set(e_hpf, a_q1, 1, srate);
        e_h1a.set(e_hpf, a_q1, 1, srate);
        e_h0b.set(e_hpf, a_q2, 1, srate);
        e_h1b.set(e_hpf, a_q2, 1, srate);
    }
    if (!e_hpon) {
        e_lp0 = 0; e_lp1 = 0;
        e_h0a.z1 = 0; e_h0a.z2 = 0; e_h1a.z1 = 0; e_h1a.z2 = 0;
        e_h0b.z1 = 0; e_h0b.z2 = 0; e_h1b.z1 = 0; e_h1b.z2 = 0;
    }
    e_tg = 1 - std::exp(-2 * PI * 1000 / srate);
    e_tgl = epow(10, e_tilt / 40);
    e_tgh = epow(10, -e_tilt / 40);
    if (!e_tlon) { e_tl0 = 0; e_tl1 = 0; }
    // saturation
    e_sd = 1 + 5 * e_sat;
    e_sdi2 = 1 / (e_sd * e_sd);
    e_sb = e_satt ? 0.2 / e_sd : 0;
    e_sc = e_tanh(e_sd * e_sb) / e_sd;
    e_sg = 1 - sqr(e_tanh(e_sd * e_sb));
    e_dck = 1 - std::exp(-2 * PI * 8 / srate);
    if (e_sat <= 0.001) { e_s0.ini = false; e_s1.ini = false; e_s0.dcs = 0; e_s1.dcs = 0; }
    // Fix (not in the JSFX 1.0): the anti-aliasing divides the change of the curve's integral by the
    // change of the input. When drive or type jumps between blocks, the integral of the last sample
    // was taken on the old curve, and the next sample could spike (up to +37 dB in tests). Taking it
    // again on the new curve removes the spike and changes nothing else.
    if (satFix && (e_sd != e_lsd || e_sb != e_lsb)) {
        Sat *ss[2] = {&e_s0, &e_s1};
        for (Sat *s : ss)
            if (s->ini) s->fp = e_lcosh(e_sd * (s->xp + e_sb)) * e_sdi2 - e_sc * s->xp;
    }
    e_lsd = e_sd;
    e_lsb = e_sb;
    // lookahead: the ramp (the attack) can't be longer than the lookahead
    e_laA = c_la ? (int)emax(0, emin(c_ld, std::floor(a_att * 0.001 * srate + 0.5))) : 0;
    if (ne(c_la, e_lla) || ne(c_ld, e_lld) || ne(e_laA, e_llaA) || ne(e_mono, e_lmono)) {
        e_lla = c_la; e_lld = c_ld; e_llaA = e_laA; e_lmono = e_mono;
        e_c0.la_reset();
        e_c1.la_reset();
    }
    // a change of the lookahead time crosses over from the old delay to the new one (no click)
    if (ne(c_ld, e_ldA)) { e_ldB = e_ldA; e_ldA = c_ld; c_xf = 0; }
    e_xfk = 1 / emax(1, 0.01 * srate);
    e_amk = c_pv(P_AMK) >= 0.5;
    e_amm = c_pv(P_AMKM) >= 0.5;
    // one gain computer for both channels: Mono, or Stereo at 100% link without lookahead, with the
    // Peak or RMS detector. A link that was below 100% hands over once both channels' gains meet.
    e_pwr = e_mono || e_lkl;
    e_one = e_detm < 2 && (e_mono || (e_lk >= 1 && !e_la));
    if (e_one && !e_mono &&
        !(e_c0.y == e_c1.y && e_c0.yf == e_c1.yf && e_c0.ys == e_c1.ys && e_c0.sa == e_c1.sa && e_c0.hold == e_c1.hold)) {
        if (std::fabs(e_c0.y - e_c1.y) < 0.0000001 && std::fabs(e_c0.yf - e_c1.yf) < 0.0000001 &&
            std::fabs(e_c0.ys - e_c1.ys) < 0.0000001 && std::fabs(e_c0.sa - e_c1.sa) < 0.0000001 &&
            eq(e_c0.hold, e_c1.hold)) {
            e_c1.y = e_c0.y; e_c1.yf = e_c0.yf; e_c1.ys = e_c0.ys; e_c1.sa = e_c0.sa; e_c1.k = e_c0.k;
        } else {
            e_one = false;
        }
    }
    // keep the filter and follower states out of the denormal range
    flush21(e_tl0); flush21(e_tl1); flush21(e_lp0); flush21(e_lp1);
    flush21(e_h0a.z1); flush21(e_h0a.z2); flush21(e_h1a.z1); flush21(e_h1a.z2);
    flush21(e_h0b.z1); flush21(e_h0b.z2); flush21(e_h1b.z1); flush21(e_h1b.z2);
    flush21(e_c0.sa); flush21(e_c1.sa);
    flush21(e_s0.dcs); flush21(e_s1.dcs);

    // auto makeup. Average GR: makes up the average gain reduction while there's signal (about 2 s).
    // Static: makes up half of what a 0 dBFS signal would be reduced by. Either way scaled by Dry/wet.
    c_amk = e_amk && !e_amm;
    if (samplesblock != c_lbs || srate != c_lbsr) {
        c_lbs = samplesblock;
        c_lbsr = srate;
        c_kmk = 1 - std::exp(-samplesblock / (2 * srate));
        c_klm = 1 - std::exp(-samplesblock / (1.5 * srate));
    }
    if (c_yn > 0) c_mkdb += c_kmk * (-c_ys / c_yn - c_mkdb);
    c_ys = 0;
    c_yn = 0;
    if (e_amk) e_mkt = e_amm ? -0.5 * rs_soft(e_curve(-c_thr, c_kn, c_sl, e_cv), e_nrg) : c_mkdb;
    else e_mkt = 0;
    c_dm = 1 - c_mix;
    c_wmt = c_mix * epow(10, e_mkt * c_mix / 20);
    c_zk = 1 - std::exp(-1 / (0.01 * srate));
    if (e_first) { c_ings = c_ing; c_dms = c_dm; c_wms = c_wmt; e_first = false; }
    // the window is open (meters, level matching, analyzer only run then)
    c_uion = a_now - U.last.load(std::memory_order_relaxed) < 0.5;
    c_vneed = c_uion || A.tgs == 1;

    // level matching (about 1.5 s), per set: how much louder the plug-in makes things
    // (not for 0.3 s after the playing set changes: the compressor is still settling)
    int side = c_side ? 1 : 0;
    if (side != c_lside) { c_lside = side; c_lmskip = 0.3 * srate; }
    if (c_lmskip > 0) { c_lmskip -= samplesblock; c_ei = 0; }
    if (c_ei > 0.0000001 * samplesblock) {
        LMI[side] += c_klm * (c_ei / samplesblock - LMI[side]);
        LMO[side] += c_klm * (c_eo / samplesblock - LMO[side]);
        LMV[side] = 1;
    }
    c_ei = 0;
    c_eo = 0;
    // held Bypass: the raw input at the playing set's loudness; held Swap: the other set at this one's
    c_bpg = (truth(LMV[side]) && LMI[side] > 0) ? std::sqrt(LMO[side] / LMI[side]) : 1;
    int lat = c_lat ? 1 : 0;
    c_trt = (c_swh && truth(LMV[0]) && truth(LMV[1]) && LMI[0] > 0 && LMI[1] > 0 && LMO[0] > 0 && LMO[1] > 0)
                ? std::sqrt((LMO[lat] / LMI[lat]) / (LMO[!lat] / LMI[!lat]))
                : 1;
    c_bps = 1 / (0.01 * srate);
    c_trk = 1 - std::exp(-1 / (0.01 * srate));

    // meters for the window: input peak (after Input), gain reduction, loudest level the detector saw
    double a_dur = samplesblock / srate;
    c_mi0 = emax(c_ip0 > 0.0000000001 ? 20 * std::log10(c_ip0) : -200, c_mi0 - 20 * a_dur);
    c_mi1 = emax(c_ip1 > 0.0000000001 ? 20 * std::log10(c_ip1) : -200, c_mi1 - 20 * a_dur);
    double c_mo0 = 0;
    if (A.myslot >= 0) {
        shm::Slot &b = shm::slot(A.myslot);
        c_mo0 = emax(b.bpk[0].load(std::memory_order_relaxed), b.bpk[1].load(std::memory_order_relaxed));
    }
    c_mo0 = c_mo0 > 0.0000000001 ? 20 * std::log10(c_mo0) : -200;
    a_t = emax(c_ip0, c_ip1);
    double c_ib = a_t > 0.0000000001 ? 20 * std::log10(a_t) : -200;   // this block's input peak
    c_ip0 = 0;
    c_ip1 = 0;
    c_ugr = emin(c_y0, c_y1);
    c_dlv = c_vdot > 0.0000000001 ? 20 * std::log10(c_vdot) : -200;
    c_vdot = 0;
    // history (only while the window is open): one entry per 10 ms
    if (c_uion) {
        c_hbi = emax(c_hbi, c_ib);
        c_hbo = emax(c_hbo, c_mo0);
        c_hbg = emin(c_hbg, c_ugr);
        c_ht += samplesblock;
        if (c_ht >= 0.01 * srate) {
            V.hin[c_hp].store((float)c_hbi, std::memory_order_relaxed);
            V.hout[c_hp].store((float)c_hbo, std::memory_order_relaxed);
            V.hgr[c_hp].store((float)c_hbg, std::memory_order_relaxed);
            c_hp = (c_hp + 1) & 1023;
            V.hp.store(c_hp, std::memory_order_release);
            c_hbi = -200; c_hbo = -200; c_hbg = 0;
            c_ht -= 0.01 * srate;
        }
    }
    c_anw = c_uion;

    // ---- "Set threshold": listen, then find the threshold that gives the target gain reduction ----
    int &c_tgs = A.tgs;
    if (c_tgs == 1) {
        // wait for signal
        if (c_dlv > -50) {
            c_tgs = 2; c_tgn = 0; c_tc = 0; c_tm = -200;
            c_tms = (int)emax(1, std::floor(srate * 0.001 + 0.5));
            int t = (int)std::floor(H.pv(P_TTIME) + 0.5);
            c_tgdur = 1000 * (t <= 0 ? 1 : (t == 1 ? 2 : (t == 2 ? 3 : (t == 3 ? 5 : (t == 4 ? 10 : (t == 5 ? 20 : 30))))));
        }
    } else if (c_tgs == 2) {
        if (c_tgn >= c_tgdur) {
            c_tgs = 3; c_tlo = -60; c_thi = 0; c_tit = 0; c_st = -30;
            c_ttype = H.pv(P_TTYPE) >= 0.5;
            c_tgt = H.pv(P_TGR);
            c_sim_begin();
        }
    } else if (c_tgs == 3) {
        // the simulation runs at 1 kHz on the recorded levels; one bisection step takes a few blocks
        c_saa = std::exp(-1 / emax(0.01, H.pv(P_ATT)));
        c_sar = std::exp(-1 / emax(1, H.pv(P_REL)));
        c_shs = std::floor(H.pv(P_HLD));
        c_sim_run(4000);
        if (c_spos >= c_tgn) {
            double a_r = c_sim_result();
            if (a_r < 0) c_tgs = 0;
            else {
                if (a_r > c_tgt) c_tlo = c_st;
                else c_thi = c_st;
                c_tit += 1;
                if (c_tit >= 16) {
                    H.setFromAudio(P_THR, std::floor(((c_tlo + c_thi) * 0.5) * 10 + 0.5) / 10);
                    c_tgs = 0;
                } else {
                    c_st = (c_tlo + c_thi) * 0.5;
                    c_sim_begin();
                }
            }
        }
    }
    A.tgel = c_tgs == 2 ? c_tgn * 0.001 : 0;

    // for the window
    V.mi0.store(c_mi0, std::memory_order_relaxed);
    V.mi1.store(c_mi1, std::memory_order_relaxed);
    V.ugr.store(c_ugr, std::memory_order_relaxed);
    V.dlv.store(c_dlv, std::memory_order_relaxed);
    V.latms.store(c_latms, std::memory_order_relaxed);
    V.ld.store(c_ld, std::memory_order_relaxed);
    V.side.store(side, std::memory_order_relaxed);
    V.mkdb.store(c_mkdb, std::memory_order_relaxed);
}

// ====================================================================== @sample (compressor)
void Engine::samples(const double *const *in, const double *const *sc, double *const *out, int n)
{
    if ((int)bq0.size() < n) {
        bq0.resize(n); bq1.resize(n); br0.resize(n); br1.resize(n); bx0.resize(n); bx1.resize(n);
    }
    const int c_tgs = A.tgs;
    for (int i = 0; i < n; ++i) {
        // Input, dry and wet gains glide (no zipper noise when they move)
        c_ings += c_zk * (c_ing - c_ings);
        c_dms += c_zk * (c_dm - c_dms);
        c_wms += c_zk * (c_wmt - c_wms);
        // the audio always goes into the lookahead line, so switching lookahead on never plays old samples
        double c_r0 = in[0][i], c_r1 = in[1][i];
        LAB0[c_lp] = c_r0;
        LAB1[c_lp] = c_r1;
        double c_x0 = c_r0 * c_ings, c_x1 = c_r1 * c_ings;
        // the detector's signal: the input or the sidechain (with its trim), Mid/Side encoded in that mode
        double c_d0, c_d1;
        if (c_ext) {
            c_d0 = (sc ? sc[0][i] : 0) * e_sct;
            c_d1 = (sc ? sc[1][i] : 0) * e_sct;
        } else {
            c_d0 = c_x0;
            c_d1 = c_x1;
        }
        if (c_mode == 2) { double t = c_d0; c_d0 = (t + c_d1) * 0.5; c_d1 = (t - c_d1) * 0.5; }
        // sidechain filters (high-pass, then tilt)
        if (e_hpon) {
            if (e_hps == 0) {
                e_lp0 += e_hpg * (c_d0 - e_lp0); c_d0 -= e_lp0;
                e_lp1 += e_hpg * (c_d1 - e_lp1); c_d1 -= e_lp1;
            } else {
                c_d0 = e_h0a.one(c_d0);
                c_d1 = e_h1a.one(c_d1);
                if (e_hps == 2) { c_d0 = e_h0b.one(c_d0); c_d1 = e_h1b.one(c_d1); }
            }
        }
        if (e_tlon) {
            e_tl0 += e_tg * (c_d0 - e_tl0); c_d0 = e_tl0 * e_tgl + (c_d0 - e_tl0) * e_tgh;
            e_tl1 += e_tg * (c_d1 - e_tl1); c_d1 = e_tl1 * e_tgl + (c_d1 - e_tl1) * e_tgh;
        }
        // the detector: the trigger ranges (band-passed, 24 dB/octave; the loudest counts, or they
        // combine as a power sum), or the whole signal
        double c_v0, c_v1, c_s0, c_s1;
        if (c_nr > 0) {
            c_v0 = 0; c_v1 = 0; c_s0 = 0; c_s1 = 0;
            double c_q0 = 0, c_q1 = 0;
            for (int r = 0; r < 4; ++r) {
                if (!c_ron[r]) continue;
                double u = c_d0;
                if (c_rhp[r]) { u = c_rh1[r].runA(u); u = c_rh2[r].runA(u); }
                if (c_rlp[r]) { u = c_rl1[r].runA(u); u = c_rl2[r].runA(u); }
                c_s0 += u; c_q0 += u * u; u = std::fabs(u); if (u > c_v0) c_v0 = u;
                if (c_two) {
                    u = c_d1;
                    if (c_rhp[r]) { u = c_rh1[r].runB(u); u = c_rh2[r].runB(u); }
                    if (c_rlp[r]) { u = c_rl1[r].runB(u); u = c_rl2[r].runB(u); }
                    c_s1 += u; c_q1 += u * u; u = std::fabs(u); if (u > c_v1) c_v1 = u;
                }
            }
            if (e_rcb) { c_v0 = std::sqrt(c_q0); c_v1 = std::sqrt(c_q1); }
        } else {
            c_v0 = std::fabs(c_d0); c_v1 = std::fabs(c_d1); c_s0 = c_d0; c_s1 = c_d1;
        }
        if (e_one) {
            // one gain for both channels: the louder level, or the power average, then one gain computer
            ch_lin(e_c0, c_d0, c_v0);
            ch_lin(e_c1, c_d1, c_v1);
            double a_m = emax(e_c0.lin, e_c1.lin);
            double a_a0 = (e_pwr ? std::log(0.5 * (e_c0.lin * e_c0.lin + e_c1.lin * e_c1.lin)) * 4.342944819032518
                                 : std::log(a_m) * 8.685889638065035) + e_fbk * e_c0.y;
            if (c_vneed && a_m > c_vdot) c_vdot = a_m;
            if (c_tgs == 2) { e_l0 = std::log(a_m) * 8.685889638065035; e_l1 = e_l0; }
            ch_gain(e_c0, a_a0);
            e_c1.y = e_c0.y; e_c1.yf = e_c0.yf; e_c1.ys = e_c0.ys; e_c1.sa = e_c0.sa; e_c1.hold = e_c0.hold; e_c1.k = e_c0.k;
        } else {
            // detector levels (dB), then feedback and the stereo link (Mono is a power average)
            ch_det(e_c0, c_d0, c_v0);
            ch_det(e_c1, c_d1, c_v1);
            e_l0 = e_c0.lv;
            e_l1 = e_c1.lv;
            double a_a0 = e_l0 + e_fbk * e_c0.y, a_a1 = e_l1 + e_fbk * e_c1.y;
            if (e_mono) {
                a_a0 = std::log(0.5 * (std::exp(a_a0 * 0.2302585092994046) + std::exp(a_a1 * 0.2302585092994046))) * 4.342944819032518;
                a_a1 = a_a0;
            } else {
                double a_ref = e_lkl ? std::log(0.5 * (std::exp(a_a0 * 0.2302585092994046) + std::exp(a_a1 * 0.2302585092994046))) * 4.342944819032518
                                     : emax(a_a0, a_a1);
                a_a0 += e_lk * (a_ref - a_a0);
                a_a1 += e_lk * (a_ref - a_a1);
            }
            if (c_vneed) { double t = std::exp(emax(e_l0, e_l1) * 0.11512925464970229); if (t > c_vdot) c_vdot = t; }
            // gain computer, lookahead and ballistics
            ch_gain(e_c0, a_a0);
            if (e_mono) { e_c1.y = e_c0.y; e_c1.k = e_c0.k; }
            else ch_gain(e_c1, a_a1);
        }
        c_y0 = e_c0.y; c_y1 = e_c1.y; c_k0 = e_c0.k; c_k1 = e_c1.k;
        // "Set threshold" is listening: the loudest detector level of each millisecond
        if (c_tgs == 2) {
            double t = emax(e_l0, e_l1);
            if (t > c_tm) c_tm = t;
            if ((c_tc += 1) >= c_tms) {
                if (c_tgn < 60000) { TGB[c_tgn] = c_tm; c_tgn += 1; }
                c_tm = -200;
                c_tc = 0;
            }
        }
        // lookahead: the audio waits, the detector doesn't (a change of the time crosses over)
        double c_qa0, c_qa1, c_q0, c_q1;
        if (e_ldA == 0 && c_xf >= 1) { c_qa0 = c_r0; c_qa1 = c_r1; }
        else { int t = (c_lp - e_ldA) & 8191; c_qa0 = LAB0[t]; c_qa1 = LAB1[t]; }
        if (c_xf < 1) {
            int t = (c_lp - e_ldB) & 8191;
            c_xf += e_xfk;
            if (c_xf > 1) c_xf = 1;
            c_q0 = LAB0[t] + c_xf * (c_qa0 - LAB0[t]);
            c_q1 = LAB1[t] + c_xf * (c_qa1 - LAB1[t]);
        } else {
            c_q0 = c_qa0;
            c_q1 = c_qa1;
        }
        c_lp = (c_lp + 1) & 8191;
        double c_dx0 = c_q0 * c_ings, c_dx1 = c_q1 * c_ings;
        // the wet signal: the gain on the original left and right (Mono shares one gain)
        double c_w0, c_w1;
        if (c_mode == 2) {
            double a_m = (c_dx0 + c_dx1) * 0.5 * c_k0, a_s = (c_dx0 - c_dx1) * 0.5 * c_k1;
            c_w0 = a_m + a_s;
            c_w1 = a_m - a_s;
        } else {
            c_w0 = c_dx0 * c_k0;
            c_w1 = c_dx1 * (c_mode == 0 ? c_k0 : c_k1);
        }
        if (e_sat > 0.001) { c_w0 = sat_run(e_s0, c_w0); c_w1 = sat_run(e_s1, c_w1); }
        // out = dry * (1 - mix) + compressed * makeup * mix. Delta = the input minus the compressed signal
        double o0, o1;
        if (c_lis) {
            if (c_mode == 0) { o0 = (c_s0 + c_s1) * 0.5; o1 = o0; }
            else { o0 = c_s0; o1 = c_s1; }
        } else if (c_del) {
            o0 = c_dx0 - c_w0;
            o1 = c_dx1 - c_w1;
        } else {
            o0 = c_dx0 * c_dms + c_w0 * c_wms;
            o1 = c_dx1 * c_dms + c_w1 * c_wms;
        }
        out[0][i] = o0;
        out[1][i] = o1;
        bq0[i] = c_q0; bq1[i] = c_q1;
        br0[i] = c_r0; br1[i] = c_r1;
        bx0[i] = c_x0; bx1[i] = c_x1;
    }
}

// ====================================================================== after the built-in Act
void Engine::post(double *const *out, int n)
{
    for (int i = 0; i < n; ++i) {
        double s0 = out[0][i], s1 = out[1][i];
        // meters and level matching are only for the window (and its hold buttons)
        if (c_uion) {
            c_ei += br0[i] * br0[i] + br1[i] * br1[i];
            c_eo += s0 * s0 + s1 * s1;
            double t = std::fabs(bx0[i]);
            if (t > c_ip0) c_ip0 = t;
            t = std::fabs(bx1[i]);
            if (t > c_ip1) c_ip1 = t;
        }
        // held Bypass (crossfaded) and held Swap (its level-matching trim, glided)
        if (c_bpv > 0 || c_bph || ne(c_tr, 1) || ne(c_trt, 1)) {
            c_bpv = emax(0, emin(1, c_bpv + (c_bph ? c_bps : -c_bps)));
            c_tr += (c_trt - c_tr) * c_trk;
            if (std::fabs(c_tr - c_trt) < 0.00001) c_tr = c_trt;
            s0 = s0 * c_tr * (1 - c_bpv) + bq0[i] * c_bpg * c_bpv;
            s1 = s1 * c_tr * (1 - c_bpv) + bq1[i] * c_bpg * c_bpv;
            out[0][i] = s0;
            out[1][i] = s1;
        }
        if (c_anw) {
            V.ani[c_ap].store((float)((bx0[i] + bx1[i]) * 0.5), std::memory_order_relaxed);
            V.ano[c_ap].store((float)((s0 + s1) * 0.5), std::memory_order_relaxed);
            c_ap = (c_ap + 1) & (Display::AN - 1);
        }
    }
    if (c_anw) V.ap.store(c_ap, std::memory_order_release);
}

} // namespace comp
} // namespace ts
