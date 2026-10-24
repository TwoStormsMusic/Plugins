// TwoStorms Chorus: chorus (up to 8 voices), ensemble (three voices, two LFOs), flanger, vibrato and through-zero
// flanger, with feedback and cross-feedback, a tape or BBD character, a drive, wet filters and width. The rate is free
// in Hz or a note value at the tempo. Only the through-zero flanger has latency (its dry waits a whole number of
// samples). Port of twostorms_chorus.jsfx: Standard quality is the JSFX's Standard, sample for sample. High quality
// goes further than the JSFX's: the drive and the BBD run at four times the rate (linear-phase half-band filters,
// whose delay the voices' delay times take off again, so still no latency), the LFOs are worked out every sample,
// and every voice is read with six-point interpolation.
#include "chorus_plugin.h"
#include "../../core/eelrand.h"
#include "../../core/filters.h"
#include "../../core/satcore.h"
#include "../../gui/analyzer.h"
#include "../../gui/effect_view.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ts {
namespace chorus {

using namespace gui;

enum {
    P_IN = 46, P_MIX = 47, P_AB = 49, P_MODE = 50, P_RATE = 51, P_SYNC = 52, P_DEPTH = 53, P_DELAY = 54, P_VOICES = 55,
    P_SPREAD = 56, P_PHASE = 57, P_SHAPE = 58, P_AN = 59, P_FB = 60, P_RAND = 61, P_HP = 62, P_LP = 63, P_CHAR = 64,
    P_AGE = 65, P_DRIVE = 66, P_WIDTH = 67, P_XFB = 68, P_HQ = 69,
};

// a note value's length in beats (index = the Rate sync setting)
static const double YBEATS[17] = {0.000000, 0.083333, 0.125000, 0.166667, 0.250000, 0.375000, 0.333333, 0.500000, 0.750000,
                                  0.666667, 1.000000, 1.500000, 1.333333, 2.000000, 3.000000, 4.000000, 8.000000};

// ---------------------------------------------------------------- parameters
static void f_pct(double v, char *b, int n) { snprintf(b, n, "%.0f%%", v); }
static void f_spct(double v, char *b, int n) { snprintf(b, n, "%+.0f%%", v); }
static void f_db(double v, char *b, int n) { snprintf(b, n, "%+.1f dB", v); }
static void f_ms(double v, char *b, int n) { snprintf(b, n, v < 10 ? "%.2f ms" : "%.1f ms", v); }
static void f_rate(double v, char *b, int n) { snprintf(b, n, v < 10 ? "%.2f Hz" : "%.1f Hz", v); }
static void f_deg(double v, char *b, int n) { snprintf(b, n, "%.0f deg", v); }
static void f_int(double v, char *b, int n) { snprintf(b, n, "%.0f", v); }
static void f_hz(double v, char *b, int n)
{
    if (v >= 10000) snprintf(b, n, "%.1f kHz", v / 1000);
    else if (v >= 1000) snprintf(b, n, "%.2f kHz", v / 1000);
    else snprintf(b, n, "%.0f Hz", v);
}

static const char *const L_MODE[] = {"Chorus", "Ensemble", "Flanger", "Vibrato", "Through-zero"};
static const char *const L_SYNC[] = {"Free", "1/32T", "1/32", "1/16T", "1/16", "1/16.", "1/8T", "1/8", "1/8.",
                                     "1/4T", "1/4", "1/4.", "1/2T", "1/2", "1/2.", "1 bar", "2 bars"};
static const char *const L_SHAPE[] = {"Sine", "Triangle", "Smooth random"};
static const char *const L_CHAR[] = {"Clean", "Tape", "BBD"};
static const char *const L_HQ[] = {"Standard", "High"};
static const char *const L_AN[] = {"Off", "Pre", "Post", "Both"};
static const char *const L_VOICES[] = {"1", "2", "3", "4", "5", "6", "7", "8"};

static const ParamTable &table()
{
    static ParamTable t = [] {
        ParamTable t;
        addActParams(t, true);
        const char *M = "Chorus";
        const uint32_t H = PF_HOST | PF_AB;
        t.add(P_IN, "Input", -24, 24, 0, H, f_db, M).unit = "dB";
        t.add(P_MIX, "Dry/wet", 0, 100, 50, H, f_pct, M).unit = "%";
        t.addEnum(P_AB, "A/B set", {"A", "B"}, 0, PF_HOST, M);
        t.addEnum(P_MODE, "Mode", {L_MODE[0], L_MODE[1], L_MODE[2], L_MODE[3], L_MODE[4]}, 0, H, M);
        t.add(P_RATE, "Rate", 0.01, 20, 0.6, H, f_rate, M).unit = "Hz";
        std::vector<const char *> sy(L_SYNC, L_SYNC + 17);
        t.addEnum(P_SYNC, "Rate sync", sy, 0, H, M);
        t.add(P_DEPTH, "Depth", 0, 15, 2.5, H, f_ms, M).unit = "ms";
        t.add(P_DELAY, "Delay", 0.1, 40, 12, H, f_ms, M).unit = "ms";
        t.add(P_VOICES, "Voices", 1, 8, 4, H | PF_INT, f_int, M);
        t.add(P_SPREAD, "Spread", 0, 100, 70, H, f_pct, M).unit = "%";
        t.add(P_PHASE, "Stereo phase", 0, 180, 90, H, f_deg, M).unit = "deg";
        t.addEnum(P_SHAPE, "LFO shape", {L_SHAPE[0], L_SHAPE[1], L_SHAPE[2]}, 0, H, M);
        t.addEnum(P_AN, "Analyzer", {L_AN[0], L_AN[1], L_AN[2], L_AN[3]}, 3, 0, M);
        t.add(P_FB, "Feedback", -95, 95, 0, H, f_spct, M).unit = "%";
        t.add(P_RAND, "Random", 0, 100, 20, H, f_pct, M).unit = "%";
        t.add(P_HP, "Wet high pass", 20, 2000, 150, H, f_hz, M).unit = "Hz";
        t.add(P_LP, "Wet low pass", 200, 20000, 16000, H, f_hz, M).unit = "Hz";
        t.addEnum(P_CHAR, "Character", {L_CHAR[0], L_CHAR[1], L_CHAR[2]}, 0, H, M);
        t.add(P_AGE, "Age", 0, 100, 30, H, f_pct, M).unit = "%";
        t.add(P_DRIVE, "Drive", 0, 100, 0, H, f_pct, M).unit = "%";
        t.add(P_WIDTH, "Width", 0, 200, 100, H, f_pct, M).unit = "%";
        t.add(P_XFB, "Cross-feedback", 0, 100, 0, H, f_pct, M).unit = "%";
        t.addEnum(P_HQ, "Quality", {L_HQ[0], L_HQ[1]}, 1, H, M);
        return t;
    }();
    return t;
}

// factory presets: Instruments, Buses, Creative, Weird
static std::string S(std::initializer_list<std::pair<int, double>> kv) { return presetStr(kv); }
enum { CHORUS, ENSEMBLE, FLANGER, VIBRATO, TZ };
enum { CLEAN, TAPE, BBD };
static const std::vector<FactoryPreset> &factoryPresets()
{
    static std::vector<FactoryPreset> v = {
        // ---- guitar
        {"Instruments/Guitar/Clean: subtle shimmer", "47=25 50=0 51=0.5 53=1.5 54=10 55=2 62=200 64=2 65=25"},
        {"Instruments/Guitar/Clean: 80s stereo chorus (BBD)", "47=50 50=0 51=0.8 53=3 54=8 55=2 57=180 62=150 64=2 65=40 67=150"},
        {"Instruments/Guitar/Clean: BBD pedal", "47=50 50=0 51=0.7 52=0 53=2.5 54=10 55=2 56=70 57=90 58=0 60=0 61=20 62=150 63=16000 64=2 65=40 66=0 67=100 68=0"},
        {"Instruments/Guitar/Clean: lush ensemble", "47=45 50=1 51=0.4 53=2.5 54=14 55=6 62=200 67=160"},
        {"Instruments/Guitar/Funk: fast wobble", "47=45 50=0 51=2.5 53=1.5 54=7 55=2 62=250"},
        {"Instruments/Guitar/Funk: 1/8 synced shimmer", "47=50 50=0 52=7 53=2 54=6 55=2 57=90 62=250"},
        {"Instruments/Guitar/Rock: slow jet flanger", "47=45 50=2 51=0.12 53=2.5 54=2 60=60 62=150"},
        {"Instruments/Guitar/Rock: tape thickener", "47=30 50=0 51=0.6 53=1.8 54=12 55=3 62=150 64=1 65=40"},
        {"Instruments/Guitar/Metal: wide doubler", "47=35 50=0 51=0.2 53=0.8 54=18 55=2 57=180 61=40 62=300 67=180"},
        {"Instruments/Guitar/Metal: lead flange (1 bar)", "47=40 50=2 52=15 53=3 54=2.5 60=55 62=200"},
        {"Instruments/Guitar/Metal: through-zero sweep", "47=50 50=4 51=0.1 53=4.5 54=5 56=60 57=60 60=40 62=100"},
        {"Instruments/Guitar/Acoustic: 12-string shimmer", S({{P_MIX, 35}, {P_MODE, CHORUS}, {P_RATE, 0.35}, {P_DEPTH, 1}, {P_DELAY, 14}, {P_VOICES, 2}, {P_PHASE, 180}, {P_HP, 250}, {P_WIDTH, 150}})},
        // ---- bass
        {"Instruments/Bass/Clean: BBD, lows dry", "47=40 50=0 51=0.6 52=0 53=2 54=9 55=2 56=70 57=90 58=0 60=0 61=20 62=220 63=16000 64=2 65=30 66=0 67=100 68=0"},
        {"Instruments/Bass/Distorted: slow flange", "47=35 50=2 51=0.2 52=0 53=2 54=2.5 55=1 56=70 57=90 58=0 60=40 61=20 62=200 63=16000 64=0 65=30 66=0 67=100 68=0"},
        {"Instruments/Bass/Slap: shimmer", "47=30 50=0 51=0.8 52=0 53=1.5 54=8 55=2 56=70 57=90 58=0 60=0 61=20 62=300 63=16000 64=0 65=30 66=0 67=100 68=0"},
        {"Instruments/Bass/Punk", "47=35 50=0 51=1 52=0 53=3 54=10 55=2 56=70 57=90 58=0 60=0 61=20 62=180 63=16000 64=2 65=30 66=0 67=100 68=0"},
        {"Instruments/Bass/Pop punk", "47=40 50=0 51=0.8 52=0 53=2.5 54=10 55=3 56=70 57=90 58=0 60=0 61=20 62=200 63=16000 64=0 65=30 66=0 67=100 68=0"},
        {"Instruments/Bass/Metal", "47=50 50=2 51=0.25 52=0 53=2 54=2.5 55=1 56=70 57=90 58=0 60=50 61=20 62=150 63=16000 64=0 65=30 66=0 67=100 68=0"},
        {"Instruments/Bass/Rock", "47=35 50=0 51=0.5 52=0 53=2.5 54=11 55=2 56=70 57=90 58=0 60=0 61=20 62=180 63=16000 64=2 65=40 66=0 67=100 68=0"},
        {"Instruments/Bass/Jazz", "47=30 50=0 51=0.4 52=0 53=1.5 54=12 55=2 56=70 57=90 58=0 60=0 61=20 62=160 63=8000 64=0 65=30 66=0 67=100 68=0"},
        {"Instruments/Bass/Pop", "47=40 50=1 51=0.6 52=0 53=2 54=12 55=4 56=70 57=90 58=0 60=0 61=20 62=200 63=16000 64=0 65=30 66=0 67=150 68=0"},
        {"Instruments/Bass/Fretless: 80s wide", S({{P_MIX, 45}, {P_MODE, CHORUS}, {P_RATE, 0.5}, {P_DEPTH, 2.5}, {P_DELAY, 10}, {P_VOICES, 2}, {P_PHASE, 180}, {P_HP, 250}, {P_CHAR, BBD}, {P_WIDTH, 160}})},
        // ---- vocals
        {"Instruments/Vocals/Lead: subtle ensemble", "47=20 50=1 51=0.4 52=0 53=1.2 54=14 55=3 56=70 57=90 58=0 60=0 61=20 62=300 63=16000 64=0 65=30 66=0 67=100 68=0"},
        {"Instruments/Vocals/Lead: ADT doubler", "47=30 50=0 51=0.25 53=0.7 54=20 55=1 57=180 61=40 67=150"},
        {"Instruments/Vocals/Spoken: doubler", "47=25 50=0 51=0.3 52=0 53=0.8 54=18 55=2 56=70 57=90 58=0 60=0 61=40 62=200 63=16000 64=0 65=30 66=0 67=100 68=0"},
        {"Instruments/Vocals/Backing: widen", S({{P_MIX, 40}, {P_MODE, ENSEMBLE}, {P_RATE, 0.3}, {P_DEPTH, 1.5}, {P_DELAY, 16}, {P_VOICES, 4}, {P_PHASE, 180}, {P_HP, 200}, {P_WIDTH, 180}})},
        {"Instruments/Vocals/Chorused 80s vocal", S({{P_MIX, 35}, {P_MODE, CHORUS}, {P_RATE, 0.8}, {P_DEPTH, 2.5}, {P_DELAY, 10}, {P_VOICES, 2}, {P_PHASE, 180}, {P_HP, 300}, {P_CHAR, BBD}, {P_WIDTH, 150}})},
        // ---- keys and synths
        {"Instruments/Keys & synths/Juno-style I (slow)", S({{P_MIX, 50}, {P_MODE, CHORUS}, {P_RATE, 0.4}, {P_DEPTH, 3}, {P_DELAY, 8}, {P_VOICES, 2}, {P_PHASE, 180}, {P_SHAPE, 1}, {P_RAND, 0}, {P_HP, 20}, {P_CHAR, BBD}, {P_WIDTH, 150}})},
        {"Instruments/Keys & synths/Juno-style II (faster)", S({{P_MIX, 50}, {P_MODE, CHORUS}, {P_RATE, 0.8}, {P_DEPTH, 3}, {P_DELAY, 8}, {P_VOICES, 2}, {P_PHASE, 180}, {P_SHAPE, 1}, {P_RAND, 0}, {P_HP, 20}, {P_CHAR, BBD}, {P_WIDTH, 150}})},
        {"Instruments/Keys & synths/String machine ensemble", "47=60 50=1 51=0.5 52=0 53=2.5 54=12 55=6 56=70 57=90 58=0 60=0 61=20 62=150 63=16000 64=0 65=30 66=0 67=100 68=0"},
        {"Instruments/Keys & synths/Synth pad: slow sweep", "47=45 50=0 51=0.15 53=4 54=10 55=3 61=10"},
        {"Instruments/Keys & synths/Synth pad: lush 8 voices", S({{P_MIX, 55}, {P_MODE, ENSEMBLE}, {P_RATE, 0.25}, {P_DEPTH, 3.5}, {P_DELAY, 14}, {P_VOICES, 8}, {P_SPREAD, 100}, {P_RAND, 40}, {P_WIDTH, 170}})},
        {"Instruments/Keys & synths/Electric piano: 70s shimmer", S({{P_MIX, 40}, {P_MODE, CHORUS}, {P_RATE, 0.9}, {P_DEPTH, 2}, {P_DELAY, 9}, {P_VOICES, 2}, {P_PHASE, 180}, {P_HP, 200}, {P_CHAR, BBD}, {P_AGE, 40}})},
        {"Instruments/Keys & synths/Organ: rotary-ish (fast, wide)", "47=50 50=0 51=6 53=0.8 54=4 55=2 57=180 67=150"},
        {"Instruments/Keys & synths/Organ: vibrato", "47=100 50=3 51=5 52=0 53=1.2 54=5 55=1 56=70 57=90 58=0 60=0 61=20 62=150 63=16000 64=0 65=30 66=0 67=100 68=0"},
        // ---- drums
        {"Instruments/Drums/Overheads: jet flanger", "47=50 50=2 51=0.15 52=0 53=2.5 54=2 55=1 56=70 57=90 58=0 60=70 61=20 62=150 63=16000 64=0 65=30 66=0 67=100 68=0"},
        {"Instruments/Drums/Hi-hats: gentle flange", S({{P_MIX, 35}, {P_MODE, FLANGER}, {P_RATE, 0.2}, {P_DEPTH, 2}, {P_DELAY, 2}, {P_VOICES, 1}, {P_FB, 40}, {P_HP, 2000}})},
        // ---- strings
        {"Instruments/Strings & brass/Strings: ensemble width", S({{P_MIX, 35}, {P_MODE, ENSEMBLE}, {P_RATE, 0.3}, {P_DEPTH, 1.5}, {P_DELAY, 15}, {P_VOICES, 4}, {P_HP, 200}, {P_WIDTH, 150}})},
        // ---- buses
        {"Buses/Instrument bus/Guitar bus: wide stereo", "47=40 50=0 51=0.3 52=0 53=1.5 54=15 55=4 56=70 57=90 58=0 60=0 61=40 62=150 63=16000 64=0 65=30 66=0 67=200 68=0"},
        {"Buses/Instrument bus/Drum bus: through-zero flange", "47=50 50=4 51=0.15 52=0 53=4.5 54=5 55=1 56=60 57=60 58=0 60=50 61=20 62=20 63=16000 64=0 65=30 66=0 67=100 68=0"},
        {"Buses/Instrument bus/Synth bus: Juno width (30%)", S({{P_MIX, 30}, {P_MODE, CHORUS}, {P_RATE, 0.5}, {P_DEPTH, 2.5}, {P_DELAY, 8}, {P_VOICES, 2}, {P_PHASE, 180}, {P_CHAR, BBD}, {P_WIDTH, 150}})},
        {"Buses/Mixbus/Barely there width (10%)", S({{P_MIX, 10}, {P_MODE, CHORUS}, {P_RATE, 0.2}, {P_DEPTH, 0.6}, {P_DELAY, 12}, {P_VOICES, 2}, {P_PHASE, 180}, {P_HP, 400}})},
        {"Buses/Master/Lo-fi master (tape wobble)", S({{P_MIX, 100}, {P_MODE, VIBRATO}, {P_RATE, 0.4}, {P_DEPTH, 0.6}, {P_SHAPE, 2}, {P_CHAR, TAPE}, {P_AGE, 40}})},
        // ---- creative
        {"Creative/Underwater", "47=60 50=0 51=0.25 53=8 54=20 55=4 61=60 63=2500"},
        {"Creative/Negative-feedback whoosh", "47=50 50=2 51=0.08 53=4 54=3 60=-85"},
        {"Creative/Swarm (8 random voices)", "47=60 50=0 51=1.2 53=4 54=15 55=8 56=100 61=100"},
        {"Creative/Stereo spinner (1/16)", "47=60 50=0 52=4 53=3 54=6 57=180 67=200"},
        {"Creative/Cross-feedback whirl", "47=50 50=2 51=0.3 53=3 54=4 60=60 68=70"},
        {"Creative/Reverb return shimmer (100% wet)", S({{P_MIX, 100}, {P_MODE, ENSEMBLE}, {P_RATE, 0.3}, {P_DEPTH, 4}, {P_DELAY, 18}, {P_VOICES, 6}, {P_HP, 300}, {P_WIDTH, 180}})},
        {"Creative/Dub flange (1/2 note, deep)", S({{P_MIX, 55}, {P_MODE, FLANGER}, {P_SYNC, 13}, {P_DEPTH, 4}, {P_DELAY, 3}, {P_FB, 75}, {P_HP, 100}})},
        // ---- weird
        {"Weird/Seasick vibrato", "47=100 50=3 51=0.7 53=6 54=8 58=2"},
        {"Weird/Robot (fast flanger)", "47=50 50=2 51=12 53=0.5 54=1 60=85"},
        {"Weird/Broken tape", "47=100 50=3 51=3.5 53=2 58=2 64=1 65=90 66=50"},
        {"Weird/Metallic comb (static)", "47=50 50=2 51=0.01 53=0 54=1.5 60=90"},
        {"Weird/Drunk choir (all random)", S({{P_MIX, 70}, {P_MODE, ENSEMBLE}, {P_RATE, 2}, {P_DEPTH, 8}, {P_DELAY, 25}, {P_VOICES, 8}, {P_SPREAD, 100}, {P_SHAPE, 2}, {P_RAND, 100}})},
        {"Weird/Plucked string ring (95% feedback)", S({{P_MIX, 60}, {P_MODE, FLANGER}, {P_RATE, 0.01}, {P_DEPTH, 0}, {P_DELAY, 2.27}, {P_FB, 95}, {P_HP, 20}})},
    };
    return v;
}

// the through-zero flanger's latency (the dry waits that long), from the latched settings
static int tzLatency(double mode, double delay, double sr)
{
    if (iclamp(iround(mode), 0, 4) != 4) return 0;
    return (int)emax(4, std::floor(clampd(delay, 0.1, 40) * 0.001 * sr + 0.5));
}

// one voice's LFO value (-1..1) for the phase p (cycles, not wrapped), voice i; shape 0 sine, 1 triangle, 2 smooth random
static inline double lfo(int shape, double p, double i)
{
    double f = p - std::floor(p);
    return shape == 0 ? std::sin(6.283185307 * f)
                      : (shape == 1 ? (f < 0.5 ? 4 * f - 1 : 3 - 4 * f) : 0.62 * std::sin(6.283185307 * f) + 0.38 * std::sin(6.283185307 * (1.618 * p + 0.31 * i)));
}

// a 2x polyphase half-band filter pair (linear phase): up() gives the two 2x samples of x in time order (the
// interpolated one, then x itself, delayed), down() takes such a pair back. 2K taps per phase; the pair delays by
// 2K - 1 samples of the lower rate.
struct HalfBand {
    int K = 2;
    double g[16] = {};
    double hu[64] = {}, hb[64] = {}, ha[64] = {};
    int pu = 0, pd = 0;
    void clear()
    {
        memset(hu, 0, sizeof hu); memset(hb, 0, sizeof hb); memset(ha, 0, sizeof ha);
        pu = pd = 0;
    }
    inline void up(double x, double &b, double &a)
    {
        pu = (pu - 1) & 31;
        hu[pu] = hu[pu + 32] = x;
        const double *h = hu + pu;
        double s = 0;
        for (int t = 0; t < 2 * K; ++t) s += g[t] * h[t];
        b = s;
        a = h[K - 1];
    }
    inline double down(double b, double a)
    {
        pd = (pd - 1) & 31;
        hb[pd] = hb[pd + 32] = b;
        ha[pd] = ha[pd + 32] = a;
        const double *h = hb + pd;
        double s = 0;
        for (int t = 0; t < 2 * K; ++t) s += g[t] * h[t];
        return 0.5 * ha[pd + K] + 0.5 * s;
    }
    // a Kaiser-windowed half-band (beta), 2K taps per phase, its taps scaled to add up to 1
    void design(int k, double beta)
    {
        K = k;
        auto i0 = [](double x) {
            double s = 1, t = 1;
            for (int j = 1; j < 40; ++j) { t *= (x / (2 * j)) * (x / (2 * j)); s += t; }
            return s;
        };
        double sum = 0, M = 2 * K - 1;
        for (int t = 0; t < 2 * K; ++t) {
            double n = 2 * t - M, u = n / (M + 1);
            double w = i0(beta * std::sqrt(std::max(0.0, 1 - u * u))) / i0(beta);
            g[t] = 2 * std::sin(PI * n / 2) / (PI * n) * w;
            sum += g[t];
        }
        for (int t = 0; t < 2 * K; ++t) g[t] /= sum;
    }
    void lagrange()
    {
        K = 2;
        g[0] = -1.0 / 16; g[1] = 9.0 / 16; g[2] = 9.0 / 16; g[3] = -1.0 / 16;
    }
};
// four times the rate: a 27-tap half-band, then a 7-tap one; the pair delays by 13 + 1.5 samples
struct Os4 {
    HalfBand a, b;
    Os4() { a.design(7, 7); b.lagrange(); }
    void clear() { a.clear(); b.clear(); }
    static constexpr double DELAY = 14.5;
    template <class F> inline double run(double x, F f)
    {
        double p, q, r, s, y0, y1;
        a.up(x, p, q);
        b.up(p, r, s);
        y0 = b.down(f(r), f(s));
        b.up(q, r, s);
        y1 = b.down(f(r), f(s));
        return a.down(y0, y1);
    }
};

// ---------------------------------------------------------------- the engine
struct Display {
    std::atomic<double> pu{0};      // the LFO's phase (cycles)
    std::atomic<double> tempo{120};
    std::atomic<int> side{0};
    AnFeed an;
};

class EngineHost {
public:
    virtual ~EngineHost() {}
    virtual double pv(int id) const = 0;
    virtual double abv(int id) const = 0;
};

class Engine {
public:
    Engine(EngineHost &h, Act &a, UiLink &u, Display &d) : H(h), A(a), U(u), V(d) {}
    AbMatch ab;

