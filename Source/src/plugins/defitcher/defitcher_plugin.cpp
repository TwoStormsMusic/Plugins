// TwoStorms Defitcher: a de-esser with three bands (frequency, Q, threshold, ratio, attack, release, maximum
// reduction). Targeted takes the selected frequencies down (a polarity-inverted copy of the band); Classic turns the
// whole signal down by the same amount. Detector on Stereo, Mid/Side, Mid or Side, from the input or the sidechain,
// Auto-Detect frequencies, optional exact lookahead (that latency), band and removed-audio monitoring, dry/wet.
// Port of twostorms_defitcher.jsfx.
#include "defitcher_plugin.h"
#include "../../core/eelrand.h"
#include "../../gui/analyzer.h"
#include "../../gui/effect_view.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ts {
namespace defitcher {

using namespace gui;

enum { P_MODE = 50, P_DET = 51, P_LA = 52, P_AUTO = 53, P_MON = 54, P_B = 55, P_MIX = 79, P_SC = 80, P_AB = 81 };
// band b: P_B + 8 b + k
enum { K_ON = 0, K_FREQ, K_Q, K_THR, K_MAX, K_RATIO, K_ATT, K_REL };

// ---------------------------------------------------------------- parameters
static void f_pct(double v, char *b, int n) { snprintf(b, n, "%.0f%%", v); }
static void f_hz(double v, char *b, int n) { snprintf(b, n, "%.0f Hz", v); }
static void f_q(double v, char *b, int n) { snprintf(b, n, "%.2f", v); }
static void f_db(double v, char *b, int n) { snprintf(b, n, "%.1f dB", v); }
static void f_ratio(double v, char *b, int n) { snprintf(b, n, "%.1f : 1", v); }
static void f_att(double v, char *b, int n) { snprintf(b, n, "%.2f ms", v); }
static void f_rel(double v, char *b, int n) { snprintf(b, n, "%.0f ms", v); }

static const char *const L_MODE[] = {"Targeted", "Classic"};
static const char *const L_DET[] = {"Stereo", "Mid/Side", "Mid", "Side"};
static const char *const L_LA[] = {"Off", "1 ms", "5 ms"};
static const char *const L_AUTO[] = {"Off", "Learn", "Reset + Learn"};
static const char *const L_MON[] = {"Off", "Band", "Removed"};

static const ParamTable &table()
{
    static ParamTable t = [] {
        ParamTable t;
        addActParams(t, true);
        const char *M = "Defitcher";
        const uint32_t H = PF_HOST | PF_AB;
        t.addEnum(P_MODE, "Reduction", {L_MODE[0], L_MODE[1]}, 0, H, M);
        t.addEnum(P_DET, "Detector mode", {L_DET[0], L_DET[1], L_DET[2], L_DET[3]}, 0, H, M);
        t.addEnum(P_LA, "Lookahead", {L_LA[0], L_LA[1], L_LA[2]}, 0, H, M);
        t.addEnum(P_AUTO, "Auto-Detect frequencies", {L_AUTO[0], L_AUTO[1], L_AUTO[2]}, 0, PF_HOST, M);
        t.addEnum(P_MON, "Monitor", {L_MON[0], L_MON[1], L_MON[2]}, 0, PF_HOST, M);
        static const char *bn[3][8] = {
            {"Band 1 enable", "Band 1 frequency", "Band 1 Q", "Band 1 threshold", "Band 1 max reduction", "Band 1 ratio", "Band 1 attack", "Band 1 release"},
            {"Band 2 enable", "Band 2 frequency", "Band 2 Q", "Band 2 threshold", "Band 2 max reduction", "Band 2 ratio", "Band 2 attack", "Band 2 release"},
            {"Band 3 enable", "Band 3 frequency", "Band 3 Q", "Band 3 threshold", "Band 3 max reduction", "Band 3 ratio", "Band 3 attack", "Band 3 release"}};
        static const char *mods[3] = {"Band 1", "Band 2", "Band 3"};
        for (int b = 0; b < 3; ++b) {
            int p = P_B + 8 * b;
            t.addEnum(p + K_ON, bn[b][0], {"Off", "On"}, b == 1 ? 1 : 0, H | PF_BOOL, mods[b]);
            t.add(p + K_FREQ, bn[b][1], 2000, 16000, b == 0 ? 4500 : (b == 1 ? 6500 : 9000), H | PF_INT, f_hz, mods[b]).unit = "Hz";
            t.add(p + K_Q, bn[b][2], 0.5, 8, 2, H, f_q, mods[b]);
            t.add(p + K_THR, bn[b][3], -60, 0, -30, H, f_db, mods[b]).unit = "dB";
            t.add(p + K_MAX, bn[b][4], 0, 30, 12, H, f_db, mods[b]).unit = "dB";
            t.add(p + K_RATIO, bn[b][5], 1, 20, 4, H, f_ratio, mods[b]);
            t.add(p + K_ATT, bn[b][6], 0.05, 10, 0.5, H, f_att, mods[b]).unit = "ms";
            t.add(p + K_REL, bn[b][7], 1, 300, 40, H | PF_INT, f_rel, mods[b]).unit = "ms";
        }
        t.add(P_MIX, "Dry/wet", 0, 100, 100, H | PF_INT, f_pct, M).unit = "%";
        t.addEnum(P_SC, "External sidechain", {"Off", "On"}, 0, PF_HOST | PF_BOOL, M);
        t.addEnum(P_AB, "A/B set", {"A", "B"}, 0, PF_HOST, M);
        return t;
    }();
    return t;
}

// factory presets: Instruments, Buses, Creative, Weird; written band by band
namespace {
struct DfP {
    std::vector<std::pair<int, double>> v;
    // band b on at f (Q), cutting up to mx dB over thr
    DfP &band(int b, double f, double q, double thr, double mx, double ratio = 4, double att = 0.5, double rel = 40)
    {
        int p = P_B + 8 * b;
        v.push_back({p + K_ON, 1}); v.push_back({p + K_FREQ, f}); v.push_back({p + K_Q, q}); v.push_back({p + K_THR, thr});
        v.push_back({p + K_MAX, mx}); v.push_back({p + K_RATIO, ratio}); v.push_back({p + K_ATT, att}); v.push_back({p + K_REL, rel});
        return *this;
    }
    DfP &off(int b) { v.push_back({P_B + 8 * b + K_ON, 0}); return *this; }
    DfP &set(int id, double x) { v.push_back({id, x}); return *this; }
    operator std::string() const { return presetStr(v); }
};
DfP D() { return DfP(); }
}

static const std::vector<FactoryPreset> &factoryPresets()
{
    static std::vector<FactoryPreset> v = {
        // ---- vocals
        {"Instruments/Vocals/Male vocal", "52=1 55=1 56=4200 57=2.0 58=-32 59=10 60=3.0 61=0.5 62=40 63=1 64=6000 65=2.0 66=-32 67=10 68=3.0 69=0.5 70=40 71=0 72=8500 73=2.0 74=-32 75=8 76=3.0 77=0.5 78=40"},
        {"Instruments/Vocals/Female vocal", "52=1 55=1 56=5800 57=2.0 58=-32 59=10 60=3.0 61=0.5 62=40 63=1 64=7800 65=2.0 66=-32 67=12 68=3.5 69=0.5 70=40 71=1 72=10500 73=1.5 74=-34 75=8 76=3.0 77=0.5 78=40"},
        {"Instruments/Vocals/Gentle", "52=1 55=0 56=4500 57=1.5 58=-30 59=6 60=2.5 61=1.0 62=80 63=1 64=6500 65=1.0 66=-28 67=6 68=2.5 69=1.0 70=80 71=0 72=9000 73=1.5 74=-30 75=6 76=2.5 77=1.0 78=80"},
        {"Instruments/Vocals/Heavy", "52=2 55=1 56=4800 57=2.5 58=-38 59=20 60=8.0 61=0.2 62=30 63=1 64=6800 65=2.5 66=-38 67=20 68=8.0 69=0.2 70=30 71=1 72=9500 73=2.0 74=-40 75=16 76=8.0 77=0.2 78=30"},
        {"Instruments/Vocals/Singing (live)", "50=0 51=0 52=0 55=0 56=4500 57=2 58=-30 59=12 60=4 61=0.5 62=40 63=1 64=6500 65=2 66=-32 67=10 68=4 69=0.5 70=50 71=1 72=9000 73=2 74=-34 75=8 76=3 77=0.5 78=60 79=100"},
        {"Instruments/Vocals/Spoken word", "50=0 51=0 52=0 55=1 56=5000 57=2 58=-30 59=10 60=4 61=0.5 62=40 63=1 64=7000 65=2 66=-32 67=8 68=4 69=0.5 70=50 71=0 72=9000 73=2 74=-30 75=12 76=4 77=0.5 78=40 79=100"},
        {"Instruments/Vocals/Podcast", "52=2 55=1 56=4800 57=1.5 58=-30 59=12 60=4.0 61=0.3 62=60 63=1 64=7000 65=1.5 66=-30 67=12 68=4.0 69=0.3 70=60 71=0 72=9500 73=2.0 74=-30 75=8 76=4.0 77=0.5 78=40"},
        {"Instruments/Vocals/Punk", "50=0 51=0 52=0 55=1 56=4000 57=1.5 58=-28 59=12 60=5 61=0.5 62=40 63=1 64=6500 65=2 66=-30 67=12 68=5 69=0.5 70=40 71=0 72=9000 73=2 74=-30 75=12 76=4 77=0.5 78=40 79=100"},
        {"Instruments/Vocals/Pop punk", "50=0 51=0 52=0 55=0 56=4500 57=2 58=-30 59=12 60=4 61=0.5 62=40 63=1 64=6500 65=2 66=-30 67=10 68=4 69=0.5 70=40 71=1 72=9000 73=2 74=-32 75=8 76=3 77=0.5 78=50 79=100"},
        {"Instruments/Vocals/Metal: screams", "50=0 51=0 52=0 55=1 56=3500 57=1 58=-26 59=12 60=6 61=0.5 62=40 63=1 64=6000 65=1.5 66=-28 67=12 68=5 69=0.5 70=40 71=0 72=9000 73=2 74=-30 75=12 76=4 77=0.5 78=40 79=100"},
        {"Instruments/Vocals/Rock", "50=0 51=0 52=0 55=0 56=4500 57=2 58=-30 59=12 60=4 61=0.5 62=40 63=1 64=6000 65=2 66=-30 67=10 68=4 69=0.5 70=40 71=0 72=9000 73=2 74=-30 75=12 76=4 77=0.5 78=40 79=100"},
        {"Instruments/Vocals/Jazz", "50=0 51=0 52=0 55=0 56=4500 57=2 58=-30 59=12 60=4 61=0.5 62=40 63=1 64=6500 65=2.5 66=-34 67=6 68=2.5 69=1 70=60 71=0 72=9000 73=2 74=-30 75=12 76=4 77=0.5 78=40 79=100"},
        {"Instruments/Vocals/Pop", "50=0 51=0 52=0 55=1 56=5500 57=2.5 58=-32 59=8 60=4 61=0.5 62=40 63=1 64=7500 65=2.5 66=-32 67=8 68=4 69=0.5 70=40 71=1 72=10000 73=2 74=-34 75=6 76=3 77=0.5 78=50 79=100"},
        {"Instruments/Vocals/Rap: fast and tight", D().band(0, 5000, 2, -30, 8, 6, 0.2, 25).band(1, 7500, 2, -30, 8, 6, 0.2, 25).set(P_LA, 1)},
        {"Instruments/Vocals/Whisper / ASMR (soft, wide)", D().band(0, 4500, 0.8, -40, 6, 2, 1, 80).band(1, 8000, 0.8, -40, 6, 2, 1, 80).band(2, 12000, 1, -42, 4, 2, 1, 80)},
        {"Instruments/Vocals/Backing stack: heavy (esses pile up)", D().band(0, 5500, 1.5, -36, 14, 6, 0.3, 40).band(1, 8000, 1.5, -36, 14, 6, 0.3, 40)},
        // ---- guitar
        {"Instruments/Guitar/Clean: string squeak tamer", "50=0 52=0 55=1 56=3000 57=2 58=-34 59=6 60=3 61=0.5 62=60 63=0 64=6500 65=2 66=-30 67=12 68=4 69=0.5 70=40 71=0 72=9000 73=2 74=-30 75=12 76=4 77=0.5 78=40 79=100"},
        {"Instruments/Guitar/Funk: tame the scratch", "50=0 52=0 55=1 56=3500 57=1.5 58=-32 59=8 60=4 61=0.3 62=50 63=1 64=6000 65=2 66=-32 67=6 68=3 69=0.3 70=50 71=0 72=9000 73=2 74=-30 75=12 76=4 77=0.5 78=40 79=100"},
        {"Instruments/Guitar/Rock: pick click softener", "50=0 52=0 55=1 56=4000 57=2 58=-30 59=6 60=4 61=0.2 62=40 63=0 64=6500 65=2 66=-30 67=12 68=4 69=0.5 70=40 71=0 72=9000 73=2 74=-30 75=12 76=4 77=0.5 78=40 79=100"},
        {"Instruments/Guitar/Metal: fizz tamer", "50=0 52=0 55=1 56=4500 57=1.2 58=-32 59=8 60=3 61=1 62=80 63=1 64=7000 65=1.5 66=-32 67=8 68=3 69=1 70=80 71=0 72=9000 73=2 74=-30 75=12 76=4 77=0.5 78=40 79=100"},
        {"Instruments/Guitar/Acoustic: fret squeak and pick", D().off(1).band(0, 2800, 1.5, -34, 8, 4, 0.5, 60).band(2, 6000, 1.5, -32, 6, 4, 0.3, 40)},
        // ---- bass
        {"Instruments/Bass/Slap: tame the click", "50=0 51=0 52=0 55=0 56=2500 57=2 58=-30 59=12 60=4 61=0.5 62=40 63=1 64=3500 65=1.5 66=-30 67=6 68=3 69=0.3 70=60 71=0 72=9000 73=2 74=-30 75=12 76=4 77=0.5 78=40 79=100"},
        {"Instruments/Bass/Distorted: tame the fizz", "50=0 51=0 52=0 55=0 56=4500 57=2 58=-30 59=12 60=4 61=0.5 62=40 63=1 64=5500 65=1.2 66=-32 67=8 68=3 69=1 70=80 71=0 72=9000 73=2 74=-30 75=12 76=4 77=0.5 78=40 79=100"},
        {"Instruments/Bass/Round wounds: less zing", D().off(1).band(0, 2500, 1.2, -36, 6, 3, 1, 80)},
        // ---- drums
        {"Instruments/Drums/Hi-hat: the ice pick (8-10 kHz)", D().off(1).band(0, 9000, 2, -32, 8, 4, 0.2, 30)},
        {"Instruments/Drums/Overheads: tame the wash (6-12 kHz)", D().band(0, 6500, 1, -32, 4, 3, 1, 80).band(1, 10000, 1, -32, 4, 3, 1, 80)},
        {"Instruments/Drums/Snare top: soften the crack", D().off(1).band(0, 5000, 1.2, -28, 6, 4, 0.1, 30)},
        // ---- others
        {"Instruments/Strings & brass/Trumpet: edge (3-5 kHz)", D().off(1).band(0, 3800, 1.2, -30, 6, 3, 1, 60)},
        {"Instruments/Strings & brass/Violin: rosin scratch", D().off(1).band(0, 3200, 1.5, -32, 6, 3, 1, 60).band(2, 7500, 1.5, -34, 4, 3, 1, 60)},
        {"Instruments/Strings & brass/Flute: breath noise", D().off(1).band(0, 7000, 1, -36, 6, 3, 1, 80)},
        // ---- buses
        {"Buses/Instrument bus/Vocal bus: gentle", D().band(0, 5500, 1.5, -32, 5, 3, 0.5, 50).band(1, 8000, 1.5, -32, 5, 3, 0.5, 50)},
        {"Buses/Instrument bus/Drum bus: cymbals", D().band(0, 7000, 1, -30, 4, 3, 1, 60).band(1, 11000, 1, -30, 3, 3, 1, 60)},
        {"Buses/Mixbus/Subtle esses (3 dB)", D().band(0, 6000, 1.5, -30, 3, 3, 0.5, 50).band(1, 8500, 1.5, -30, 3, 3, 0.5, 50)},
        {"Buses/Master/Transparent esses (mid only, 2 dB)", D().band(0, 6500, 1.5, -28, 2, 2, 0.5, 60).set(P_DET, 2).set(P_LA, 1)},
        // ---- creative
        {"Creative/Dark vocal (classic: everything above 4k)", D().off(1).band(0, 4000, 0.5, -36, 10, 4, 1, 80).set(P_MODE, 1)},
        {"Creative/Reverb esses: side only", D().band(0, 6000, 1, -40, 12, 6, 0.5, 60).band(1, 9000, 1, -40, 12, 6, 0.5, 60).set(P_DET, 3)},
        {"Creative/Lisp (overdone on purpose)", D().band(0, 4500, 1, -46, 24, 20, 0.05, 120).band(1, 7000, 1, -46, 24, 20, 0.05, 120).band(2, 10000, 1, -46, 24, 20, 0.05, 120)},
        // ---- weird
        {"Weird/Swallowed consonants (slow release)", D().band(0, 3000, 0.5, -44, 30, 20, 0.05, 300).band(1, 6000, 0.5, -44, 30, 20, 0.05, 300).set(P_MODE, 1)},
        {"Weird/Pumping top (16 kHz)", D().off(1).band(2, 14000, 0.8, -50, 24, 20, 0.05, 150).set(P_MODE, 1)},
        {"Weird/Half the hiss (dry/wet 50%)", D().band(0, 5000, 0.5, -50, 30, 20, 0.05, 40).band(1, 9000, 0.5, -50, 30, 20, 0.05, 40).set(P_MIX, 50).set(P_MODE, 1)},
    };
    return v;
}

static int laSamples(double la, double sr)
{
    int t = (int)std::floor(la + 0.5);
    return t == 1 ? (int)std::floor(0.001 * sr + 0.5) : (t == 2 ? (int)std::floor(0.005 * sr + 0.5) : 0);
}

// a complex FFT of 2048 points (re, im interleaved), natural order out (EEL2's fft + fft_permute; only powers are used)
static void fft2048(double *x)
{
    const int N = 2048;
    static double cs[N / 2], sn[N / 2];
    static bool made = false;
    if (!made) {
        for (int i = 0; i < N / 2; ++i) { cs[i] = std::cos(2 * PI * i / N); sn[i] = -std::sin(2 * PI * i / N); }
        made = true;
    }
    for (int i = 1, j = 0; i < N; ++i) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(x[2 * i], x[2 * j]); std::swap(x[2 * i + 1], x[2 * j + 1]); }
    }
    for (int len = 2; len <= N; len <<= 1) {
        int half = len / 2, step = N / len;
        for (int i = 0; i < N; i += len)
            for (int k = 0; k < half; ++k) {
                double wr = cs[k * step], wi = sn[k * step];
                int a = i + k, b = a + half;
                double xr = x[2 * b] * wr - x[2 * b + 1] * wi, xi = x[2 * b] * wi + x[2 * b + 1] * wr;
                x[2 * b] = x[2 * a] - xr;
                x[2 * b + 1] = x[2 * a + 1] - xi;
                x[2 * a] += xr;
                x[2 * a + 1] += xi;
            }
    }
}

