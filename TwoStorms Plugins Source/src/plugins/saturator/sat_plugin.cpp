// TwoStorms Saturator: the plug-in (parameters, presets, the window). The sound is in sat_engine.cpp.
// Port of twostorms_saturator.jsfx.
#include "sat_plugin.h"
#include "sat_engine.h"
#include "../../core/satcore.h"
#include "../../gui/effect_view.h"
#include "../../gui/modslots.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

namespace ts {
namespace sat {

using namespace gui;

// ---------------------------------------------------------------- parameters
static void f_pct(double v, char *b, int n) { snprintf(b, n, "%.0f%%", v); }
static void f_spct(double v, char *b, int n) { snprintf(b, n, "%+.0f%%", v); }
static void f_dbs(double v, char *b, int n) { snprintf(b, n, "%+.1f dB", v); }
static void f_lvl(double v, char *b, int n)
{
    if (v <= -59.9) snprintf(b, n, "-inf dB");
    else snprintf(b, n, "%+.1f dB", v);
}
static void f_dyn(double v, char *b, int n)
{
    if (std::fabs(v) < 0.5) snprintf(b, n, "off");
    else snprintf(b, n, v > 0 ? "comp %.0f%%" : "gate %.0f%%", std::fabs(v));
}
static void f_hz(double v, char *b, int n)
{
    if (v >= 10000) snprintf(b, n, "%.1f kHz", v / 1000);
    else if (v >= 1000) snprintf(b, n, "%.2f kHz", v / 1000);
    else snprintf(b, n, "%.0f Hz", v);
}
static void f_int(double v, char *b, int n) { snprintf(b, n, "%.0f", v); }
static void f_rate(double v, char *b, int n) { snprintf(b, n, v < 10 ? "%.2f" : "%.1f", v); }
static void f_rel(double v, char *b, int n)
{
    if (v < 1000) snprintf(b, n, "%.0f ms", v);
    else snprintf(b, n, "%.2f s", v / 1000);
}

static const char *const L_SLOPE[] = {"12 dB/oct", "24 dB/oct", "48 dB/oct"};
static const char *const L_OS[] = {"Off", "2x", "4x"};
static const char *const L_AN[] = {"Off", "Pre", "Post", "Both"};
static const char *const L_CH[] = {"Stereo", "Mid/Side"};
static const char *const L_MTYPE[] = {"Off", "LFO", "Follower", "MIDI"};
static const char *const L_MBAND[] = {"Global", "Band 1", "Band 2", "Band 3", "Band 4", "Band 5", "Band 6", "All bands"};
static const char *const L_MGLOB[] = {"Global drive", "Global mix", "Input", "Global dynamics"};
static const char *const L_MPAR[] = {"Drive", "Mix", "Level", "Dynamics", "Feedback", "FB freq", "Bass", "Mid", "Treble", "Presence", "Drive pan", "Level pan"};
static const char *const L_MSYNC[] = {"Free", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars", "1/16T", "1/8T", "1/4T", "1/8."};
static const char *const L_MSRC[] = {"Input", "Band", "Sidechain"};

template <size_t N> static std::vector<const char *> labels(const char *const (&a)[N]) { return std::vector<const char *>(a, a + N); }

static const ParamTable &table()
{
    static std::deque<std::string> names;   // the band and slot names, kept for the table's lifetime
    static ParamTable t = [] {
        ParamTable t;
        auto nm = [](const std::string &s) { names.push_back(s); return names.back().c_str(); };
        addActParams(t, true);
        const char *M = "Saturator";
        const uint32_t H = PF_HOST | PF_AB;
        t.add(P_SEL, "Selected band", 0, 5, 0, PF_INT, f_int, M);
        t.add(P_NB, "Bands", 1, 6, 3, H | PF_INT, f_int, M);
        t.addEnum(P_SLOPE, "Crossover slope", labels(L_SLOPE), 1, H, M);
        // in presets, not in the A/B set: a held Swap never changes the latency
        t.addEnum(P_OS, "Oversampling", labels(L_OS), 1, PF_HOST | PF_PRESET, M);
        t.add(P_IN, "Input", -24, 24, 0, H, f_dbs, M).unit = "dB";
        t.add(P_GDRV, "All drive", -100, 100, 0, H, f_spct, M).unit = "%";
        t.add(P_GDYN, "All dynamics", -100, 100, 0, H, f_spct, M).unit = "%";
        t.add(P_MIX, "Dry/wet", 0, 100, 100, H, f_pct, M).unit = "%";
        t.addEnum(P_AN, "Analyzer", labels(L_AN), 3, 0, M);
        t.addEnum(P_AB, "A/B set", {"A", "B"}, 0, PF_HOST, M);
        t.addEnum(P_DELTA, "Delta", {"Off", "On"}, 0, PF_HOST | PF_BOOL, M);
        t.add(P_MSEL, "Selected modulation slot", 0, 7, 0, PF_INT, f_int, M);
        t.addEnum(P_MODOPEN, "Modulation section", {"Closed", "Open"}, 0, 0, M);
        static const double XFD[5] = {150, 3000, 6000, 9000, 13000};
        for (int i = 0; i < 5; ++i)
            t.add(P_XF + i, nm("Crossover " + std::to_string(i + 1)), 20, 20000, XFD[i], H, f_hz, M).unit = "Hz";
        for (int b = 0; b < NB; ++b) {
            std::string L = "Band " + std::to_string(b + 1) + " ";
            const char *mod = nm("Band " + std::to_string(b + 1));
            auto n = [&](const char *s) { return nm(L + s); };
            t.addEnum(ep(b, K_ON), n("on"), {"Bypassed", "On"}, 1, H | PF_BOOL, mod);
            t.addEnum(ep(b, K_MUTE), n("mute"), {"Off", "On"}, 0, H | PF_BOOL, mod);
            t.addEnum(ep(b, K_STYLE), n("style"), labels(STYLE_NAMES), 1, H, mod);
            t.add(ep(b, K_DRIVE), n("drive"), 0, 100, 25, H, f_pct, mod).unit = "%";
            t.add(ep(b, K_MIX), n("mix"), 0, 100, 100, H, f_pct, mod).unit = "%";
            t.add(ep(b, K_LEVEL), n("level"), -60, 36, 0, H, f_lvl, mod).unit = "dB";
            t.add(ep(b, K_DYN), n("dynamics"), -100, 100, 0, H, f_dyn, mod).unit = "%";
            t.add(ep(b, K_FB), n("feedback"), 0, 100, 0, H, f_pct, mod).unit = "%";
            t.add(ep(b, K_FBF), n("feedback frequency"), 20, 20000, 1000, H, f_hz, mod).unit = "Hz";
            t.add(ep(b, K_BASS), n("bass"), -12, 12, 0, H, f_dbs, mod).unit = "dB";
            t.add(ep(b, K_MID), n("mid"), -12, 12, 0, H, f_dbs, mod).unit = "dB";
            t.add(ep(b, K_TRE), n("treble"), -12, 12, 0, H, f_dbs, mod).unit = "dB";
            t.add(ep(b, K_PRES), n("presence"), -12, 12, 0, H, f_dbs, mod).unit = "dB";
            t.add(ep(b, K_DPAN), n("drive balance"), -100, 100, 0, H, f_spct, mod).unit = "%";
            t.add(ep(b, K_LPAN), n("level balance"), -100, 100, 0, H, f_spct, mod).unit = "%";
            t.addEnum(ep(b, K_CH), n("channels"), labels(L_CH), 0, H, mod);
        }
        for (int s = 0; s < NMOD; ++s) {
            std::string L = "Mod " + std::to_string(s + 1) + " ";
            const char *mod = nm("Modulation " + std::to_string(s + 1));
            auto n = [&](const char *x) { return nm(L + x); };
            t.addEnum(mp(s, M_TYPE), n("type"), labels(L_MTYPE), 0, H, mod);
            t.addEnum(mp(s, M_TBAND), n("target band"), labels(L_MBAND), 1, H, mod);
            t.add(mp(s, M_TPAR), n("target setting"), 0, 11, 0, H | PF_INT, f_int, mod);
            t.add(mp(s, M_AMT), n("amount"), -100, 100, 50, H, f_spct, mod).unit = "%";
            t.add(mp(s, M_RATE), n("rate / attack"), 0.01, 500, 1, H, f_rate, mod);
            t.add(mp(s, M_RATE2), n("release"), 1, 2000, 200, H, f_rel, mod).unit = "ms";
            t.add(mp(s, M_OPT), n("shape / source / CC"), 0, 129, 0, H | PF_INT, f_int, mod);
            t.addEnum(mp(s, M_SYNC), n("tempo sync"), labels(L_MSYNC), 0, H, mod);
        }
        return t;
    }();
    return t;
}

// factory presets: Instruments, Buses, Creative, Weird; written band by band (styles as STYLE_NAMES)
namespace {
enum {
    CLEAN_TUBE, WARM_TUBE, HOT_TUBE, BROKEN_TUBE, CLEAN_TAPE, WARM_TAPE, HOT_TAPE, EXTREME_TAPE, TAPE_HEAD, SUBTLE_XF, WARM_XF, LUSH_XF,
    AMERICAN, BRITISH, SMOOTH_AMP, CRUNCH_AMP, SCREAM_AMP, CONSOLE_CH, CONSOLE_BUS, EVEN, INFLATOR, FUZZ, SMUDGE, BREAKDOWN, FOLDBACK,
    SINE_FOLD, RECTIFY, DESTROY, SOFT_CLIP, HARD_CLIP
};
struct SatP {
    std::vector<std::pair<int, double>> v;
    SatP &nb(int n) { v.push_back({P_NB, (double)n}); return *this; }
    SatP &xf(int i, double f) { v.push_back({P_XF + i, f}); return *this; }
    SatP &band(int b, int style, double drive, double mix = 100, double level = 0)
    {
        v.push_back({ep(b, K_STYLE), (double)style});
        v.push_back({ep(b, K_DRIVE), drive});
        v.push_back({ep(b, K_MIX), mix});
        v.push_back({ep(b, K_LEVEL), level});
        return *this;
    }
    SatP &clean(int b) { v.push_back({ep(b, K_ON), 0}); return *this; }   // the band passes untouched
    SatP &tone(int b, double bass, double mid, double tre, double pres = 0)
    {
        v.push_back({ep(b, K_BASS), bass}); v.push_back({ep(b, K_MID), mid}); v.push_back({ep(b, K_TRE), tre}); v.push_back({ep(b, K_PRES), pres});
        return *this;
    }
    SatP &dyn(int b, double d) { v.push_back({ep(b, K_DYN), d}); return *this; }
    SatP &fb(int b, double amt, double f) { v.push_back({ep(b, K_FB), amt}); v.push_back({ep(b, K_FBF), f}); return *this; }
    SatP &set(int id, double x) { v.push_back({id, x}); return *this; }
    operator std::string() const { return presetStr(v); }
};
SatP S1(int style, double drive, double mix = 100, double level = 0) { return SatP().nb(1).band(0, style, drive, mix, level); }
}

static const std::vector<FactoryPreset> &factoryPresets()
{
    static std::vector<FactoryPreset> v = {
        // ---- guitar
        {"Instruments/Guitar/Clean: tube warmth", "45=1 51=100 70=1 72=1 73=20 74=100 75=0 79=0 80=0 81=0 82=0"},
        {"Instruments/Guitar/Clean: tape glow", "45=1 51=100 70=1 72=5 73=25 74=100 75=0 79=0 80=0 81=0 82=0"},
        {"Instruments/Guitar/Funk: light American edge", "45=1 51=100 70=1 72=12 73=25 74=100 75=0 79=0 80=0 81=2 82=0"},
        {"Instruments/Guitar/Rock: British crunch", "45=1 51=100 70=1 72=13 73=55 74=100 75=0 79=-2 80=2 81=0 82=1"},
        {"Instruments/Guitar/Rock: radio intro tube (mild)", "45=1 51=100 70=1 72=0 73=25 74=100 75=0 79=0 80=0 81=0 82=0"},
        {"Instruments/Guitar/Rock: even-harmonic thickener", "45=1 51=100 70=1 72=19 73=40 74=100 75=0 79=0 80=0 81=0 82=0"},
        {"Instruments/Guitar/Metal: scream, tight lows", "45=2 51=100 60=150 70=1 72=9 73=0 74=100 75=0 79=0 80=0 81=0 82=0 86=1 88=16 89=70 90=100 91=0 95=-2 96=2 97=0 98=2"},
        {"Instruments/Guitar/Metal: fuzz wall", "45=1 51=100 70=1 72=21 73=60 74=100 75=0 79=0 80=0 81=-2 82=0"},
        {"Instruments/Guitar/Acoustic: transformer body", S1(SUBTLE_XF, 25).tone(0, 1, 0, 1)},
        // ---- bass
        {"Instruments/Bass/Clean: warm glow", "45=1 47=0 51=100 70=1 72=1 73=20 74=100 75=0 76=0 79=0 80=0 81=0 82=0"},
        {"Instruments/Bass/Distorted: clean lows, driven mids", "45=3 47=0 51=100 60=150 61=2500 70=1 72=0 73=10 74=100 75=0 76=0 79=0 80=0 81=0 82=0 86=1 88=13 89=60 90=100 91=0 92=0 95=0 96=0 97=0 98=0 102=1 104=21 105=30 106=100 107=-6 108=0 111=0 112=0 113=-3 114=0"},
        {"Instruments/Bass/Slap: tape snap", "45=1 47=0 51=100 70=1 72=5 73=25 74=100 75=0 76=0 79=0 80=0 81=0 82=2"},
        {"Instruments/Bass/Punk", "45=3 47=0 51=100 60=150 61=2500 70=1 72=0 73=12 74=100 75=0 76=0 79=0 80=0 81=0 82=0 86=1 88=15 89=65 90=100 91=0 92=0 95=0 96=0 97=0 98=0 102=1 104=21 105=35 106=100 107=-6 108=0 111=0 112=0 113=0 114=0"},
        {"Instruments/Bass/Pop punk", "45=3 47=0 51=100 60=150 61=2500 70=1 72=0 73=10 74=100 75=0 76=0 79=0 80=0 81=0 82=0 86=1 88=13 89=45 90=100 91=0 92=0 95=0 96=0 97=0 98=0 102=1 104=28 105=20 106=100 107=-4 108=0 111=0 112=0 113=0 114=0"},
        {"Instruments/Bass/Metal", "45=3 47=0 51=100 60=120 61=3000 70=1 72=0 73=15 74=100 75=0 76=0 79=0 80=0 81=0 82=0 86=1 88=16 89=75 90=100 91=0 92=0 95=0 96=2 97=0 98=0 102=1 104=29 105=40 106=100 107=-8 108=0 111=0 112=0 113=-4 114=0"},
        {"Instruments/Bass/Rock", "45=3 47=0 51=100 60=150 61=3000 70=1 72=1 73=15 74=100 75=0 76=0 79=0 80=0 81=0 82=0 86=1 88=12 89=45 90=100 91=0 92=0 95=0 96=0 97=0 98=0 102=1 104=1 105=20 106=100 107=-4 108=0 111=0 112=0 113=0 114=0"},
        {"Instruments/Bass/Jazz", "45=1 47=0 51=100 70=1 72=1 73=15 74=60 75=0 76=0 79=0 80=0 81=0 82=0"},
        {"Instruments/Bass/Pop", "45=2 47=0 51=100 60=120 70=1 72=0 73=10 74=100 75=0 76=0 79=0 80=0 81=0 82=0 86=1 88=19 89=30 90=70 91=0 92=0 95=0 96=0 97=0 98=0"},
        {"Instruments/Bass/Small speakers: harmonics above 200 Hz", SatP().nb(2).xf(0, 200).clean(0).band(1, EVEN, 45, 60).tone(1, 0, 2, -2)},
        {"Instruments/Bass/Synth bass: grit on top", SatP().nb(2).xf(0, 250).band(0, CLEAN_TAPE, 15).band(1, CRUNCH_AMP, 50, 70).tone(1, 0, 1, -3)},
        // ---- vocals
        {"Instruments/Vocals/Lead: transformer warmth", "45=1 47=0 51=100 70=1 72=10 73=15 74=100 75=0 76=0 79=0 80=0 81=0 82=0"},
        {"Instruments/Vocals/Lead: subtle tube (parallel 40%)", S1(WARM_TUBE, 35, 40)},
        {"Instruments/Vocals/Lead: air (highs only)", "45=2 47=1 51=100 60=5000 70=1 72=0 73=0 74=0 75=0 76=0 79=0 80=0 81=0 82=0 86=1 88=19 89=35 90=60 91=0 92=0 95=0 96=0 97=0 98=0"},
        {"Instruments/Vocals/Rap: crunch and presence", S1(CRUNCH_AMP, 25, 45).tone(0, -2, 1, 0, 2)},
        {"Instruments/Vocals/Distorted vocal (rock, 60%)", SatP().nb(1).band(0, BRITISH, 60, 60).tone(0, -6, 3, -3, 2)},
        {"Instruments/Vocals/Spoken: console", "45=1 47=0 51=100 70=1 72=17 73=10 74=100 75=0 76=0 79=0 80=0 81=0 82=0"},
        // ---- drums
        {"Instruments/Drums/Kick: weight (tape on the lows)", SatP().nb(2).xf(0, 150).band(0, WARM_TAPE, 40).band(1, CLEAN_TAPE, 10)},
        {"Instruments/Drums/Snare: crack and crunch", S1(BRITISH, 35, 40).tone(0, 0, 2, 1)},
        {"Instruments/Drums/Toms: tape thump", S1(HOT_TAPE, 30)},
        {"Instruments/Drums/Room: smashed", S1(EXTREME_TAPE, 60, 70).dyn(0, 60)},
        {"Instruments/Drums/Hi-hats: softer edges (tape head)", S1(TAPE_HEAD, 30).tone(0, 0, 0, -2)},
        // ---- keys
        {"Instruments/Keys/Electric piano: tube bark", S1(HOT_TUBE, 40).tone(0, 0, 2, 0)},
        {"Instruments/Keys/Piano: tape glue", S1(WARM_TAPE, 20)},
        {"Instruments/Keys/Synth: folded harmonics (30%)", S1(SINE_FOLD, 40, 30)},
        {"Instruments/Keys/Organ: rotary-style growl", S1(SMOOTH_AMP, 45).tone(0, -2, 2, -2)},
        // ---- buses
        {"Buses/Instrument bus/Drum bus: tape", "45=1 47=1 51=100 70=1 72=5 73=30 74=100 75=0 76=0 79=0 80=0 81=0 82=0"},
        {"Buses/Instrument bus/Drum bus: parallel crush (30%)", S1(DESTROY, 50, 30)},
        {"Buses/Instrument bus/Guitar bus: transformer", S1(WARM_XF, 25)},
        {"Buses/Instrument bus/Vocal bus: console", S1(CONSOLE_CH, 20)},
        {"Buses/Mixbus/Console", "45=1 47=1 51=100 70=1 72=18 73=20 74=100 75=0 76=0 79=0 80=0 81=0 82=0"},
        {"Buses/Mixbus/Tape: gentle", S1(WARM_TAPE, 15).set(P_OS, 2)},
        {"Buses/Master/Inflate", "45=1 47=1 51=100 70=1 72=20 73=40 74=100 75=0 76=0 79=0 80=0 81=0 82=0"},
        {"Buses/Master/Soft clip before the limiter", S1(SOFT_CLIP, 20).set(P_OS, 2)},
        {"Buses/Master/Lows in tape, highs clean", SatP().nb(2).xf(0, 200).band(0, WARM_TAPE, 20).clean(1).set(P_OS, 2)},
        // ---- creative
        {"Creative/Lo-fi destroy", "45=1 47=0 51=100 70=1 72=27 73=50 74=60 75=0 76=0 79=0 80=0 81=0 82=0"},
        {"Creative/Old radio (narrow, broken tube)", SatP().nb(3).xf(0, 400).xf(1, 3000).band(0, CLEAN_TUBE, 0, 100, -60).band(1, BROKEN_TUBE, 60).band(2, CLEAN_TUBE, 0, 100, -60)},
        {"Creative/Fuzz bass (lows clean, top fuzzed)", SatP().nb(2).xf(0, 180).clean(0).band(1, FUZZ, 70, 80, -4).tone(1, 0, 2, -4)},
        {"Creative/Grit for a reverb return", S1(HOT_TUBE, 60).tone(0, -6, 0, -4)},
        {"Creative/Pumping fuzz (dynamics)", S1(FUZZ, 60).dyn(0, 80)},
        {"Creative/Gated crunch (quiet parts clean)", S1(CRUNCH_AMP, 50).dyn(0, -60)},
        // ---- weird
        {"Weird/Octave up (rectify)", S1(RECTIFY, 100, 100, -6)},
        {"Weird/Foldback chaos", S1(FOLDBACK, 80).fb(0, 60, 1500)},
        {"Weird/Whistling feedback (3 kHz)", S1(HOT_TUBE, 50).fb(0, 90, 3000)},
        {"Weird/Smudged and broken", S1(SMUDGE, 80).set(P_OS, 0)},
        {"Weird/Left clean, right wrecked", S1(BREAKDOWN, 70).set(ep(0, K_DPAN), 100)},
        {"Weird/Six bands, six styles", SatP().nb(6).band(0, WARM_TAPE, 40).band(1, BRITISH, 40).band(2, FUZZ, 40, 60).band(3, SINE_FOLD, 40, 50)
                                         .band(4, RECTIFY, 50, 40).band(5, DESTROY, 50, 40)},
    };
    return v;
}

// ---------------------------------------------------------------- the plug-in
class SatPlugin : public Plugin, public EngineHost {
public:
    explicit SatPlugin(const clap_host *h) : Plugin(h, kind()), eng(*this, act, uiLink, view) {}
    UiLink uiLink;
    Display view;
    Engine eng;
    double pv(int id) const override { return P.get(id); }
    double abv(int id) const override { return abValue(id); }
    double plo(int id) const override { const ParamDef *d = T.get(id); return d ? d->min : 0; }
    double phi(int id) const override { const ParamDef *d = T.get(id); return d ? d->max : 1; }
    gui::View *createView() override;

protected:
    int latencyFromParams(double) const override { return osLatency(iround(P.get(P_OS))); }
    void dspActivate(double sr, uint32_t) override { eng.init(sr); }
    void dspReset() override { eng.reset(); }
    void dspProcess(const double *const *in, const double *const *sc, double *const *out, int n) override
    {
        eng.block(n, tr, midiIn);
        act.block(n);
        eng.afterAct();
        eng.samples(in, sc, out, n);
        act.process(out, 2, n);
        eng.post(out, n);
        act.audioKey(out, 2, n);
    }
};

static Plugin *create(const clap_host *h) { return new SatPlugin(h); }

// ---------------------------------------------------------------- the window
static Col bandCol(int b, float a = 1)
{
    static const Col c[6] = {Col(1.0f, 0.58f, 0.14f), Col(0.66f, 0.55f, 1.0f), Col(0.30f, 0.80f, 0.78f),
                             Col(0.42f, 0.85f, 0.45f), Col(0.42f, 0.62f, 1.0f), Col(0.98f, 0.47f, 0.66f)};
    return c[((b % 6) + 6) % 6].alpha(a);
}

class SatView : public EffectView {
public:
    explicit SatView(SatPlugin &p) : EffectView(p, p.uiLink, 1000, 788, "SATURATOR"), D(p)
    {
        SH = 196 + 34 + 15;
        help = {"Each band saturates its own range: drag a handle up or down for its drive, a crossover sideways; double-click to split.",
                "Dynamics: + levels a band before it saturates, - gates it. Feedback: the band's output fed back through a band pass.",
                "Delta plays only what the saturation adds. Oversampling (2x / 4x) adds a few samples of latency; Off has none.",
                "Ctrl while changing Input moves the output the other way. Shift + click Delta or S: only while the mouse is held."};
    }
    ~SatView() override { D.view.solo.store(-1); }
    void frame(Ui &ui) override;
    void opened() override { spi.reset(); spo.reset(); }

protected:
    void hashMore(std::function<void(uint64_t)> mix) override
    {
        mix((uint64_t)D.view.side.load());
        mix((uint64_t)(ih + 2) | (uint64_t)(hh + 2) << 6 | (uint64_t)(xh + 2) << 12 | (uint64_t)(rg + 2) << 18);
        mix((uint64_t)(soloL + 2) | (uint64_t)(soloM + 2) << 6 | (uint64_t)(xdrag + 2) << 12 | (uint64_t)(hdrag + 2) << 18);
    }
    bool extraFull() override { return xdrag >= 0 || hdrag >= 0; }

private:
    SatPlugin &D;
    Spectrum spi, spo;
    int nb = 3, sel = 0;
    double GXF[5] = {};
    float GRX0[NB] = {}, GRX1[NB] = {}, GHX[NB] = {}, GHY[NB] = {}, GXX[5] = {};
    int ih = -1, hh = -1, xh = -1, rg = -1;      // under the mouse: a solo / mute icon, a handle, a crossover, a band
    int xdrag = -1, hdrag = -1;
    float hdy = 0, xdx = 0;
    int soloL = -1, soloM = -1;                  // latched and momentary solo
    double hclkT = -10, dclkT = -10;
    int hclkB = -1;
    struct Mark { int id; R bar; int sc; };
    std::vector<Mark> marks;
    bool modsOn = false;

    // the bands as the window sees them: how many, and where the crossovers are (kept apart as the audio does)
    void prep()
    {
        nb = iclamp(iround(D.get(P_NB)), 1, NB);
        double pf = 20;
        for (int i = 0; i < nb - 1; ++i) {
            double f = std::max(D.get(P_XF + i), pf * 1.19);
            GXF[i] = std::min(f, 20000.0);
            pf = f;
        }
    }
    double lo(int b) const { return b <= 0 ? 20 : GXF[b - 1]; }
    double hi(int b) const { return b >= nb - 1 ? 20000 : GXF[b]; }
    int bandAt(double f) const
    {
        int b = 0;
        for (int i = 0; i < nb - 1; ++i)
            if (f >= GXF[i]) b++;
        return b;
    }
    void select(int b) { if (iround(D.get(P_SEL)) != b) D.setOnce(P_SEL, b); }
    void bcopy(int s, int d) { for (int k = 0; k < EBN; ++k) D.setOnce(ep(d, k), D.get(ep(s, k))); }
    void breset(int b) { for (int k = 0; k < EBN; ++k) D.setOnce(ep(b, k), D.T.def(ep(b, k))); }
    // split band j at f: a new band above, a copy of j
    void split(int j, double f)
    {
        int n = nb;
        if (!(n < NB && f > lo(j) * 1.2 && f < hi(j) / 1.2)) return;
        for (int i = n - 1; i > j; --i) bcopy(i, i + 1);
        bcopy(j, j + 1);
        for (int i = n - 2; i >= j; --i) D.setOnce(P_XF + i + 1, D.get(P_XF + i));
        D.setOnce(P_XF + j, f);
        D.setOnce(P_NB, n + 1);
        select(j);
        prep();
    }
    // take crossover c out: bands c and c+1 become one (keepup: with the upper one's settings)
    void merge(int c, bool keepup)
    {
        int n = nb;
        if (n <= 1) return;
        soloL = -1;
        soloM = -1;
        if (keepup) bcopy(c + 1, c);
        for (int i = c + 2; i < n; ++i) bcopy(i, i - 1);
        breset(n - 1);
        for (int i = c + 1; i < n - 1; ++i) D.setOnce(P_XF + i - 1, D.get(P_XF + i));
        D.setOnce(P_XF + n - 2, D.T.def(P_XF + n - 2));
        D.setOnce(P_NB, n - 1);
        select(std::min(c, n - 2));
        prep();
    }
    void delband(int b) { if (b == 0) merge(0, true); else merge(b - 1, false); }
    // split the widest band in its middle
    void autosplit()
    {
        int j = 0;
        double r = 0;
        for (int b = 0; b < nb; ++b) {
            double br = hi(b) / lo(b);
            if (br > r) { r = br; j = b; }
        }
        split(j, std::sqrt(lo(j) * hi(j)));
    }
    int soloAct() const { return soloM >= 0 ? soloM : soloL; }
    void markRow(Ui &ui, const R &r, const char *label, int id, int sc, float lw = 72, float bw = 84)
    {
        ui.paramRow(r, label, id, sc, lw, bw, true, sc == SC_LOG ? KD_LOG : KD_OTHER);
        marks.push_back({id, R(r.x + lw, r.y, r.w - lw - bw - 6, r.h), sc});
    }
    void styleDD(Ui &ui, const R &r, int b);
    void display(Ui &ui, const R &r);
    void meters(Ui &ui, const R &r);
    void bandsRow(Ui &ui, float y);
    void tabs(Ui &ui, float y);
    void bandTab(Ui &ui, float y);
    void curve(Ui &ui, const R &r, int b);
};

gui::View *SatPlugin::createView() { return new SatView(*this); }

// ---- the style menu: the 30 styles in their groups ----
void SatView::styleDD(Ui &ui, const R &r, int b)
{
    int st = iclamp(iround(D.get(ep(b, K_STYLE))), 0, NSTY - 1);
    if (!ui.dropdown(r, STYLE_NAMES[st], true, "Style")) return;
    static const struct { const char *name; int a, z; } G[8] = {{"Tube", 0, 3}, {"Tape", 4, 8}, {"Transformer", 9, 11}, {"Amp", 12, 16},
                                                                 {"Console", 17, 18}, {"Harmonics", 19, 20}, {"Wild", 21, 26}, {"Digital", 27, 29}};
    std::vector<MenuItem> m;
    for (auto &g : G) {
        std::vector<MenuItem> s;
        for (int i = g.a; i <= g.z; ++i) s.push_back(MenuItem::item(STYLE_NAMES[i], i + 1, i == st));
        MenuItem sm = MenuItem::submenu(g.name, s);
        sm.checked = st >= g.a && st <= g.z;
        m.push_back(sm);
    }
    SatPlugin *pp = &D;
    int id = ep(b, K_STYLE);
    ui.menu(m, r.x, r.b() + 4, [pp, id](int k) { if (k > 0) pp->setOnce(id, k - 1); }, r.w);
}

// ---- the display: spectrum behind, one region per band (its top edge = its drive), the crossovers between ----
void SatView::display(Ui &ui, const R &r)
{
    FreqScale fs(r);
    for (int b = 0; b < nb; ++b) {
        GRX0[b] = b == 0 ? r.x : (float)fs.x(GXF[b - 1]);
        GRX1[b] = b == nb - 1 ? r.r() : (float)fs.x(GXF[b]);
        GHX[b] = (GRX0[b] + GRX1[b]) * 0.5f;
        GHY[b] = r.y + r.h * (1 - (float)std::max(0.0, std::min(100.0, D.get(ep(b, K_DRIVE)))) / 100);
    }
    for (int i = 0; i < nb - 1; ++i) GXX[i] = (float)fs.x(GXF[i]);
    const int sa = soloAct();
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        freqGrid(ui, r);
        for (int i = 1; i <= 3; ++i) ui.fill(R(r.x, r.y + r.h * i / 4, r.w, 1 / ui.s), Paint(th::grid()));
        // what is under the mouse: a solo / mute icon, a drive handle, a crossover, or a band's region
        float mx = ui.mx(), my = ui.my();
        bool ind = ui.hit(r) && !ui.busy();
        ih = hh = xh = rg = -1;
        if (ind) {
            float best = 1e9f;
            for (int b = 0; b < nb; ++b) {
                if (GRX1[b] - GRX0[b] >= 22 && mx >= GRX0[b] + 3 && mx < GRX0[b] + 15) {
                    if (my >= r.y + 16 && my < r.y + 28) ih = b * 2;
                    if (my >= r.y + 30 && my < r.y + 42) ih = b * 2 + 1;
                }
                float hy = std::max(r.y + 8, std::min(r.b() - 8, GHY[b]));
                float d = std::hypot(mx - GHX[b], my - hy);
                if (d < 10 && d < best) { best = d; hh = b; }
            }
            for (int i = 0; i < nb - 1; ++i)
                if (std::fabs(mx - GXX[i]) < 5) xh = i;
            rg = bandAt(fs.f(mx));
            if (ih >= 0) { hh = -1; xh = -1; }
            if (hh >= 0) xh = -1;
        }
        if (xdrag >= 0) { xh = xdrag; hh = -1; ih = -1; }
        if (hdrag >= 0) { hh = hdrag; xh = -1; ih = -1; }
        if (xh >= 0) ui.cursor = CUR_HRESIZE;
        else if (hh >= 0 || ih >= 0) ui.cursor = CUR_HAND;
        if (ind && ui.lclick && !ui.used) {
            ui.used = true;
            if (ih >= 0) {
                int b = ih / 2;
                select(b);
                if ((ih % 2) == 0) {
                    if (ui.mods & M_SHIFT) soloM = b;
                    else soloL = soloL == b ? -1 : b;
                } else {
                    D.setOnce(ep(b, K_MUTE), D.get(ep(b, K_MUTE)) < 0.5 ? 1 : 0);
                }
            } else if (hh >= 0) {
                select(hh);
                if (ui.now - hclkT < 0.35 && hclkB == hh) {
                    D.setOnce(ep(hh, K_DRIVE), D.T.def(ep(hh, K_DRIVE)));
                    hclkT = -10;
                } else {
                    hdrag = hh;
                    hdy = my - GHY[hh];
                    hclkT = ui.now;
                    hclkB = hh;
                    D.beginEdit(ep(hh, K_DRIVE));
                }
            } else if (xh >= 0) {
                xdrag = xh;
                xdx = mx - GXX[xh];
                D.beginEdit(P_XF + xh);
            } else if (rg >= 0) {
                select(rg);
                if (ui.now - dclkT < 0.35) { split(rg, fs.f(mx)); dclkT = -10; }
                else dclkT = ui.now;
            }
        }
        if (ind && ui.rclick && !ui.used) {
            ui.used = true;
            if (xh >= 0) {
                int c = xh;
                ui.menu({MenuItem::item("Remove this crossover (merge the bands)", 1)}, mx, my, [this, c](int k) {
                    if (k == 1) merge(c, false);
                });
            } else if (rg >= 0) {
                int b = rg;
                double f = fs.f(mx);
                select(b);
                ui.menu({MenuItem::item("Solo this band", 1, soloL == b), MenuItem::item("Mute this band", 2, D.get(ep(b, K_MUTE)) >= 0.5),
                         MenuItem::item("Split here", 3, false, nb < NB), MenuItem::item("Delete band", 4, false, nb > 1)},
                        mx, my, [this, b, f](int k) {
                            if (k == 1) soloL = soloL == b ? -1 : b;
                            if (k == 2) D.setOnce(ep(b, K_MUTE), D.get(ep(b, K_MUTE)) < 0.5 ? 1 : 0);
                            if (k == 3) split(b, f);
                            if (k == 4) delband(b);
                        });
            }
        }
        if (ind && ui.wheel != 0 && !ui.used && !(ui.mods & M_SHIFT) && rg >= 0) {
            ui.used = true;
            select(rg);
            D.setOnce(ep(rg, K_DRIVE), std::max(0.0, std::min(100.0, D.get(ep(rg, K_DRIVE)) + (ui.wheel > 0 ? 1 : -1) * (ui.fine ? 0.5 : 2))));
        }
        if (xdrag >= 0) {
            int i = xdrag;
            if (ui.ldown) {
                double flo = i == 0 ? 24 : GXF[i - 1] * 1.2, fhi = i == nb - 2 ? 20000 : GXF[i + 1] / 1.2;
                D.edit(P_XF + i, std::max(flo, std::min(fhi, fs.f(mx - xdx))));
            } else {
                D.endEdit(P_XF + i);
                xdrag = -1;
            }
        }
        if (hdrag >= 0) {
            if (ui.ldown) {
                D.edit(ep(hdrag, K_DRIVE), std::floor(std::max(0.0, std::min(100.0, (double)(r.b() - (my - hdy)) / r.h * 100)) * 10 + 0.5) / 10);
            } else {
                D.endEdit(ep(hdrag, K_DRIVE));
                hdrag = -1;
            }
        }
        if (!ui.ldown) soloM = -1;
        D.view.solo.store(soloAct(), std::memory_order_relaxed);
        // the line of help (or what is under the mouse)
        char hint[220], b2[32];
        if (hh >= 0) {
            snprintf(hint, sizeof hint, "Band %d: %s    drive %.1f%%    drag up or down; double-click: back to %.0f%%", hh + 1,
                     STYLE_NAMES[iclamp(iround(D.get(ep(hh, K_STYLE))), 0, NSTY - 1)], D.get(ep(hh, K_DRIVE)), D.T.def(ep(hh, K_DRIVE)));
        } else if (xh >= 0) {
            f_hz(GXF[xh], b2, sizeof b2);
            snprintf(hint, sizeof hint, "Crossover %d: %s    drag sideways; right-click to remove", xh + 1, b2);
        } else if (ih >= 0) {
            snprintf(hint, sizeof hint, (ih % 2) == 0 ? "Solo band %d (Shift + click and hold: only while held)" : "Mute band %d", ih / 2 + 1);
        } else {
            snprintf(hint, sizeof hint, "Drag a handle up or down for the band's drive, a crossover sideways. Double-click a band to split it; right-click for more.");
        }
        ui.textFit(R(r.x + 2, r.b() + 4, r.w - 4, 14), hint, (hh >= 0 || xh >= 0 || ih >= 0) ? th::text() : th::faint(), F_REGULAR, 11);
    }
    if (!ui.drawD()) return;
    Canvas &c = ui.cv();
    double sr = D.sampleRate();
    c.pushClip(ui.X(r.x + 1), ui.Y(r.y + 1), ui.W(r.w - 2), ui.W(r.h - 2));
    // the analyzer: pre = grey fill, post = a light line
    int an = iclamp(iround(D.get(P_AN)), 0, 3);
    if (live && an > 0) {
        int ap = D.view.an.ap.load(std::memory_order_acquire);
        if (an == 1 || an == 3) {
            spi.update(D.view.an.in, AnFeed::N - 1, ap, ui.dt, sr);
            Paint p = Paint::vertical(ui.Y(r.y), Col::hex(0x34343e), ui.Y(r.b()), Col::hex(0x18181d));
            drawSpectrum(ui, fs, spi, sr, &p, nullptr);
        }
        if (an == 2 || an == 3) {
            spo.update(D.view.an.out, AnFeed::N - 1, ap, ui.dt, sr);
            Paint p = Paint::vertical(ui.Y(r.y), Col::hex(0x40260d), ui.Y(r.b()), Col::hex(0x1a1008));
            Col lc = Col::hex(0xc8c3bb).alpha(0.5f);
            drawSpectrum(ui, fs, spo, sr, an == 2 ? &p : nullptr, &lc, 1.1f);
        }
    }
    // the regions
    for (int b = 0; b < nb; ++b) {
        float x0 = GRX0[b], x1 = GRX1[b];
        bool on = D.get(ep(b, K_ON)) >= 0.5 && D.get(ep(b, K_MUTE)) < 0.5;
        if (b == sel) ui.fill(R(x0, r.y, x1 - x0, r.h), Paint(bandCol(b, 0.08f)));
        ui.fill(R(x0, GHY[b], x1 - x0, r.b() - GHY[b]), Paint::vertical(ui.Y(GHY[b]), on ? bandCol(b, 0.30f) : Col(0.35f, 0.34f, 0.33f, 0.12f),
                                                                       ui.Y(r.b()), on ? bandCol(b, 0.10f) : Col(0.35f, 0.34f, 0.33f, 0.05f)));
        ui.fill(R(x0, GHY[b] - 1, x1 - x0, 2), Paint(on ? bandCol(b, 0.95f) : Col(0.40f, 0.38f, 0.36f)));
        // the band's level in and out
        if (live && x1 - x0 > 30) {
            auto mh = [&](double v) { return r.h * (float)std::max(0.0, std::min(1.0, ((v > 1e-10 ? 20 * std::log10(v) : -200) + 60) / 60)); };
            float h1 = mh(D.view.ef[b].load()), h2 = mh(D.view.eo[b].load());
            ui.fill(R(x1 - 12, r.b() - h1, 3, h1), Paint(Col(0.40f, 0.40f, 0.45f, 0.7f)));
            ui.fill(R(x1 - 8, r.b() - h2, 3, h2), Paint(bandCol(b, 0.85f)));
        }
        // number, solo, mute, style
        if (x1 - x0 >= 22) {
            char s[8];
            snprintf(s, sizeof s, "%d", b + 1);
            ui.text(R(x0 + 3, r.y + 2, 12, 12), s, bandCol(b), F_BOLD, 10.5f, 1);
            bool so = sa == b, mu = D.get(ep(b, K_MUTE)) >= 0.5;
            R rs(x0 + 3, r.y + 16, 12, 12), rm(x0 + 3, r.y + 30, 12, 12);
            ui.fill(rs, Paint(so ? bandCol(b) : (ih == b * 2 ? th::hover() : th::raised())), 2);
            ui.stroke(rs, Paint(so ? bandCol(b) : th::border()), 2);
            ui.text(rs, "S", so ? th::orangeInk() : th::dim(), F_BOLD, 8.5f, 1);
            ui.fill(rm, Paint(mu ? th::red() : (ih == b * 2 + 1 ? th::hover() : th::raised())), 2);
            ui.stroke(rm, Paint(mu ? th::red() : th::border()), 2);
            ui.text(rm, "M", mu ? th::orangeInk() : th::dim(), F_BOLD, 8.5f, 1);
            int st = iclamp(iround(D.get(ep(b, K_STYLE))), 0, NSTY - 1);
            ui.textFit(R(x0 + 20, r.y + 2, x1 - x0 - 26, 12), STYLE_NAMES[st], th::dim(), F_REGULAR, 10);
        }
    }
    // the crossovers
    for (int i = 0; i < nb - 1; ++i) {
        bool hv = xh == i;
        Col lc = hv ? th::text() : Col(0.50f, 0.48f, 0.46f);
        ui.fill(R(GXX[i] - (hv ? 1 : 0.5f), r.y, hv ? 2 : 1, r.h), Paint(lc));
        R g(GXX[i] - 3, r.y + r.h * 0.5f - 12, 6, 24);
        ui.fill(g, Paint(hv ? th::text() : th::raised()), 2);
        ui.stroke(g, Paint(th::borderHi()), 2);
        if (hv) {
            char b2[32];
            f_hz(GXF[i], b2, sizeof b2);
            R t(GXX[i] - 38, r.y + r.h * 0.5f - 32, 76, 16);
            ui.fill(t, Paint(Col(0.06f, 0.06f, 0.07f, 0.85f)), 4);
            ui.text(t, b2, th::text(), F_SEMIBOLD, 10.5f, 1);
        }
    }
    // the drive handles
    for (int b = 0; b < nb; ++b) {
        float hy = std::max(r.y + 8, std::min(r.b() - 8, GHY[b]));
        float rad = (b == sel || b == hh) ? 8 : 6.5f;
        c.fillCircle(ui.X(GHX[b]), ui.Y(hy), ui.W(rad), Paint(bandCol(b)));
        if (b == sel) c.strokeCircle(ui.X(GHX[b]), ui.Y(hy), ui.W(8.5f), ui.W(1.2f), Paint(th::text()));
        char s[8];
        snprintf(s, sizeof s, "%d", b + 1);
        ui.text(R(GHX[b] - 8, hy - 7, 16, 14), s, Col::hex(0x0d0b08), F_BOLD, 10, 1);
    }
    // the drive scale, the frequencies
    for (int i = 1; i <= 3; ++i) {
        char s[8];
        snprintf(s, sizeof s, "%d%%", 100 - 25 * i);
        ui.text(R(r.r() - 44, r.y + r.h * i / 4 - 12, 34, 11), s, th::faint(), F_REGULAR, 9.5f, 2);
    }
    freqLabels(ui, r);
    c.popClip();
    ui.stroke(r, Paint(th::border()), 6);
}

void SatView::meters(Ui &ui, const R &r)
{
    double range = av.meterRange();
    float top = r.y + 22, h = r.h - 22 - 10;
    float x = r.x + 6, ox = r.r() - 6 - 22;
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        ui.stroke(r, Paint(th::border()), 6);
        ui.text(R(x - 2, r.y + 6, 26, 12), "IN", th::faint(), F_SEMIBOLD, 10, 1);
        ui.text(R(ox - 2, r.y + 6, 26, 12), "OUT", th::faint(), F_SEMIBOLD, 10, 1);
    }
    if (!ui.drawD()) return;
    vMeter(ui, R(x, top, 10, h), D.view.mi0.load(), live, range);
    vMeter(ui, R(x + 12, top, 10, h), D.view.mi1.load(), live, range);
    double o0 = -200, o1 = -200, h0 = -200, h1 = -200;
    if (av.myslot >= 0) {
        shm::Slot &b = shm::slot(av.myslot);
        o0 = b.pkd[0].load(); o1 = b.pkd[1].load(); h0 = b.pkh[0].load(); h1 = b.pkh[1].load();
    }
    vMeter(ui, R(ox, top, 10, h), o0, live, range, h0);
    vMeter(ui, R(ox + 12, top, 10, h), o1, live, range, h1);
}

// ---- the bands (a chip each: click selects, + adds one), the crossover slope, oversampling, Delta ----
void SatView::bandsRow(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float h = 26;
    label(ui, R(M, y, 48, h), "Bands");
    for (int b = 0; b < NB; ++b) {
        R r(M + 50 + b * 34, y, 30, h);
        bool lv = b < nb, on = lv && D.get(ep(b, K_ON)) >= 0.5 && D.get(ep(b, K_MUTE)) < 0.5, sl = lv && b == sel;
        bool hv = ui.hit(r);
        ui.fill(r, Paint(hv ? th::hover() : (sl ? th::raised2() : th::panel())), 5);
        if (on) ui.fill(R(r.x + 4, r.b() - 4, r.w - 8, 2), Paint(bandCol(b)), 1);
        ui.stroke(r, Paint(sl ? th::text().alpha(0.8f) : th::border()), 5, sl ? 1.4f : 1);
        char s[8];
        snprintf(s, sizeof s, lv ? "%d" : "+", b + 1);
        ui.text(R(r.x, r.y - 1, r.w, r.h), s, lv ? (sl ? th::text() : th::dim()) : th::faint(), F_SEMIBOLD, 12, 1);
        if (hv && ui.lclick && !ui.used && !ui.busy()) {
            ui.used = true;
            if (lv) select(b);
            else autosplit();
        }
    }
    float x = M + 50 + NB * 34 + 14;
    enumDD(ui, R(x, y, 186, h), P_SLOPE, L_SLOPE, 3, "Slope");
    enumDD(ui, R(x + 196, y, 186, h), P_OS, L_OS, 3, "Oversampling");
    toggle(ui, R(DW - M - 110, y, 110, h), "Delta", P_DELTA, BS_PURPLE, true);
}

// ---- the tabs: the selected band, the modulation ----
void SatView::tabs(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float h = 28;
    float w = (DW - 2 * M - 6) / 2;
    int tab = D.get(P_MODOPEN) >= 0.5 ? 1 : 0;
    for (int t = 0; t < 2; ++t) {
        R r(M + t * (w + 6), y, w, h);
        bool active = tab == t, hv = ui.hit(r);
        ui.fill(r, Paint(active ? th::raised2() : (hv ? th::hover() : th::panel())), 6);
        ui.stroke(r, Paint(active ? th::borderHi() : th::border()), 6);
        char lb[64];
        Col tc = active ? th::text() : th::dim();
        if (t == 0) {
            bool on = D.get(ep(sel, K_ON)) >= 0.5;
            ui.fill(R(r.x + 12, r.y + 10, 8, 8), Paint(bandCol(sel, on ? 1.f : 0.3f)), 4);
            snprintf(lb, sizeof lb, "BAND %d   %s%s", sel + 1, STYLE_NAMES[iclamp(iround(D.get(ep(sel, K_STYLE))), 0, NSTY - 1)], on ? "" : "  (bypassed)");
            ui.text(R(r.x + 28, r.y, r.w - 40, r.h), lb, tc, F_SEMIBOLD, 11.5f);
        } else {
            int n = modSlotsOn(D);
            ui.text(R(r.x + 12, r.y, r.w - 60, r.h), "MODULATION", tc, F_SEMIBOLD, 11.5f);
            if (n > 0) {
                snprintf(lb, sizeof lb, "%d", n);
                ui.fill(R(r.r() - 34, r.y + 6, 22, 16), Paint(th::purple()), 8);
                ui.text(R(r.r() - 34, r.y + 6, 22, 16), lb, th::orangeInk(), F_BOLD, 10.5f, 1);
            }
        }
        if (active) ui.fill(R(r.x + 10, r.b() - 3, r.w - 20, 2), Paint(t == 0 ? bandCol(sel) : th::purple()), 1);
        if (hv && ui.lclick && !ui.used && !ui.busy()) {
            ui.used = true;
            if (tab != t) D.setOnce(P_MODOPEN, t);
        }
    }
}

// ---- the selected band: on, mute, solo, style, mode, range; its settings; its curve ----
void SatView::bandTab(Ui &ui, float y)
{
    const int b = sel;
    const float h = 26;
    if (ui.drawS()) {
        toggle(ui, R(M, y, 54, h), "On", ep(b, K_ON));
        toggle(ui, R(M + 60, y, 62, h), "Mute", ep(b, K_MUTE));
        R rs(M + 128, y, 62, h);
        if (ui.button(rs, "Solo", soloAct() == b, true, BS_NORMAL)) {
            if (ui.mods & M_SHIFT) soloM = b;
            else soloL = soloL == b ? -1 : b;
            D.view.solo.store(soloAct(), std::memory_order_relaxed);
        }
        styleDD(ui, R(M + 200, y, 236, h), b);
        enumDD(ui, R(M + 446, y, 156, h), ep(b, K_CH), L_CH, 2, "Mode");
        // the band's range: its crossovers (the lowest starts at 20 Hz, the highest ends at 20 kHz)
        float x = M + 616;
        label(ui, R(x, y, 44, h), "Range");
        x += 46;
        if (b > 0) ui.valueBox(R(x, y, 84, h), P_XF + b - 1, true, KD_LOG, std::log(1000.0) / 138, std::log(1000.0) / 1380);
        else ui.text(R(x, y, 84, h), "20 Hz", th::faint(), F_REGULAR, 11.5f, 1);
        ui.text(R(x + 84, y, 22, h), "to", th::faint(), F_REGULAR, 11.5f, 1);
        if (b < nb - 1) ui.valueBox(R(x + 106, y, 84, h), P_XF + b, true, KD_LOG, std::log(1000.0) / 138, std::log(1000.0) / 1380);
        else ui.text(R(x + 106, y, 84, h), "20 kHz", th::faint(), F_REGULAR, 11.5f, 1);
        if (nb > 1 && ui.button(R(DW - M - 92, y, 92, h), "Delete band")) delband(b);
        // the settings in two columns, the curve on the right
        const float y0 = y + h + 10, dy = 30, cw = 318;
        const char *dl = D.get(ep(b, K_CH)) >= 0.5 ? "Drive M/S" : "Drive L/R", *ll = D.get(ep(b, K_CH)) >= 0.5 ? "Level M/S" : "Level L/R";
        markRow(ui, R(M, y0, cw, h), "Drive", ep(b, K_DRIVE), SC_LIN);
        markRow(ui, R(M, y0 + dy, cw, h), "Mix", ep(b, K_MIX), SC_LIN);
        markRow(ui, R(M, y0 + 2 * dy, cw, h), "Level", ep(b, K_LEVEL), SC_LIN);
        markRow(ui, R(M, y0 + 3 * dy, cw, h), "Dynamics", ep(b, K_DYN), SC_LIN);
        markRow(ui, R(M, y0 + 4 * dy, cw, h), "Feedback", ep(b, K_FB), SC_LIN);
        markRow(ui, R(M, y0 + 5 * dy, cw, h), "FB freq", ep(b, K_FBF), SC_LOG);
        float x2 = M + cw + 14;
        markRow(ui, R(x2, y0, cw, h), "Bass", ep(b, K_BASS), SC_LIN);
        markRow(ui, R(x2, y0 + dy, cw, h), "Mid", ep(b, K_MID), SC_LIN);
        markRow(ui, R(x2, y0 + 2 * dy, cw, h), "Treble", ep(b, K_TRE), SC_LIN);
        markRow(ui, R(x2, y0 + 3 * dy, cw, h), "Presence", ep(b, K_PRES), SC_LIN);
        markRow(ui, R(x2, y0 + 4 * dy, cw, h), dl, ep(b, K_DPAN), SC_LIN);
        markRow(ui, R(x2, y0 + 5 * dy, cw, h), ll, ep(b, K_LPAN), SC_LIN);
        ui.textFit(R(M, y0 + 6 * dy, DW - 2 * M, 16),
                   "Dynamics: + levels the band before it saturates, - gates it.   Feedback: the band's output fed back through a band pass at FB freq.   "
                   "Bass .. Presence: its tone.",
                   th::faint(), F_REGULAR, 11);
    }
    const float y0 = y + h + 10;
    float cx = M + 2 * 318 + 28;
    curve(ui, R(cx, y0, DW - M - cx, 5 * 30 + 26), b);
}

// the transfer curve of band b's style at its drive (the memoryless part)
void SatView::curve(Ui &ui, const R &r, int b)
{
    int st = iclamp(iround(D.get(ep(b, K_STYLE))), 0, NSTY - 1);
    double d = std::max(0.0, std::min(100.0, D.get(ep(b, K_DRIVE)) + D.get(P_GDRV)));
    double gdb = -6 + 0.5 * d + STYT[st][0], gn = std::pow(10.0, gdb / 20);
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f + 6, hw = r.w * 0.5f - 10, hh2 = r.h * 0.5f - 18;
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        ui.fill(R(r.x + 6, cy, r.w - 12, 1 / ui.s), Paint(th::grid()));
        ui.fill(R(cx, r.y + 18, 1 / ui.s, r.h - 22), Paint(th::grid()));
        ui.line(cx - hw, cy + hh2, cx + hw, cy - hh2, th::gridHi(), 1);
        std::vector<Pt> pts(65);
        for (int i = 0; i <= 64; ++i) {
            double u = -1 + i / 32.0;
            double v = std::max(-1.25, std::min(1.25, ds_core(st, u * gn)));
            pts[i] = {ui.X(cx + (float)u * hw), ui.Y(cy - (float)(v / 1.25) * hh2)};
        }
        ui.cv().strokePolyline(pts.data(), 65, ui.W(2), Paint(bandCol(b)));
        char s[64];
        snprintf(s, sizeof s, "%s   in %+.1f dB", STYLE_NAMES[st], gdb);
        ui.textFit(R(r.x + 8, r.y + 4, r.w - 16, 12), s, th::dim(), F_REGULAR, 10.5f);
        ui.stroke(r, Paint(th::border()), 6);
    }
    // the band's level now, on the curve's input axis
    if (ui.drawD() && live) {
        double ef = D.view.ef[b].load();
        if (ef > 0.0001) {
            float e = (float)std::min(1.0, ef);
            ui.fill(R(cx + e * hw - 1, r.y + 18, 2, r.h - 22), Paint(bandCol(b, 0.35f)));
            ui.fill(R(cx - e * hw - 1, r.y + 18, 2, r.h - 22), Paint(bandCol(b, 0.35f)));
        }
    }
}