    void init(double sr)
    {
        srate = sr;
        if (!nonceDrawn) { eel_rand(1000000); nonceDrawn = true; }   // the instance nonce (the JSFX's first rand in @init)
        YDLEN = 1;
        while (YDLEN < 0.12 * srate + 32) YDLEN *= 2;
        YDM = YDLEN - 1;
        YB.assign(2 * YDLEN, 0.0);
        YDB.assign(2 * YDLEN, 0.0);
        memset(YOS, 0, sizeof YOS);
        memset(YBA, 0, sizeof YBA); memset(YBHD, 0, sizeof YBHD);
        memset(YCUR, 0, sizeof YCUR); memset(YSTP, 0, sizeof YSTP); memset(YPJ, 0, sizeof YPJ);
        memset(YWG, 0, sizeof YWG); memset(YWGc, 0, sizeof YWGc);
        memset(YCZ, 0, sizeof YCZ); memset(YFZ, 0, sizeof YFZ); memset(YFC, 0, sizeof YFC); memset(YBMC, 0, sizeof YBMC);
        memset(YBMZ, 0, sizeof YBMZ); memset(YDX, 0, sizeof YDX); memset(YDY, 0, sizeof YDY);
        memset(YWP, 0, sizeof YWP); memset(YWV, 0, sizeof YWV); memset(YCH, 0, sizeof YCH);
        y_wp = 0; y_tc = 0; y_snap = true; y_hq = true; y_bt = 0; y_bps = 0.000005; y_pu = 0; y_pf = 0;
        y_mode = 0; y_n = 4; y_nl = 4; y_sync = 0; y_cycinv = 0; y_rate = 0.6; y_Dc = 12; y_Dt = 12; y_vc = 2.5; y_vt = 2.5;
        y_stph = 0.25; y_shape = 0; y_rnd = 0.2;
        y_fbc = 0; y_fbt = 0; y_spm = 0.7; y_char = 0; y_ca = 0.5; y_nz = 0; y_dgo = 1; y_hpon = false; y_lpon = false; y_wowa = 0;
        y_xfc = 0; y_xft = 0; y_hql = 7; y_ddn = 0; y_dgb = 0; y_bbdon = false; y_bclk = 1; y_baa = 0.5; y_bph = 0; y_sil = 0;
        y_dl = 0; y_dr = 0; y_wdc = 1; y_wdt = 1; y_mxc = 0.5; y_mxt = 0.5; y_dyc = 0.8; y_dyt = 0.8; y_wl = 0; y_wr = 0; y_wetabs = 0;
        y_wn = -1; y_wsp = -1; y_ingc = 1; y_ingt = 1;
        for (auto &o : OSD) o.clear();
        for (auto &o : OSB) o.clear();
        memset(BB4, 0, sizeof BB4); memset(BH4, 0, sizeof BH4);
        y_b4ph = 0; y_hqlc = 0;
        memset(PP0, 0, sizeof PP0); memset(PP1, 0, sizeof PP1); memset(EP0, 0, sizeof EP0); memset(EP1, 0, sizeof EP1);
        memset(BD0, 0, sizeof BD0); memset(BD1, 0, sizeof BD1);
        y_vc0 = y_vc1 = 2.5;
        // the JSFX's @block on a new sample rate
        DC_R = 1 - 2 * PI * 8 / srate;
        YKS = 1 - std::exp(-1 / (0.01 * srate));
        V.an.clear();
    }
    // the host asked for a clean start: empty lines and filters (the LFO runs on)
    void reset()
    {
        std::fill(YB.begin(), YB.end(), 0.0);
        std::fill(YDB.begin(), YDB.end(), 0.0);
        memset(YOS, 0, sizeof YOS); memset(YBA, 0, sizeof YBA); memset(YBHD, 0, sizeof YBHD);
        memset(YCZ, 0, sizeof YCZ); memset(YFZ, 0, sizeof YFZ); memset(YBMZ, 0, sizeof YBMZ);
        memset(YDX, 0, sizeof YDX); memset(YDY, 0, sizeof YDY);
        for (auto &o : OSD) o.clear();
        for (auto &o : OSB) o.clear();
        memset(BB4, 0, sizeof BB4); memset(BH4, 0, sizeof BH4);
        y_wl = 0; y_wr = 0; y_wetabs = 0; y_sil = 0;
    }

