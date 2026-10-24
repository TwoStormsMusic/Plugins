// The Tuner's pitch detector, offline: tuner_test <mono.f64> <srate> <range 0 guitar 1 bass 2 vocals 3 wide> [hop]
// prints, per analysis frame: time (s), frequency (Hz, 0 = none), clarity, level (dBFS), and the time per frame
#include "core/pitch.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: tuner_test <mono.f64> <srate> <range> [hop]\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    std::vector<double> d;
    double v;
    while (fread(&v, 8, 1, f) == 1) d.push_back(v);
    fclose(f);
    double sr = atof(argv[2]);
    int range = atoi(argv[3]), hop = argc > 4 ? atoi(argv[4]) : 1600;
    ts::PitchDetector p;
    p.setup(sr, range);
    int need = p.need();
    std::vector<float> x(need);
    double tsum = 0;
    int nf = 0;
    for (size_t e = need; e <= d.size(); e += hop) {
        for (int i = 0; i < need; ++i) x[i] = (float)d[e - need + i];
        ts::PitchDetector::Result r;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = p.detect(x.data(), r);
        tsum += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        ++nf;
        printf("%.4f %.6f %.4f %.1f\n", e / sr, ok ? r.f : 0.0, r.clarity, r.rmsDb);
    }
    fprintf(stderr, "frames %d, %.3f ms per frame (window %d)\n", nf, nf ? tsum / nf * 1000 : 0, need);
    return 0;
}
