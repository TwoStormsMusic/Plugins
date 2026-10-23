#include "act_shaper.h"
#include "../../core/eel.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ts {
namespace actp {

static const double TSK2 = 2 * std::sqrt(2.0);
static const double KMAX = 3.981071705534972;   // 12 dB

Shaper::Shaper() : LAB(8 * 4 * TSLN, 0.0) {}

int Shaper::latency(const ShaperIn &in, double srate)
{
    if (!(in.sw && in.la)) return 0;
    return (int)std::max(1.0, std::min((double)TSLM, std::floor(in.lams * 0.001 * srate + 0.5)));
}

// a crossover's coefficients for f Hz (Q = 1/sqrt 2, so two in a row make a Linkwitz-Riley 24 dB/oct)
void Shaper::xo(double f, int i)
{
    double g = std::tan(PI * std::min(f, 0.45 * sr) / sr);
    TSXC[i * 3] = 1 / (1 + g * (g + std::sqrt(2.0)));
    TSXC[i * 3 + 1] = g * TSXC[i * 3];
    TSXC[i * 3 + 2] = g * TSXC[i * 3 + 1];
}

// one state-variable filter step (topology-preserving): returns the low pass, bp_ the band pass
double Shaper::tsvf(double x, double *st, double a1, double a2, double a3)
{
    double v3 = x - st[1];
    double v1 = a1 * st[0] + a2 * v3;
    double v2 = st[1] + a2 * st[0] + a3 * v3;
    st[0] = 2 * v1 - st[0];
    st[1] = 2 * v2 - st[1];
    bp_ = v1;
    return v2;
}

void Shaper::block(const ShaperIn &in, double srate, int nchs, int ngIn, const int *ga, const int *gn)
{
    sw = in.sw;
    nb = 1 + (in.xo1 ? 1 : 0) + (in.xo2 ? 1 : 0);
    bool any = false;
    for (int i = 0; i < 3; ++i) {
        TSA[i] = i < nb ? in.att[i] * 0.01 : 0;
        TSU[i] = i < nb ? in.sus[i] * 0.01 : 0;
        if (ne(TSA[i], 0) || ne(TSU[i], 0)) any = true;
    }
    on = sw && any;
    ld = latency(in, srate);
    run = on || mix > 0;
    double t = in.x1, r = in.x2;
    fa = nb == 3 ? std::min(t, r) : (in.xo1 ? t : r);
    fb = std::max(std::max(t, r), fa * 1.26);
    if (run) {
        // starting, or the bands changed: the filters and followers start from zero
        if (!was || nb != nbw) {
            memset(TSX, 0, sizeof TSX);
            memset(TSF, 0, sizeof TSF);
            memset(TSS, 0, sizeof TSS);
            memset(TSL, 0, sizeof TSL);
            memset(TSH, 0, sizeof TSH);
            for (double &g : TSG) g = 1;
            if (!was) { mix = 0; warm = -1; }
            fast = false;
        }
        nbw = nb;
        if (srate != sr) {
            sr = srate;
            kfa = 1 - std::exp(-1 / (0.0003 * srate));
            ksa = 1 - std::exp(-1 / (0.015 * srate));
            kfr = 1 - std::exp(-1 / (0.05 * srate));
            ksr = 1 - std::exp(-1 / (0.3 * srate));
            kg = 1 - std::exp(-1 / (0.0005 * srate));
            kmix = 1 / (0.01 * srate);
            xc1 = xc2 = -1;
        }
        // starting: 50 ms for the filters and followers to settle (the output stays dry), then the fade in
        if (warm < 0) warm = std::floor(0.05 * srate);
        if (fa != xc1) { xc1 = fa; xo(fa, 0); }
        if (fb != xc2) { xc2 = fb; xo(fb, 1); }
        TSHS[0] = std::floor(0.025 * srate);
        TSHS[1] = std::floor(std::max(0.003, std::min(0.025, 2.5 / fa)) * srate);
        TSHS[2] = std::floor(std::max(0.003, std::min(0.025, 2.5 / fb)) * srate);
        ng = ngIn;
        for (int g = 0; g < ng; ++g) { TGA[g] = ga[g]; TGN[g] = gn[g]; }
        for (double &v : TSGI) v = -1;
        for (int g = 0; g < ng; ++g) {
            int c = TGA[g];
            for (int k = 0; k < TGN[g]; ++k) TSGI[c++] = g * 3;
        }
        // one linked stereo pair and nothing else: the fast path (its own state; a change of layout starts it from zero)
        bool t2 = nchs == 2 && ng == 1 && TGN[0] == 2 && TGA[0] == 0;
        if (t2 && !fast) {
            tl1a = tl1b = tl2a = tl2b = tl3a = tl3b = tl4a = tl4b = tl5a = tl5b = 0;
            tr1a = tr1b = tr2a = tr2b = tr3a = tr3b = tr4a = tr4b = tr5a = tr5b = 0;
            tf0 = ts0 = tr0 = th0 = tf1 = ts1 = tr1 = th1 = tf2 = ts2 = tr2 = th2 = 0;
            tg0 = tg1 = tg2 = 1;
        }
        fast = t2;
        // the meters in the window: the most each band went up or down in this block
        for (int i = 0; i < 3; ++i) {
            TSMD[i] = 20 * std::log10(TSBX[i] * TSBN[i] >= 1 ? TSBX[i] : TSBN[i]);
            TSBX[i] = 1;
            TSBN[i] = 1;
        }
        // no denormals in silence
        for (double &v : TSX) if (std::fabs(v) < 0.000000000001) v = 0;
        for (int i = 0; i < 24; ++i) {
            if (TSF[i] < 0.000000000001) TSF[i] = 0;
            if (TSS[i] < 0.000000000001) TSS[i] = 0;
            if (TSL[i] < 0.000000000001) TSL[i] = 0;
        }
        if (fast) {
            if (std::fabs(tl1a) + std::fabs(tl1b) + std::fabs(tl2a) + std::fabs(tl2b) + std::fabs(tl3a) + std::fabs(tl3b) + std::fabs(tl4a) +
                    std::fabs(tl4b) + std::fabs(tl5a) + std::fabs(tl5b) + std::fabs(tr1a) + std::fabs(tr1b) + std::fabs(tr2a) + std::fabs(tr2b) +
                    std::fabs(tr3a) + std::fabs(tr3b) + std::fabs(tr4a) + std::fabs(tr4b) + std::fabs(tr5a) + std::fabs(tr5b) <
                0.000000000001) {
                tl1a = tl1b = tl2a = tl2b = tl3a = tl3b = tl4a = tl4b = tl5a = tl5b = 0;
                tr1a = tr1b = tr2a = tr2b = tr3a = tr3b = tr4a = tr4b = tr5a = tr5b = 0;
            }
            if (tf0 < 0.000000000001) tf0 = 0;
            if (ts0 < 0.000000000001) ts0 = 0;
            if (tr0 < 0.000000000001) tr0 = 0;
            if (tf1 < 0.000000000001) tf1 = 0;
            if (ts1 < 0.000000000001) ts1 = 0;
            if (tr1 < 0.000000000001) tr1 = 0;
            if (tf2 < 0.000000000001) tf2 = 0;
            if (ts2 < 0.000000000001) ts2 = 0;
            if (tr2 < 0.000000000001) tr2 = 0;
        }
        x1a = TSXC[0]; x1b = TSXC[1]; x1c = TSXC[2]; x2a = TSXC[3]; x2b = TSXC[4]; x2c = TSXC[5];
    } else {
        TSMD[0] = TSMD[1] = TSMD[2] = 0;
    }
    was = run;
}

// a band's followers on level x, and the gain they ask for (the fast path's variables by reference)
static inline double follow(double x, double &f, double &s, double &r, double &h, double hs, double ta, double tu, double kfa, double kfr,
                            double ksa, double ksr)
{
    if (x > f) { f += kfa * (x - f); h = hs; }
    else { if (h > 0) h -= 1; else f += kfr * (x - f); }
    s += (f > s ? ksa : kfr) * (f - s);
    r = f > r ? f : r + ksr * (f - r);
    double k = 1, e;
    if (ne(ta, 0)) { e = (f + 0.00001) / (s + 0.00001); if (e > 1) k = std::pow(std::min(KMAX, e), ta); }
    if (ne(tu, 0)) { e = (r + 0.00001) / (f + 0.00001); if (e > 1) k *= std::pow(std::min(KMAX, e), tu); }
    return k;
}

void Shaper::process(double *const *ch, int nchs, int n)
{
    if (!run) {
        // switched on with lookahead but nothing to shape: just the delay, so the latency stays the same
        if (ld > 0) {
            for (int i = 0; i < n; ++i) {
                int q = (lp - ld) & TSLM;
                for (int c = 0; c < nchs; ++c) {
                    double *ln = LAB.data() + (c * 4 + 3) * TSLN;
                    ln[lp] = ch[c][i];
                    ch[c][i] = ln[q];
                }
                lp = (lp + 1) & TSLM;
            }
        }
        return;
    }
    double *L = LAB.data();
    for (int i = 0; i < n; ++i) {
        // fade in or out over 10 ms (after the 50 ms settling); once faded out, it stops (see block)
        if (warm > 0) warm -= 1;
        else mix = on ? std::min(1.0, mix + kmix) : std::max(0.0, mix - kmix);
        int q = (lp - ld) & TSLM;
        if (fast) {
            double s0 = ch[0][i], s1 = ch[1][i];
            double v1, v2, v3, l1, lo, hi, l2, bp;
            if (nb == 1) b00 = s0;
            else {
                v3 = s0 - tl1b; v1 = x1a * tl1a + x1b * v3; v2 = tl1b + x1b * tl1a + x1c * v3; tl1a = 2 * v1 - tl1a; tl1b = 2 * v2 - tl1b; l1 = v2; bp = v1;
                v3 = l1 - tl2b; v1 = x1a * tl2a + x1b * v3; v2 = tl2b + x1b * tl2a + x1c * v3; tl2a = 2 * v1 - tl2a; tl2b = 2 * v2 - tl2b; lo = v2;
                hi = s0 - TSK2 * bp - lo;
                if (nb == 2) { b00 = lo; b10 = hi; }
                else {
                    v3 = hi - tl3b; v1 = x2a * tl3a + x2b * v3; v2 = tl3b + x2b * tl3a + x2c * v3; tl3a = 2 * v1 - tl3a; tl3b = 2 * v2 - tl3b; l2 = v2; bp = v1;
                    v3 = l2 - tl4b; v1 = x2a * tl4a + x2b * v3; v2 = tl4b + x2b * tl4a + x2c * v3; tl4a = 2 * v1 - tl4a; tl4b = 2 * v2 - tl4b; b10 = v2;
                    b20 = hi - TSK2 * bp - b10;
                    v3 = lo - tl5b; v1 = x2a * tl5a + x2b * v3; v2 = tl5b + x2b * tl5a + x2c * v3; tl5a = 2 * v1 - tl5a; tl5b = 2 * v2 - tl5b; bp = v1;
                    b00 = lo - TSK2 * bp;
                }
            }
            if (nb == 1) b01 = s1;
            else {
                v3 = s1 - tr1b; v1 = x1a * tr1a + x1b * v3; v2 = tr1b + x1b * tr1a + x1c * v3; tr1a = 2 * v1 - tr1a; tr1b = 2 * v2 - tr1b; l1 = v2; bp = v1;
                v3 = l1 - tr2b; v1 = x1a * tr2a + x1b * v3; v2 = tr2b + x1b * tr2a + x1c * v3; tr2a = 2 * v1 - tr2a; tr2b = 2 * v2 - tr2b; lo = v2;
                hi = s1 - TSK2 * bp - lo;
                if (nb == 2) { b01 = lo; b11 = hi; }
                else {
                    v3 = hi - tr3b; v1 = x2a * tr3a + x2b * v3; v2 = tr3b + x2b * tr3a + x2c * v3; tr3a = 2 * v1 - tr3a; tr3b = 2 * v2 - tr3b; l2 = v2; bp = v1;
                    v3 = l2 - tr4b; v1 = x2a * tr4a + x2b * v3; v2 = tr4b + x2b * tr4a + x2c * v3; tr4a = 2 * v1 - tr4a; tr4b = 2 * v2 - tr4b; b11 = v2;
                    b21 = hi - TSK2 * bp - b11;
                    v3 = lo - tr5b; v1 = x2a * tr5a + x2b * v3; v2 = tr5b + x2b * tr5a + x2c * v3; tr5a = 2 * v1 - tr5a; tr5b = 2 * v2 - tr5b; bp = v1;
                    b01 = lo - TSK2 * bp;
                }
            }
            // each band's followers (on both channels' level), then its gain
            if (ne(TSA[0], 0) || ne(TSU[0], 0)) {
                double k = follow(std::max(std::fabs(b00), std::fabs(b01)), tf0, ts0, tr0, th0, TSHS[0], TSA[0], TSU[0], kfa, kfr, ksa, ksr);
                tg0 += kg * (k - tg0);
                if (tg0 > TSBX[0]) TSBX[0] = tg0;
                if (tg0 < TSBN[0]) TSBN[0] = tg0;
            } else tg0 += kg * (1 - tg0);
            if (ne(TSA[1], 0) || ne(TSU[1], 0)) {
                double k = follow(std::max(std::fabs(b10), std::fabs(b11)), tf1, ts1, tr1, th1, TSHS[1], TSA[1], TSU[1], kfa, kfr, ksa, ksr);
                tg1 += kg * (k - tg1);
                if (tg1 > TSBX[1]) TSBX[1] = tg1;
                if (tg1 < TSBN[1]) TSBN[1] = tg1;
            } else tg1 += kg * (1 - tg1);
            if (ne(TSA[2], 0) || ne(TSU[2], 0)) {
                double k = follow(std::max(std::fabs(b20), std::fabs(b21)), tf2, ts2, tr2, th2, TSHS[2], TSA[2], TSU[2], kfa, kfr, ksa, ksr);
                tg2 += kg * (k - tg2);
                if (tg2 > TSBX[2]) TSBX[2] = tg2;
                if (tg2 < TSBN[2]) TSBN[2] = tg2;
            } else tg2 += kg * (1 - tg2);
            // lookahead: the bands and the dry signal go through the delay; the followers above heard them first
            double d0 = s0, d1 = s1;
            if (ld > 0) {
                L[lp] = b00; L[TSLN + lp] = b10; L[2 * TSLN + lp] = b20; L[3 * TSLN + lp] = s0;
                L[4 * TSLN + lp] = b01; L[5 * TSLN + lp] = b11; L[6 * TSLN + lp] = b21; L[7 * TSLN + lp] = s1;
                b00 = L[q]; b10 = L[TSLN + q]; b20 = L[2 * TSLN + q]; d0 = L[3 * TSLN + q];
                b01 = L[4 * TSLN + q]; b11 = L[5 * TSLN + q]; b21 = L[6 * TSLN + q]; d1 = L[7 * TSLN + q];
            }
            double y = b00 * tg0, z = b01 * tg0;
            if (nb > 1) { y += b10 * tg1; z += b11 * tg1; }
            if (nb > 2) { y += b20 * tg2; z += b21 * tg2; }
            if (mix >= 1) { ch[0][i] = y; ch[1][i] = z; }
            else { ch[0][i] = d0 + mix * (y - d0); ch[1][i] = d1 + mix * (z - d1); }
        } else {
            // any other layout: the bands of each channel of each group
            for (int g = 0; g < ng; ++g) {
                int a = TGA[g], c = a;
                for (int k = 0; k < TGN[g]; ++k, ++c) {
                    double x = ch[c][i];
                    double *st = TSX + c * 10, *tb = TSB + c * 3;
                    if (nb == 1) tb[0] = x;
                    else {
                        double l1 = tsvf(x, st, x1a, x1b, x1c), bp = bp_;
                        double lo = tsvf(l1, st + 2, x1a, x1b, x1c);
                        double hi = x - TSK2 * bp - lo;
                        if (nb == 2) { tb[0] = lo; tb[1] = hi; }
                        else {
                            double l2 = tsvf(hi, st + 4, x2a, x2b, x2c);
                            bp = bp_;
                            tb[1] = tsvf(l2, st + 6, x2a, x2b, x2c);
                            tb[2] = hi - TSK2 * bp - tb[1];
                            tsvf(lo, st + 8, x2a, x2b, x2c);
                            tb[0] = lo - TSK2 * bp_;
                        }
                    }
                }
                // each band: its followers on the group's level, then its gain
                for (int b = 0; b < nb; ++b) {
                    int ii = g * 3 + b;
                    double k = 1;
                    if (ne(TSA[b], 0) || ne(TSU[b], 0)) {
                        double x = std::fabs(TSB[a * 3 + b]);
                        if (TGN[g] == 2) { double t = std::fabs(TSB[a * 3 + 3 + b]); if (t > x) x = t; }
                        double f = TSF[ii];
                        if (x > f) { f += kfa * (x - f); TSH[ii] = TSHS[b]; }
                        else { if (TSH[ii] > 0) TSH[ii] -= 1; else f += kfr * (x - f); }
                        TSF[ii] = f;
                        double t = TSS[ii];
                        t += (f > t ? ksa : kfr) * (f - t);
                        TSS[ii] = t;
                        double r = TSL[ii];
                        r = f > r ? f : r + ksr * (f - r);
                        TSL[ii] = r;
                        double e;
                        if (ne(TSA[b], 0)) { e = (f + 0.00001) / (t + 0.00001); if (e > 1) k = std::pow(std::min(KMAX, e), TSA[b]); }
                        if (ne(TSU[b], 0)) { e = (r + 0.00001) / (f + 0.00001); if (e > 1) k *= std::pow(std::min(KMAX, e), TSU[b]); }
                    }
                    TSG[ii] += kg * (k - TSG[ii]);
                    k = TSG[ii];
                    if (k > TSBX[b]) TSBX[b] = k;
                    if (k < TSBN[b]) TSBN[b] = k;
                }
            }
            // every channel through the delay (lookahead; none: straight through), the bands back together at their gains
            for (int c = 0; c < nchs; ++c) {
                double *ln = L + c * 4 * TSLN;
                ln[3 * TSLN + lp] = ch[c][i];
                double d0 = ln[3 * TSLN + q];
                int ii = (int)TSGI[c];
                if (ii >= 0) {
                    double *tb = TSB + c * 3;
                    ln[lp] = tb[0]; ln[TSLN + lp] = tb[1]; ln[2 * TSLN + lp] = tb[2];
                    double y = ln[q] * TSG[ii];
                    if (nb > 1) y += ln[TSLN + q] * TSG[ii + 1];
                    if (nb > 2) y += ln[2 * TSLN + q] * TSG[ii + 2];
                    ch[c][i] = mix >= 1 ? y : d0 + mix * (y - d0);
                } else ch[c][i] = d0;
            }
        }
        lp = (lp + 1) & TSLM;
    }
}

} // namespace actp
} // namespace ts