// ---------------------------------------------------------------- the engine
struct Display {
    std::atomic<double> ugr[4];   // the loudest reduction of each band (and of all) over the last block, dB
    std::atomic<double> accw{0};
    std::atomic<int> scActive{0}, side{0}, la{0};
    AnFeed an;
    Display()
    {
        for (auto &u : ugr) u.store(0);
    }
};

class EngineHost {
public:
    virtual ~EngineHost() {}
    virtual double pv(int id) const = 0;
    virtual double abv(int id) const = 0;
    virtual void setAudio(int id, double v) = 0;
};

// one band: a bandpass (TPT state-variable, unity peak), its delay line for the lookahead, the detector and the cut
struct Band {
    double ic1l = 0, ic2l = 0, ic1r = 0, ic2r = 0, sc1l = 0, sc2l = 0, sc1r = 0, sc2r = 0;
    double a1 = 0, a2 = 0, a3 = 0, kf = 0, enva = 0, envb = 0;
    double on = 0, thr_db = 0, maxred = 0, inv_ratio = 0, atk_c = 0, rel_c = 0;
    double base[4096] = {};
    double gra = 0, grb = 0, cutl = 0, cutr = 0, lisl = 0, lisr = 0;

    void off(int wi)
    {
        gra = grb = cutl = cutr = lisl = lisr = 0;
        ic1l = ic2l = ic1r = ic2r = sc1l = sc2l = sc1r = sc2r = enva = envb = 0;
        base[wi] = 0;
        base[2048 + wi] = 0;
    }
    void params(double en, double thr, double mr, double ir, double ac, double rc)
    {
        on = en; thr_db = thr; maxred = mr; inv_ratio = ir; atk_c = ac; rel_c = rc;
    }
    void freq(double f, double q, double sr)
    {
        double fc = emin(f, sr * 0.45), g = std::tan(PI * fc / sr);
        kf = 1 / q;
        a1 = 1 / (1 + g * (g + kf));
        a2 = g * a1;
        a3 = g * a2;
    }
    void run(double inl, double inr, double sl, double sr, bool usesc, int wi, int ri, int md)
    {
        // bandpass on L and R
        double v3 = inl - ic2l, v1 = a1 * ic1l + a2 * v3, v2 = ic2l + a2 * ic1l + a3 * v3;
        ic1l = 2 * v1 - ic1l;
        ic2l = 2 * v2 - ic2l;
        double bl = kf * v1;
        v3 = inr - ic2r; v1 = a1 * ic1r + a2 * v3; v2 = ic2r + a2 * ic1r + a3 * v3;
        ic1r = 2 * v1 - ic1r;
        ic2r = 2 * v2 - ic2r;
        double br = kf * v1;
        // the band signal waits as long as the dry one
        base[wi] = bl;
        base[2048 + wi] = br;
        double dl2 = base[ri], dr2 = base[2048 + ri];
        // the detector listens to the band, or to the same band of the sidechain
        double dtl = bl, dtr = br;
        if (usesc) {
            v3 = sl - sc2l; v1 = a1 * sc1l + a2 * v3; v2 = sc2l + a2 * sc1l + a3 * v3;
            sc1l = 2 * v1 - sc1l;
            sc2l = 2 * v2 - sc2l;
            dtl = kf * v1;
            v3 = sr - sc2r; v1 = a1 * sc1r + a2 * v3; v2 = sc2r + a2 * sc1r + a3 * v3;
            sc1r = 2 * v1 - sc1r;
            sc2r = 2 * v2 - sc2r;
            dtr = kf * v1;
        }
        double m = (dtl + dtr) * 0.5, s = (dtl - dtr) * 0.5;
        double da = emax(std::fabs(dtl), std::fabs(dtr)), db = da;
        if (md == 1) { da = std::fabs(m); db = std::fabs(s); }
        if (md == 2) { da = std::fabs(m); db = da; }
        if (md == 3) { da = std::fabs(s); db = da; }
        if (da > enva) enva += atk_c * (da - enva);
        else enva += rel_c * (da - enva);
        if (db > envb) envb += atk_c * (db - envb);
        else envb += rel_c * (db - envb);
        // gain reduction in dB
        gra = 20 * std::log10(emax(enva, 0.000000001)) - thr_db;
        gra = gra > 0 ? emin(gra * (1 - inv_ratio), maxred) : 0;
        grb = 20 * std::log10(emax(envb, 0.000000001)) - thr_db;
        grb = grb > 0 ? emin(grb * (1 - inv_ratio), maxred) : 0;
        gra *= on;
        grb *= on;
        // the amount of inverted band to mix in: at the band's centre the level goes down by gr dB
        double ga = 1 - std::exp(-gra * 0.11512925), gb = 1 - std::exp(-grb * 0.11512925);
        cutl = ga * dl2;
        cutr = ga * dr2;
        if (md == 1) {
            double dm = (dl2 + dr2) * 0.5, ds = (dl2 - dr2) * 0.5;
            cutl = ga * dm + gb * ds;
            cutr = ga * dm - gb * ds;
        }
        lisl = on * dl2;
        lisr = on * dr2;
    }
};