void SatView::frame(Ui &ui)
{
    prep();
    sel = iclamp(iround(D.get(P_SEL)), 0, nb - 1);
    if (ui.drawS() && sel != iround(D.get(P_SEL))) D.setOnce(P_SEL, sel);
    modsOn = modSlotsOn(D) > 0;
    double sr = D.sampleRate() > 0 ? D.sampleRate() : 48000;
    R gear = begin(ui, osLatency(iround(D.get(P_OS))) / sr * 1000, D.view.side.load() != 0);
    if (ui.drawS()) marks.clear();
    float y = 66;
    const float dh = 196, metW = 62;
    display(ui, R(M, y, DW - 2 * M - metW - 6, dh));
    meters(ui, R(DW - M - metW, y, metW, dh));
    y += dh + 26;
    bandsRow(ui, y);
    y += 26 + 10;
    tabs(ui, y);
    y += 28 + 10;
    if (D.get(P_MODOPEN) >= 0.5) {
        static const ModSlotLabels L = {L_MBAND, 8, L_MGLOB, 4, L_MPAR, 12, L_MSRC};
        modSlots(ui, D, M, y, DW - 2 * M, 29, L, D.view.mv, live);
    } else {
        bandTab(ui, y);
    }
    y += 20 + 8 * 29 + 4;
    // the whole saturator: every band's drive and dynamics; Input (Ctrl moves the output back) and dry/wet
    if (ui.drawS()) {
        ui.fill(R(M, y, DW - 2 * M, 1), Paint(th::border()));
        float w = (DW - 2 * M - 20) / 2;
        markRow(ui, R(M, y + 8, w, 26), "All drive", P_GDRV, SC_LIN);
        markRow(ui, R(M + w + 20, y + 8, w, 26), "All dyn.", P_GDYN, SC_LIN);
        inputRow(ui, R(M, y + 42, 600, 26), P_IN, 72, 84);
        marks.push_back({P_IN, R(M + 72, y + 42, 600 - 72 - 84 - 6, 26), SC_LIN});
        markRow(ui, R(M + 620, y + 42, DW - 2 * M - 620, 26), "Dry/wet", P_MIX, SC_LIN);
    }
    y += 42 + 26 + 12;
    actCardAt(ui, y);
    // where the modulation has the bars it moves now
    if (ui.drawD() && live && modsOn) {
        for (const Mark &m : marks) {
            const ParamDef *d = D.T.get(m.id);
            if (!d) continue;
            double e = D.view.eff[m.id].load(), v = D.get(m.id);
            if (std::fabs(e - v) <= 0.0002 * (d->max - d->min)) continue;
            double f = std::max(0.0, std::min(1.0, barFrac(m.sc, e, d->min, d->max)));
            ui.fill(R(m.bar.x + (float)f * m.bar.w - 1.5f, m.bar.y - 1, 3, m.bar.h + 2), Paint(th::purple()), 1.5f);
        }
    }
    finish(ui, gear, [this](Ui &u, const R &in, float yy) {
        label(u, R(in.x, yy, 64, 24), "Analyzer");
        enumDD(u, R(in.x + 64, yy, 86, 24), P_AN, L_AN, 4);
        u.text(R(in.x + 164, yy, in.w - 164, 24), "Pre: the input, filled. Post: the output, as a line. Only while the window is open.", th::faint(),
               F_REGULAR, 11);
        return yy + 24 + 10;
    });
}

// ---------------------------------------------------------------- the kind
static const char *s_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DISTORTION, CLAP_PLUGIN_FEATURE_MULTI_EFFECTS,
                                   CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const PluginKind &kind()
{
    static const PluginKind k = {
        {CLAP_VERSION_INIT, "com.twostorms.saturator", "TwoStorms Saturator", "TwoStorms", "", "", "", TS_VERSION,
         "A multiband saturator with an Act built in: up to six bands, 30 styles (tube, tape, transformer, amp, console, harmonics, "
         "wild, digital), dynamics, feedback, tone, mid/side, eight modulation slots and Delta. Oversampling optional (Off: no latency).",
         s_features},
        &table,
        &factoryPresets,
        "Saturator",
        "TWOSTORMS SATURATOR",
        shm::KIND_POINT,
        shm::SK_SATURATOR,
        true,
        true,
        P_AB,
        &create,
        2,
        true,
    };
    return k;
}

} // namespace sat
} // namespace ts
