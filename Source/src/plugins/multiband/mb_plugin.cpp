// TwoStorms Multiband: the plug-in (parameters, presets, the window). The sound is in mb_engine.cpp.
// Port of twostorms_multiband.jsfx.
#include "mb_plugin.h"
#include "mb_engine.h"
#include "../../gui/effect_view.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

namespace ts {
namespace mb {

using namespace gui;

// ---------------------------------------------------------------- parameters
static void f_pct(double v, char *b, int n) { snprintf(b, n, "%.0f%%", v); }
static void f_dbs(double v, char *b, int n) { snprintf(b, n, "%+.1f dB", v); }
static void f_db(double v, char *b, int n) { snprintf(b, n, "%.1f dB", v); }
static void f_hz(double v, char *b, int n)
{
    if (v >= 1000) snprintf(b, n, "%.2f kHz", v / 1000);
    else snprintf(b, n, "%.0f Hz", v);
}
static void f_rat(double v, char *b, int n)
{
    if (v >= 99.5) snprintf(b, n, "inf : 1");
    else snprintf(b, n, v < 10 ? "%.2f : 1" : "%.1f : 1", v);
}
static void f_att(double v, char *b, int n) { snprintf(b, n, v < 1 ? "%.2f ms" : (v < 10 ? "%.1f ms" : "%.0f ms"), v); }
static void f_rel(double v, char *b, int n)
{
    if (v < 1000) snprintf(b, n, "%.0f ms", v);
    else snprintf(b, n, "%.2f s", v / 1000);
}
static void f_hold(double v, char *b, int n) { snprintf(b, n, "%.0f ms", v); }
static void f_rmst(double v, char *b, int n)
{
    if (v < 1) snprintf(b, n, "%.2f ms", v);
    else if (v < 10) snprintf(b, n, "%.1f ms", v);
    else if (v < 1000) snprintf(b, n, "%.0f ms", v);
    else snprintf(b, n, "%.2f s", v / 1000);
}
static void f_lams(double v, char *b, int n) { snprintf(b, n, "%.1f ms", v); }
static void f_int(double v, char *b, int n) { snprintf(b, n, "%.0f", v); }

static const char *const L_XM[] = {"Zero latency", "Linear phase"};
static const char *const L_DET[] = {"Peak", "RMS", "Hybrid"};
static const char *const L_CMODE[] = {"Mono", "Stereo", "Mid/Side", "Left/Right"};
static const char *const L_LKL[] = {"Max", "Average (power)"};
static const char *const L_SC[] = {"Main input", "Sidechain (3-4)"};
static const char *const L_TTYPE[] = {"Peak GR", "Average GR"};
static const char *const L_TTIME[] = {"1 s", "2 s", "3 s", "5 s", "10 s", "20 s", "30 s"};
static const char *const L_AN[] = {"Off", "Both", "Pre", "Post"};
static const char *const L_MODE[] = {"Compress", "Expand"};
static const char *const L_CHAR[] = {"Custom", "Clean VCA", "Opto", "FET", "Vari-mu"};
static const char *const L_BASE[] = {"Clean VCA", "Opto", "FET", "Vari-mu"};
static const char *const L_ENV[] = {"Raw (classic)", "Smooth"};
static const char *const L_RELM[] = {"Classic", "Program-dependent", "Dual-stage"};
static const char *const L_CURVE[] = {"Standard", "Soft (vari-mu)", "Opto-like"};
static const char *const L_SATT[] = {"Soft", "Warm"};
// the window's shorter names
static const char *const L_ENVW[] = {"Raw", "Smooth"};
static const char *const L_RELMW[] = {"Classic", "Program", "2-stage"};
static const char *const L_CURVEW[] = {"Standard", "Soft", "Opto-like"};
// each band's defaults: slower for the lows, faster for the highs
static const double MDA[NB] = {30, 15, 8, 4}, MDR[NB] = {200, 150, 100, 80};
static const double XFD[3] = {150, 2500, 8000};

template <size_t N> static std::vector<const char *> labels(const char *const (&a)[N]) { return std::vector<const char *>(a, a + N); }

static const ParamTable &table()
{
    static std::deque<std::string> names;
    static ParamTable t = [] {
        ParamTable t;
        auto nm = [](const std::string &s) { names.push_back(s); return names.back().c_str(); };
        addActParams(t, true);
        const char *M = "Multiband";
        const uint32_t H = PF_HOST | PF_AB;
        for (int i = 0; i < 3; ++i) {
            std::string L = "Crossover " + std::to_string(i + 1);
            t.add(P_X1 + 2 * i, nm(L), 20, 20000, XFD[i], H, f_hz, M).unit = "Hz";
            t.addEnum(P_X1 + 2 * i + 1, nm(L + " used"), {"Off", "On"}, i == 0 ? 1 : 0, H | PF_BOOL, M);
        }
        t.addEnum(P_XM, "Crossovers", labels(L_XM), 0, PF_HOST, M);
        t.add(P_IN, "Input", -24, 24, 0, H, f_dbs, M).unit = "dB";
        t.add(P_MIX, "Dry/wet", 0, 100, 100, H, f_pct, M).unit = "%";
        t.addEnum(P_DET, "Detection", labels(L_DET), 0, H, M);
        t.add(P_RMST, "Detector RMS time", 0.01, 20000, 20, H, f_rmst, M).unit = "ms";
        t.addEnum(P_CMODE, "Channel mode", labels(L_CMODE), 1, H, M);
        t.add(P_SLINK, "Stereo link", 0, 100, 100, H, f_pct, M).unit = "%";
        t.addEnum(P_LKL, "Stereo link law", labels(L_LKL), 0, H, M);
        t.addEnum(P_SC, "Detector source", labels(L_SC), 0, H, M);
        t.add(P_SCT, "Sidechain trim", -24, 24, 0, H, f_dbs, M).unit = "dB";
        t.addEnum(P_LA, "Lookahead", {"Off", "On"}, 0, PF_HOST | PF_BOOL, M);
        t.add(P_LAMS, "Lookahead time", 0.1, 10, 5, PF_HOST, f_lams, M).unit = "ms";
        t.addEnum(P_DELTA, "Delta", {"Off", "On"}, 0, PF_HOST | PF_BOOL, M);
        t.addEnum(P_TTYPE, "Set thresholds: target type", labels(L_TTYPE), 0, PF_AB, M);
        t.addEnum(P_TTIME, "Set thresholds: listening time", labels(L_TTIME), 3, 0, M);
        t.addEnum(P_AB, "A/B set", {"A", "B"}, 0, PF_HOST, M);
        t.add(P_SEL, "Selected band", 0, 3, 0, PF_INT, f_int, M);
        t.add(P_BLEND, "Hybrid: RMS share", 0, 100, 50, PF_AB, f_pct, M).unit = "%";
        t.addEnum(P_AN, "Analyzer", labels(L_AN), 1, 0, M);
        for (int b = 0; b < NB; ++b) {
            std::string L = "Band " + std::to_string(b + 1) + " ";
            const char *mod = nm("Band " + std::to_string(b + 1));
            auto n = [&](const char *s) { return nm(L + s); };
            t.addEnum(bp(b, K_ON), n("on"), {"Off", "On"}, 1, H | PF_BOOL, mod);
            t.addEnum(bp(b, K_SOLO), n("solo"), {"Off", "On"}, 0, PF_HOST | PF_BOOL, mod);
            t.addEnum(bp(b, K_MODE), n("mode"), labels(L_MODE), 0, H, mod);
            t.add(bp(b, K_THR), n("threshold"), -60, 0, -24, H, f_db, mod).unit = "dB";
            t.add(bp(b, K_RAT), n("ratio"), 1, 100, 3, H, f_rat, mod);
            t.add(bp(b, K_KNEE), n("knee"), 0, 24, 6, H, f_db, mod).unit = "dB";
            t.add(bp(b, K_ATT), n("attack"), 0.01, 500, MDA[b], H, f_att, mod).unit = "ms";
            t.add(bp(b, K_REL), n("release"), 1, 5000, MDR[b], H, f_rel, mod).unit = "ms";
            t.add(bp(b, K_RNG), n("range"), -48, 24, -3, H, f_dbs, mod).unit = "dB";
            t.add(bp(b, K_MK), n("makeup"), -24, 24, 0, H, f_dbs, mod).unit = "dB";
            t.addEnum(bp(b, K_AMK), n("auto makeup"), {"Off", "On"}, 0, H | PF_BOOL, mod);
            t.add(bp(b, K_HLD), n("hold"), 0, 500, 0, H, f_hold, mod).unit = "ms";
            t.addEnum(bp(b, K_CHAR), n("character"), labels(L_CHAR), 1, H, mod);
            t.addEnum(bp(b, K_BASE), n("custom base"), labels(L_BASE), 0, H, mod);
            t.addEnum(bp(b, K_ENV), n("detector envelope"), labels(L_ENV), 1, H, mod);
            t.addEnum(bp(b, K_RELM), n("release mode"), labels(L_RELM), 1, H, mod);
            t.addEnum(bp(b, K_CURVE), n("gain curve"), labels(L_CURVE), 0, H, mod);
            t.add(bp(b, K_FB), n("feedback"), 0, 100, 0, H, f_pct, mod).unit = "%";
            t.add(bp(b, K_SAT), n("saturation"), 0, 100, 0, H, f_pct, mod).unit = "%";
            t.addEnum(bp(b, K_SATT), n("saturation type"), labels(L_SATT), 0, H, mod);
        }
        for (int b = 0; b < NB; ++b)
            t.add(P_TGR0 + b, nm("Band " + std::to_string(b + 1) + " target GR"), 0.5, 30, 3, PF_AB, f_db, M).unit = "dB";
        return t;
    }();
    return t;
}

// factory presets: Instruments, Buses, Creative, Weird; written band by band (band 0 the lowest). A band's range:
// negative cuts (compress), positive with Expand lifts what's under the threshold (upward)
namespace {
enum { CUSTOM, VCA, OPTO, FET, VARIMU };
struct MbP {
    std::vector<std::pair<int, double>> v;
    MbP &xo(int i, double f) { v.push_back({P_X1 + 2 * i, f}); v.push_back({P_X1 + 2 * i + 1, 1}); return *this; }
    MbP &band(int b, double thr, double ratio, double att, double rel, double rng, int ch = VCA)
    {
        v.push_back({bp(b, K_THR), thr}); v.push_back({bp(b, K_RAT), ratio}); v.push_back({bp(b, K_ATT), att});
        v.push_back({bp(b, K_REL), rel}); v.push_back({bp(b, K_RNG), rng}); v.push_back({bp(b, K_CHAR), (double)ch});
        return *this;
    }
    MbP &up(int b) { v.push_back({bp(b, K_MODE), 1}); return *this; }        // expand (with a positive range: upward)
    MbP &off(int b) { v.push_back({bp(b, K_ON), 0}); return *this; }
    MbP &amk(int b) { v.push_back({bp(b, K_AMK), 1}); return *this; }
    MbP &set(int id, double x) { v.push_back({id, x}); return *this; }
    operator std::string() const { return presetStr(v); }
};
MbP M() { return MbP(); }
}

static const std::vector<FactoryPreset> &factoryPresets()
{
    static std::vector<FactoryPreset> v = {
        // ---- guitar
        {"Instruments/Guitar/Clean: gentle glue", "50=200 51=1 52=3000 53=1 54=8000 55=0 94=1.5 96=30 97=200 98=-2 114=1.5 116=30 117=200 118=-2 134=2 136=10 137=150 138=-2 70=0 170=1.5 171=1.5 172=1.5"},
        {"Instruments/Guitar/Funk: snappy mids", "50=200 51=1 52=2000 53=1 54=8000 55=0 94=2 96=20 97=120 98=-2 114=4 116=5 117=60 118=-5 134=2 136=5 137=80 138=-3 70=0 119=2 170=2 171=4 172=2"},
        {"Instruments/Guitar/Rock: de-mud and fizz control", "50=250 51=1 52=3000 53=1 54=8000 55=0 94=2 96=20 97=150 98=-3 114=1.5 116=20 117=150 118=-2 134=3 136=2 137=60 138=-4 70=0 170=2 171=1.5 172=3"},
        {"Instruments/Guitar/Rock: palm-mute tamer (120-350 Hz)", "50=120 51=1 52=350 53=1 54=8000 55=0 94=1.5 96=20 97=150 98=-2 114=4 116=5 117=80 118=-6 134=1 136=20 137=150 138=0 70=0 170=1.5 171=4"},
        {"Instruments/Guitar/Metal: tight chug", "50=150 51=1 52=2500 53=1 54=5000 55=1 94=4 96=10 97=80 98=-6 114=1.5 116=20 117=120 118=-2 134=3 136=2 137=60 138=-4 154=2 156=2 157=60 158=-3 70=0 170=4 171=1 172=3 173=2"},
        {"Instruments/Guitar/Metal: lead fizz tamer", "50=2500 51=1 52=6000 53=1 54=8000 55=0 94=1 96=20 97=150 98=0 114=3 116=1 117=50 118=-5 134=4 136=1 137=50 138=-6 70=0 171=3 172=4"},
        {"Instruments/Guitar/Acoustic: tame the plucks (fast 2-6 kHz)", M().xo(0, 2000).xo(1, 6000).off(0).band(1, -30, 4, 1, 60, -5).off(2)},
        // ---- bass
        {"Instruments/Bass/Clean: steady lows", "50=150 51=1 52=2500 53=0 54=8000 55=0 70=0 94=4 96=30 97=200 98=-6 114=2 116=15 117=150 118=-3 170=3 171=2"},
        {"Instruments/Bass/Distorted: tight lows, tamed fizz", "50=150 51=1 52=3000 53=1 54=8000 55=0 70=0 94=6 96=20 97=150 98=-6 114=2 116=10 117=120 118=-3 134=4 136=3 137=80 138=-6 170=4 171=2 172=3"},
        {"Instruments/Bass/Slap: pops under control", "50=120 51=1 52=2500 53=1 54=8000 55=0 70=0 94=3 96=20 97=150 98=-4 114=3 116=10 117=120 118=-4 134=8 136=0.5 137=60 138=-8 170=2 171=2 172=5"},
        {"Instruments/Bass/Punk", "50=150 51=1 52=2500 53=1 54=8000 55=0 70=0 94=5 96=20 97=120 98=-6 114=3 116=8 117=100 118=-4 134=4 136=3 137=80 138=-5 170=4 171=3 172=3"},
        {"Instruments/Bass/Pop punk", "50=120 51=1 52=2000 53=1 54=8000 55=0 70=0 94=6 96=25 97=150 98=-6 114=2.5 116=10 117=120 118=-3 134=3 136=5 137=100 138=-4 170=4 171=2 172=2"},
        {"Instruments/Bass/Metal", "50=100 51=1 52=800 53=1 54=4000 55=1 70=0 94=8 96=15 97=100 98=-8 114=3 116=8 117=100 118=-4 134=3 136=5 137=90 138=-4 154=6 156=1 157=60 158=-8 170=5 171=3 172=3 173=4"},
        {"Instruments/Bass/Rock", "50=150 51=1 52=2500 53=1 54=8000 55=0 70=0 94=4 96=25 97=150 98=-5 114=2 116=12 117=150 118=-3 134=3 136=5 137=100 138=-4 170=3 171=2 172=2"},
        {"Instruments/Bass/Jazz", "50=200 51=1 52=2500 53=0 54=8000 55=0 70=1 94=2 96=40 97=300 98=-3 114=1.5 116=20 117=250 118=-2 170=2 171=1"},
        {"Instruments/Bass/Pop", "50=100 51=1 52=3000 53=1 54=8000 55=0 70=0 94=6 96=20 97=120 98=-6 114=2 116=10 117=120 118=-3 134=3 136=3 137=80 138=-4 170=4 171=2 172=2"},
        // ---- vocals
        {"Instruments/Vocals/Lead: de-mud, de-harsh", "50=250 51=1 52=3500 53=1 54=8000 55=0 70=0 94=3 96=15 97=150 98=-4 114=1.5 116=10 117=150 118=-2 134=4 136=2 137=80 138=-6 170=2 171=1 172=3"},
        {"Instruments/Vocals/Lead: de-ess (5-10 kHz)", M().xo(0, 5000).xo(1, 10000).off(0).band(1, -34, 6, 0.5, 50, -8, VCA).off(2)},
        {"Instruments/Vocals/Lead: proximity boom (under 200 Hz)", M().xo(0, 200).band(0, -30, 4, 10, 120, -6).off(1)},
        {"Instruments/Vocals/Spoken: even", "50=200 51=1 52=4000 53=1 54=8000 55=0 70=0 94=3 96=15 97=150 98=-5 114=2.5 116=10 117=120 118=-4 134=4 136=2 137=80 138=-6 170=3 171=3 172=3"},
        // ---- drums
        {"Instruments/Drums/Kick: tighten the sub (under 80 Hz)", M().xo(0, 80).band(0, -24, 4, 20, 80, -6).off(1)},
        {"Instruments/Drums/Snare: tame the ring (400-1k)", M().xo(0, 400).xo(1, 1000).off(0).band(1, -30, 4, 5, 120, -6).off(2)},
        {"Instruments/Drums/Overheads: cymbal tamer (above 6k)", M().xo(0, 6000).off(0).band(1, -30, 3, 5, 120, -5)},
        {"Instruments/Drums/Room: duck the boom", M().xo(0, 250).band(0, -30, 6, 5, 150, -8, FET).off(1)},
        // ---- keys
        {"Instruments/Keys/Piano: even the low notes", M().xo(0, 300).band(0, -26, 3, 15, 150, -4, OPTO).off(1)},
        {"Instruments/Keys/Synth: control the resonance (1-4k)", M().xo(0, 1000).xo(1, 4000).off(0).band(1, -28, 4, 2, 80, -6).off(2)},
        // ---- buses
        {"Buses/Instrument bus/Drum bus", "50=120 51=1 52=5000 53=1 54=8000 55=0 70=0 94=4 96=30 97=120 98=-6 114=2 116=20 117=100 118=-3 134=3 136=5 137=80 138=-4 170=3 171=2 172=2"},
        {"Buses/Instrument bus/Bass: lows duck under the kick (kick on 3-4)", M().xo(0, 200).band(0, -30, 4, 2, 100, -6).off(1).set(P_SC, 1)},
        {"Buses/Instrument bus/Music: make room for the vocal (vocal on 3-4)", M().xo(0, 1000).xo(1, 5000).off(0).band(1, -30, 2, 15, 150, -3).off(2).set(P_SC, 1)},
        {"Buses/Mixbus/Gentle 3 band", "50=120 51=1 52=4000 53=1 54=8000 55=0 70=1 94=2 96=30 97=200 98=-3 114=1.5 116=20 117=200 118=-2 134=2 136=10 137=150 138=-3 170=1.5 171=1 172=1.5"},
        {"Buses/Mixbus/Glue: 2:1, 100 ms, 2 bands (200 Hz)", M().xo(0, 200).band(0, -24, 2, 100, 100, -3).band(1, -24, 2, 100, 100, -3)},
        {"Buses/Mixbus/Low-end lift (upward)", "50=120 51=1 52=2500 53=0 54=8000 55=0 70=0 92=1 93=-40 94=2 96=20 97=200 98=3 110=0"},
        {"Buses/Master/Glue in 3 bands (200 Hz, 2 kHz)", M().xo(0, 200).xo(1, 2000).band(0, -24, 2, 30, 200, -2).band(1, -24, 1.7, 30, 200, -2).band(2, -26, 2, 10, 120, -2).set(P_SLINK, 100)},
        {"Buses/Master/Tame the harsh top (above 6k)", M().xo(0, 6000).off(0).band(1, -28, 3, 3, 100, -3)},
        {"Buses/Master/De-mud (200-500 Hz)", "50=200 51=1 52=500 53=1 54=8000 55=0 70=0 90=0 114=4 116=10 117=150 118=-6 130=0 171=3"},
        {"Buses/Master/De-harsh (2-5 kHz)", "50=2000 51=1 52=5000 53=1 54=8000 55=0 70=0 90=0 114=4 116=2 117=80 118=-6 130=0 171=3"},
        {"Buses/Master/Low end in check (mid/side)", M().xo(0, 150).band(0, -24, 3, 20, 150, -3).off(1).set(P_CMODE, 2)},
        // ---- creative
        {"Creative/Sparkle (upward on the highs)", M().xo(0, 6000).off(0).band(1, -40, 3, 5, 150, 6).up(1)},
        {"Creative/Pumping lows (key on 3-4)", M().xo(0, 150).band(0, -36, 20, 0.5, 200, -18, FET).off(1).set(P_SC, 1)},
        {"Creative/Lift the quiet details (upward mids)", M().xo(0, 300).xo(1, 5000).off(0).band(1, -45, 3, 10, 200, 8).up(1).off(2)},
        {"Creative/Dark when loud (highs duck on peaks)", M().xo(0, 3000).off(0).band(1, -36, 10, 1, 300, -12)},
        // ---- weird
        {"Weird/Squash every band (OTT-ish)", M().xo(0, 200).xo(1, 2500).band(0, -40, 10, 1, 60, -24).band(1, -40, 10, 1, 60, -24).band(2, -40, 10, 1, 60, -24)
                                              .amk(0).amk(1).amk(2).set(P_MIX, 60)},
        {"Weird/Crushed mids only (20:1)", M().xo(0, 300).xo(1, 3000).off(0).band(1, -40, 20, 0.5, 40, -30, FET).off(2)},
        {"Weird/Breathing highs (slow, deep)", M().xo(0, 2000).off(0).band(1, -40, 8, 200, 1500, -18, VARIMU)},
        {"Weird/Gated lows (expander under 150)", M().xo(0, 150).band(0, -30, 4, 1, 80, -30).up(0).off(1)},
    };
    return v;
}

// ---------------------------------------------------------------- the plug-in
class MbPlugin : public Plugin, public EngineHost {
public:
    explicit MbPlugin(const clap_host *h) : Plugin(h, kind()), eng(*this, act, uiLink, view) {}
    UiLink uiLink;
    Display view;
    Engine eng;
    double pv(int id) const override { return P.get(id); }
    double abv(int id) const override { return abValue(id); }
    void setAudio(int id, double v) override { setFromAudio(id, v); }
    gui::View *createView() override;
    // a band's Range resets by its mode: Compress -3 dB, Expand +3 dB
    double pdef(int id) const override
    {
        if (id >= MBB && id < MBB + NB * MBK && (id - MBB) % MBK == K_RNG) return P.get(id - K_RNG + K_MODE) >= 0.5 ? 3 : -3;
        return T.def(id);
    }

protected:
    int latencyFromParams(double sr) const override { return latency(P.get(P_XM) >= 0.5, P.get(P_LA) >= 0.5, P.get(P_LAMS), sr); }
    // the Gain Stage's Set thresholds row: Peak / Average (70), the listening time (71) and the bands' targets (as
    // 56..59); it starts when a band is on
    int hkRset(int x) override { return (x == P_TTYPE || x == P_TTIME) ? x : ((x >= 56 && x <= 59) ? P_TGR0 + x - 56 : 0); }
    void hkTgStart() override { eng.tgStart(); }
    void hkPublish(shm::Slot &b) override
    {
        b.p[P_TTYPE].store(P.get(P_TTYPE), std::memory_order_relaxed);
        b.p[P_TTIME].store(P.get(P_TTIME), std::memory_order_relaxed);
        for (int i = 0; i < NB; ++i) {
            b.p[56 + i].store(P.get(P_TGR0 + i), std::memory_order_relaxed);
            b.p[60 + i].store(eng.bandOn(i) ? 1 : 0, std::memory_order_relaxed);
        }
    }
    void dspActivate(double sr, uint32_t) override { eng.init(sr); }
    void dspReset() override { eng.reset(); }
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

static Plugin *create(const clap_host *h) { return new MbPlugin(h); }

// ---------------------------------------------------------------- the window
static Col bandCol(int b, float a = 1)
{
    static const Col c[NB] = {Col(1.0f, 0.58f, 0.14f), Col(0.66f, 0.55f, 1.0f), Col(0.30f, 0.80f, 0.78f), Col(0.42f, 0.85f, 0.45f)};
    return c[((b % NB) + NB) % NB].alpha(a);
}
static const char *bandName(int b, int n)
{
    if (n == 1) return "Full band";
    if (n == 2) return b == 0 ? "Low" : "High";
    if (n == 3) return b == 0 ? "Low" : (b == 1 ? "Mid" : "High");
    return b == 0 ? "Low" : (b == 1 ? "Low mid" : (b == 2 ? "High mid" : "High"));
}

// what each Character does (shown in the row Custom uses, so nothing jumps when it changes)
static const char *const CHAR_DESC[] = {
    "",
    "Clean VCA:  standard knee \xC2\xB7 program-dependent release \xC2\xB7 feed-forward \xC2\xB7 no saturation",
    "Opto:  opto-like progressive curve \xC2\xB7 two-stage release \xC2\xB7 50% feedback \xC2\xB7 15% soft saturation",
    "FET:  standard knee \xC2\xB7 program-dependent release \xC2\xB7 60% feedback \xC2\xB7 35% soft saturation",
    "Vari-mu:  soft curve \xC2\xB7 program-dependent release \xC2\xB7 40% feedback \xC2\xB7 30% warm saturation",
};

class MbView : public EffectView {
public:
    explicit MbView(MbPlugin &p) : EffectView(p, p.uiLink, 1000, 760, "MULTIBAND"), D(p)
    {
        SH = 196 + 34 + 15;
        SW = 740;
        help = {"Drag a band's bar up or down for its threshold, a crossover sideways; double-click the display to split a band,",
                "right-click for more. The bar on each band's right edge is its detector level; red from the top is its gain change.",
                "Set thresholds listens, then sets every band's threshold so it reaches its own Target GR (Peak: 95% of the time).",
                "Ctrl while changing Input moves the output the other way. Shift + click Solo or Delta: only while the mouse is held."};
    }
    void frame(Ui &ui) override;
    void opened() override
    {
        spi.reset();
        spo.reset();
        for (int b = 0; b < NB; ++b) { grs[b] = 0; lvs[b] = -200; }
    }

protected:
    void hashMore(std::function<void(uint64_t)> mix) override
    {
        mix((uint64_t)D.view.side.load());
        mix((uint64_t)(xh + 2) | (uint64_t)(th + 2) << 6 | (uint64_t)(xdrag + 2) << 12 | (uint64_t)(tdrag + 2) << 18);
        int tgs = av.myslot >= 0 ? (int)shm::slot(av.myslot).tgs.load() : 0;
        mix((uint64_t)tgs | (uint64_t)(tgs == 2 ? std::floor(shm::slot(av.myslot).tgel.load() * 10) : 0) << 4);
    }
    bool extraFull() override { return xdrag >= 0 || tdrag >= 0; }

private:
    MbPlugin &D;
    Spectrum spi, spo;
    int nb = 2, sel = 0;
    double GXF[3] = {};
    int GXI[3] = {};
    float GRX0[NB] = {}, GRX1[NB] = {}, GTY[NB] = {}, GXX[3] = {};
    int xh = -1, th = -1, rg = -1;      // under the mouse: a crossover, a threshold bar, a band's region
    int xdrag = -1, tdrag = -1;
    float tdy = 0, xdx = 0;
    double tclkT = -10, dclkT = -10;
    int tclkB = -1;
    float grs[NB] = {}, lvs[NB] = {-200, -200, -200, -200};   // the gain change and level as shown (they fall back slowly)