class Engine {
public:
    Engine(EngineHost &h, Act &a, UiLink &u, Display &d) : H(h), A(a), U(u), V(d) {}
    AbMatch ab;

    void init(double sr)
    {
        srate = sr;
        if (!nonceDrawn) { eel_rand(1000000); nonceDrawn = true; }
        for (Band &b : db) b = Band();
        memset(DRY, 0, sizeof DRY); memset(D_AR, 0, sizeof D_AR); memset(D_ASR, 0, sizeof D_ASR);
        memset(D_ACC, 0, sizeof D_ACC);
        for (int i = 0; i < 2048; ++i) D_WIN[i] = 0.5 - 0.5 * std::cos(2 * PI * i / 2048);
        d_binhz = srate / 2048;
        d_ka = std::floor(150 / d_binhz);
        d_kb = std::floor(3800 / d_binhz);
        d_kc = std::floor(emin(12000, srate * 0.45) / d_binhz);
        d_kp0 = std::floor(3000 / d_binhz);
        d_kp1 = std::floor(emin(13000, srate * 0.45) / d_binhz);
        d_pm = d_kp1 - d_kp0;
        d_smw = emax(1, std::floor(250 / d_binhz + 0.5));
        d_accw = 0; d_fqlock = false;
        d_ap = 0; d_wp = 0; d_awp = 0; d_hop = 0;
        d_sclenv = 0; d_screnv = 0; d_pauto = 0; d_fqinit = false; d_freqdirty = false;
        d_pk[0] = d_pk[1] = d_pk[2] = d_pk[3] = 0;
        V.an.clear();
    }
    void reset()
    {
        memset(DRY, 0, sizeof DRY);
        for (Band &b : db) {
            b.ic1l = b.ic2l = b.ic1r = b.ic2r = b.sc1l = b.sc2l = b.sc1r = b.sc2r = b.enva = b.envb = 0;
            memset(b.base, 0, sizeof b.base);
        }
    }

