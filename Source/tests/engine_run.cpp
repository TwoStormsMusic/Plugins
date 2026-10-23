// Runs the ported Compressor engine + built-in Act offline (no CLAP), with the same arguments as
// tools/jsfx_ref, so the two outputs can be compared sample by sample.
//   engine_run <in.f64> <out.f64> <srate> <block> [n=v ...] [t<frame>:n=v ...]
#include "../src/core/act.h"
#include "../src/plugins/compressor/comp_engine.h"
#include "../src/plugins/compressor/comp_params.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ts;

struct Ev { long frame; int id; double v; };

struct Host : ActHost, comp::EngineHost {
    ParamValues P{comp::table()};
    double AB[256] = {};
    double pv(int id) const override { return P.get(id); }
    double abv(int id) const override { return AB[id]; }
    void setFromAudio(int id, double v) override { P.put(id, v); }
    void putQuiet(int id, double v) override { P.put(id, v); }
    double clampParam(int id, double v) const override { return comp::table().clamp(id, v); }
    void hkTgStart() override {}
};

int main(int argc, char **argv)
{
    if (argc < 5) return 1;
    double srate = atof(argv[3]);
    int block = atoi(argv[4]);
    std::vector<Ev> evs;
    bool tgstart = false;
    std::vector<int> finals;
    for (int i = 5; i < argc; ++i) {
        const char *s = argv[i];
        if (!strcmp(s, "tgstart")) { tgstart = true; continue; }
        if (!strncmp(s, "final=", 6)) { finals.push_back(atoi(s + 6)); continue; }
        long fr = 0;
        if (*s == 't') { fr = strtol(s + 1, (char **)&s, 10); if (*s == ':') ++s; }
        const char *eq = strchr(s, '=');
        if (!eq) continue;
        evs.push_back({fr, atoi(s), atof(eq + 1)});
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long frames = ftell(f) / (sizeof(double) * 4);
    fseek(f, 0, SEEK_SET);
    std::vector<double> in(frames * 4);
    if (fread(in.data(), sizeof(double), in.size(), f) != in.size()) return 1;
    fclose(f);

    Host h;
    Act act(h, shm::KIND_POINT, shm::SK_COMPRESSOR);
    comp::UiLink ui;
    comp::Display view;
    comp::Engine eng(h, act, ui, view);
    act.init(srate);
    eng.init(srate);
    if (getenv("TS_EXACT")) eng.satFix = false;
    for (auto &e : evs)
        if (e.frame == 0) h.P.put(e.id, comp::table().clamp(e.id, e.v));
    for (int id : comp::table().abIds()) h.AB[id] = h.P.get(id);
    act.sliderSync();
    if (tgstart) act.tgs = 1;

    std::vector<double> b[6];
    for (auto &x : b) x.resize(block);
    std::vector<double> out(frames * 2);
    for (long pos = 0; pos < frames; pos += block) {
        int n = (int)std::min<long>(block, frames - pos);
        bool ch = false;
        for (auto &e : evs)
            if (e.frame > 0 && e.frame >= pos && e.frame < pos + n) { h.P.put(e.id, comp::table().clamp(e.id, e.v)); ch = true; }
        if (ch) act.sliderSync();
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 4; ++c) b[c][i] = in[(pos + i) * 4 + c];
        const double *ins[2] = {b[0].data(), b[1].data()};
        const double *sc[2] = {b[2].data(), b[3].data()};
        double *outs[2] = {b[4].data(), b[5].data()};
        eng.block(n);
        act.block(n);
        eng.samples(ins, sc, outs, n);
        act.process(outs, 2, n);
        eng.post(outs, n);
        act.audioKey(outs, 2, n);
        for (int i = 0; i < n; ++i) { out[(pos + i) * 2] = outs[0][i]; out[(pos + i) * 2 + 1] = outs[1][i]; }
    }
    f = fopen(argv[2], "wb");
    fwrite(out.data(), sizeof(double), out.size(), f);
    fclose(f);
    fprintf(stderr, "latency %d\n", eng.latency());
    for (int n : finals) printf("slider%d %.17g\n", n, h.P.get(n));
    return 0;
}