    // @block, the chorus's part (before the Act's)
    void block(int samplesblock, const Transport &tr)
    {
        ab.block(U, A.now(), H.pv(P_AB) >= 0.5, samplesblock, srate);
        V.side.store(ab.side ? 1 : 0, std::memory_order_relaxed);
        double y_tempo = tr.tempo > 0 ? tr.tempo : 120;
        y_bps = y_tempo / (60 * srate);
        if (tr.playing) y_bt = tr.beat;
        V.tempo.store(y_tempo, std::memory_order_relaxed);
        // flush the tiny leftovers (denormals)
        for (int i = 0; i < 8; ++i) { flush21(YFZ[i]); flush21(YCZ[i]); }
        anw = ab.uion && H.pv(P_AN) >= 0.5;
    }

    // after the Act's @block: the instance ID's draw (the JSFX's a_new_id), so the noise follows the same sequence
    void afterAct()
    {
        if (!idDrawn && A.myid != 0) { eel_rand(899999); idDrawn = true; }
    }

    // @sample, the chorus's part (before the Act)
    void samples(const double *const *in, double *const *out, int n)
    {
        if ((int)br0.size() < n) { br0.resize(n); br1.resize(n); bq0.resize(n); bq1.resize(n); }
        for (int i = 0; i < n; ++i) {
            double r0 = in[0][i], r1 = in[1][i];
            br0[i] = r0; br1[i] = r1;
            y_ingc += (y_ingt - y_ingc) * 0.002;
            run(r0 * y_ingc, r1 * y_ingc);
            y_mxc += (y_mxt - y_mxc) * 0.002;
            y_dyc += (y_dyt - y_dyc) * 0.002;
            if (y_mode == 4) { bq0[i] = y_dl / y_ingc; bq1[i] = y_dr / y_ingc; }
            else { bq0[i] = r0; bq1[i] = r1; }
            out[0][i] = y_dl * y_dyc + y_wl * y_mxc;
            out[1][i] = y_dr * y_dyc + y_wr * y_mxc;
        }
        V.pu.store(y_pu, std::memory_order_relaxed);
    }