    void block(int samplesblock)
    {
        ab.block(U, A.now(), H.pv(P_AB) >= 0.5, samplesblock, srate);
        V.side.store(ab.side ? 1 : 0, std::memory_order_relaxed);
        d_uion = ab.uion;
        d_cls = cpv(P_MODE) >= 0.5;
        d_mode = iclamp((int)std::floor(cpv(P_DET) + 0.5), 0, 3);
        d_la = laSamples(cpv(P_LA), srate);
        V.la.store(d_la, std::memory_order_relaxed);
        d_auto = (int)std::floor(cpv(P_AUTO) + 0.5);
        if (d_auto != d_pauto) {
            if (d_auto == 2) { memset(D_ACC, 0, sizeof D_ACC); d_accw = 0; d_fqlock = false; }
            if (d_auto == 1 && d_pauto == 0) d_fqlock = false;
            d_pauto = d_auto;
        }
        // a detected frequency shows up in the parameters (the automation and the window follow it)
        if (d_freqdirty) {
            d_freqdirty = false;
            H.setAudio(P_B + 1, std::floor(d_fq[0] + 0.5));
            H.setAudio(P_B + 9, std::floor(d_fq[1] + 0.5));
            H.setAudio(P_B + 17, std::floor(d_fq[2] + 0.5));
        }
        // while Auto-Detect is on, the frequencies come from the analyser
        if (d_auto == 0 || !d_fqinit) {
            d_fq[0] = cpv(P_B + 1); d_fq[1] = cpv(P_B + 9); d_fq[2] = cpv(P_B + 17);
            d_fqinit = true;
        }
        for (int b = 0; b < 3; ++b) {
            int p = P_B + 8 * b;
            d_q[b] = cpv(p + K_Q);
            db[b].freq(d_fq[b], d_q[b], srate);
        }
        for (int b = 0; b < 3; ++b) {
            int p = P_B + 8 * b;
            db[b].params(cpv(p + K_ON), cpv(p + K_THR), cpv(p + K_MAX), 1 / cpv(p + K_RATIO), tc(cpv(p + K_ATT)), tc(cpv(p + K_REL)));
        }
        d_usesc = cpv(P_SC) >= 0.5;
        d_mon = (int)std::floor(cpv(P_MON) + 0.5);
        d_mixv = cpv(P_MIX) * 0.01;
        // the loudest reduction of each band since the last block, for the window's meters
        for (int i = 0; i < 4; ++i) { V.ugr[i].store(d_pk[i], std::memory_order_relaxed); d_pk[i] = 0; }
        V.accw.store(d_accw, std::memory_order_relaxed);
        V.scActive.store(d_sclenv > 0.0000001 || d_screnv > 0.0000001, std::memory_order_relaxed);
    }

