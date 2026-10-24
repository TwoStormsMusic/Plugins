// The spectrum analyzer the TwoStorms effect windows share (the JSFX's an_fft / an_x / an_y / an_draw, sharper).
// The audio thread keeps the newest 32768 samples of what is analyzed (mono sums) in rings, only while the window
// is open; the window takes an FFT of about the newest 170 ms each frame (8192 points at 44.1 / 48 kHz, 16384 up to
// 96 kHz, 32768 above), lets the levels fall slowly, and draws them on a log frequency scale (20 Hz .. 20 kHz) with a
// 4.5 dB/octave tilt around 1 kHz (so music looks flat). Where a bin is wider than a pixel (the lows) the levels are
// joined by a smooth curve through the bins, so the lows read as a line instead of steps.
#pragma once
#include "ui.h"
#include <atomic>
#include <vector>

namespace ts {

// the rings (written by the audio thread, read by the window)
struct AnFeed {
    static constexpr int N = 32768;
    std::atomic<float> in[N], out[N], sc[N];
    std::atomic<int> ap{0};   // where the next sample goes (published once per block)
    int p = 0;                // the audio thread's own copy
    AnFeed() { clear(); }
    void clear()
    {
        for (int i = 0; i < N; ++i) { in[i].store(0); out[i].store(0); sc[i].store(0); }
        p = 0;
        ap.store(0);
    }
    inline void put(double i, double o, double s = 0)
    {
        in[p].store((float)i, std::memory_order_relaxed);
        out[p].store((float)o, std::memory_order_relaxed);
        sc[p].store((float)s, std::memory_order_relaxed);
        p = (p + 1) & (N - 1);
    }
    void publish() { ap.store(p, std::memory_order_release); }
};

namespace gui {

// levels per bin (0 .. n/2 - 1 of an n-point FFT), falling slowly
class Spectrum {
public:
    int n = 8192;
    std::vector<float> db = std::vector<float>(4096, -200.f);
    void reset()
    {
        for (auto &v : db) v = -200;
    }
    // the FFT size for sample rate sr (about 170 ms), at most what a ring of mask + 1 holds
    static int sizeFor(double sr, int mask);
    // the newest n samples before ap of a ring (mask = its size - 1); dt: the time since the last frame
    void update(const std::atomic<float> *ring, int mask, int ap, double dt, double sr);
    // the level at fractional bin x: a smooth (monotone cubic) curve through the bins
    float at(double x) const;
};

// 20 Hz .. 20 kHz across r on a log scale; levels tilted 4.5 dB/octave around 1 kHz, 90 dB tall
struct FreqScale {
    R r;
    explicit FreqScale(const R &rr = R()) : r(rr) {}
    double x(double f) const;
    double f(double x) const;
    double y(double db, double f) const;
};

// the frequency grid (50 Hz .. 10 kHz): lines (still pass) and labels along the bottom
void freqGrid(Ui &ui, const R &r);
void freqLabels(Ui &ui, const R &r);
// a spectrum across fs.r as filled columns (fill) and / or a line; dynamic pass; sr: the sample rate
void drawSpectrum(Ui &ui, const FreqScale &fs, const Spectrum &sp, double sr, const Paint *fill, const Col *line, float lw = 1.4f);

} // namespace gui
} // namespace ts
