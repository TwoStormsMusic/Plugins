#include "pvshift.h"
#include "fft.h"
#include <algorithm>
#include <cmath>

namespace ts {

static const double TWO_PI = 6.283185307179586;

static inline double princarg(double a) { return a - TWO_PI * std::floor(a / TWO_PI + 0.5); }

int PvShift::sizeFor(double sr) { return sr <= 50000 ? 4096 : (sr <= 100000 ? 8192 : 16384); }

void PvShift::init(int n)
{
    N = n;
    H = n / 4;
    const int B = N / 2;
    win.assign(N, 0);
    for (int i = 0; i < N; ++i) win[i] = 0.5 - 0.5 * std::cos(TWO_PI * i / N);
    buf.assign(N, 0);
    for (int c = 0; c < 2; ++c) {
        in[c].assign(N, 0);
        Chan &h = ch[c];
        h.xr.assign(B, 0); h.xi.assign(B, 0); h.pxr.assign(B, 0); h.pxi.assign(B, 0); h.m2.assign(B, 0);
        h.pk.assign(B, 0); h.lo.assign(B, 0); h.hi.assign(B, 0); h.own.assign(B, -1); h.pown.assign(B, -1);
        h.om.assign(B, 0); h.ph.assign(B, 0);
    }
    for (int v = 0; v < MAXV; ++v)
        for (int c = 0; c < 2; ++c) {
            vo[v].psi[c].assign(B, 0);
            vo[v].ppsi[c].assign(B, 0);
            vo[v].ring[c].assign(N, 0);
        }
    reset();
}

void PvShift::reset()
{
    pos = 0;
    cnt = 0;
    for (int c = 0; c < 2; ++c) {
        std::fill(in[c].begin(), in[c].end(), 0.0);
        Chan &h = ch[c];
        std::fill(h.pxr.begin(), h.pxr.end(), 0.0);
        std::fill(h.pxi.begin(), h.pxi.end(), 0.0);
        std::fill(h.pown.begin(), h.pown.end(), -1);
        h.npk = 0;
        h.en = h.pen = 0;
        h.start = true;
        h.attack = false;
    }
    for (int v = 0; v < MAXV; ++v) {
        vo[v].fresh = true;
        for (int c = 0; c < 2; ++c) {
            std::fill(vo[v].ring[c].begin(), vo[v].ring[c].end(), 0.0);
            y[v][c] = 0;
        }
    }
}

void PvShift::setRatio(int v, double r)
{
    Voice &o = vo[v];
    bool on = std::fabs(r - 1) > 1e-9;
    if (on && !o.on) {
        for (int c = 0; c < 2; ++c) std::fill(o.ring[c].begin(), o.ring[c].end(), 0.0);
        o.fresh = true;
    }
    if (!on) { y[v][0] = y[v][1] = 0; }
    o.on = on;
    o.r = r;
}

void PvShift::frame()
{
    for (int c = 0; c < 2; ++c) analyse(c);
    for (int v = 0; v < MAXV; ++v) {
        if (!vo[v].on) continue;
        for (int c = 0; c < 2; ++c) synth(vo[v], c);
        vo[v].fresh = false;
        for (int c = 0; c < 2; ++c) std::swap(vo[v].psi[c], vo[v].ppsi[c]);
    }
    for (int c = 0; c < 2; ++c) {
        Chan &h = ch[c];
        std::swap(h.xr, h.pxr);
        std::swap(h.xi, h.pxi);
        std::swap(h.own, h.pown);
        h.pen = h.en;
        h.start = false;
    }
}

// this frame of channel c: its spectrum, its peaks (each with the bins around it) and their frequencies
void PvShift::analyse(int c)
{
    Chan &h = ch[c];
    const int mask = N - 1, B = N / 2;
    // the last N samples, oldest first, windowed, turned so the frame's middle is at 0 (zero phase)
    for (int i = 0; i < N; ++i) buf[(i + B) & mask] = in[c][(pos + i) & mask] * win[i];
    fft::realForward(buf.data(), N);
    double en = 0, mx = 0;
    h.xr[0] = h.xi[0] = h.m2[0] = 0;
    for (int k = 1; k < B; ++k) {
        double a = buf[2 * k], b = buf[2 * k + 1];
        h.xr[k] = a;
        h.xi[k] = b;
        double m = a * a + b * b;
        h.m2[k] = m;
        en += m;
        mx = std::max(mx, m);
    }
    h.en = en;
    // a sharp attack: the phases start over (the shifted sound then keeps its punch)
    h.attack = !h.start && en > 4 * h.pen + 1e-12;
    // the peaks: louder than two bins either side, and within 70 dB of the loudest
    const double thr = std::max(mx * 1e-7, 1e-20);
    int n = 0;
    for (int k = 2; k < B - 2; ++k) {
        double m = h.m2[k];
        if (m > thr && m > h.m2[k - 1] && m >= h.m2[k + 1] && m > h.m2[k - 2] && m >= h.m2[k + 2]) h.pk[n++] = k;
    }
    h.npk = n;
    if (n == 0) {
        std::fill(h.own.begin(), h.own.end(), -1);
        return;
    }
    // each peak's region: down to the quietest bin between it and the next
    h.lo[0] = 1;
    for (int i = 0; i + 1 < n; ++i) {
        int b = h.pk[i] + 1;
        for (int k = h.pk[i] + 1; k < h.pk[i + 1]; ++k)
            if (h.m2[k] < h.m2[b]) b = k;
        h.hi[i] = b;
        h.lo[i + 1] = b + 1;
    }
    h.hi[n - 1] = B - 1;
    h.own[0] = -1;
    for (int i = 0; i < n; ++i)
        for (int k = h.lo[i]; k <= h.hi[i]; ++k) h.own[k] = i;
    // each peak's frequency, from how far its phase moved since the last frame
    const double adv = TWO_PI * H / N;
    for (int i = 0; i < n; ++i) {
        int p = h.pk[i];
        double ph = std::atan2(h.xi[p], h.xr[p]);
        h.ph[i] = ph;
        double w = TWO_PI * p / N;
        if (!h.start && (h.pxr[p] != 0 || h.pxi[p] != 0)) {
            double pp = std::atan2(h.pxi[p], h.pxr[p]);
            w += princarg(ph - pp - adv * p) / H;
        }
        h.om[i] = w;
    }
}

// voice v, channel c: every region moved (a whole number of bins) so its peak lands at the new frequency; the
// peak's phase carried on from where its track was in the last frame, the bins around it locked to it
void PvShift::synth(Voice &v, int c)
{
    Chan &h = ch[c];
    const int mask = N - 1, B = N / 2;
    std::fill(buf.begin(), buf.end(), 0.0);
    const double r = v.r, toBins = N / TWO_PI;
    bool restart = v.fresh || h.attack || h.start;
    for (int i = 0; i < h.npk; ++i) {
        int p = h.pk[i];
        double w = h.om[i];
        int d = (int)std::lround((r - 1) * w * toBins);
        int j = h.pown[p];
        double psi = (!restart && j >= 0) ? princarg(v.ppsi[c][j] + H * r * w) : h.ph[i];
        v.psi[c][i] = psi;
        double rot = psi - h.ph[i], cr = std::cos(rot), sr = std::sin(rot);
        int k0 = std::max(h.lo[i], 1 - d), k1 = std::min(h.hi[i], B - 1 - d);
        for (int k = k0; k <= k1; ++k) {
            int t = k + d;
            double a = h.xr[k], b = h.xi[k];
            buf[2 * t] += a * cr - b * sr;
            buf[2 * t + 1] += a * sr + b * cr;
        }
    }
    buf[0] = buf[1] = 0;
    fft::realInverse(buf.data(), N);
    // back from zero phase, windowed again, added to what is playing: Hann twice at a quarter hop sums to 1.5
    const double sc = 1.0 / (2.0 * N * 1.5);
    double *ring = v.ring[c].data();
    for (int i = 0; i < N; ++i) ring[(pos + i) & mask] += buf[(i + B) & mask] * win[i] * sc;
}

} // namespace ts