    void afterAct()
    {
        if (!idDrawn && A.myid != 0) { eel_rand(899999); idDrawn = true; }
    }

    void samples(const double *const *in, const double *const *sc, double *const *out, int n)
    {
        if ((int)br0.size() < n) { br0.resize(n); br1.resize(n); bq0.resize(n); bq1.resize(n); }
        for (int i = 0; i < n; ++i) {
            double xl = in[0][i], xr = in[1][i];
            double s2 = sc ? sc[0][i] : 0, s3 = sc ? sc[1][i] : 0;
            // the sidechain (a mono sidechain on input 3 is used for both sides)
            double scl = s2;
            d_sclenv = emax(std::fabs(s2), d_sclenv * 0.99995);
            d_screnv = emax(std::fabs(s3), d_screnv * 0.99995);
            double scr = d_screnv > 0.0000001 ? s3 : s2;
            // the dry signal waits for the lookahead (the detectors don't)
            DRY[d_wp] = xl;
            DRY[2048 + d_wp] = xr;
            int rp = (d_wp - d_la + 2048) & 2047;
            double dl = DRY[rp], dr = DRY[2048 + rp];
            br0[i] = xl; br1[i] = xr; bq0[i] = dl; bq1[i] = dr;
            for (Band &b : db) {
                if (truth(b.on)) b.run(xl, xr, scl, scr, d_usesc, d_wp, rp, d_mode);
                else b.off(d_wp);
            }
            // the deepest reduction (dB) any band asks for, on the (Mid or left + right) side and on the Side
            double ga = emax(emax(db[0].gra, db[1].gra), db[2].gra), gb = emax(emax(db[0].grb, db[1].grb), db[2].grb);
            double ol, orr, cl, cr;
            if (d_cls) {
                // Classic: the whole signal goes down by that much (Mid/Side independent: each its own gain)
                double kag = std::exp(-ga * 0.11512925);
                if (d_mode == 1) {
                    double kbg = std::exp(-gb * 0.11512925);
                    double mm = (dl + dr) * 0.5 * kag, ss = (dl - dr) * 0.5 * kbg;
                    ol = mm + ss;
                    orr = mm - ss;
                } else {
                    ol = dl * kag;
                    orr = dr * kag;
                }
                cl = dl - ol;
                cr = dr - orr;
            } else {
                // Targeted: the dry signal plus the polarity-inverted, gated sibilance of all bands
                cl = db[0].cutl + db[1].cutl + db[2].cutl;
                cr = db[0].cutr + db[1].cutr + db[2].cutr;
                ol = dl - cl;
                orr = dr - cr;
            }
            if (d_mon == 1) { ol = db[0].lisl + db[1].lisl + db[2].lisl; orr = db[0].lisr + db[1].lisr + db[2].lisr; }
            if (d_mon == 2) { ol = cl; orr = cr; }
            if (d_mon == 0) { ol = dl + (ol - dl) * d_mixv; orr = dr + (orr - dr) * d_mixv; }
            out[0][i] = ol;
            out[1][i] = orr;
            if (d_uion) {
                for (int b = 0; b < 3; ++b) {
                    double t = emax(db[b].gra, db[b].grb);
                    if (t > d_pk[b]) d_pk[b] = t;
                }
                double t = emax(ga, gb);
                if (t > d_pk[3]) d_pk[3] = t;
            }
            // Auto-Detect listens to the un-delayed mono sum (or the sidechain)
            D_AR[d_awp] = (xl + xr) * 0.5;
            D_ASR[d_awp] = (scl + scr) * 0.5;
            d_awp = (d_awp + 1) & 2047;
            d_wp = (d_wp + 1) & 2047;
            if (++d_hop >= 1024) {
                d_hop = 0;
                if (d_auto > 0) autoDetect();
            }
        }
    }

    void post(double *const *out, int n)
    {
        for (int i = 0; i < n; ++i) {
            if (d_uion) V.an.put((br0[i] + br1[i]) * 0.5, (out[0][i] + out[1][i]) * 0.5);
            ab.post(br0[i], br1[i], out[0][i], out[1][i], bq0[i], bq1[i]);
        }
        if (d_uion) V.an.publish();
    }

private:
    EngineHost &H;
    Act &A;
    UiLink &U;
    Display &V;
    double srate = 44100;
    bool nonceDrawn = false, idDrawn = false;
    std::vector<double> br0, br1, bq0, bq1;
    Band db[3];
    double DRY[4096] = {}, D_AR[2048] = {}, D_ASR[2048] = {}, D_ACC[2048] = {}, D_PS[2049] = {}, D_SM[2048] = {}, D_WIN[2048] = {};
    double D_FB[4096] = {};
    double d_binhz = 1, d_ka = 0, d_kb = 0, d_kc = 0, d_kp0 = 0, d_kp1 = 0, d_pm = 0, d_smw = 1, d_adec = 0.985, d_accw = 0;
    bool d_fqlock = false, d_fqinit = false, d_freqdirty = false, d_uion = false, d_cls = false, d_usesc = false;
    int d_ap = 0, d_wp = 0, d_awp = 0, d_hop = 0, d_la = 0, d_mode = 0, d_auto = 0, d_pauto = 0, d_mon = 0;
    double d_sclenv = 0, d_screnv = 0, d_mixv = 1, d_fq[3] = {}, d_q[3] = {}, d_pk[4] = {};

    double cpv(int p) const
    {
        const ParamDef *d = table().get(p);
        return (ab.swh && d && (d->flags & PF_AB)) ? H.abv(p) : H.pv(p);
    }
    double tc(double ms) const { return 1 - std::exp(-1 / (emax(ms, 0.01) * 0.001 * srate)); }
    double pw(int k) const { return D_FB[2 * k] * D_FB[2 * k] + D_FB[2 * k + 1] * D_FB[2 * k + 1]; }

