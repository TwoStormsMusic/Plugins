#include <chrono>
// Reference renderer: runs a JSFX through ysfx on a raw signal, for sample-by-sample comparison
// with the CLAP port.
//   jsfx_ref <file.jsfx> <in.f64> <out.f64> <srate> <block> [n=v ...] [t<frame>:n=v ...] [dump=var1,var2]
// in.f64: interleaved doubles, 4 channels (in L, in R, sidechain L, sidechain R)
// out.f64: interleaved doubles, 2 channels. With dump=..., <out.f64>.vars gets one line per block.
#include "ysfx.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct Ev { long frame; int slider; double v; };

static void logrep(intptr_t, ysfx_log_level lvl, const char *msg)
{
    if (lvl >= ysfx_log_warning) fprintf(stderr, "[ysfx %s] %s\n", ysfx_log_level_string(lvl), msg);
}

int main(int argc, char **argv)
{
    if (argc < 6) { fprintf(stderr, "usage\n"); return 1; }
    const char *jsfx = argv[1];
    double srate = atof(argv[4]);
    int block = atoi(argv[5]);
    std::vector<Ev> evs;
    std::vector<std::string> dump;
    std::vector<std::pair<std::string, double>> setvars;   // var:name=value, set after @init
    std::vector<int> finals;                                // final=n: print slider n at the end
    int inch = 4, outch = 2;                                // inch=K outch=M: channels in the files
    // the transport (as clap_host_test gives it): tempo=, play=0/1, beat0= (song position at frame 0), ts=N/D;
    // the song position is quantized like CLAP's fixed point so both see the same numbers
    double tempo = 120, beat0 = 0;
    int play = 0, tsn = 4, tsd = 4;
    struct Mev { long frame; uint8_t d[3]; };
    std::vector<Mev> mevs;                                  // m<frame>:status,data1,data2
    for (int i = 6; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("dump=", 0) == 0) {
            std::string r = a.substr(5);
            size_t p = 0;
            while (p < r.size()) {
                size_t q = r.find(',', p);
                if (q == std::string::npos) q = r.size();
                dump.push_back(r.substr(p, q - p));
                p = q + 1;
            }
            continue;
        }
        if (a.rfind("var:", 0) == 0) {
            size_t e = a.find('=');
            setvars.push_back({a.substr(4, e - 4), atof(a.c_str() + e + 1)});
            continue;
        }
        if (a.rfind("final=", 0) == 0) { finals.push_back(atoi(a.c_str() + 6)); continue; }
        if (a.rfind("inch=", 0) == 0) { inch = atoi(a.c_str() + 5); continue; }
        if (a.rfind("tempo=", 0) == 0) { tempo = atof(a.c_str() + 6); continue; }
        if (a.rfind("play=", 0) == 0) { play = atoi(a.c_str() + 5); continue; }
        if (a.rfind("beat0=", 0) == 0) { beat0 = atof(a.c_str() + 6); continue; }
        if (a.rfind("ts=", 0) == 0) { sscanf(a.c_str() + 3, "%d/%d", &tsn, &tsd); continue; }
        if (a[0] == 'm' && isdigit((unsigned char)a[1])) {
            Mev e{};
            int b0 = 0, b1 = 0, b2 = 0;
            if (sscanf(a.c_str() + 1, "%ld:%d,%d,%d", &e.frame, &b0, &b1, &b2) == 4) {
                e.d[0] = (uint8_t)b0; e.d[1] = (uint8_t)b1; e.d[2] = (uint8_t)b2;
                mevs.push_back(e);
            }
            continue;
        }
        if (a.rfind("outch=", 0) == 0) { outch = atoi(a.c_str() + 6); continue; }
        long fr = 0;
        const char *s = a.c_str();
        if (*s == 't') { fr = strtol(s + 1, (char **)&s, 10); if (*s == ':') ++s; }
        int n = atoi(s);
        const char *eq = strchr(s, '=');
        if (!eq) continue;
        evs.push_back({fr, n, atof(eq + 1)});
    }
    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror("in"); return 1; }
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    long frames = bytes / (sizeof(double) * inch);
    std::vector<double> in(frames * inch);
    if (fread(in.data(), sizeof(double), in.size(), f) != in.size()) { fclose(f); return 1; }
    fclose(f);

    ysfx_config_t *cfg = ysfx_config_new();
    ysfx_set_log_reporter(cfg, &logrep);
    ysfx_register_builtin_audio_formats(cfg);
    ysfx_guess_file_roots(cfg, jsfx);
    ysfx_t *fx = ysfx_new(cfg);
    ysfx_config_free(cfg);
    if (!ysfx_load_file(fx, jsfx, 0)) { fprintf(stderr, "load failed\n"); return 1; }
    if (!ysfx_compile(fx, 0)) { fprintf(stderr, "compile failed\n"); return 1; }
    ysfx_set_sample_rate(fx, srate);
    ysfx_set_block_size(fx, block);
    ysfx_init(fx);
    for (auto &e : evs)
        if (e.frame == 0) ysfx_slider_set_value(fx, e.slider - 1, e.v);
    for (auto &v : setvars) {
        ysfx_real *p = ysfx_find_var(fx, v.first.c_str());
        if (p) *p = v.second;
        else fprintf(stderr, "no var %s\n", v.first.c_str());
    }

    std::vector<std::vector<double>> bi(inch), bo(outch);
    for (auto &b : bi) b.resize(block);
    for (auto &b : bo) b.resize(block);
    std::vector<double> out(frames * outch);
    FILE *vf = nullptr;
    std::vector<ysfx_real *> vp;
    if (!dump.empty()) {
        std::string vn = std::string(argv[3]) + ".vars";
        vf = fopen(vn.c_str(), "w");
        for (auto &d : dump) {
            ysfx_real *p = ysfx_find_var(fx, d.c_str());
            if (!p) fprintf(stderr, "no var %s\n", d.c_str());
            vp.push_back(p);
        }
    }
    double tproc = 0;
    for (long pos = 0; pos < frames; pos += block) {
        long n = std::min<long>(block, frames - pos);
        for (auto &e : evs)
            if (e.frame > 0 && e.frame >= pos && e.frame < pos + n) ysfx_slider_set_value(fx, e.slider - 1, e.v);
        const double *ins[16];
        double *outs[16];
        for (int c = 0; c < inch; ++c) {
            for (long i = 0; i < n; ++i) bi[c][i] = in[(pos + i) * inch + c];
            ins[c] = bi[c].data();
        }
        for (int c = 0; c < outch; ++c) outs[c] = bo[c].data();
        ysfx_time_info_t ti{};
        ti.tempo = tempo;
        ti.playback_state = play ? ysfx_playback_playing : 0;
        ti.time_position = pos / srate;
        double bt = beat0 + (play ? pos / srate * tempo / 60 : 0);
        ti.beat_position = (double)llround(bt * 2147483648.0) / 2147483648.0;
        ti.time_signature[0] = (uint32_t)tsn;
        ti.time_signature[1] = (uint32_t)tsd;
        ysfx_set_time_info(fx, &ti);
        for (auto &e : mevs)
            if (e.frame >= pos && e.frame < pos + n) {
                ysfx_midi_event_t me{};
                me.bus = 0;
                me.offset = (uint32_t)(e.frame - pos);
                me.size = 3;
                me.data = e.d;
                ysfx_send_midi(fx, &me);
            }
        auto t0 = std::chrono::steady_clock::now();
        ysfx_process_double(fx, ins, outs, inch, outch, (uint32_t)n);
        tproc += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        { ysfx_midi_event_t me; while (ysfx_receive_midi(fx, &me)) {} }
        for (long i = 0; i < n; ++i)
            for (int c = 0; c < outch; ++c) out[(pos + i) * outch + c] = bo[c][i];
        if (vf) {
            for (size_t k = 0; k < vp.size(); ++k) fprintf(vf, "%s%.17g", k ? " " : "", vp[k] ? *vp[k] : 0.0);
            fprintf(vf, "\n");
        }
    }
    if (vf) fclose(vf);
    f = fopen(argv[3], "wb");
    fwrite(out.data(), sizeof(double), out.size(), f);
    fclose(f);
    fprintf(stderr, "pdc %g\n", ysfx_get_pdc_delay(fx));
    fprintf(stderr, "process time %.2f ms\n", tproc * 1000);
    for (int n : finals) printf("slider%d %.17g\n", n, ysfx_slider_get_value(fx, n - 1));
    ysfx_free(fx);
    return 0;
}