    // after the Act: held Bypass / Swap, then the analyzer's rings
    void post(double *const *out, int n)
    {
        for (int i = 0; i < n; ++i) {
            ab.post(br0[i], br1[i], out[0][i], out[1][i], bq0[i], bq1[i]);
            if (anw) V.an.put((br0[i] + br1[i]) * 0.5, (out[0][i] + out[1][i]) * 0.5);
        }
        if (anw) V.an.publish();
    }

private:
    EngineHost &H;
    Act &A;
    UiLink &U;
    Display &V;
    double srate = 44100;
    bool nonceDrawn = false, idDrawn = false, anw = false;
    std::vector<double> br0, br1, bq0, bq1;

    // the delay lines (left, then right) and the through-zero flanger's dry line; sizes are powers of two
    int64_t YDLEN = 1, YDM = 0;
    std::vector<double> YB, YDB;
    double YOS[128] = {};
    const double YOSC[8] = {-0.000539342, 0.018795537, -0.113860873, 0.595604679, 0.595604679, -0.113860873, 0.018795537, -0.000539342};
    double YBA[4] = {}, YBHD[2] = {}, YCUR[16] = {}, YSTP[16] = {}, YPJ[8] = {}, YWG[16] = {}, YWGc[16] = {};
    double YCZ[8] = {}, YFZ[8] = {}, YFC[20] = {}, YBMC[20] = {}, YBMZ[4] = {}, YDX[4] = {}, YDY[4] = {};
    double YWP[4] = {}, YWV[4] = {}, YCH[8] = {};
    double DC_R = 0, YKS = 0;
    int64_t y_wp = 0;
    int y_tc = 0;
    bool y_snap = true, y_hq = true, y_hpon = false, y_lpon = false, y_bbdon = false;
    double y_bt = 0, y_bps = 0.000005, y_pu = 0, y_pf = 0;
    int y_mode = 0, y_sync = 0, y_shape = 0, y_char = 0, y_nl = 4;
    double y_n = 4, y_cycinv = 0, y_rate = 0.6, y_Dc = 12, y_Dt = 12, y_vc = 2.5, y_vt = 2.5, y_stph = 0.25, y_rnd = 0.2;
    double y_fbc = 0, y_fbt = 0, y_spm = 0.7, y_ca = 0.5, y_nz = 0, y_dgo = 1, y_wowa = 0;
    double y_xfc = 0, y_xft = 0, y_hql = 7, y_ddn = 0, y_dgb = 0, y_bclk = 1, y_baa = 0.5, y_bph = 0, y_sil = 0;
    double y_dl = 0, y_dr = 0, y_wdc = 1, y_wdt = 1, y_mxc = 0.5, y_mxt = 0.5, y_dyc = 0.8, y_dyt = 0.8, y_wl = 0, y_wr = 0, y_wetabs = 0;
    double y_wn = -1, y_wsp = -1, y_ingc = 1, y_ingt = 1;
    // High quality: the drive and the BBD at 4x (per side), and each voice's LFO every sample: where its phase, centre
    // and depth were at the last ctl() (0) and where they are heading (1)
    Os4 OSD[2], OSB[2];
    double BB4[4] = {}, BH4[2] = {}, y_b4ph = 0, y_baa4 = 0.5, y_bclk4 = 1, y_hqlc = 0;
    double PP0[16] = {}, PP1[16] = {}, EP0[16] = {}, EP1[16] = {}, BD0[2] = {}, BD1[2] = {}, y_vc0 = 2.5, y_vc1 = 2.5;

    // the playing set's value (the other set's while Swap is held)
    double cpv(int p) const { return ab.swh ? H.abv(p) : H.pv(p); }

    // a biquad (b0 b1 b2 a1 a2, a0 = 1), transposed direct form II; z = two states
    static inline double bq(double x, const double *o, double *z)
    {
        double y = o[0] * x + z[0];
        z[0] = o[1] * x - o[3] * y + z[1];
        z[1] = o[2] * x - o[4] * y;
        return y;
    }
    // a read d samples back from the slot about to be written (Catmull-Rom between four neighbours)
    inline double rd(const double *b, double d) const
    {
        double p = (double)y_wp - d, fi = std::floor(p), f = p - fi;
        int64_t i = (int64_t)fi;
        double a = b[(i - 1) & YDM], g = b[i & YDM], c = b[(i + 1) & YDM], e = b[(i + 2) & YDM];
        return g + 0.5 * f * (c - a + f * (2 * a - 5 * g + 4 * c - e + f * (3 * (g - c) + e - a)));
    }
    // the same with six neighbours (5th-order Lagrange): flatter at high frequencies, less aliasing when the time moves
    inline double rd6(const double *b, double d) const
    {
        double p = (double)y_wp - d, fi = std::floor(p), f = p - fi;
        int64_t i = (int64_t)fi;
        double a = f + 2, c = f + 1, e = f - 1, g = f - 2, h = f - 3;
        double t = c * f, u = e * g, v = a * f, w = a * c;
        double x = t * u * h * -0.008333333333;
        double y = v * u * h * 0.041666666667;
        double z = w * e * g * h * -0.083333333333;
        double j = w * f * g * h * 0.083333333333;
        x = x * b[(i - 2) & YDM] + y * b[(i - 1) & YDM] + z * b[i & YDM] + j * b[(i + 1) & YDM];
        return x + w * f * e * h * -0.041666666667 * b[(i + 2) & YDM] + w * f * e * g * 0.008333333333 * b[(i + 3) & YDM];
    }
    // a soft ceiling just over full scale, so a feedback over 100% can't run away
    static inline double lim(double x)
    {
        double a = std::fabs(x);
        return a < 1 ? x : (x > 0 ? 1 + 0.5 * ds_th((a - 1) * 2) : -1 - 0.5 * ds_th((a - 1) * 2));
    }

    // what happens to the wet signal of one side: character (tape / BBD output filter), the 7-sample path of the
    // drive (at 4x in High quality), high pass, low pass
    inline double postWet(int c, double x)
    {
        if (y_char > 0) {
            double *z = YCZ + c * 4;
            x += (eel_rand(2) - 1) * y_nz;
            z[0] += y_ca * (x - z[0]); x = z[0];
            z[1] += y_ca * (x - z[1]); x = z[1];
            if (y_char == 1) x = bq(x, YBMC, YBMZ + c * 2);
        }
        if (y_hq) {
            // drive at four times the rate (always through the filters, so the wet's timing never jumps; their delay
            // is taken off the voices' delay times)
            const double g = y_dgo, b = y_dgb;
            if (g > 1.0001) x = OSD[c].run(x, [g, b](double u) { return u + (ds_th(u * g) / g - u) * b; });
            else x = OSD[c].run(x, [](double u) { return u; });
        } else if (y_dgo > 1.0001) {
            double y = ds_th(x * y_dgo) / y_dgo;
            x += (y - x) * y_dgb;
        }
        if (y_hpon) x = bq(x, YFC, YFZ + c * 4);
        if (y_lpon) x = bq(x, YFC + 5, YFZ + c * 4 + 2);
        return x;
    }

    // the BBD's input side: an anti-alias filter (then a hold at the chip's clock rate, in run)
    inline double bbdIn(int c, double x)
    {
        double *z = YBA + c * 2;
        z[0] += y_baa * (x - z[0]);
        z[1] += y_baa * (z[0] - z[1]);
        return z[1];
    }