    // Auto-Detect: every 1024 samples, a 2048-point spectrum; sibilant frames (most energy above 3.8 kHz) add up into
    // a smoothed peak picture; its three strongest separate peaks become the bands' frequencies
    void autoDetect()
    {
        const double *src = d_usesc ? D_ASR : D_AR;
        double ms = 0;
        for (int i = 0; i < 2048; ++i) {
            double xs = src[(d_awp + i) & 2047] * D_WIN[i];
            D_FB[2 * i] = xs;
            D_FB[2 * i + 1] = 0;
            ms += xs * xs;
        }
        ms *= 8.0 / (3 * 2048);
        fft2048(D_FB);
        double elf = 0, ehf = 0;
        for (int k = (int)d_ka; k < (int)d_kb; ++k) elf += pw(k);
        for (int k = (int)d_kb; k < (int)d_kc; ++k) ehf += pw(k);
        double hfr = ehf / (elf + ehf + 0.000000000000000000000000000001);
        if (!(ms > 0.000001 && hfr > 0.5)) return;
        double wgt = emin((hfr - 0.5) / 0.35, 1), psum = 0;
        int kp0 = (int)d_kp0, pm = (int)d_pm;
        for (int k = 0; k < pm; ++k) psum += pw(kp0 + k);
        double scl = wgt / (psum + 0.000000000000000000000000000001);
        for (int j = 0; j < pm; ++j) D_ACC[j] = D_ACC[j] * d_adec + scl * pw(kp0 + j);
        d_accw = d_accw * d_adec + wgt;
        if (d_accw <= 1.5) return;
        D_PS[0] = 0;
        for (int j = 0; j < pm; ++j) D_PS[j + 1] = D_PS[j] + D_ACC[j];
        int smw = (int)d_smw;
        for (int j = 0; j < pm; ++j) {
            int lo = std::max(j - smw, 0), hi = std::min(j + smw + 1, pm);
            D_SM[j] = (D_PS[hi] - D_PS[lo]) / (hi - lo);
        }
        double v1 = -1;
        int j1 = 0;
        for (int j = 0; j < pm; ++j)
            if (D_SM[j] > v1) { v1 = D_SM[j]; j1 = j; }
        double f1 = (d_kp0 + j1) * d_binhz, f2, f3;
        double v2 = 0;
        int j2 = -1;
        for (int j = 1; j < pm - 1; ++j) {
            double fj = (d_kp0 + j) * d_binhz;
            if (D_SM[j] > v2 && D_SM[j] >= D_SM[j - 1] && D_SM[j] >= D_SM[j + 1] && (fj < f1 * 0.84 || fj > f1 * 1.19)) { v2 = D_SM[j]; j2 = j; }
        }
        f2 = (j2 >= 0 && v2 >= 0.3 * v1) ? (d_kp0 + j2) * d_binhz : f1 * 0.72;
        double v3 = 0;
        int j3 = -1;
        for (int j = 1; j < pm - 1; ++j) {
            double fj = (d_kp0 + j) * d_binhz;
            if (D_SM[j] > v3 && D_SM[j] >= D_SM[j - 1] && D_SM[j] >= D_SM[j + 1] && (fj < f1 * 0.84 || fj > f1 * 1.19) &&
                (fj < f2 * 0.84 || fj > f2 * 1.19)) {
                v3 = D_SM[j];
                j3 = j;
            }
        }
        f3 = (j3 >= 0 && v3 >= 0.3 * v1) ? (d_kp0 + j3) * d_binhz : f1 * 1.4;
        double pa = f1, pb = f2, pc = f3;
        if (pa > pb) std::swap(pa, pb);
        if (pb > pc) std::swap(pb, pc);
        if (pa > pb) std::swap(pa, pb);
        if (d_fqlock) {
            d_fq[0] += 0.2 * (pa - d_fq[0]);
            d_fq[1] += 0.2 * (pb - d_fq[1]);
            d_fq[2] += 0.2 * (pc - d_fq[2]);
        } else {
            d_fq[0] = pa; d_fq[1] = pb; d_fq[2] = pc;
            d_fqlock = true;
        }
        for (int b = 0; b < 3; ++b) {
            d_fq[b] = emin(emax(d_fq[b], 2000), 16000);
            db[b].freq(d_fq[b], d_q[b], srate);
        }
        d_freqdirty = true;
    }
};

// ---------------------------------------------------------------- the plug-in
class DefitcherPlugin : public Plugin, public EngineHost {
public:
    explicit DefitcherPlugin(const clap_host *h) : Plugin(h, kind()), eng(*this, act, uiLink, view) {}
    UiLink uiLink;
    Display view;
    Engine eng;
    double pv(int id) const override { return P.get(id); }
    double abv(int id) const override { return abValue(id); }
    void setAudio(int id, double v) override { setFromAudio(id, v); }
    gui::View *createView() override;

protected:
    void dspActivate(double sr, uint32_t) override { eng.init(sr); }
    void dspReset() override { eng.reset(); }
    int latencyFromParams(double sr) const override { return laSamples(P.get(P_LA), sr); }
    void dspProcess(const double *const *in, const double *const *sc, double *const *out, int n) override
    {
        eng.block(n);
        act.block(n);
        eng.afterAct();
        eng.samples(in, sc, out, n);
        act.process(out, 2, n);
        eng.post(out, n);
        act.audioKey(out, 2, n);
    }
};

static Plugin *create(const clap_host *h) { return new DefitcherPlugin(h); }

// ---------------------------------------------------------------- the window
class DefitcherView : public EffectView {
public:
    explicit DefitcherView(DefitcherPlugin &p) : EffectView(p, p.uiLink, 940, 704, "DEFITCHER"), D(p)
    {
        help = {"Targeted turns down only the selected bands (a polarity-inverted copy of the band): the tone changes where the sibilance is.",
                "Classic turns the whole signal down by the same number of dB: the tone stays and only the volume changes.",
                "Auto-detect finds the sibilant frequencies (Learn) and keeps following them; Reset + Learn starts again."};
    }
    void frame(Ui &ui) override;
    void opened() override { spi.reset(); spo.reset(); }

protected:
    void hashMore(std::function<void(uint64_t)> mix) override
    {
        mix((uint64_t)D.view.side.load() | (uint64_t)D.view.la.load() << 2);
        mix(D.view.accw.load() > 1.5);
        mix((uint64_t)D.view.scActive.load());
        mix((uint64_t)(ndrag + 1) | (uint64_t)(nhover + 2) << 4);
    }
    bool extraFull() override { return ndrag > 0; }

private:
    DefitcherPlugin &D;
    Spectrum spi, spo;
    int ndrag = 0, nhover = -1;

