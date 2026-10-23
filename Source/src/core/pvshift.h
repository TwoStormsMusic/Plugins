// A pitch shifter for chords as well as single notes: a phase vocoder that moves each spectral peak, with the bins
// around it, to its new frequency (Laroche & Dolson, "New phase-vocoder techniques for pitch-shifting, harmonizing
// and other exotic effects", 1999: peak-locked shifting; the phases start over on a sharp attack). Frames of N
// samples (Hann, zero-phase), hop N / 4, two channels; the latency is N. One analysis per channel feeds every voice
// (each with its own ratio), so several shifts of the same sound cost one forward FFT per channel per hop.
#pragma once
#include <vector>

namespace ts {

class PvShift {
public:
    static constexpr int MAXV = 4;
    static int sizeFor(double sr);   // N: 4096 up to 50 kHz, 8192 up to 100 kHz, 16384 above
    void init(int n);
    void reset();
    int size() const { return N; }
    // voice v's ratio (2^(semitones / 12)); 1 = off (that voice is not computed and gives silence)
    void setRatio(int v, double r);
    // one sample per channel in; then out(v, c) is voice v's output for it (the input N samples ago, shifted)
    void push(double x0, double x1)
    {
        for (int v = 0; v < MAXV; ++v) {
            if (!vo[v].on) continue;
            for (int c = 0; c < 2; ++c) {
                double *r = vo[v].ring[c].data();
                y[v][c] = r[pos];
                r[pos] = 0;
            }
        }
        in[0][pos] = x0;
        in[1][pos] = x1;
        pos = (pos + 1) & (N - 1);
        if (++cnt >= H) { cnt = 0; frame(); }
    }
    double out(int v, int c) const { return y[v][c]; }

private:
    int N = 0, H = 0, pos = 0, cnt = 0;
    std::vector<double> win, buf;
    std::vector<double> in[2];
    struct Chan {
        std::vector<double> xr, xi, pxr, pxi, m2;   // this frame's bins, the last frame's, |X|^2
        std::vector<int> pk, lo, hi, own, pown;      // peaks, their regions, which region each bin is in (now, last frame)
        std::vector<double> om, ph;                  // each peak's frequency (radians per sample) and phase
        int npk = 0;
        double en = 0, pen = 0;
        bool start = true, attack = false;
    } ch[2];
    struct Voice {
        double r = 1;
        bool on = false, fresh = true;
        std::vector<double> psi[2], ppsi[2];   // the synthesis phase of each peak (this frame, the last)
        std::vector<double> ring[2];           // the output being added up, indexed by output time mod N
    } vo[MAXV];
    double y[MAXV][2] = {};
    void frame();
    void analyse(int c);
    void synth(Voice &v, int c);
};

} // namespace ts