    // every 8 samples: the settings, and where each voice's delay is heading
    void ctl()
    {
        // ---- settings ----
        int ln = (int)clampd(std::floor(cpv(P_MODE) + 0.5), 0, 4);
        if (ln == 4 && y_mode != 4) std::fill(YDB.begin(), YDB.end(), 0.0);   // a fresh dry line for the through-zero flanger
        y_mode = ln;
        y_n = y_mode == 0 ? clampd(std::floor(cpv(P_VOICES) + 0.5), 1, 8) : (y_mode == 1 ? 3 : 1);
        y_sync = (int)clampd(std::floor(cpv(P_SYNC) + 0.5), 0, 16);
        y_cycinv = y_sync > 0 ? 1 / YBEATS[y_sync] : 0;
        y_rate = cpv(P_RATE);
        y_Dt = clampd(cpv(P_DELAY), 0.1, 40);
        y_vt = emin(clampd(cpv(P_DEPTH), 0, 15), emax(0, y_Dt - 0.1));
        // through-zero: the dry signal waits a whole number of samples (the latency), the wet one sweeps around it
        y_ddn = y_mode == 4 ? emax(4, std::floor(y_Dt * 0.001 * srate + 0.5)) : 0;
        y_stph = clampd(cpv(P_PHASE), 0, 180) / 360;
        y_shape = (int)clampd(std::floor(cpv(P_SHAPE) + 0.5), 0, 2);
        y_rnd = clampd(cpv(P_RAND), 0, 100) * 0.01;
        y_fbt = clampd(cpv(P_FB), -95, 95) * 0.01;
        y_xft = clampd(cpv(P_XFB), 0, 100) * 0.01;
        y_spm = clampd(cpv(P_SPREAD), 0, 100) * 0.01;
        y_hq = cpv(P_HQ) >= 0.5;
        y_hql = 0;
        y_ingt = epow(10, clampd(cpv(P_IN), -24, 24) / 20);
        // voice weights: spread across the stereo field (equal power), scaled so the sum keeps its level
        if (y_n != y_wn || y_spm != y_wsp || y_snap) {
            y_wn = y_n; y_wsp = y_spm;
            for (int i = 0; i < 8; ++i) {
                if (i < y_n) {
                    double rk = (i & 1) ? y_n - 1 - (double)(i - 1) / 2 : (double)i / 2;
                    double ps = y_n > 1 ? 2 * rk / (y_n - 1) - 1 : 0;
                    double w = 1.41421356 * std::cos((ps + 1) * 0.78539816);
                    YWG[i] = (1 + y_spm * (w - 1)) / std::sqrt(y_n);
                    w = 1.41421356 * std::sin((ps + 1) * 0.78539816);
                    YWG[8 + i] = (1 + y_spm * (w - 1)) / std::sqrt(y_n);
                } else {
                    YWG[i] = 0; YWG[8 + i] = 0;
                }
            }
        }
        y_nl = 0;
        for (int i = 0; i < 8; ++i) {
            if (y_snap) { YWGc[i] = YWG[i]; YWGc[8 + i] = YWG[8 + i]; }
            else {
                YWGc[i] += (YWG[i] - YWGc[i]) * 0.15;
                YWGc[8 + i] += (YWG[8 + i] - YWGc[8 + i]) * 0.15;
            }
            if (YWGc[i] > 0.0004 || YWGc[8 + i] > 0.0004) y_nl = i + 1;
        }
        // character: tape has a head bump, a loss and a little wow; a BBD has a clock (its delay over its stage count),
        // an anti-alias filter in front of it and a reconstruction filter behind it, both following that clock
        y_char = (int)clampd(std::floor(cpv(P_CHAR) + 0.5), 0, 2);
        double ag = clampd(cpv(P_AGE), 0, 100) * 0.01;
        double f = y_char == 2 ? 102.4 / (y_Dt * 0.001) * epow(0.4, ag) : 15000 * epow(0.22, ag);
        y_ca = 1 - std::exp(-2 * PI * emin(f, srate * 0.45) / srate);
        y_bbdon = y_char == 2;
        if (y_bbdon) {
            f = 256 / (y_Dt * 0.001);   // the clock, Hz
            y_bclk = emin(1, f / srate);
            y_baa = 1 - std::exp(-2 * PI * emin(0.45 * f, srate * 0.45) / srate);
            // High quality: the same at four times the rate
            y_bclk4 = emin(1, f / (4 * srate));
            y_baa4 = 1 - std::exp(-2 * PI * emin(0.45 * f, 4 * srate * 0.45) / (4 * srate));
        }
        // High quality: the 4x filters' delay (the drive's always, the BBD's when it's on), taken off the voices' times
        if (y_hq) y_hql = Os4::DELAY + (y_bbdon ? Os4::DELAY : 0);
        y_nz = y_char == 2 ? ag * ag * 0.004 : (y_char == 1 ? ag * ag * 0.0012 : 0);
        y_wowa = y_char == 1 ? 0.02 + 0.08 * ag : 0;
        if (y_char == 1 && (ag != YCH[0] || y_snap)) { YCH[0] = ag; flt::coef(0, 70, 1.2 + 2.3 * ag, 0.8, 0, YBMC, srate); }
        double x = clampd(cpv(P_DRIVE), 0, 100);
        y_dgo = 1 + x * 0.09;
        y_dgb = emin(1, x * 0.2);
        // wet filters
        x = clampd(cpv(P_HP), 20, 2000);
        y_hpon = x > 20.5;
        if (y_hpon && (x != YCH[1] || y_snap)) { YCH[1] = x; flt::coef(6, x, 0, 0.7071, 0, YFC, srate); }
        x = clampd(cpv(P_LP), 200, 20000);
        y_lpon = x < 19500;
        if (y_lpon && (x != YCH[2] || y_snap)) { YCH[2] = x; flt::coef(10, x, 0, 0.7071, 0, YFC + 5, srate); }
        // width, dry/wet (equal power; a vibrato is all wet)
        y_wdt = clampd(cpv(P_WIDTH), 0, 200) * 0.01;
        x = clampd(cpv(P_MIX), 0, 100) * 0.01;
        if (y_mode == 3) x = 1;
        y_mxt = std::sin(x * PI * 0.5);
        y_dyt = std::cos(x * PI * 0.5);
        // ---- the LFO ----
        if (y_sync > 0) y_pu = y_bt * y_cycinv;
        else y_pu += y_rate * 8 / srate;
        y_pf += 5.5 * 8 / srate;
        y_pf -= std::floor(y_pf);
        y_Dc += (y_Dt - y_Dc) * (y_snap ? 1 : 0.03);
        y_vc += (y_vt - y_vc) * (y_snap ? 1 : 0.03);
        double dz = y_mode == 4 ? y_ddn * 1000 / srate : y_Dc;   // the centre of the sweep (ms)
        if (y_hq) { y_vc0 = y_snap ? y_vc : y_vc1; y_vc1 = y_vc; }
        // the tape's wow: two slow random values, one per side
        for (int c = 0; c < 2; ++c) {
            double t = YWP[c] + 0.6 * 8 / srate;
            if (t >= 1) { t -= 1; YWV[c * 2] = YWV[c * 2 + 1]; YWV[c * 2 + 1] = eel_rand(2) - 1; }
            YWP[c] = t;
        }
        int nv = (int)emax(y_n, y_nl);
        if (y_hq)
            for (int c = 0; c < 2; ++c) {
                double w = dz;
                if (y_wowa > 0) {
                    double t = YWP[c];
                    w += y_wowa * (YWV[c * 2] + (YWV[c * 2 + 1] - YWV[c * 2]) * (t * t * (3 - 2 * t)));
                }
                BD0[c] = y_snap ? w : BD1[c];
                BD1[c] = w;
            }
        for (int i = 0; i < nv; ++i) {
            YPJ[i] = YPJ[i] * 0.997 + (eel_rand(2) - 1) * 0.006 * y_rnd;
            for (int c = 0; c < 2; ++c) {
                double p = y_pu + ((y_mode == 0 || y_mode == 1) ? i / y_n : 0) + c * y_stph + YPJ[i];
                if (y_hq) {
                    int k = c * 8 + i;
                    double e = y_pf + (double)i / 3 + c * y_stph * 0.5;
                    PP0[k] = y_snap ? p : PP1[k];
                    PP1[k] = p;
                    // (the second LFO wraps: keep its path the short way round)
                    double e0 = y_snap ? e : EP1[k];
                    e0 -= std::floor(e0);
                    e -= std::floor(e);
                    if (e < e0 - 0.5) e += 1;
                    EP0[k] = e0;
                    EP1[k] = e;
                }
                double l = lfo(y_shape, p, i);
                if (y_mode == 1) l = 0.7 * l + 0.3 * std::sin(6.283185307 * (y_pf + (double)i / 3 + c * y_stph * 0.5));
                double d = dz + y_vc * l;
                if (y_wowa > 0) {
                    double t = YWP[c];
                    d += y_wowa * (YWV[c * 2] + (YWV[c * 2 + 1] - YWV[c * 2]) * (t * t * (3 - 2 * t)));
                }
                d = emax(4, emin((double)(YDLEN - 8), d * 0.001 * srate - y_hql));
                int k = c * 8 + i;
                if (y_snap) { YCUR[k] = d; YSTP[k] = 0; }
                else YSTP[k] = (d - YCUR[k]) * 0.125;
            }
        }
        if (y_snap) { y_ingc = y_ingt; y_fbc = y_fbt; y_xfc = y_xft; y_wdc = y_wdt; y_mxc = y_mxt; y_dyc = y_dyt; }
        y_snap = false;
    }