    // the combined response (dB) of the three parallel cancel paths: H = 1 - sum(g * BP)
    double resp(double f, const double *fq, const double *q, const double *g) const
    {
        double re = 1, im = 0;
        for (int b = 0; b < 3; ++b) {
            double x = f / fq[b], a = 1 - x * x, bb = x / q[b], d = a * a + bb * bb + 0.0000000001;
            re -= g[b] * bb * bb / d;
            im -= g[b] * a * bb / d;
        }
        return 10 * std::log10(re * re + im * im + 0.0000000001);
    }
    void analyzer(Ui &ui, const R &r);
    void grBar(Ui &ui, const R &r, double gr, double mx);
    void bandCard(Ui &ui, int b, const R &r);
};

gui::View *DefitcherPlugin::createView() { return new DefitcherView(*this); }

void DefitcherView::grBar(Ui &ui, const R &r, double gr, double mx)
{
    ui.fill(r, Paint(th::well()), 3);
    float f = (float)std::min(1.0, std::max(0.0, gr / std::max(0.1, mx)));
    if (f > 0) ui.fill(R(r.x + 1, r.y + 1, (r.w - 2) * f, r.h - 2), Paint(th::red()), 2);
    ui.stroke(r, Paint(th::border()), 3);
}

// the analyzer: input (filled) and output (a line), with what the bands do and their nodes
void DefitcherView::analyzer(Ui &ui, const R &r)
{
    FreqScale fs(r);
    const float ey = r.y + 50;   // the zero line of the bands' response
    bool cls = D.get(P_MODE) >= 0.5;
    int dauto = iround(D.get(P_AUTO));
    double fq[3], q[3];
    bool en[3];
    for (int b = 0; b < 3; ++b) {
        fq[b] = D.get(P_B + 8 * b + K_FREQ);
        q[b] = D.get(P_B + 8 * b + K_Q);
        en[b] = D.get(P_B + 8 * b + K_ON) >= 0.5;
    }
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        freqGrid(ui, r);
        // nodes: drag = frequency (not while Auto-detect is on), wheel = Q, right-click = on / off
        float mx = ui.mx(), my = ui.my();
        nhover = -1;
        if (ui.hit(r))
            for (int b = 0; b < 3; ++b)
                if (sqr(mx - fs.x(fq[b])) + sqr(my - ey) < 110) nhover = b;
        if (nhover >= 0 && !ui.used && !ui.busy()) {
            int p = P_B + 8 * nhover;
            if (ui.lclick) {
                ui.used = true;
                ndrag = nhover + 1;
                if (dauto == 0) D.beginEdit(p + K_FREQ);
            } else if (ui.rclick) {
                ui.used = true;
                D.setOnce(p + K_ON, en[nhover] ? 0 : 1);
            } else if (ui.wheel != 0) {
                ui.used = true;
                D.setOnce(p + K_Q, D.T.clamp(p + K_Q, q[nhover] * std::exp(ui.wheel > 0 ? 0.12 : -0.12)));
            }
        }
        if (ndrag > 0) {
            int p = P_B + 8 * (ndrag - 1) + K_FREQ;
            if (ui.ldown) {
                if (dauto == 0) D.edit(p, D.T.clamp(p, std::floor(fs.f(mx) + 0.5)));
            } else {
                if (dauto == 0) D.endEdit(p);
                ndrag = 0;
            }
        }
        if (nhover >= 0 || ndrag > 0) ui.cursor = dauto == 0 ? CUR_HRESIZE : CUR_HAND;
        ui.text(R(r.x + 10, r.y + 6, 520, 12),
                cls ? "CLASSIC: the whole signal goes down by the loudest band's reduction" : "TARGETED: only the selected frequencies go down",
                th::dim(), F_MEDIUM, 10.5f);
        ui.text(R(r.x + 400, r.y + 6, r.w - 410, 12), "drag a node: frequency   wheel: Q   right-click: on / off", th::faint(), F_REGULAR,
                10.5f, 2);
        ui.stroke(r, Paint(th::border()), 6);
    }
    if (!ui.drawD()) return;
    Canvas &c = ui.cv();
    double sr = D.sampleRate();
    c.pushClip(ui.X(r.x + 1), ui.Y(r.y + 1), ui.W(r.w - 2), ui.W(r.h - 2));
    if (live) {
        int ap = D.view.an.ap.load(std::memory_order_acquire);
        spi.update(D.view.an.in, AnFeed::N - 1, ap, ui.dt, sr);
        spo.update(D.view.an.out, AnFeed::N - 1, ap, ui.dt, sr);
        Paint p = Paint::vertical(ui.Y(r.y), Col::hex(0x3a3a46), ui.Y(r.b()), Col::hex(0x1c1c22));
        Col lc = Col::hex(0xc8c3bb).alpha(0.55f);
        drawSpectrum(ui, fs, spi, sr, &p, nullptr);
        drawSpectrum(ui, fs, spo, sr, nullptr, &lc, 1.1f);
    }
    freqLabels(ui, r);
    // what the bands do: faint = full depth, bright = right now. Targeted: a dip at each band. Classic: the whole signal.
    double mr[3], lv[3], gm[3], gl[3], lvt = live ? D.view.ugr[3].load() : 0;
    for (int b = 0; b < 3; ++b) {
        mr[b] = en[b] ? D.get(P_B + 8 * b + K_MAX) : 0;
        lv[b] = (live && en[b]) ? D.view.ugr[b].load() : 0;
        gm[b] = 1 - std::exp(-mr[b] * 0.11512925);
        gl[b] = 1 - std::exp(-lv[b] * 0.11512925);
    }
    ui.fill(R(r.x, ey, r.w, 1 / ui.s), Paint(th::orange().alpha(0.28f)));
    int np = (int)r.w / 2 + 1;
    std::vector<Pt> pm(np), pl(np);
    std::vector<float> bottoms(np);
    for (int i = 0; i < np; ++i) {
        float x = r.x + std::min((float)i * 2, r.w);
        double f = fs.f(x), ym, yl;
        if (cls) {
            ym = ey + 2 * std::max(mr[0], std::max(mr[1], mr[2]));
            yl = ey + 2 * lvt;
        } else {
            ym = ey - resp(f, fq, q, gm) * 2;
            yl = ey - resp(f, fq, q, gl) * 2;
        }
        ym = std::min(ym, (double)r.b() - 2);
        yl = std::min(yl, (double)r.b() - 2);
        pm[i] = {ui.X(x), ui.Y((float)ym)};
        pl[i] = {ui.X(x), ui.Y((float)yl)};
        if (yl > ey + 0.5) c.fillRect(ui.X(x), ui.Y(ey), ui.W(2), ui.W((float)yl - ey), Paint(th::orange().alpha(0.28f)));
    }
    c.strokePolyline(pm.data(), np, ui.W(1.2f), Paint(th::purple().alpha(0.6f)));
    c.strokePolyline(pl.data(), np, ui.W(1.6f), Paint(th::orange()));
    for (int b = 0; b < 3; ++b) {
        float nx = (float)fs.x(fq[b]);
        bool hot = nhover == b || ndrag == b + 1;
        if (hot) c.fillCircle(ui.X(nx), ui.Y(ey), ui.W(12), Paint(th::orange().alpha(0.30f)));
        c.fillCircle(ui.X(nx), ui.Y(ey), ui.W(8), Paint(en[b] ? th::purple() : Col(0.16f, 0.15f, 0.17f)));
        c.strokeCircle(ui.X(nx), ui.Y(ey), ui.W(8), ui.W(1.2f), Paint(en[b] ? th::orange() : Col(0.45f, 0.42f, 0.40f)));
        char s[40];
        snprintf(s, sizeof s, "%d", b + 1);
        ui.text(R(nx - 8, ey - 8, 16, 16), s, en[b] ? Col(0.04f, 0.03f, 0.05f) : Col(0.62f, 0.58f, 0.60f), F_BOLD, 11, 1);
        if (hot) {
            snprintf(s, sizeof s, "%.0f Hz   Q %.2f", fq[b], q[b]);
            float tw = ui.textW(F_REGULAR, 11, s);
            float tx = std::min(std::max(nx - tw / 2, r.x + 4), r.r() - tw - 12);
            ui.fill(R(tx - 6, ey + 16, tw + 12, 18), Paint(Col(0.04f, 0.04f, 0.05f, 0.95f)), 4);
            ui.stroke(R(tx - 6, ey + 16, tw + 12, 18), Paint(th::orange()), 4);
            ui.text(R(tx, ey + 16, tw, 18), s, th::text(), F_REGULAR, 11);
        }
    }
    c.popClip();
}