    // the crossovers as the window sees them (the same sorting as the audio): GXF[0 .. nb - 2], and which box each is
    void prep()
    {
        nb = 1;
        for (int i = 0; i < 3; ++i)
            if (D.get(P_X1 + 2 * i + 1) >= 0.5) { GXF[nb - 1] = D.get(P_X1 + 2 * i); GXI[nb - 1] = i; nb++; }
        for (int r = 0; r < 2; ++r)
            for (int i = 0; i < nb - 2; ++i)
                if (GXF[i] > GXF[i + 1]) { std::swap(GXF[i], GXF[i + 1]); std::swap(GXI[i], GXI[i + 1]); }
        for (int i = 1; i < nb - 1; ++i) GXF[i] = std::max(GXF[i], GXF[i - 1] * 1.26);
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
    // a band's settings move with it when bands are split or merged (they are kept by position, lowest first)
    void bcopy(int s, int d)
    {
        for (int k = 0; k < MBK; ++k) D.setOnce(bp(d, k), D.get(bp(s, k)));
        D.setOnce(P_TGR0 + d, D.get(P_TGR0 + s));
    }
    void breset(int b)
    {
        for (int k = 0; k < MBK; ++k) D.setOnce(bp(b, k), D.T.def(bp(b, k)));
        D.setOnce(P_TGR0 + b, D.T.def(P_TGR0 + b));
    }
    // split band j at f: a new band above, a copy of j (in a crossover box not in use)
    void split(int j, double f)
    {
        if (nb >= NB || !(f > lo(j) * 1.26 && f < hi(j) / 1.26)) return;
        int u = -1;
        for (int i = 0; i < 3 && u < 0; ++i)
            if (D.get(P_X1 + 2 * i + 1) < 0.5) u = i;
        if (u < 0) return;
        for (int i = nb - 1; i > j; --i) bcopy(i, i + 1);
        bcopy(j, j + 1);
        D.setOnce(P_X1 + 2 * u, std::floor(f + 0.5));
        D.setOnce(P_X1 + 2 * u + 1, 1);
        select(j);
        prep();
    }
    // take crossover c out (in sorted order): bands c and c + 1 become one (keepup: with the upper one's settings)
    void merge(int c, bool keepup)
    {
        if (nb <= 1 || c < 0 || c > nb - 2) return;
        int n = nb;
        if (keepup) bcopy(c + 1, c);
        for (int i = c + 2; i < n; ++i) bcopy(i, i - 1);
        breset(n - 1);
        D.setOnce(P_X1 + 2 * GXI[c] + 1, 0);
        select(std::min(std::max(0, c), n - 2));
        prep();
    }
    // a crossover box's tick: on splits the band there (if it can), off merges its two bands
    void tick(int i)
    {
        bool on = D.get(P_X1 + 2 * i + 1) >= 0.5;
        if (on) {
            for (int c = 0; c < nb - 1; ++c)
                if (GXI[c] == i) { merge(c, false); return; }
            D.setOnce(P_X1 + 2 * i + 1, 0);
        } else {
            double f = D.get(P_X1 + 2 * i);
            int j = bandAt(f);
            if (f > lo(j) * 1.26 && f < hi(j) / 1.26) {
                for (int k = nb - 1; k > j; --k) bcopy(k, k + 1);
                bcopy(j, j + 1);
            }
            D.setOnce(P_X1 + 2 * i + 1, 1);
            prep();
        }
    }
    // Compress / Expand: switching moves Range to the new mode's default while it is still at a default
    void setMode(int b, int m)
    {
        double rg2 = D.get(bp(b, K_RNG));
        if (std::fabs(rg2 + 3) < 1e-9 || std::fabs(rg2 - 3) < 1e-9 || std::fabs(rg2 + 6) < 1e-9) D.setOnce(bp(b, K_RNG), m == 1 ? 3 : -3);
        D.setOnce(bp(b, K_MODE), m);
    }
    // load a Character's voicing (curve, release, feedback, saturation) into band b's editable parameters
    void charLoad(int c, int b)
    {
        D.setOnce(bp(b, K_CURVE), c == 2 ? 2 : (c == 4 ? 1 : 0));
        D.setOnce(bp(b, K_RELM), c == 2 ? 2 : 1);
        D.setOnce(bp(b, K_FB), c == 2 ? 50 : (c == 3 ? 60 : (c == 4 ? 40 : 0)));
        D.setOnce(bp(b, K_SAT), c == 2 ? 15 : (c == 3 ? 35 : (c == 4 ? 30 : 0)));
        D.setOnce(bp(b, K_SATT), c == 4 ? 1 : 0);
    }
    // threshold <-> y in the display: 0 dB at the top, -60 dB at the bottom (above the frequency labels)
    static float thrY(const R &r, double db) { return r.y + 6 + (r.h - 22) * (float)(-std::max(-60.0, std::min(0.0, db)) / 60); }
    static double yThr(const R &r, float y) { return std::max(-60.0, std::min(0.0, -(double)(y - r.y - 6) / (r.h - 22) * 60)); }
    void display(Ui &ui, const R &r);
    void meters(Ui &ui, const R &r);
    void xoRow(Ui &ui, float y);
    void trigRow(Ui &ui, float y);
    void bandRow(Ui &ui, float y);
    void bandRows(Ui &ui, float y);
    void charRow(Ui &ui, float y);
    void globalRows(Ui &ui, float y);
};

gui::View *MbPlugin::createView() { return new MbView(*this); }

// ---- the display: the spectrum behind; each band between its crossovers with its threshold bar (drag it up or down),
// its detector level (the bar on its right edge) and its gain change hanging from the top (red: cut, green: lift; 24 dB
// is the full height) ----
void MbView::display(Ui &ui, const R &r)
{
    FreqScale fs(r);
    for (int b = 0; b < nb; ++b) {
        GRX0[b] = b == 0 ? r.x : (float)fs.x(GXF[b - 1]);
        GRX1[b] = b == nb - 1 ? r.r() : (float)fs.x(GXF[b]);
        GTY[b] = thrY(r, D.get(bp(b, K_THR)));
    }
    for (int i = 0; i < nb - 1; ++i) GXX[i] = (float)fs.x(GXF[i]);
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        freqGrid(ui, r);
        for (int d = -12; d >= -48; d -= 12) ui.fill(R(r.x, thrY(r, d), r.w, 1 / ui.s), Paint(th::grid()));
        // what is under the mouse: a crossover, a threshold bar, or a band's region
        float mx = ui.mx(), my = ui.my();
        bool ind = ui.hit(r) && !ui.busy();
        xh = th = rg = -1;
        if (ind) {
            for (int i = 0; i < nb - 1; ++i)
                if (std::fabs(mx - GXX[i]) < 5) xh = i;
            rg = bandAt(fs.f(mx));
            if (xh < 0 && rg >= 0 && std::fabs(my - GTY[rg]) < 7 && mx > GRX0[rg] + 2 && mx < GRX1[rg] - 2) th = rg;
        }
        if (xdrag >= 0) { xh = xdrag; th = -1; }
        if (tdrag >= 0) { th = tdrag; xh = -1; }
        if (xh >= 0) ui.cursor = CUR_HRESIZE;
        else if (th >= 0) ui.cursor = CUR_VRESIZE;
        if (ind && ui.lclick && !ui.used) {
            ui.used = true;
            if (xh >= 0) {
                xdrag = xh;
                xdx = mx - GXX[xh];
                D.beginEdit(P_X1 + 2 * GXI[xh]);
            } else if (th >= 0) {
                select(th);
                if (ui.now - tclkT < 0.35 && tclkB == th) {
                    D.setOnce(bp(th, K_THR), D.T.def(bp(th, K_THR)));
                    tclkT = -10;
                } else {
                    tdrag = th;
                    tdy = my - GTY[th];
                    tclkT = ui.now;
                    tclkB = th;
                    D.beginEdit(bp(th, K_THR));
                }
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
                bool ex = D.get(bp(b, K_MODE)) >= 0.5;
                select(b);
                ui.menu({MenuItem::item("Band on", 1, D.get(bp(b, K_ON)) >= 0.5), MenuItem::item("Solo", 2, D.get(bp(b, K_SOLO)) >= 0.5),
                         MenuItem::sep(), MenuItem::item("Compress", 3, !ex), MenuItem::item("Expand", 4, ex), MenuItem::sep(),
                         MenuItem::item("Threshold back to -24 dB", 5), MenuItem::item("Split here", 6, false, nb < NB),
                         MenuItem::item("Merge with the band below", 7, false, b > 0), MenuItem::item("Merge with the band above", 8, false, b < nb - 1)},
                        mx, my, [this, b, f](int k) {
                            if (k == 1) D.setOnce(bp(b, K_ON), D.get(bp(b, K_ON)) >= 0.5 ? 0 : 1);
                            if (k == 2) D.setOnce(bp(b, K_SOLO), D.get(bp(b, K_SOLO)) >= 0.5 ? 0 : 1);
                            if (k == 3 || k == 4) setMode(b, k - 3);
                            if (k == 5) D.setOnce(bp(b, K_THR), D.T.def(bp(b, K_THR)));
                            if (k == 6) split(b, f);
                            if (k == 7) merge(b - 1, true);
                            if (k == 8) merge(b, false);
                        });
            }
        }
        if (ind && ui.wheel != 0 && !ui.used && rg >= 0) {
            ui.used = true;
            select(rg);
            int id = bp(rg, K_THR);
            D.setOnce(id, std::max(-60.0, std::min(0.0, D.get(id) + (ui.wheel > 0 ? 1 : -1) * (ui.fine ? 0.1 : 0.5))));
        }
        if (xdrag >= 0) {
            int i = xdrag;
            if (ui.ldown && i < nb - 1) {
                double flo = i == 0 ? 20 : GXF[i - 1] * 1.26, fhi = i == nb - 2 ? 20000 : GXF[i + 1] / 1.26;
                D.edit(P_X1 + 2 * GXI[i], std::floor(std::max(flo, std::min(fhi, fs.f(mx - xdx))) + 0.5));
            } else {
                if (i < nb - 1) D.endEdit(P_X1 + 2 * GXI[i]);
                xdrag = -1;
            }
        }
        if (tdrag >= 0) {
            int id = bp(tdrag, K_THR);
            if (ui.ldown) {
                double v = yThr(r, my - tdy);
                D.edit(id, std::floor(v * 10 + 0.5) / 10);
            } else {
                D.endEdit(id);
                tdrag = -1;
            }
        }
        // the line of help (or what is under the mouse)
        char hint[220], b2[32];
        if (th >= 0) {
            snprintf(hint, sizeof hint, "%s: threshold %.1f dB    drag up or down (Shift: fine); double-click: back to -24 dB", bandName(th, nb),
                     D.get(bp(th, K_THR)));
        } else if (xh >= 0) {
            f_hz(GXF[xh], b2, sizeof b2);
            snprintf(hint, sizeof hint, "Crossover at %s    drag sideways; right-click to remove", b2);
        } else {
            snprintf(hint, sizeof hint, "Drag a band's bar up or down for its threshold, a crossover sideways. Double-click to split a band; right-click for more.");
        }
        ui.textFit(R(r.x + 2, r.b() + 4, r.w - 4, 14), hint, (th >= 0 || xh >= 0) ? th::text() : th::faint(), F_REGULAR, 11);
    }
    if (!ui.drawD()) return;
    Canvas &c = ui.cv();
    double sr = D.sampleRate() > 0 ? D.sampleRate() : 48000;
    c.pushClip(ui.X(r.x + 1), ui.Y(r.y + 1), ui.W(r.w - 2), ui.W(r.h - 2));
    // the analyzer: the input filled, the output as a line
    int an = iclamp(iround(D.get(P_AN)), 0, 3);
    if (live && an > 0) {
        int ap = D.view.an.ap.load(std::memory_order_acquire);
        if (an == 1 || an == 2) {
            spi.update(D.view.an.in, AnFeed::N - 1, ap, ui.dt, sr);
            Paint p = Paint::vertical(ui.Y(r.y), Col::hex(0x2c2c34), ui.Y(r.b()), Col::hex(0x141418));
            drawSpectrum(ui, fs, spi, sr, &p, nullptr);
        }
        if (an == 1 || an == 3) {
            spo.update(D.view.an.out, AnFeed::N - 1, ap, ui.dt, sr);
            Paint p = Paint::vertical(ui.Y(r.y), Col::hex(0x40260d), ui.Y(r.b()), Col::hex(0x1a1008));
            Col lc = Col::hex(0xc8c3bb).alpha(0.45f);
            drawSpectrum(ui, fs, spo, sr, an == 3 ? &p : nullptr, &lc, 1.1f);
        }
    }
    float k = (float)std::min(1.0, ui.dt * 6);
    for (int b = 0; b < nb; ++b) {
        float x0 = GRX0[b], x1 = GRX1[b], w = x1 - x0;
        bool on = D.get(bp(b, K_ON)) >= 0.5, ex = D.get(bp(b, K_MODE)) >= 0.5;
        Col bc = on ? bandCol(b) : Col(0.42f, 0.40f, 0.38f);
        // the shown gain change falls back slowly (about 20 dB a second), so short moves stay readable; the level too
        float gr = live && on ? D.view.bgr[b].load() : 0;
        grs[b] = std::fabs(gr) >= std::fabs(grs[b]) ? gr : grs[b] + (gr - grs[b]) * k;
        float lv = live && on ? D.view.blv[b].load() : -200;
        lvs[b] = lv >= lvs[b] ? lv : std::max(lv, lvs[b] - (float)ui.dt * 30);
        if (b == sel) ui.fill(R(x0, r.y, w, r.h), Paint(bandCol(b, 0.07f)));
        // the gain change, hanging from the top
        float gh = std::min(1.0f, std::fabs(grs[b]) / 24) * r.h;
        if (gh > 0.5f) ui.fill(R(x0 + 3, r.y, w - 6, gh), Paint::vertical(ui.Y(r.y), (grs[b] < 0 ? th::red() : th::green()).alpha(0.38f),
                                                                         ui.Y(r.y + gh), (grs[b] < 0 ? th::red() : th::green()).alpha(0.18f)));
        ui.fill(R(x0, r.y, w, 2), Paint(bc));
        // the detector level, on the band's right edge (brighter past the threshold)
        double thr = D.get(bp(b, K_THR));
        if (w > 26 && lvs[b] > -60) {
            float ly = thrY(r, lvs[b]), bot = thrY(r, -60), tyy = GTY[b];
            R lb(x1 - 11, ly, 5, bot - ly);
            ui.fill(lb, Paint(bc.alpha(0.40f)), 1);
            bool past = ex ? lvs[b] < thr : lvs[b] > thr;
            if (past) {
                if (!ex && ly < tyy) ui.fill(R(x1 - 11, ly, 5, tyy - ly), Paint(bc), 1);
                if (ex) ui.fill(R(x1 - 11, ly, 5, std::max(0.0f, bot - ly)), Paint(bc.alpha(0.75f)), 1);
            }
        }
        // the threshold bar and its grip
        bool hv = th == b;
        float ty = GTY[b], cx = x0 + w * 0.5f;
        ui.fill(R(x0 + 4, ty - (hv ? 1 : 0.75f), w - 8, hv ? 2 : 1.5f), Paint(bc.alpha(hv ? 1 : 0.9f)));
        if (w > 40) {
            R g(cx - 15, ty - 5, 30, 10);
            ui.fill(g, Paint(hv ? th::text() : bc), 5);
            for (int i = -1; i <= 1; ++i) ui.fill(R(cx + i * 5 - 0.5f, ty - 2.5f, 1, 5), Paint(Col::hex(0x0d0b08).alpha(0.7f)));
            char s[24];
            snprintf(s, sizeof s, "%.1f dB", thr);
            if (w > 120) ui.text(R(cx + 20, ty - 15, 60, 12), s, hv ? th::text() : bc.alpha(0.9f), F_SEMIBOLD, 10);
        }
        // the band's name and its gain change now
        if (w > 30) {
            char s[40];
            snprintf(s, sizeof s, "%s%s", bandName(b, nb), !on ? " (off)" : (ex ? "  \xC2\xB7  expand" : ""));
            ui.textFit(R(x0 + 7, r.y + 6, w - 12, 12), s, bc, F_SEMIBOLD, 10.5f);
            snprintf(s, sizeof s, std::fabs(grs[b]) < 0.05f ? "0.0 dB" : "%+.1f dB", grs[b]);
            ui.textFit(R(x0 + 7, r.y + 20, w - 12, 12), s, on && std::fabs(grs[b]) >= 0.05f ? th::text() : th::faint(), F_REGULAR, 10);
        }
    }
    // the crossovers
    for (int i = 0; i < nb - 1; ++i) {
        bool hv = xh == i;
        Col lc = hv ? th::text() : Col(0.50f, 0.48f, 0.46f);
        ui.fill(R(GXX[i] - (hv ? 1 : 0.5f), r.y, hv ? 2 : 1, r.h), Paint(lc));
        if (hv) {
            char b2[32];
            f_hz(GXF[i], b2, sizeof b2);
            R t(GXX[i] - 38, r.y + r.h * 0.5f - 8, 76, 16);
            ui.fill(t, Paint(Col(0.06f, 0.06f, 0.07f, 0.88f)), 4);
            ui.text(t, b2, th::text(), F_SEMIBOLD, 10.5f, 1);
        }
    }
    // the threshold scale, the frequencies
    for (int d = -12; d >= -48; d -= 12) {
        char s[8];
        snprintf(s, sizeof s, "%d", d);
        ui.text(R(r.r() - 46, thrY(r, d) - 12, 30, 11), s, th::faint(), F_REGULAR, 9.5f, 2);
    }
    freqLabels(ui, r);
    c.popClip();
    ui.stroke(r, Paint(th::border()), 6);
}

void MbView::meters(Ui &ui, const R &r)
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

// ---- crossovers: a tick box in front of each (ticked: it splits there); the selected band's Solo, then Delta ----
void MbView::xoRow(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float h = 26;
    label(ui, R(M, y, 76, h), "Crossovers");
    for (int i = 0; i < 3; ++i) {
        float x = M + 80 + i * 132;
        bool on = D.get(P_X1 + 2 * i + 1) >= 0.5;
        if (ui.checkbox(R(x, y + 5, 16, 16), on)) tick(i);
        ui.valueBox(R(x + 22, y, 96, h), P_X1 + 2 * i, on, KD_LOG, 0.004, 0.0004);
    }
    toggle(ui, R(DW - M - 196, y, 96, h), "Solo", bp(sel, K_SOLO), BS_PURPLE, true);
    toggle(ui, R(DW - M - 96, y, 96, h), "Delta", P_DELTA, BS_PURPLE, true);
}

// ---- what drives every band's detector: the source, peak / RMS / hybrid, and the channels (with the stereo link) ----
void MbView::trigRow(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float h = 26;
    label(ui, R(M, y, 76, h), "Trigger");
    enumDD(ui, R(M + 80, y, 160, h), P_SC, L_SC, 2);
    enumDD(ui, R(M + 248, y, 140, h), P_DET, L_DET, 3, "Detect");
    if (D.get(P_DET) >= 0.5) ui.valueBox(R(M + 394, y, 104, h), P_RMST, true, KD_LOG, std::log(2000000.0) / 140, std::log(2000000.0) / 1400, 2, nullptr, "RMS");
    enumDD(ui, R(M + 512, y, 170, h), P_CMODE, L_CMODE, 4, "Mode");
    if (D.get(P_CMODE) >= 0.5) ui.valueBox(R(M + 688, y, 110, h), P_SLINK, true, KD_OTHER, 1, 0.1, 2, nullptr, "Link");
}

// ---- the bands: a chip per band (select it), then the selected band's On, Compress / Expand, Character, Auto makeup ----
void MbView::bandRow(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float h = 26;
    for (int b = 0; b < nb; ++b) {
        R r(M + b * 94, y, 90, h);
        bool on = D.get(bp(b, K_ON)) >= 0.5, sl = b == sel, hv = ui.hit(r);
        ui.fill(r, Paint(hv ? th::hover() : (sl ? th::raised2() : th::panel())), 5);
        ui.fill(R(r.x + 6, r.b() - 4, r.w - 12, 2), Paint(on ? bandCol(b) : th::ghost()), 1);
        ui.stroke(r, Paint(sl ? th::text().alpha(0.8f) : th::border()), 5, sl ? 1.4f : 1);
        ui.text(R(r.x, r.y - 1, r.w, r.h), bandName(b, nb), on ? (sl ? th::text() : th::dim()) : th::faint(), F_SEMIBOLD, 11.5f, 1);
        if (hv && ui.lclick && !ui.used && !ui.busy()) {
            ui.used = true;
            select(b);
        }
    }
    float x = M + NB * 94 + 6;
    const int b = sel;
    toggle(ui, R(x, y, 50, h), "On", bp(b, K_ON));
    {
        R r(x + 56, y, 120, h);
        int cv = D.get(bp(b, K_MODE)) >= 0.5 ? 1 : 0;
        if (ui.dropdown(r, L_MODE[cv])) {
            std::vector<MenuItem> m = {MenuItem::item(L_MODE[0], 1, cv == 0), MenuItem::item(L_MODE[1], 2, cv == 1)};
            ui.menu(m, r.x, r.b() + 4, [this, b, cv](int k) { if (k > 0 && k - 1 != cv) setMode(b, k - 1); }, r.w);
        }
    }
    {
        // the four voicings, then Custom last; choosing Custom keeps the voicing you were on as its base
        R r(x + 182, y, 182, h);
        int cv = iclamp(iround(D.get(bp(b, K_CHAR))), 0, 4);
        if (ui.dropdown(r, L_CHAR[cv], true, "Character")) {
            std::vector<MenuItem> m;
            for (int i = 1; i <= 5; ++i) {
                int v = i % 5;
                if (v == 0) m.push_back(MenuItem::sep());
                m.push_back(MenuItem::item(L_CHAR[v], v + 1, v == cv));
            }
            ui.menu(m, r.x, r.b() + 4, [this, b, cv](int k) {
                int nv = k - 1;
                if (nv < 0) return;
                if (nv == 0 && cv > 0) { D.setOnce(bp(b, K_BASE), cv - 1); charLoad(cv, b); }
                D.setOnce(bp(b, K_CHAR), nv);
            }, r.w);
        }
    }
    toggle(ui, R(x + 370, y, 110, h), "Auto makeup", bp(b, K_AMK));
    // what the band covers
    char s[64], f1[24], f2[24];
    if (nb == 1) snprintf(s, sizeof s, "the whole signal");
    else {
        f_hz(lo(b), f1, sizeof f1);
        f_hz(hi(b), f2, sizeof f2);
        if (b == 0) snprintf(s, sizeof s, "below %s", f2);
        else if (b == nb - 1) snprintf(s, sizeof s, "above %s", f1);
        else snprintf(s, sizeof s, "%s to %s", f1, f2);
    }
    ui.textFit(R(x + 488, y, DW - M - (x + 488), h), s, th::faint(), F_REGULAR, 11, 2);
}

// ---- the selected band: threshold, ratio, knee | attack, hold, release | range, makeup, its gain change now ----
void MbView::bandRows(Ui &ui, float y)
{
    const float h = 26, dy = 32, cw = 314;
    const int b = sel;
    const float x2 = M + cw + 15, x3 = M + 2 * (cw + 15);
    const bool on = D.get(bp(b, K_ON)) >= 0.5, amk = D.get(bp(b, K_AMK)) >= 0.5;
    const R mkBar(x3 + 72, y + dy, cw - 72 - 84 - 6, h), mkBox(x3 + cw - 84, y + dy, 84, h), nowR(x3 + 72, y + 2 * dy + 6, cw - 72, h - 12);
    if (ui.drawS()) {
        ui.paramRow(R(M, y, cw, h), "Threshold", bp(b, K_THR), SC_LIN, 72, 84, on);
        ui.paramRow(R(M, y + dy, cw, h), "Ratio", bp(b, K_RAT), SC_LOG, 72, 84, on, KD_LOG);
        ui.paramRow(R(M, y + 2 * dy, cw, h), "Knee", bp(b, K_KNEE), SC_LIN, 72, 84, on);
        ui.paramRow(R(x2, y, cw, h), "Attack", bp(b, K_ATT), SC_LOG, 72, 84, on, KD_LOG);
        ui.paramRow(R(x2, y + dy, cw, h), "Hold", bp(b, K_HLD), SC_LIN, 72, 84, on);
        ui.paramRow(R(x2, y + 2 * dy, cw, h), "Release", bp(b, K_REL), SC_LOG, 72, 84, on, KD_LOG);
        ui.paramRow(R(x3, y, cw, h), "Range", bp(b, K_RNG), SC_LIN, 72, 84, on);
        if (amk) {
            // auto: the bar and the box show the makeup it uses now (drawn live)
            label(ui, R(x3, y + dy, 72, h), "Makeup", on);
            ui.fill(mkBar, Paint(th::well()), 5);
            ui.stroke(mkBar, Paint(th::border()), 5);
            ui.fill(mkBox, Paint(th::well()), 5);
            ui.stroke(mkBox, Paint(th::border()), 5);
        } else {
            ui.paramRow(R(x3, y + dy, cw, h), "Makeup", bp(b, K_MK), SC_LIN, 72, 84, on);
        }
        label(ui, R(x3, y + 2 * dy, 72, h), "Now", on);
        ui.fill(nowR, Paint(th::well()), 3);
        ui.stroke(nowR, Paint(th::border()), 3);
        ui.fill(R(nowR.x + nowR.w * 0.5f, nowR.y, 1 / ui.s, nowR.h), Paint(th::borderHi()));
    }
    if (!ui.drawD()) return;
    if (amk) {
        double mk = live ? D.view.bmka[b].load() : 0;
        float f = (float)((std::max(-24.0, std::min(24.0, mk)) + 24) / 48);
        ui.fill(R(mkBar.x + f * mkBar.w - 1.5f, mkBar.y + 3, 3, mkBar.h - 6), Paint(th::dim()), 1.5f);
        char s[24];
        snprintf(s, sizeof s, "auto %+.1f dB", mk);
        ui.text(R(mkBox.x + 4, mkBox.y, mkBox.w - 10, mkBox.h), s, th::faint(), F_REGULAR, 11.5f, 2);
    }
    // the band's gain change now (centre: none; left: cut, right: lift; full width 24 dB)
    float g = std::max(-24.0f, std::min(24.0f, grs[b])), cx = nowR.x + nowR.w * 0.5f, hw = nowR.w * 0.5f - 1;
    if (std::fabs(g) > 0.01f) {
        float wpx = std::fabs(g) / 24 * hw;
        ui.fill(R(g < 0 ? cx - wpx : cx, nowR.y + 2, wpx, nowR.h - 4), Paint(g < 0 ? th::red() : th::green()), 1);
    }
}

// ---- Custom character: a base template, then envelope, release, curve, feedback and saturation by hand; otherwise what
// the Character does ----
void MbView::charRow(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float h = 26;
    const int b = sel;
    int cv = iclamp(iround(D.get(bp(b, K_CHAR))), 0, 4);
    if (cv != 0) {
        ui.fill(R(M, y, DW - 2 * M, h), Paint(Col::hex(0x0f0f11)), 5);
        ui.stroke(R(M, y, DW - 2 * M, h), Paint(th::border().alpha(0.6f)), 5);
        ui.text(R(M + 12, y, DW - 2 * M - 24, h), CHAR_DESC[cv], th::faint(), F_REGULAR, 11.5f, 0);
        ui.text(R(M + 12, y, DW - 2 * M - 24, h), "choose Custom to shape it", th::ghost().lighter(0.25f), F_REGULAR, 11, 2);
        return;
    }
    int bs = iclamp(iround(D.get(bp(b, K_BASE))), 0, 3);
    R br(M, y, 136, h);
    if (ui.dropdown(br, L_BASE[bs], true, "Base")) {
        std::vector<MenuItem> m;
        for (int i = 0; i < 4; ++i) m.push_back(MenuItem::item(L_BASE[i], i + 1, i == bs));
        ui.menu(m, br.x, br.b() + 4, [this, b](int r) { if (r > 0) { D.setOnce(bp(b, K_BASE), r - 1); charLoad(r, b); } }, br.w);
    }
    enumDD(ui, R(M + 142, y, 160, h), bp(b, K_ENV), L_ENVW, 2, "Envelope");
    enumDD(ui, R(M + 308, y, 170, h), bp(b, K_RELM), L_RELMW, 3, "Release");
    enumDD(ui, R(M + 484, y, 170, h), bp(b, K_CURVE), L_CURVEW, 3, "Curve");
    // feedback can't look ahead: hidden while lookahead is on
    if (D.get(P_LA) < 0.5) ui.valueBox(R(M + 660, y, 114, h), bp(b, K_FB), true, KD_OTHER, 1, 0.1, 2, nullptr, "Feedback");
    ui.valueBox(R(M + 780, y, 108, h), bp(b, K_SAT), true, KD_OTHER, 1, 0.1, 2, nullptr, "Saturation");
    enumDD(ui, R(M + 894, y, DW - M - (M + 894), h), bp(b, K_SATT), L_SATT, 2);
}

// ---- every band: lookahead and the crossover kind, Set thresholds at the right edge (with the selected band's Target
// GR); then input and dry/wet ----
void MbView::globalRows(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float h = 26;
    ui.fill(R(M, y - 8, DW - 2 * M, 1), Paint(th::border()));
    toggle(ui, R(M, y, 96, h), "Lookahead", P_LA);
    if (D.get(P_LA) >= 0.5) ui.valueBox(R(M + 100, y, 70, h), P_LAMS, true, KD_OTHER, 0.1, 0.01);
    enumDD(ui, R(M + 176, y, 150, h), P_XM, L_XM, 2);
    int tgs = av.myslot >= 0 ? (int)shm::slot(av.myslot).tgs.load() : 0;
    R set(DW - M - 132, y, 132, h);
    if (ui.button(set, tgs > 0 ? "Cancel" : "Set thresholds", tgs > 0, av.myslot >= 0))
        D.act.send(tgs > 0 ? shm::C_TGSTOP : shm::C_TGSTART, 0);
    R tim(set.x - 6 - 66, y, 66, h);
    if (tgs > 0) {
        char b[32];
        if (tgs == 1) snprintf(b, sizeof b, "waiting");
        else if (tgs == 2) snprintf(b, sizeof b, "%.1f s", shm::slot(av.myslot).tgel.load());
        else snprintf(b, sizeof b, "working");
        ui.button(tim, b, true, true, BS_PURPLE);
    } else {
        enumDD(ui, tim, P_TTIME, L_TTIME, 7);
    }
    R typ(tim.x - 6 - 110, y, 110, h);
    enumDD(ui, typ, P_TTYPE, L_TTYPE, 2);
    R tb(typ.x - 6 - 140, y, 140, h);
    ui.valueBox(tb, P_TGR0 + sel, D.get(bp(sel, K_ON)) >= 0.5, KD_OTHER, 0.1, 0.01, 2, nullptr, "Target GR");
    y += h + 8;
    inputRow(ui, R(M, y, 470, h), P_IN, 72, 84);
    ui.paramRow(R(M + 490, y, 300, h), "Dry/wet", P_MIX, SC_LIN, 62, 84);
}

void MbView::frame(Ui &ui)
{
    prep();
    sel = iclamp(iround(D.get(P_SEL)), 0, nb - 1);
    if (ui.drawS() && sel != iround(D.get(P_SEL))) D.setOnce(P_SEL, sel);
    double sr = D.sampleRate() > 0 ? D.sampleRate() : 48000;
    int lat = latency(D.get(P_XM) >= 0.5, D.get(P_LA) >= 0.5, D.get(P_LAMS), sr);
    R gear = begin(ui, lat / sr * 1000, D.view.side.load() != 0, lat > 0 ? (D.get(P_XM) >= 0.5 ? "linear phase" : "lookahead") : "");
    float y = 66;
    const float dh = 250, metW = 62;
    display(ui, R(M, y, DW - 2 * M - metW - 6, dh));
    meters(ui, R(DW - M - metW, y, metW, dh));
    y += dh + 24;
    xoRow(ui, y);
    y += 26 + 8;
    trigRow(ui, y);
    y += 26 + 10;
    if (ui.drawS()) ui.fill(R(M, y, DW - 2 * M, 1), Paint(th::border()));
    y += 10;
    bandRow(ui, y);
    y += 26 + 10;
    bandRows(ui, y);
    y += 2 * 32 + 26 + 10;
    charRow(ui, y);
    y += 26 + 18;
    globalRows(ui, y);
    y += 26 + 8 + 26 + 12;
    actCardAt(ui, y);
    finish(ui, gear, [this](Ui &u, const R &in, float yy) {
        float x = in.x;
        label(u, R(x, yy, 60, 24), "Analyzer");
        enumDD(u, R(x + 60, yy, 80, 24), P_AN, L_AN, 4);
        x += 60 + 80 + 16;
        label(u, R(x, yy, 60, 24), "Link law", D.get(P_CMODE) >= 0.5);
        enumDD(u, R(x + 60, yy, 140, 24), P_LKL, L_LKL, 2, nullptr, D.get(P_CMODE) >= 0.5);
        x += 60 + 140 + 16;
        if (D.get(P_DET) >= 1.5) {
            label(u, R(x, yy, 70, 24), "RMS share");
            u.valueBox(R(x + 70, yy, 64, 24), P_BLEND, true, KD_OTHER, 1, 0.1);
            x += 70 + 64 + 16;
        }
        if (D.get(P_SC) >= 0.5) {
            label(u, R(x, yy, 96, 24), "Sidechain trim");
            u.valueBox(R(x + 96, yy, 76, 24), P_SCT, true, KD_OTHER, 0.1, 0.01);
        }
        return yy + 24 + 10;
    });
}

// ---------------------------------------------------------------- the kind
static const char *s_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_COMPRESSOR, CLAP_PLUGIN_FEATURE_EXPANDER,
                                   CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const PluginKind &kind()
{
    static const PluginKind k = {
        {CLAP_VERSION_INIT, "com.twostorms.multiband", "TwoStorms Multiband", "TwoStorms", "", "", "", TS_VERSION,
         "A multiband compressor / expander with an Act built in: one to four bands with Linkwitz-Riley (zero latency) or "
         "linear-phase crossovers; per band compress or expand, range, makeup (by hand or auto) and a Character; sidechain, "
         "lookahead (optional), Delta and Set thresholds.",
         s_features},
        &table,
        &factoryPresets,
        "Multiband",
        "TWOSTORMS MULTIBAND",
        shm::KIND_POINT,
        shm::SK_MULTIBAND,
        true,
        true,
        P_AB,
        &create,
        2,
        false,
    };
    return k;
}

} // namespace mb
} // namespace ts