    // one sample: the input in, the wet signal (y_wl, y_wr) and the dry signal (y_dl, y_dr: delayed in through-zero) out
    inline void run(double il, double ir)
    {
        if (++y_tc >= 8) { y_tc = 0; ctl(); }
        y_fbc += (y_fbt - y_fbc) * YKS;
        y_xfc += (y_xft - y_xfc) * YKS;
        y_wdc += (y_wdt - y_wdc) * YKS;
        int64_t pm = y_wp & YDM;
        double *L = YB.data(), *Rt = YB.data() + YDLEN;
        // the dry signal (only the through-zero flanger delays it)
        if (y_mode == 4) {
            int64_t dn = (int64_t)y_ddn;
            y_dl = YDB[(y_wp - dn) & YDM];
            y_dr = YDB[YDLEN + ((y_wp - dn) & YDM)];
            YDB[pm] = il;
            YDB[YDLEN + pm] = ir;
        } else {
            y_dl = il; y_dr = ir;
        }
        // nothing in and nothing coming out for a whole line's length: nothing to compute (an aged character's noise keeps it going)
        if (emax(std::fabs(il), std::fabs(ir)) > 0.000000001 || y_wetabs > 0.000000001 || y_nz > 0.000000001) y_sil = 0;
        else y_sil += 1;
        if (y_sil >= YDLEN) {
            y_wl = 0; y_wr = 0; y_wetabs = 0;
            L[pm] = il; Rt[pm] = ir;
        } else {
            double wl = 0, wr = 0;
            if (y_hq) {
                // High quality: each voice's delay from its LFO this very sample, every voice read with six points
                y_hqlc = y_hql;
                double fr = (y_tc + 1) * 0.125, vc = y_vc0 + (y_vc1 - y_vc0) * fr, ms = 0.001 * srate;
                double bl = BD0[0] + (BD1[0] - BD0[0]) * fr, br = BD0[1] + (BD1[1] - BD0[1]) * fr;
                double hi = (double)(YDLEN - 8);
                for (int i = 0; i < y_nl; ++i) {
                    for (int c = 0; c < 2; ++c) {
                        int k = c * 8 + i;
                        double p = PP0[k] + (PP1[k] - PP0[k]) * fr;
                        double l = lfo(y_shape, p, i);
                        if (y_mode == 1) l = 0.7 * l + 0.3 * std::sin(6.283185307 * (EP0[k] + (EP1[k] - EP0[k]) * fr));
                        YCUR[k] = emax(4, emin(hi, ((c ? br : bl) + vc * l) * ms - y_hqlc));
                    }
                    double w = YWGc[i];
                    if (w > 0.0004) wl += w * rd6(L, YCUR[i]);
                    w = YWGc[8 + i];
                    if (w > 0.0004) wr += w * rd6(Rt, YCUR[8 + i]);
                }
            } else {
                bool u6 = y_hq && y_n <= 4;   // six-point reads where a voice is heard clearly; four for a crowd or a voice panned away
                for (int i = 0; i < y_nl; ++i) {
                    YCUR[i] += YSTP[i];
                    YCUR[8 + i] += YSTP[8 + i];
                    double w = YWGc[i];
                    if (w > 0.0004) wl += w * ((u6 && w >= 0.3) ? rd6(L, YCUR[i]) : rd(L, YCUR[i]));
                    w = YWGc[8 + i];
                    if (w > 0.0004) wr += w * ((u6 && w >= 0.3) ? rd6(Rt, YCUR[8 + i]) : rd(Rt, YCUR[8 + i]));
                }
            }
            wl = postWet(0, wl);
            wr = postWet(1, wr);
            // the feedback: the wet signal, without its DC, back into the lines (a share of it crosses to the other side)
            double wi, wj;
            if (std::fabs(y_fbc) > 0.0005) {
                double fl = wl - YDX[0] + DC_R * YDY[0];
                YDX[0] = wl; YDY[0] = fl;
                double fr = wr - YDX[1] + DC_R * YDY[1];
                YDX[1] = wr; YDY[1] = fr;
                fl = lim(fl); fr = lim(fr);
                wi = il + y_fbc * (fl + (fr - fl) * y_xfc);
                wj = ir + y_fbc * (fr + (fl - fr) * y_xfc);
            } else {
                wi = il; wj = ir;
            }
            if (y_bbdon && y_hq) {
                // High quality: the BBD at four times the rate (its anti-alias filter, then the hold at its clock)
                double ph = y_b4ph;
                for (int c = 0; c < 2; ++c) {
                    double *z = BB4 + c * 2;
                    double *hd = BH4 + c;
                    double cph = ph;
                    const double aa = y_baa4, ck = y_bclk4;
                    double &io = c ? wj : wi;
                    io = OSB[c].run(io, [z, hd, &cph, aa, ck](double u) {
                        z[0] += aa * (u - z[0]);
                        z[1] += aa * (z[0] - z[1]);
                        if ((cph += ck) >= 1) { cph -= 1; *hd = z[1]; }
                        return *hd;
                    });
                    if (c == 1) y_b4ph = cph;
                }
            } else if (y_bbdon) {
                wi = bbdIn(0, wi);
                wj = bbdIn(1, wj);
                if ((y_bph += y_bclk) >= 1) { y_bph -= 1; YBHD[0] = wi; YBHD[1] = wj; }
                wi = YBHD[0]; wj = YBHD[1];
            }
            L[pm] = wi; Rt[pm] = wj;
            // width
            double m = (wl + wr) * 0.5, s = (wl - wr) * 0.5 * y_wdc;
            y_wl = m + s; y_wr = m - s;
            y_wetabs = emax(std::fabs(y_wl), std::fabs(y_wr));
        }
        y_bt += y_bps;
        y_wp = (y_wp + 1) & 1073741823;
    }
};

// ---------------------------------------------------------------- the plug-in
class ChorusPlugin : public Plugin, public EngineHost {
public:
    explicit ChorusPlugin(const clap_host *h) : Plugin(h, kind()), eng(*this, act, uiLink, view) {}
    UiLink uiLink;
    Display view;
    Engine eng;
    double pv(int id) const override { return P.get(id); }
    double abv(int id) const override { return abValue(id); }
    gui::View *createView() override;

protected:
    void dspActivate(double sr, uint32_t) override { eng.init(sr); }
    void dspReset() override { eng.reset(); }
    int latencyFromParams(double sr) const override { return tzLatency(P.get(P_MODE), P.get(P_DELAY), sr); }
    void dspProcess(const double *const *in, const double *const *, double *const *out, int n) override
    {
        eng.block(n, tr);
        act.block(n);
        eng.afterAct();
        eng.samples(in, out, n);
        act.process(out, 2, n);
        eng.post(out, n);
        act.audioKey(out, 2, n);
    }
};

static Plugin *create(const clap_host *h) { return new ChorusPlugin(h); }

// ---------------------------------------------------------------- the window
class ChorusView : public EffectView {
public:
    explicit ChorusView(ChorusPlugin &p) : EffectView(p, p.uiLink, 940, 676, "CHORUS"), C(p)
    {
        SH = 196 + 34 + 15;
        help = {"Chorus: up to 8 voices. Ensemble: three voices on two LFOs. Flanger: a short delay with feedback. Vibrato: all wet.",
                "Through-zero: the dry waits as long as the Delay (that is the latency) and the voices sweep around it.",
                "Character: Tape adds a head bump, loss and wow; BBD a clock, its filters and noise. Age says how much.",
                "High quality: drive and BBD at 4x, the LFOs every sample, every voice finely read (the shortest delay is then 0.3 ms)."};
    }
    void frame(Ui &ui) override;
    void opened() override { spi.reset(); spo.reset(); }

protected:
    void hashMore(std::function<void(uint64_t)> mix) override
    {
        mix((uint64_t)(C.view.tempo.load() * 100));
        mix((uint64_t)C.view.side.load());
        mix((uint64_t)(fdrag + 2));
    }
    bool extraFull() override { return fdrag >= 0; }

private:
    ChorusPlugin &C;
    Spectrum spi, spo;
    int fdrag = -1, nhover = -1;
    // the response curve, worked out again only when a setting changed or the LFO moved a little
    std::vector<float> curve;
    double sigp = -1, cpp = 0;
    float cnp = 0;