void DefitcherView::bandCard(Ui &ui, int b, const R &r)
{
    int p = P_B + 8 * b;
    bool en = D.get(p + K_ON) >= 0.5;
    int dauto = iround(D.get(P_AUTO));
    if (ui.drawS()) {
        card(ui, r);
        char s[16];
        snprintf(s, sizeof s, "BAND %d", b + 1);
        toggle(ui, R(r.x + 10, r.y + 10, 84, 24), s, p + K_ON, BS_PURPLE);
        float y = r.y + 44, lw = 70, bw = 80, w = r.w - 20;
        auto row = [&](const char *lbl, int k, int sc, bool e = true) {
            ui.paramRow(R(r.x + 10, y, w, 24), lbl, p + k, sc, lw, bw, e);
            y += 29;
        };
        row("Frequency", K_FREQ, SC_LOG, dauto == 0);
        row("Q", K_Q, SC_LOG);
        row("Threshold", K_THR, SC_LIN);
        row("Ratio", K_RATIO, SC_LIN);
        row("Max GR", K_MAX, SC_LIN);
        row("Attack", K_ATT, SC_LOG);
        row("Release", K_REL, SC_LOG);
    }
    if (ui.drawD()) {
        double gr = (live && en) ? D.view.ugr[b].load() : 0;
        grBar(ui, R(r.x + 102, r.y + 16, r.w - 102 - 74, 12), gr, D.get(p + K_MAX));
        char s[24];
        snprintf(s, sizeof s, "%.1f dB", gr > 0.05 ? -gr : 0.0);
        ui.text(R(r.r() - 70, r.y + 10, 60, 24), s, gr > 0.05 ? th::red() : th::faint(), F_SEMIBOLD, 11.5f, 2, true);
    }
}

void DefitcherView::frame(Ui &ui)
{
    int la = D.view.la.load();
    R gear = begin(ui, la > 0 ? la * 1000.0 / D.sampleRate() : 0, D.view.side.load() != 0);
    float y = 66;
    analyzer(ui, R(M, y, DW - 2 * M, 200));
    y += 200 + 10;
    bool cls = D.get(P_MODE) >= 0.5;
    if (ui.drawS()) {
        // detector, lookahead, Auto-detect, monitor, sidechain
        enumDD(ui, R(M, y, 200, 26), P_DET, L_DET, 4, "Detector");
        enumDD(ui, R(M + 208, y, 170, 26), P_LA, L_LA, 3, "Lookahead");
        enumDD(ui, R(M + 386, y, 230, 26), P_AUTO, L_AUTO, 3, "Auto-detect");
        enumDD(ui, R(M + 624, y, 160, 26), P_MON, L_MON, 3, "Monitor");
        toggle(ui, R(M + 792, y, DW - 2 * M - 792, 26), "Sidechain", P_SC, BS_NORMAL);
        float y2 = y + 36;
        if (ui.button(R(M, y2, 100, 26), "Targeted", !cls)) D.setOnce(P_MODE, 0);
        if (ui.button(R(M + 104, y2, 100, 26), "Classic", cls)) D.setOnce(P_MODE, 1);
        std::string st;
        if (iround(D.get(P_AUTO)) > 0) st = D.view.accw.load() <= 1.5 ? "Auto-detect: listening" : "Auto-detect: tracking";
        else if (D.get(P_SC) >= 0.5) st = D.view.scActive.load() ? "Sidechain: active" : "Sidechain: no signal";
        ui.text(R(M + 214, y2, 170, 26), st.c_str(), th::dim(), F_REGULAR, 11.5f);
        ui.text(R(M + 386, y2, 70, 26), "Reduction", th::dim(), F_REGULAR, 12);
        ui.paramRow(R(DW - M - 262, y2, 262, 26), "Dry/wet", P_MIX, SC_LIN, 60, 76);
    }
    if (ui.drawD()) {
        float y2 = y + 36;
        double mx = 1;
        for (int b = 0; b < 3; ++b)
            if (D.get(P_B + 8 * b + K_ON) >= 0.5) mx = std::max(mx, D.get(P_B + 8 * b + K_MAX));
        double gr = live ? D.view.ugr[3].load() : 0;
        grBar(ui, R(M + 456, y2 + 7, 120, 12), gr, mx);
        char s[24];
        snprintf(s, sizeof s, "%.1f dB", gr > 0.05 ? -gr : 0.0);
        ui.text(R(M + 580, y2, 56, 26), s, gr > 0.05 ? th::red() : th::faint(), F_SEMIBOLD, 11.5f, 2, true);
    }
    y += 36 + 26 + 12;
    float cw = (DW - 2 * M - 2 * 12) / 3;
    for (int b = 0; b < 3; ++b) bandCard(ui, b, R(M + b * (cw + 12), y, cw, 250));
    y += 250 + 12;
    actCardAt(ui, y);
    finish(ui, gear);
}

// ---------------------------------------------------------------- the kind
static const char *s_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DEESSER, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const PluginKind &kind()
{
    static const PluginKind k = {
        {CLAP_VERSION_INIT, "com.twostorms.defitcher", "TwoStorms Defitcher", "TwoStorms", "", "", "", TS_VERSION,
         "A de-esser with an Act built in: three bands, Targeted or Classic reduction, Stereo / Mid-Side / Mid / Side detection, "
         "sidechain, Auto-detect frequencies, optional lookahead (its latency), band and removed-audio monitoring.",
         s_features},
        &table,
        &factoryPresets,
        "Defitcher",
        "TWOSTORMS DEFITCHER",
        shm::KIND_POINT,
        shm::SK_DEFITCHER,
        true,
        true,
        P_AB,
        &create,
        2,
        false,
    };
    return k;
}

} // namespace defitcher
} // namespace ts
