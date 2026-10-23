#include "analyzer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ts {
namespace gui {

// an n-point real FFT (power per bin) through an n/2-point complex one; tables made once per size
struct RealFft {
    int N, H, LB;
    std::vector<float> win, cs, sn, tc, ts, re, im;
    std::vector<int> rev;
    explicit RealFft(int n) : N(n), H(n / 2), LB(0)
    {
        while ((1 << LB) < H) LB++;
        win.resize(N); cs.resize(H); sn.resize(H); tc.resize(H / 2 + 1); ts.resize(H / 2 + 1); re.resize(H); im.resize(H); rev.resize(H);
        for (int i = 0; i < N; ++i) win[i] = (float)(0.5 - 0.5 * std::cos(6.283185307179586 * i / N));
        for (int i = 0; i < H; ++i) { cs[i] = (float)std::cos(6.283185307179586 * i / H); sn[i] = (float)-std::sin(6.283185307179586 * i / H); }
        for (int k = 0; k <= H / 2; ++k) { tc[k] = (float)std::cos(3.141592653589793 * k / H); ts[k] = (float)-std::sin(3.141592653589793 * k / H); }
        for (int i = 0; i < H; ++i) {
            int r = 0;
            for (int b = 0; b < LB; ++b)
                if (i & (1 << b)) r |= 1 << (LB - 1 - b);
            rev[i] = r;
        }
    }
    // N samples from a ring (Hann window), power of bins 0..H-1
    void run(const std::atomic<float> *src, int p0, int mask, float *pw)
    {
        for (int i = 0; i < H; ++i) {
            int j = rev[i];
            re[j] = src[(p0 + 2 * i) & mask].load(std::memory_order_relaxed) * win[2 * i];
            im[j] = src[(p0 + 2 * i + 1) & mask].load(std::memory_order_relaxed) * win[2 * i + 1];
        }
        for (int len = 2; len <= H; len <<= 1) {
            int step = H / len, half = len / 2;
            for (int i = 0; i < H; i += len)
                for (int j = 0; j < half; ++j) {
                    float wr = cs[j * step], wi = sn[j * step];
                    int a = i + j, b = a + half;
                    float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - xr;
                    im[b] = im[a] - xi;
                    re[a] += xr;
                    im[a] += xi;
                }
        }
        // split the packed result into the real signal's spectrum
        for (int k = 0; k < H; ++k) {
            int nk = (H - k) & (H - 1);
            float er = 0.5f * (re[k] + re[nk]), ei = 0.5f * (im[k] - im[nk]);
            float or_ = 0.5f * (im[k] + im[nk]), oi = -0.5f * (re[k] - re[nk]);
            float c = k <= H / 2 ? tc[k] : -tc[H - k], s = k <= H / 2 ? ts[k] : ts[H - k];
            float xr = er + (or_ * c - oi * s), xi = ei + (or_ * s + oi * c);
            pw[k] = xr * xr + xi * xi;
        }
    }
};

int Spectrum::sizeFor(double sr, int mask)
{
    int n = sr <= 50000 ? 8192 : (sr <= 100000 ? 16384 : 32768);
    while (n > mask + 1) n /= 2;
    return n;
}

void Spectrum::update(const std::atomic<float> *ring, int mask, int ap, double dt, double sr)
{
    // windows are drawn on the main thread, one at a time
    static RealFft *F[17] = {};
    static std::vector<float> pw;
    int nn = sizeFor(sr, mask);
    if (nn != n || (int)db.size() != nn / 2) { n = nn; db.assign(nn / 2, -200.f); }
    int lb = 0;
    while ((1 << lb) < n) lb++;
    if (!F[lb]) F[lb] = new RealFft(n);
    if ((int)pw.size() < n / 2) pw.resize(n / 2);
    int p0 = (ap - n) & mask;
    F[lb]->run(ring, p0, mask, pw.data());
    float k = (float)std::min(1.0, dt * 5);
    float norm = (float)(20 * std::log10(n / 4.0));   // a full-scale sine reads 0 dB
    for (int i = 1; i < n / 2; ++i) {
        float m = pw[i];
        float d = m > 0 ? 10 * std::log10(m) - norm : -200;
        db[i] = d > db[i] ? d : db[i] + (d - db[i]) * k;
    }
}

// a monotone cubic (Fritsch-Carlson) through the bins: smooth, and never overshoots between two bins
float Spectrum::at(double x) const
{
    int H = (int)db.size();
    if (x <= 1) return db[1];
    if (x >= H - 1) return db[H - 1];
    int k = (int)std::floor(x);
    double t = x - k;
    auto y = [&](int i) { return (double)db[std::max(1, std::min(H - 1, i))]; };
    double y0 = y(k - 1), y1 = y(k), y2 = y(k + 1), y3 = y(k + 2);
    double d0 = y1 - y0, d1 = y2 - y1, d2 = y3 - y2;
    auto slope = [](double a, double b) { return (a * b <= 0) ? 0.0 : 2 / (1 / a + 1 / b); };
    double m1 = slope(d0, d1), m2 = slope(d1, d2);
    double t2 = t * t, t3 = t2 * t;
    return (float)((2 * t3 - 3 * t2 + 1) * y1 + (t3 - 2 * t2 + t) * m1 + (-2 * t3 + 3 * t2) * y2 + (t3 - t2) * m2);
}

double FreqScale::x(double fr) const { return r.x + r.w * std::log(std::max(20.0, fr) / 20) / std::log(1000.0); }
double FreqScale::f(double xx) const { return 20 * std::pow(1000.0, (xx - r.x) / r.w); }
double FreqScale::y(double d, double fr) const
{
    return r.y + r.h * std::min(1.0, std::max(0.0, -(d + 4.5 * std::log(fr / 1000) / std::log(2.0)) / 90));
}

static const int FR[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000};

void freqGrid(Ui &ui, const R &r)
{
    FreqScale fs(r);
    for (int f : FR) ui.fill(R((float)fs.x(f), r.y, 1 / ui.s, r.h), Paint(th::grid()));
}

void freqLabels(Ui &ui, const R &r)
{
    FreqScale fs(r);
    for (int f : FR) {
        char b[16];
        if (f >= 1000) snprintf(b, sizeof b, "%dk", f / 1000);
        else snprintf(b, sizeof b, "%d", f);
        ui.text(R((float)fs.x(f) + 3, r.b() - 14, 30, 11), b, th::faint(), F_REGULAR, 10);
    }
}

void drawSpectrum(Ui &ui, const FreqScale &fs, const Spectrum &sp, double sr, const Paint *fill, const Col *line, float lw)
{
    Canvas &c = ui.cv();
    const R &r = fs.r;
    int px0 = (int)std::floor(ui.X(r.x)), px1 = (int)std::floor(ui.X(r.r()));
    if (px1 <= px0 || sr <= 0) return;
    std::vector<Pt> pts;
    std::vector<float> tops;
    pts.reserve(px1 - px0);
    tops.reserve(px1 - px0);
    const int H = (int)sp.db.size();
    const double bpf = sp.n / sr;   // bins per Hz
    for (int px = px0; px < px1; ++px) {
        double f0 = fs.f((px - ui.ox) / ui.s), f1 = fs.f((px + 1 - ui.ox) / ui.s);
        double b0 = f0 * bpf, b1 = f1 * bpf;
        // the loudest of the curve at the pixel's edges and of the bins inside it (the lows: the smooth curve; the
        // highs, many bins to a pixel: their peak)
        float mx = std::max(sp.at(b0), sp.at(b1));
        for (int b = std::max(1, (int)std::ceil(b0)), be = std::min(H - 1, (int)std::floor(b1)); b <= be; ++b) mx = std::max(mx, sp.db[b]);
        float y = ui.Y((float)fs.y(mx, (f0 + f1) * 0.5));
        tops.push_back(y);
        pts.push_back({(float)px + 0.5f, y});
    }
    if (fill) c.columns(px0, tops.data(), (int)tops.size(), ui.Y(r.b()), *fill);
    if (line) c.strokeThin(pts.data(), (int)pts.size(), ui.W(lw), *line);
}

} // namespace gui
} // namespace ts