    // the settings as the window draws them (from the parameters)
    int mode = 0, shape = 0, nv = 4, sy = 0;
    double ddn = 0, dly = 12, dev = 2.5, stph = 0.25, hz = 0.6, fr = 1;
    void settingsNow()
    {
        double sr = C.sampleRate();
        mode = iclamp(iround(C.get(P_MODE)), 0, 4);
        shape = iclamp(iround(C.get(P_SHAPE)), 0, 2);
        nv = mode == 0 ? iclamp(iround(C.get(P_VOICES)), 1, 8) : (mode == 1 ? 3 : 1);
        ddn = tzLatency(C.get(P_MODE), C.get(P_DELAY), sr);
        dly = mode == 4 ? ddn * 1000 / sr : C.get(P_DELAY);
        dev = std::min(C.get(P_DEPTH), std::max(0.0, dly - 0.1));
        stph = C.get(P_PHASE) / 360;
        sy = iclamp(iround(C.get(P_SYNC)), 0, 16);
        hz = sy > 0 ? C.view.tempo.load() / 60 / YBEATS[sy] : C.get(P_RATE);
        fr = 5.5 / std::max(0.01, hz);
    }
    // voice i's delay in ms for channel c when the LFO is at phase p0 (cycles)
    double vd(double p0, int i, int c) const
    {
        double p = p0 + ((mode == 0 || mode == 1) ? (double)i / nv : 0) + c * stph;
        double l = lfo(shape, p, i);
        if (mode == 1) l = 0.7 * l + 0.3 * std::sin(6.283185307 * (p0 * fr + (double)i / 3 + c * stph * 0.5));
        return dly + dev * l;
    }
    static Col voiceCol(int v)
    {
        static const uint32_t c[8] = {0, 0, 0x40c7c7, 0x66d166, 0x6699ff, 0xf273a6, 0xebcc4d, 0xd96659};
        return v == 0 ? th::orange() : (v == 1 ? th::purple() : Col::hex(c[v & 7]));
    }
    void modeMenu(Ui &ui, const R &r);
    void delays(Ui &ui, const R &r);
    void response(Ui &ui, const R &r);
    void controls(Ui &ui, float y);
};

gui::View *ChorusPlugin::createView() { return new ChorusView(*this); }

// a new mode comes with settings that suit it
void ChorusView::modeMenu(Ui &ui, const R &r)
{
    int m = iclamp(iround(C.get(P_MODE)), 0, 4);
    if (!ui.dropdown(r, L_MODE[m], true, "Mode")) return;
    std::vector<MenuItem> items;
    for (int i = 0; i < 5; ++i) items.push_back(MenuItem::item(L_MODE[i], i + 1, i == m));
    ChorusPlugin *pp = &C;
    ui.menu(items, r.x, r.b() + 4, [pp](int r2) {
        if (r2 <= 0) return;
        int k = r2 - 1;
        auto set = [pp](int id, double v) { pp->setOnce(id, v); };
        set(P_MODE, k);
        set(P_XFB, 0);
        if (k == 0) { set(P_DELAY, 12); set(P_DEPTH, 2.5); set(P_RATE, 0.6); set(P_SYNC, 0); set(P_VOICES, 4); set(P_FB, 0); set(P_MIX, 50); set(P_HP, 150); set(P_SPREAD, 70); }
        if (k == 1) { set(P_DELAY, 14); set(P_DEPTH, 3); set(P_RATE, 0.5); set(P_SYNC, 0); set(P_FB, 0); set(P_MIX, 50); set(P_HP, 150); set(P_SPREAD, 90); }
        if (k == 2) { set(P_DELAY, 2.5); set(P_DEPTH, 2); set(P_RATE, 0.2); set(P_SYNC, 0); set(P_FB, 60); set(P_MIX, 50); set(P_HP, 20); set(P_SPREAD, 60); }
        if (k == 3) { set(P_DELAY, 6); set(P_DEPTH, 3); set(P_RATE, 5); set(P_SYNC, 0); set(P_FB, 0); set(P_HP, 20); set(P_SPREAD, 0); }
        if (k == 4) { set(P_DELAY, 5); set(P_DEPTH, 4.5); set(P_RATE, 0.15); set(P_SYNC, 0); set(P_FB, 50); set(P_MIX, 50); set(P_HP, 20); set(P_SPREAD, 60); set(P_PHASE, 60); }
    }, r.w);
}

// display 1: each voice's delay over two cycles of the LFO (left solid, right dim), and where the LFO is now
void ChorusView::delays(Ui &ui, const R &r)
{
    const float py0 = r.y + 24, ph = r.h - 32;   // the plot, under the header strip
    double top = std::max(1.0, (dly + dev) * 1.15);
    auto yOf = [&](double d) { return py0 + ph - (float)(std::max(0.0, d) / top) * ph; };
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        double stp = top <= 3 ? 1 : (top <= 8 ? 2 : (top <= 16 ? 5 : (top <= 40 ? 10 : 20)));
        for (double d = 0; d <= top; d += stp) {
            float yy = yOf(d);
            ui.fill(R(r.x, yy, r.w, 1 / ui.s), Paint(d == 0 ? th::gridHi() : th::grid()));
            char s[16];
            snprintf(s, sizeof s, "%.0f ms", d);
            ui.text(R(r.x + 6, yy - 12, 40, 11), s, th::faint(), F_REGULAR, 9.5f);
        }
        ui.fill(R(r.x + r.w * 0.5f, py0, 1 / ui.s, ph), Paint(th::grid()));
        static const char *desc[] = {"Chorus: up to 8 voices", "Ensemble: three voices, two LFOs", "Flanger: a short delay with feedback",
                                     "Vibrato: all wet, only the pitch wobble", "Through-zero: the dry waits too (latency)"};
        ui.text(R(r.x + 10, r.y + 6, 300, 12), desc[mode], th::dim(), F_MEDIUM, 10.5f);
        ui.text(R(r.x + 300, r.y + 6, r.w - 310, 12), "each voice's delay over two LFO cycles: left solid, right dim", th::faint(), F_REGULAR, 10.5f, 2);
        // the lines (the right side dim, under the left)
        Canvas &c = ui.cv();
        c.pushClip(ui.X(r.x + 1), ui.Y(r.y + 1), ui.W(r.w - 2), ui.W(r.h - 2));
        std::vector<Pt> pts(176);
        for (int v = nv - 1; v >= 0; --v)
            for (int ch = 1; ch >= 0; --ch) {
                for (int i = 0; i < 176; ++i) {
                    double d = vd(i / 175.0 * 2, v, ch);
                    pts[i] = {ui.X(r.x + i * r.w / 175), ui.Y(yOf(d))};
                }
                Col col = voiceCol(v).alpha(ch ? 0.35f : 1.f);
                c.strokePolyline(pts.data(), 176, ui.W(ch ? 1.2f : 1.6f), Paint(col));
            }
        if (mode == 4) ui.fill(R(r.x, yOf(dly), r.w, 1.2f), Paint(Col(0.9f, 0.9f, 0.92f, 0.5f)));   // the dry
        c.popClip();
        ui.stroke(r, Paint(th::border()), 6);
    }
    if (ui.drawD() && live) {
        double pu = C.view.pu.load();
        float cp = (float)(pu - std::floor(pu));
        Col mk(1.0f, 0.95f, 0.88f, 0.6f);
        ui.fill(R(r.x + cp * 0.5f * r.w, py0 - 4, 1.2f, ph + 4), Paint(mk));
        ui.fill(R(r.x + (cp * 0.5f + 0.5f) * r.w, py0 - 4, 1.2f, ph + 4), Paint(mk));
    }
}

// display 2: the whole plug-in's frequency response with the voices where they are now, over the spectrum;
// H (the wet high pass) and L (the wet low pass) slide sideways
void ChorusView::response(Ui &ui, const R &r)
{
    FreqScale fs(r);
    auto ey = [&](double db) { return r.y + r.h * 0.5f - (float)(db / 18) * r.h * 0.5f; };
    double hpf = C.get(P_HP), lpf = C.get(P_LP);
    float nxh = (float)fs.x(hpf), nxl = (float)fs.x(lpf), ny = ey(0);
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        freqGrid(ui, r);
        for (int d = -12; d <= 12; d += 6) {
            ui.fill(R(r.x, ey(d), r.w, 1 / ui.s), Paint(d == 0 ? th::gridHi() : th::grid()));
            char s[8];
            snprintf(s, sizeof s, d > 0 ? "+%d" : "%d", d);
            ui.text(R(r.x + 6, ey(d) - 12, 30, 11), s, th::faint(), F_REGULAR, 9.5f);
        }
        // the nodes: hover, drag, right-click to reset
        float mx = ui.mx(), my = ui.my();
        bool ind = ui.hit(r);
        nhover = -1;
        if (ind) {
            float best = 1e9f, d = std::hypot(mx - nxh, my - ny);
            if (d < 10 && d < best) { best = d; nhover = 0; }
            d = std::hypot(mx - nxl, my - ny);
            if (d < 10 && d < best) { best = d; nhover = 1; }
        }
        if (fdrag >= 0) nhover = fdrag;
        if (nhover >= 0 || fdrag >= 0) ui.cursor = CUR_HRESIZE;
        if (ind && ui.lclick && !ui.used && nhover >= 0 && !ui.busy()) {
            ui.used = true;
            fdrag = nhover;
            C.beginEdit(fdrag == 0 ? P_HP : P_LP);
        }
        if (ind && ui.rclick && !ui.used && nhover >= 0 && !ui.busy()) {
            ui.used = true;
            int which = nhover;
            ChorusPlugin *pp = &C;
            ui.menu({MenuItem::item("Reset this filter", 1)}, mx, my, [pp, which](int k) {
                if (k == 1) pp->setOnce(which == 0 ? P_HP : P_LP, which == 0 ? 150 : 16000);
            });
        }
        if (fdrag >= 0) {
            int id = fdrag == 0 ? P_HP : P_LP;
            if (ui.ldown) {
                double fv = std::max(20.0, std::min(20000.0, fs.f(mx)));
                C.edit(id, fdrag == 0 ? std::max(20.0, std::min(2000.0, fv)) : std::max(200.0, std::min(20000.0, fv)));
            } else {
                C.endEdit(id);
                fdrag = -1;
            }
        }
        char hint[160], b[32];
        if (nhover == 0) { f_hz(hpf, b, sizeof b); snprintf(hint, sizeof hint, "Wet high pass: %s", b); }
        else if (nhover == 1) { f_hz(lpf, b, sizeof b); snprintf(hint, sizeof hint, "Wet low pass: %s", b); }
        else snprintf(hint, sizeof hint, "the whole plug-in with the voices where they are now; drag H (wet high pass) and L (wet low pass), right-click resets");
        ui.text(R(r.x + 40, r.y + 6, r.w - 50, 12), hint, nhover >= 0 ? th::text() : th::faint(), F_REGULAR, 10.5f);
        ui.stroke(r, Paint(th::border()), 6);
    }
    if (!ui.drawD()) return;
    Canvas &c = ui.cv();
    double sr = C.sampleRate();
    c.pushClip(ui.X(r.x + 1), ui.Y(r.y + 1), ui.W(r.w - 2), ui.W(r.h - 2));
    // the spectrum: the input filled, the output as a line (filled when it is alone)
    int an = iclamp(iround(C.get(P_AN)), 0, 3);
    if (live && an > 0) {
        int ap = C.view.an.ap.load(std::memory_order_acquire);
        if (an == 1 || an == 3) {
            spi.update(C.view.an.in, AnFeed::N - 1, ap, ui.dt, sr);
            Paint p = Paint::vertical(ui.Y(r.y), Col::hex(0x3a3a46), ui.Y(r.b()), Col::hex(0x1c1c22));
            drawSpectrum(ui, fs, spi, sr, &p, nullptr);
        }
        if (an == 2 || an == 3) {
            spo.update(C.view.an.out, AnFeed::N - 1, ap, ui.dt, sr);
            Paint p = Paint::vertical(ui.Y(r.y), Col::hex(0x40260d), ui.Y(r.b()), Col::hex(0x1a1008));
            Col lc = Col::hex(0xc8c3bb).alpha(0.5f);
            drawSpectrum(ui, fs, spo, sr, an == 2 ? &p : nullptr, &lc, 1.1f);
        }
    }
    freqLabels(ui, r);
    // the response curve
    double pu = C.view.pu.load(), cp = pu - std::floor(pu);
    double co[40];
    int ns0 = hpf > 20.5 ? flt::coef(6, hpf, 0, 0.7071, 0, co, sr) : 0;
    int ns1 = lpf < 19500 ? flt::coef(10, lpf, 0, 0.7071, 0, co + 20, sr) : 0;
    double mx2 = mode == 3 ? 1 : C.get(P_MIX) * 0.01, dg = std::cos(mx2 * PI * 0.5), wg = std::sin(mx2 * PI * 0.5);
    double fb = C.get(P_FB) * 0.01, xf = C.get(P_XFB) * 0.01, vw = 1 / std::sqrt((double)nv);
    float cn = std::floor(r.w / 2);
    double sig = mode * 7.1 + nv * 13.3 + dly * 17.7 + dev * 19.9 + stph * 23.1 + shape * 29.3 + hpf * 0.031 + lpf * 0.0037 + mx2 * 41.7 + fb * 43.9 +
                 xf * 47.3 + fr * 3.7 + r.w + sr;
    double dph = std::fabs(cp - cpp);
    dph = std::min(dph, 1 - dph);
    if (sig != sigp || dph > 0.015 || cn != cnp) {
        sigp = sig; cpp = cp; cnp = cn;
        int n = (int)cn + 1;
        curve.resize(n);
        for (int i = 0; i < n; ++i) {
            double f = fs.f(r.x + i * r.w / cn), w = 2 * PI * f / sr, cw = std::cos(w), c2 = std::cos(2 * w);
            double fm = 1;
            if (ns0) fm *= std::sqrt(std::max(1e-12, flt::mag2(co, ns0, cw, c2)));
            if (ns1) fm *= std::sqrt(std::max(1e-12, flt::mag2(co + 20, ns1, cw, c2)));
            double sre = 0, sim = 0;
            for (int j = 0; j < nv; ++j) {
                double dl = vd(cp, j, 0) * 0.001 * sr;
                sre += vw * std::cos(w * dl);
                sim -= vw * std::sin(w * dl);
            }
            sre *= fm; sim *= fm;
            // the feedback loop: with a cross-feedback the left and right sums feed it (worked out for their average)
            double lr = sre, li = sim;
            if (std::fabs(fb) > 0.0005 && xf > 0.001) {
                double s2r = 0, s2i = 0;
                for (int j = 0; j < nv; ++j) {
                    double dl = vd(cp, j, 1) * 0.001 * sr;
                    s2r += vw * std::cos(w * dl);
                    s2i -= vw * std::sin(w * dl);
                }
                lr = sre + (s2r * fm - sre) * xf;
                li = sim + (s2i * fm - sim) * xf;
            }
            // with feedback: S / (1 - fb S)
            double den = (1 - fb * lr) * (1 - fb * lr) + fb * li * fb * li;
            double hr = (sre * (1 - fb * lr) - sim * fb * li) / std::max(0.000001, den);
            double hi = (sim * (1 - fb * lr) + sre * fb * li) / std::max(0.000001, den);
            // the dry (waiting a whole number of samples in through-zero)
            double tr = dg * std::cos(w * ddn) + wg * hr, ti = -dg * std::sin(w * ddn) + wg * hi;
            double tot = 10 * std::log10(std::max(1e-12, tr * tr + ti * ti));
            curve[i] = ey(std::max(-18.0, std::min(18.0, tot)));
        }
    }
    {
        int n = (int)curve.size();
        std::vector<Pt> pts(n);
        for (int i = 0; i < n; ++i) pts[i] = {ui.X(r.x + i * r.w / cn), ui.Y(curve[i])};
        c.strokePolyline(pts.data(), n, ui.W(1.5f), Paint(th::orange()));
    }
    // the nodes, on top
    for (int i = 0; i < 2; ++i) {
        float nx = i == 0 ? nxh : nxl, rad = i == nhover ? 8 : 6.5f;
        c.fillCircle(ui.X(nx), ui.Y(ny), ui.W(rad), Paint(i == nhover ? th::orangeHi() : th::orange()));
        ui.text(R(nx - 8, ny - 7, 16, 14), i == 0 ? "H" : "L", th::orangeInk(), F_BOLD, 10, 1);
    }
    c.popClip();
}

// the controls: a 5 x 3 grid where every control always has its place; what doesn't apply to the mode or the
// character now is shown greyed (Cross fb without feedback, Age with a clean character, Spread with one voice, Dry/wet
// for the vibrato, the rate in Hz while it follows the tempo)
void ChorusView::controls(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float cw = (DW - 2 * M - 2 * 20) / 3, h = 26, dy = 32, lw = 72, bw = 84;
    float x[3] = {M, M + cw + 20, M + 2 * (cw + 20)};
    auto row = [&](int col, int r, const char *lbl, int id, int sc, bool en = true) {
        ui.paramRow(R(x[col], y + r * dy, cw, h), lbl, id, sc, lw, bw, en, sc == SC_LOG ? KD_LOG : KD_OTHER);
    };
    // the rate: free in Hz, or a note value at the tempo
    {
        float yy = y;
        label(ui, R(x[0], yy, lw, h), "Rate");
        enumDD(ui, R(x[0] + lw, yy, 92, h), P_SYNC, L_SYNC, 17);
        char b[32];
        f_rate(hz, b, sizeof b);
        ui.valueBox(R(x[0] + lw + 100, yy + 2, cw - lw - 100, h - 4), P_RATE, sy == 0, KD_LOG, std::log(2000.0) / 138, std::log(2000.0) / 1380, 2,
                    sy > 0 ? b : nullptr);
    }
    row(1, 0, "Random", P_RAND, SC_LIN);
    inputRow(ui, R(x[2], y, cw, h), P_IN, lw, bw);
    row(0, 1, "Delay", P_DELAY, SC_LOG);
    row(1, 1, "Feedback", P_FB, SC_LIN);
    row(2, 1, "Drive", P_DRIVE, SC_LIN);
    row(0, 2, "Depth", P_DEPTH, SC_LIN);
    row(1, 2, "Cross fb", P_XFB, SC_LIN, std::fabs(C.get(P_FB)) > 0.05);
    row(2, 2, "Age", P_AGE, SC_LIN, C.get(P_CHAR) >= 0.5);
    row(0, 3, "Phase", P_PHASE, SC_LIN);
    row(1, 3, "High pass", P_HP, SC_LOG);
    row(2, 3, "Width", P_WIDTH, SC_LIN);
    row(0, 4, "Spread", P_SPREAD, SC_LIN, nv > 1);
    row(1, 4, "Low pass", P_LP, SC_LOG);
    row(2, 4, "Dry/wet", P_MIX, SC_LIN, mode != 3);
}

void ChorusView::frame(Ui &ui)
{
    settingsNow();
    R gear = begin(ui, ddn * 1000 / C.sampleRate(), C.view.side.load() != 0);
    float y = 66;
    // the mode and the switches
    if (ui.drawS()) {
        modeMenu(ui, R(M, y, 214, 26));
        {
            R vr(M + 222, y, 124, 26);
            char b[8];
            snprintf(b, sizeof b, "%d", nv);
            if (ui.dropdown(vr, b, mode == 0, "Voices")) {
                std::vector<MenuItem> m;
                int cv = iclamp(iround(C.get(P_VOICES)), 1, 8);
                for (int i = 1; i <= 8; ++i) m.push_back(MenuItem::item(i == 1 ? "1 voice" : std::string(L_VOICES[i - 1]) + " voices", i, i == cv));
                ChorusPlugin *pp = &C;
                ui.menu(m, vr.x, vr.b() + 4, [pp](int r2) { if (r2 > 0) pp->setOnce(P_VOICES, r2); }, vr.w);
            }
        }
        enumDD(ui, R(M + 354, y, 184, 26), P_CHAR, L_CHAR, 3, "Character");
        enumDD(ui, R(M + 546, y, 196, 26), P_SHAPE, L_SHAPE, 3, "LFO");
        enumDD(ui, R(M + 750, y, DW - 2 * M - 750, 26), P_HQ, L_HQ, 2, "Quality");
    }
    y += 36;
    delays(ui, R(M, y, DW - 2 * M, 132));
    y += 132 + 10;
    response(ui, R(M, y, DW - 2 * M, 164));
    y += 164 + 12;
    controls(ui, y);
    y += 4 * 32 + 26 + 12;
    actCardAt(ui, y);
    finish(ui, gear, [this](Ui &u, const R &in, float yy) {
        label(u, R(in.x, yy, 64, 24), "Analyzer");
        enumDD(u, R(in.x + 64, yy, 86, 24), P_AN, L_AN, 4);
        u.text(R(in.x + 164, yy, in.w - 164, 24), "Pre: the input, filled. Post: the output, as a line. Only while the window is open.", th::faint(),
               F_REGULAR, 11);
        return yy + 24 + 10;
    });
}

// ---------------------------------------------------------------- the kind
static const char *s_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_CHORUS, CLAP_PLUGIN_FEATURE_FLANGER,
                                   CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const PluginKind &kind()
{
    static const PluginKind k = {
        {CLAP_VERSION_INIT, "com.twostorms.chorus", "TwoStorms Chorus", "TwoStorms", "", "", "", TS_VERSION,
         "A chorus with an Act built in: chorus, ensemble, flanger, vibrato and through-zero flanger, feedback, a tape or BBD character, "
         "drive, wet filters and width; the rate free or at the tempo. No latency except the through-zero flanger's.",
         s_features},
        &table,
        &factoryPresets,
        "Chorus",
        "TWOSTORMS CHORUS",
        shm::KIND_POINT,
        shm::SK_CHORUS,
        false,
        true,
        P_AB,
        &create,
        2,
        false,
    };
    return k;
}

} // namespace chorus
} // namespace ts
