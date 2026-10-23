// TwoStorms EQ: the plug-in (parameters, presets, the window). The sound is in eq_engine.cpp.
// Port of twostorms_eq.jsfx.
#include "eq_plugin.h"
#include "eq_engine.h"
#include "../../core/filters.h"
#include "../../gui/effect_view.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

namespace ts {
namespace peq {

using namespace gui;
using flt::cutHp;
using flt::cutS;
using flt::cutTy;

// ---------------------------------------------------------------- parameters
static void f_pct(double v, char *b, int n) { snprintf(b, n, "%.0f%%", v); }
static void f_dbs(double v, char *b, int n) { snprintf(b, n, "%+.1f dB", v); }
static void f_db(double v, char *b, int n) { snprintf(b, n, "%.1f dB", v); }
static void f_hz(double v, char *b, int n)
{
    if (v >= 10000) snprintf(b, n, "%.1f kHz", v / 1000);
    else if (v >= 1000) snprintf(b, n, "%.2f kHz", v / 1000);
    else snprintf(b, n, "%.0f Hz", v);
}
static void f_q(double v, char *b, int n) { snprintf(b, n, v < 10 ? "%.2f" : "%.1f", v); }
static void f_att(double v, char *b, int n) { snprintf(b, n, v < 1 ? "%.2f ms" : (v < 10 ? "%.1f ms" : "%.0f ms"), v); }
static void f_rel(double v, char *b, int n)
{
    if (v < 1000) snprintf(b, n, "%.0f ms", v);
    else snprintf(b, n, "%.2f s", v / 1000);
}
static void f_lams(double v, char *b, int n) { snprintf(b, n, "%.1f ms", v); }
static void f_dtp(double v, char *b, int n) { snprintf(b, n, "%.1f%% of the time", v); }
static void f_int(double v, char *b, int n) { snprintf(b, n, "%.0f", v); }
static void f_rat(double v, char *b, int n)
{
    if (v >= 99.5) snprintf(b, n, "inf : 1");
    else snprintf(b, n, v < 10 ? "%.2f : 1" : "%.1f : 1", v);
}
static void f_hold(double v, char *b, int n) { snprintf(b, n, "%.0f ms", v); }
static const char *const L_DM[] = {"Off", "Compress", "Expand"};
static const char *const L_DET[] = {"Peak", "RMS"};
static const char *const L_TRIG[] = {"Main input", "Sidechain (3-4)"};
static const char *const L_QM[] = {"Q", "Light Q", "Medium Q", "Strong Q"};
// the packed settings, as the host shows them
static void f_dyn(double v, char *b, int n)
{
    char r[24];
    f_rat(pkRt(v), r, sizeof r);
    snprintf(b, n, "%s, %s, %s, hold %.0f ms", L_DM[pkDm(v)], L_DET[pkDet(v)], r, pkHold(v));
}
static void f_dsrc(double v, char *b, int n) { snprintf(b, n, "%s, %s, knee %.1f dB", L_TRIG[pkSrc(v)], L_QM[pkQm(v)], pkKn(v)); }
// f_dyn's text back to the packed value: "Compress, RMS, 2.00 : 1, hold 10 ms"
static bool p_dyn(const char *s, double *out)
{
    std::string t = s, part[4];
    int k = 0;
    size_t q = 0;
    while (k < 4) {
        size_t e = t.find(", ", q);
        part[k++] = t.substr(q, e == std::string::npos ? std::string::npos : e - q);
        if (e == std::string::npos) break;
        q = e + 2;
    }
    if (k != 4) return false;
    int dm = -1, det = -1;
    for (int i = 0; i < 3; ++i) if (part[0] == L_DM[i]) dm = i;
    for (int i = 0; i < 2; ++i) if (part[1] == L_DET[i]) det = i;
    if (dm < 0 || det < 0) return false;
    int rn = 10000;
    if (part[2].compare(0, 3, "inf") != 0) {
        double r = atof(part[2].c_str());
        if (!(r >= 1)) return false;
        rn = std::max(0, std::min(10000, (int)std::lround(10000 * std::log(r) / std::log(100.0))));
    }
    if (part[3].compare(0, 5, "hold ") != 0) return false;
    double h = std::max(0.0, std::min(500.0, atof(part[3].c_str() + 5)));
    *out = dm + 3.0 * det + 6.0 * rn + 60006.0 * std::floor(h / 0.5 + 0.5);
    return true;
}

static const char *const L_TYPE[] = {"Bell", "Low shelf", "High shelf", "Notch", "Band pass", "High pass 6", "High pass 12", "High pass 24",
                                     "High pass 48", "Low pass 6", "Low pass 12", "Low pass 24", "Low pass 48", "High pass 18",
                                     "High pass 96", "Low pass 18", "Low pass 96"};
static const char *const L_UTYPE[] = {"Bell", "Low shelf", "High shelf", "Notch", "Band pass", "High pass", "Low pass"};
static const char *const L_SLOPE[] = {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "48 dB/oct", "96 dB/oct"};
static const char *const L_PLACE[] = {"Stereo", "Left", "Right", "Mid", "Side"};
static const char *const L_AN[] = {"Off", "Both", "Pre", "Post"};
static const char *const L_DRNG[] = {"+/-6 dB", "+/-12 dB", "+/-18 dB", "+/-30 dB"};
static const char *const L_LIN[] = {"Zero latency", "Linear: Low", "Linear: Medium", "Linear: High", "Linear: Max",
                                    "Natural: Low", "Natural: Medium", "Natural: High"};
static const char *const L_AGM[] = {"Curve", "Program"};
static const char *const L_TTIME[] = {"1 s", "2 s", "3 s", "5 s", "10 s", "20 s", "30 s"};
static const double EFD[NB] = {30, 1000, 250, 4000, 500, 2000, 120, 8000, 350, 1400, 700, 2800, 180, 5600, 80, 12000};

template <size_t N> static std::vector<const char *> labels(const char *const (&a)[N]) { return std::vector<const char *>(a, a + N); }

static const ParamTable &table()
{
    static std::deque<std::string> names;
    static ParamTable t = [] {
        ParamTable t;
        auto nm = [](const std::string &s) { names.push_back(s); return names.back().c_str(); };
        addActParams(t, true);
        const char *M = "EQ";
        const uint32_t H = PF_HOST | PF_AB;
        t.addEnum(P_SCV, "Sidechain view", {"Off", "On"}, 0, 0, M);
        t.add(P_SEL, "Selected band", 0, 15, 1, PF_INT, f_int, M);
        t.addEnum(P_DRNG, "Display range", labels(L_DRNG), 1, 0, M);
        t.addEnum(P_AGM, "Auto gain mode", labels(L_AGM), 0, H, M);
        t.add(P_SCT, "Unused (was Sidechain trim)", -24, 24, 0, PF_AB, f_dbs, M);
        t.add(V_RAT, "Ratio", 1, 100, 2, 0, f_rat, M);
        t.add(P_IN, "Input", -24, 24, 0, H, f_dbs, M).unit = "dB";
        t.add(P_MIX, "Dry/wet", 0, 100, 100, H, f_pct, M).unit = "%";
        t.addEnum(P_AUTOG, "Auto gain", {"Off", "On"}, 0, H | PF_BOOL, M);
        t.add(P_DTP, "Set dynamic thresholds: full range", 0.1, 50, 4.2, 0, f_dtp, M).unit = "%";
        t.addEnum(P_LIN, "Phase mode", labels(L_LIN), 0, H, M);
        t.addEnum(P_LA, "Lookahead", {"Off", "On"}, 0, H | PF_BOOL, M);
        t.add(P_LAMS, "Lookahead time", 0.1, 20, 5, H, f_lams, M).unit = "ms";
        t.addEnum(P_AN, "Analyzer", labels(L_AN), 1, 0, M);
        t.addEnum(P_AB, "A/B set", {"A", "B"}, 0, PF_HOST, M);
        t.addEnum(P_DELTA, "Delta", {"Off", "On"}, 0, PF_HOST | PF_BOOL, M);
        t.add(V_HLD, "Hold", 0, 500, 0, 0, f_hold, M).unit = "ms";
        t.addEnum(P_TGTM, "Set dynamic thresholds: listening time", labels(L_TTIME), 3, 0, M);
        t.add(P_NBOX, "Band boxes shown", 1, 16, 4, PF_INT, f_int, M);
        t.add(V_KNE, "Knee", 0, 24, 6, 0, f_db, M).unit = "dB";
        for (int b = 0; b < NB; ++b) {
            std::string L = "Band " + std::to_string(b + 1) + " ";
            const char *mod = nm("Band " + std::to_string(b + 1));
            auto n = [&](const char *s) { return nm(L + s); };
            t.addEnum(ep(b, K_ON), n("on"), {"Off", "On"}, b == 1 ? 1 : 0, H | PF_BOOL, mod);
            t.addEnum(ep(b, K_TYPE), n("type"), labels(L_TYPE), b == 0 ? 6 : 0, H, mod);
            t.add(ep(b, K_FREQ), n("frequency"), 20, 20000, EFD[b], H, f_hz, mod).unit = "Hz";
            t.add(ep(b, K_GAIN), n("gain"), -30, 30, 0, H, f_dbs, mod).unit = "dB";
            t.add(ep(b, K_Q), n("Q"), 0.1, 40, b == 0 ? 0.71 : 1, H, f_q, mod);
            t.addEnum(ep(b, K_PLACE), n("channel"), labels(L_PLACE), 0, H, mod);
            t.add(ep(b, K_DYN), n("dynamics"), 0, 60066005, 9030, H | PF_INT, f_dyn, mod).tparse = p_dyn;
            t.add(ep(b, K_DSRC), n("trigger, Q mode, knee"), 0, 1927, 480, H | PF_INT, f_dsrc, mod);
            t.add(ep(b, K_THR), n("dynamic threshold"), -60, 0, -30, H, f_db, mod).unit = "dB";
            t.add(ep(b, K_RNG), n("dynamic range"), -30, 30, -3, H, f_dbs, mod).unit = "dB";
            t.add(ep(b, K_ATT), n("attack"), 0.1, 500, 10, H, f_att, mod).unit = "ms";
            t.add(ep(b, K_REL), n("release"), 1, 5000, 100, H, f_rel, mod).unit = "ms";
        }
        return t;
    }();
    return t;
}

// factory presets, written band by band (types as L_TYPE: 0 bell, 1 low shelf, 2 high shelf, 3 notch, 4 band pass,
// 5-8 high pass 6/12/24/48, 9-12 low pass 6/12/24/48, 13 high pass 18, 14 high pass 96, 15 low pass 18, 16 low pass 96)
namespace {
enum { BELL = 0, LSH = 1, HSH = 2, NOTCH = 3, BPASS = 4, HP12 = 6, HP24 = 7, HP48 = 8, LP12 = 10, LP24 = 11, LP48 = 12 };
enum { STEREO = 0, LEFT = 1, RIGHT = 2, MID = 3, SIDE = 4 };
struct EqP {
    std::vector<std::pair<int, double>> v;
    EqP &band(int b, int type, double f, double g = 0, double q = 0.71, int place = STEREO)
    {
        v.push_back({ep(b, K_ON), 1});
        v.push_back({ep(b, K_TYPE), (double)type});
        v.push_back({ep(b, K_FREQ), f});
        v.push_back({ep(b, K_GAIN), g});
        v.push_back({ep(b, K_Q), q});
        if (place != STEREO) v.push_back({ep(b, K_PLACE), (double)place});
        return *this;
    }
    // the band moves by rng (dB) as the level goes over thr (compress: 1); ratio, attack, release
    EqP &dyn(int b, double thr, double rng, double ratio = 2, double att = 10, double rel = 100, int mode = 1)
    {
        int rn = (int)std::lround(10000 * std::log(ratio) / std::log(100.0));
        v.push_back({ep(b, K_DYN), mode + 6.0 * rn});
        v.push_back({ep(b, K_THR), thr});
        v.push_back({ep(b, K_RNG), rng});
        v.push_back({ep(b, K_ATT), att});
        v.push_back({ep(b, K_REL), rel});
        return *this;
    }
    EqP &set(int id, double x) { v.push_back({id, x}); return *this; }
    operator std::string() const { return presetStr(v); }
};
EqP E() { return EqP(); }
}

static const std::vector<FactoryPreset> &factoryPresets()
{
    static std::vector<FactoryPreset> v = {
        // ---- guitar
        {"Instruments/Guitar/Clean: sparkle (high pass 150, shelf +2 at 6k)", "60=1 61=7 62=150 63=0 64=0.71 72=1 73=0 74=400 75=-2 76=1.2 84=1 85=2 86=6000 87=2 88=0.71"},
        {"Instruments/Guitar/Clean: soft and open", "60=1 61=7 62=140 63=0 64=0.71 72=1 73=0 74=400 75=-1.5 76=1.0 84=1 85=2 86=7000 87=1.5 88=0.71"},
        {"Instruments/Guitar/Funk: bright and tight", "60=1 61=7 62=120 63=0 64=0.71 72=1 73=0 74=250 75=-3 76=1.0 84=1 85=0 86=2500 87=3 88=1.2 96=1 97=2 98=8000 99=2 100=0.71 108=1 109=10 110=12000 111=0 112=0.71"},
        {"Instruments/Guitar/Funk: quack (mid push)", "60=1 61=7 62=150 63=0 64=0.71 72=1 73=0 74=300 75=-2 76=1.0 84=1 85=0 86=900 87=4 88=1.5 96=1 97=10 98=8000 99=0 100=0.71"},
        {"Instruments/Guitar/Rock: Marshall-style post EQ", "60=1 61=7 62=110 63=0 64=0.71 72=1 73=0 74=300 75=-2 76=1.2 84=1 85=0 86=1600 87=2 88=1.0 96=1 97=10 98=9000 99=0 100=0.71"},
        {"Instruments/Guitar/Rock: tube screamer boost (pre)", "60=1 61=6 62=120 63=0 64=0.71 72=1 73=0 74=750 75=4 76=0.7 84=1 85=2 86=6000 87=-3 88=0.71 50=6"},
        {"Instruments/Guitar/Rock: radio intro (band pass 500-3k)", "60=1 61=7 62=500 63=0 64=0.71 72=1 73=11 74=3000 75=0 76=0.71 84=1 85=0 86=1200 87=2 88=1.0"},
        {"Instruments/Guitar/Metal: Rectifier-style post EQ", "60=1 61=7 62=100 63=0 64=0.71 72=1 73=0 74=250 75=-1.5 76=1.0 84=1 85=0 86=400 87=-2.5 88=1.2 96=1 97=0 98=1800 99=1.5 100=1.0 108=1 109=0 110=3200 111=0 112=2.5 114=9031 116=-30 117=-3 118=5 119=80 120=1 121=10 122=9000 123=0 124=0.71"},
        {"Instruments/Guitar/Metal: modern tight post EQ", "60=1 61=7 62=120 63=0 64=0.71 72=1 73=0 74=180 75=0 76=1.5 78=9031 80=-30 81=-3 82=5 83=80 84=1 85=0 86=400 87=-3 88=1.4 96=1 97=0 98=2500 99=2 100=1.2 108=1 109=0 110=4000 111=0 112=3.0 114=9031 116=-30 117=-3 118=5 119=80 120=1 121=10 122=10000 123=0 124=0.71"},
        {"Instruments/Guitar/Metal: lead presence", "60=1 61=7 62=100 63=0 64=0.71 72=1 73=0 74=500 75=-2 76=1.0 84=1 85=0 86=3000 87=2.5 88=1.0 96=1 97=10 98=11000 99=0 100=0.71"},
        {"Instruments/Guitar/Acoustic: clear and airy", "60=1 61=6 62=80 63=0 64=0.71 72=1 73=0 74=200 75=-3 76=1 84=1 85=0 86=2500 87=2 88=1 96=1 97=2 98=10000 99=2 100=0.7"},
        {"Instruments/Guitar/Acoustic: piezo pickup (less quack)", E().band(0, HP24, 70).band(1, BELL, 220, -2.5, 1.2).band(2, BELL, 1100, -3, 1.5).band(3, HSH, 9000, 2.5)},
        // ---- bass
        {"Instruments/Bass/Clean: warm and round", "60=1 61=6 62=35 63=0 64=0.71 72=1 73=0 74=80 75=2 76=0.9 84=1 85=0 86=250 87=-2.5 88=1.2 96=1 97=0 98=800 99=1.5 100=1 108=1 109=2 110=3500 111=1.5 112=0.7"},
        {"Instruments/Bass/Distorted: growl", "60=1 61=7 62=40 63=0 64=0.71 72=1 73=0 74=100 75=2 76=1 84=1 85=0 86=500 87=-3 88=1 96=1 97=0 98=1600 99=3 100=1.2 108=1 109=10 110=6500 111=0 112=0.71"},
        {"Instruments/Bass/Slap: scooped and bright", "60=1 61=6 62=40 63=0 64=0.71 72=1 73=1 74=80 75=3 76=0.7 84=1 85=0 86=400 87=-4 88=1 96=1 97=2 98=3000 99=4 100=0.7 108=1 109=10 110=12000 111=0 112=0.71"},
        {"Instruments/Bass/Punk", "60=1 61=6 62=40 63=0 64=0.71 72=1 73=0 74=120 75=2 76=1 84=1 85=0 86=400 87=-3 88=1 96=1 97=0 98=1200 99=4 100=1.2 108=1 109=10 110=8000 111=0 112=0.71"},
        {"Instruments/Bass/Pop punk", "60=1 61=6 62=35 63=0 64=0.71 72=1 73=0 74=90 75=3 76=1 84=1 85=0 86=300 87=-3 88=1 96=1 97=0 98=2000 99=3 100=1 108=1 109=2 110=5000 111=1 112=0.7"},
        {"Instruments/Bass/Metal", "60=1 61=7 62=45 63=0 64=0.71 72=1 73=0 74=100 75=2 76=1.2 84=1 85=0 86=650 87=-4 88=1.4 96=1 97=0 98=2500 99=4 100=1.2 108=1 109=10 110=7000 111=0 112=0.71"},
        {"Instruments/Bass/Rock", "60=1 61=6 62=35 63=0 64=0.71 72=1 73=0 74=100 75=2 76=1 84=1 85=0 86=300 87=-2 88=1 96=1 97=0 98=700 99=2 100=1 108=1 109=10 110=9000 111=0 112=0.71"},
        {"Instruments/Bass/Jazz", "60=1 61=6 62=30 63=0 64=0.71 72=1 73=0 74=120 75=2 76=0.8 84=1 85=0 86=1000 87=-1 88=1 96=1 97=10 98=5000 99=0 100=0.71"},
        {"Instruments/Bass/Pop", "60=1 61=6 62=30 63=0 64=0.71 72=1 73=0 74=60 75=3 76=1 84=1 85=0 86=250 87=-3 88=1 96=1 97=2 98=2500 99=2 100=0.7"},
        {"Instruments/Bass/Upright: wood, less boom", E().band(0, HP24, 40).band(1, BELL, 120, -2, 1.2).band(2, BELL, 400, 2.5, 1.2).band(3, LP12, 7000)},
        {"Instruments/Bass/Synth bass: sub and edge", E().band(0, HP48, 28).band(1, LSH, 60, 2).band(2, BELL, 300, -3, 1).band(3, BELL, 1200, 2.5, 1.5)},
        // ---- vocals
        {"Instruments/Vocals/Lead: singing", "60=1 61=7 62=90 63=0 64=0.71 72=1 73=0 74=300 75=-2 76=1.2 84=1 85=0 86=3000 87=2 88=1 96=1 97=2 98=10000 99=2 100=0.7"},
        {"Instruments/Vocals/Lead: presence and air", "60=1 61=7 62=80 63=0 64=0.71 72=1 73=0 74=250 75=-2 76=1 84=1 85=0 86=3500 87=2.5 88=1 96=1 97=2 98=12000 99=2 100=0.7"},
        {"Instruments/Vocals/Lead: de-mud (dynamic at 250 Hz)", E().band(0, HP24, 90).band(1, BELL, 250, 0, 1.4).dyn(1, -30, -4, 3, 15, 120).band(2, BELL, 3500, 2, 1).band(3, HSH, 12000, 2)},
        {"Instruments/Vocals/Lead: de-ess (dynamic at 6.5 kHz)", E().band(0, HP24, 90).band(1, BELL, 6500, 0, 2.5).dyn(1, -34, -7, 4, 1, 60)},
        {"Instruments/Vocals/Rap: chest and bite", E().band(0, HP24, 80).band(1, BELL, 180, 1.5, 1).band(2, BELL, 450, -2.5, 1.4).band(3, BELL, 3000, 3, 1.2).band(4, HSH, 10000, 1.5)},
        {"Instruments/Vocals/Backing: thin and tucked", E().band(0, HP24, 200).band(1, BELL, 3000, -2, 1).band(2, HSH, 10000, 1.5)},
        {"Instruments/Vocals/Spoken: clear (podcast)", "60=1 61=7 62=120 63=0 64=0.71 72=1 73=0 74=250 75=-3 76=1 84=1 85=0 86=2500 87=3 88=1 96=1 97=10 98=10000 99=0 100=0.71"},
        // ---- drums
        {"Instruments/Drums/Kick: thump and click", "60=1 61=7 62=30 63=0 64=0.71 72=1 73=0 74=60 75=3 76=1.2 84=1 85=0 86=350 87=-4 88=1.4 96=1 97=0 98=3500 99=3 100=1.5"},
        {"Instruments/Drums/Kick: modern sub", E().band(0, HP48, 30).band(1, BELL, 55, 4, 1.2).band(2, BELL, 380, -5, 1.6).band(3, BELL, 4500, 3, 1.4)},
        {"Instruments/Drums/Snare: body and crack", "60=1 61=6 62=80 63=0 64=0.71 72=1 73=0 74=200 75=2 76=1.2 84=1 85=0 86=900 87=-2 88=1.5 96=1 97=2 98=5000 99=3 100=0.7"},
        {"Instruments/Drums/Snare: less ring (notch 900)", E().band(0, HP24, 80).band(1, BELL, 900, -6, 6).band(2, BELL, 220, 2, 1.2).band(3, HSH, 8000, 2)},
        {"Instruments/Drums/Toms: tone, less box", E().band(0, HP24, 60).band(1, BELL, 100, 2.5, 1.2).band(2, BELL, 450, -4, 1.4).band(3, BELL, 3500, 2.5, 1.2)},
        {"Instruments/Drums/Overheads: cymbals and air", E().band(0, HP24, 250).band(1, BELL, 500, -1.5, 1).band(2, HSH, 9000, 2)},
        {"Instruments/Drums/Hi-hat: no clank", E().band(0, HP24, 400).band(1, BELL, 900, -2, 1.2).band(2, BELL, 8000, 0, 2).dyn(2, -30, -4, 3, 2, 80)},
        {"Instruments/Drums/Room: dark and big", E().band(0, HP24, 60).band(1, BELL, 400, -3, 1).band(2, LP12, 7000)},
        // ---- keys
        {"Instruments/Keys/Piano: clear in the mix", E().band(0, HP24, 60).band(1, BELL, 200, -2, 1).band(2, BELL, 3500, 1.5, 1).band(3, HSH, 10000, 1.5)},
        {"Instruments/Keys/Electric piano: warm", E().band(0, HP24, 50).band(1, BELL, 300, 1.5, 0.8).band(2, BELL, 2500, -1.5, 1.2).band(3, LP12, 9000)},
        {"Instruments/Keys/Organ: tame the low end", E().band(0, HP24, 70).band(1, LSH, 150, -2).band(2, BELL, 2500, 1.5, 1)},
        {"Instruments/Keys/Synth pad: make room (cut 500, 2k)", E().band(0, HP24, 120).band(1, BELL, 500, -3, 0.8).band(2, BELL, 2000, -2, 0.8).band(3, HSH, 10000, 1.5)},
        // ---- strings, brass
        {"Instruments/Strings & brass/Strings: silky", E().band(0, HP24, 80).band(1, BELL, 250, -1.5, 1).band(2, BELL, 2500, -2, 1.4).band(3, HSH, 12000, 2)},
        {"Instruments/Strings & brass/Brass: smooth the blat", E().band(0, HP24, 80).band(1, BELL, 150, -2, 1).band(2, BELL, 3000, 0, 1.2).dyn(2, -28, -4, 3, 3, 80).band(3, HSH, 9000, 1)},
        {"Instruments/Strings & brass/Saxophone: honk and reed noise", E().band(0, HP24, 70).band(1, BELL, 350, -2.5, 1.4).band(2, BELL, 1500, -1.5, 1.2).band(3, BELL, 6000, -2, 2)},
        // ---- buses
        {"Buses/Instrument bus/Guitar bus: rock", "60=1 61=7 62=100 63=0 64=0.71 72=1 73=0 74=200 75=0 76=1.2 78=9031 80=-30 81=-3 82=5 83=80 84=1 85=0 86=3200 87=0 88=2.0 90=9031 92=-30 93=-3 94=5 95=80 96=1 97=10 98=9500 99=0 100=0.71"},
        {"Buses/Instrument bus/Drum bus: weight and snap", E().band(0, HP24, 30).band(1, LSH, 80, 1.5).band(2, BELL, 400, -2, 0.8).band(3, HSH, 8000, 1.5)},
        {"Buses/Instrument bus/Vocal bus: smooth", E().band(0, HP24, 100).band(1, BELL, 300, -1.5, 0.8).band(2, BELL, 4000, 0, 1.4).dyn(2, -30, -3, 2, 5, 120).band(3, HSH, 12000, 1.5)},
        {"Buses/Instrument bus/Music bus: room for the vocal (dyn 3k)", E().band(0, BELL, 3000, 0, 1).dyn(0, -30, -3, 2, 15, 150)},
        {"Buses/Mixbus/Tight low end, air", "60=1 61=6 62=25 63=0 64=0.71 72=1 73=0 74=300 75=-1 76=0.8 84=1 85=2 86=12000 87=1.5 88=0.7"},
        {"Buses/Mixbus/Gentle smile", E().band(0, HP24, 20).band(1, LSH, 80, 1).band(2, BELL, 400, -0.8, 0.7).band(3, HSH, 10000, 1.2)},
        {"Buses/Mixbus/Low end in check (dynamic 100 Hz)", E().band(0, LSH, 100, 0, 0.71).dyn(0, -24, -3, 2, 20, 150)},
        {"Buses/Master/Clean up: low cut and air", E().band(0, HP48, 22).band(1, HSH, 14000, 1)},
        {"Buses/Master/Harshness tamer (dynamic 3 kHz)", E().band(0, BELL, 3200, 0, 1.2).dyn(0, -24, -2.5, 2, 5, 120)},
        {"Buses/Master/Bass in mono (side high pass 120)", E().band(0, HP24, 120, 0, 0.71, SIDE).band(1, HSH, 10000, 1, 0.71, SIDE)},
        // ---- creative
        {"Creative/Telephone", "60=1 61=7 62=400 63=0 64=0.71 72=1 73=0 74=1500 75=4 76=1 84=1 85=11 86=3500 87=0 88=0.71"},
        {"Creative/AM radio", E().band(0, HP48, 400).band(1, LP48, 2800).band(2, BELL, 1400, 5, 1.2)},
        {"Creative/Megaphone", E().band(0, HP48, 600).band(1, LP24, 4000).band(2, BELL, 1800, 9, 2)},
        {"Creative/Through the wall", E().band(0, LP48, 300).band(1, BELL, 120, 3, 1)},
        {"Creative/Lo-fi tape", E().band(0, HP24, 90).band(1, LP24, 6500).band(2, BELL, 200, 2, 0.8)},
        {"Creative/Wide air (side shelf)", E().band(0, HSH, 6000, 4, 0.71, SIDE).band(1, HP24, 150, 0, 0.71, SIDE)},
        {"Creative/Filter sweep: start here (automate the high pass)", E().band(0, HP48, 20).band(1, LP48, 20000)},
        // ---- weird
        {"Weird/Ringing in A (110, 220, 440, 880)", E().band(0, BELL, 110, 12, 30).band(1, BELL, 220, 12, 30).band(2, BELL, 440, 12, 30).band(3, BELL, 880, 10, 30).set(P_IN, -12)},
        {"Weird/Underwater", E().band(0, LP48, 450).band(1, BELL, 300, 8, 4).band(2, BELL, 160, 4, 2)},
        {"Weird/Hollow (deep notches)", E().band(0, NOTCH, 300, 0, 3).band(1, NOTCH, 800, 0, 3).band(2, NOTCH, 2200, 0, 3).band(3, NOTCH, 6000, 0, 3)},
        {"Weird/Left dark, right bright", E().band(0, LP24, 1500, 0, 0.71, LEFT).band(1, HP24, 1500, 0, 0.71, RIGHT)},
        {"Weird/Rising in the gaps (1.2k up when quiet)", E().band(0, BELL, 1200, 0, 1.5).dyn(0, -30, 12, 4, 30, 400, 2)},
        {"Weird/Only the sides (mid cut)", E().band(0, LP12, 20, 0, 0.71, MID)},
    };
    return v;
}

// ---------------------------------------------------------------- the plug-in
class EqPlugin : public Plugin, public EngineHost {
public:
    explicit EqPlugin(const clap_host *h) : Plugin(h, kind()), eng(*this, act, uiLink, view) {}
    UiLink uiLink;
    Display view;
    Engine eng;
    double pv(int id) const override { return P.get(id); }
    double abv(int id) const override { return abValue(id); }
    void setAudio(int id, double v) override { setFromAudio(id, v); }
    gui::View *createView() override;
    int sel() const { return iclamp(iround(P.get(P_SEL)), 0, NB - 1); }
    // the window's ratio, hold and knee: the selected band's, kept in its packed parameters
    int vpTarget(int id) const override
    {
        if (id == V_RAT || id == V_HLD) return ep(sel(), K_DYN);
        if (id == V_KNE) return ep(sel(), K_DSRC);
        return 0;
    }
    double vpGet(int id) const override
    {
        if (id == V_KNE) return pkKn(P.get(ep(sel(), K_DSRC)));
        double dv = P.get(ep(sel(), K_DYN));
        return id == V_RAT ? pkRt(dv) : pkHold(dv);
    }
    double vpPacked(int id, double v) const override
    {
        if (id == V_KNE) {
            int64_t n = (int64_t)std::floor(P.get(ep(sel(), K_DSRC)) + 0.5);
            return (double)(n % 8 + 8 * (int64_t)emax(0, emin(240, std::floor(v * 10 + 0.5))));
        }
        int64_t dv = (int64_t)std::floor(P.get(ep(sel(), K_DYN)) + 0.5);
        if (id == V_RAT) {
            int64_t n = v >= 99.5 ? 10000 : (int64_t)emax(0, emin(10000, std::floor(10000 * std::log(emax(1, v)) / std::log(100.0) + 0.5)));
            return (double)(dv % 6 + 6 * n + 60006 * (dv / 60006));
        }
        return (double)(dv % 60006 + 60006 * (int64_t)emax(0, emin(1000, std::floor(v * 2 + 0.5))));
    }

protected:
    int latencyFromParams(double sr) const override { return latency(iround(P.get(P_LIN)), P.get(P_LA) >= 0.5, P.get(P_LAMS), sr); }
    int hkRset(int x) override { return x == GS_TTIME ? P_TGTM : 0; }
    void hkTgStart() override { eng.tgStart(); }
    void hkPublish(shm::Slot &b) override { b.p[GS_TTIME].store(P.get(P_TGTM), std::memory_order_relaxed); }
    void dspActivate(double sr, uint32_t) override { eng.init(sr); }
    void dspReset() override { eng.reset(); }
    void dspProcess(const double *const *in, const double *const *sc, double *const *out, int n) override
    {
        eng.block(n);
        act.block(n);
        eng.afterAct();
        eng.samples(in, sc, out, n);
        act.process(out, 2, n);
        eng.post(sc, out, n);
        act.audioKey(out, 2, n);
    }
};

static Plugin *create(const clap_host *h) { return new EqPlugin(h); }

// ---------------------------------------------------------------- the window
static Col bandCol(int b, float a = 1)
{
    static const Col c[8] = {Col(1.0f, 0.58f, 0.14f), Col(0.66f, 0.55f, 1.0f), Col(0.30f, 0.80f, 0.78f), Col(0.42f, 0.85f, 0.45f),
                             Col(0.42f, 0.62f, 1.0f), Col(0.98f, 0.47f, 0.66f), Col(0.95f, 0.82f, 0.32f), Col(0.90f, 0.42f, 0.36f)};
    return c[((b % 8) + 8) % 8].alpha(a);
}

class EqView : public EffectView {
public:
    explicit EqView(EqPlugin &p) : EffectView(p, p.uiLink, 1000, BASE_H, "EQ"), D(p)
    {
        SH = 196 + 34 + 34 + 15;
        help = {"Double-click the display to add a band; drag a node (frequency, gain), wheel: Q or slope; right-click: more.",
                "Dynamic bands (bells and shelves) compress or expand from the main input or the sidechain (3-4).",
                "Set dynamic thresholds listens, then sets every dynamic band's threshold to reach its range on the loudest moments.",
                "Ctrl while changing Input moves the output the other way. Shift + click Solo, Delta or Auto gain: only while held."};
    }
    ~EqView() override { D.view.solo.store(-1); }
    void frame(Ui &ui) override;
    void opened() override { spi.reset(); spo.reset(); sps.reset(); }

protected:
    void hashMore(std::function<void(uint64_t)> mix) override
    {
        mix((uint64_t)D.view.side.load());
        mix((uint64_t)(nh + 2) | (uint64_t)(ih + 2) << 6 | (uint64_t)(ndrag + 2) << 12 | (uint64_t)(soloL + 2) << 18 | (uint64_t)(soloM + 2) << 24);
        int tgs = av.myslot >= 0 ? (int)shm::slot(av.myslot).tgs.load() : 0;
        mix((uint64_t)tgs | (uint64_t)(tgs == 2 ? std::floor(shm::slot(av.myslot).tgel.load() * 10) : 0) << 4);
        mix((uint64_t)std::llround(D.view.agdb.load() * 10 + 1000));
    }
    bool extraFull() override { return ndrag >= 0; }

private:
    static constexpr float BASE_H = 656, SCV_H = 118;
    EqPlugin &D;
    Spectrum spi, spo, sps;
    int sel = 1, lin = 0, nsc = 0, ndyn = 0;
    double dr = 12;
    // the window's copies of each band's filter (a dynamic band at the gain it has now) and where its node is
    double GCO[NB * 40] = {}, GEG[NB] = {};
    int GNS[NB] = {};
    // a dynamic band at rest (its own gain) and at the end of its range: the curve's reach, drawn faintly
    double GRC[NB * 40] = {}, GXC[NB * 40] = {};
    int GRS[NB] = {}, GXS[NB] = {};
    bool GON[NB] = {};
    float GNX[NB] = {}, GNY[NB] = {}, GIX[NB] = {}, GIY[NB] = {};
    int nh = -1, ih = -1, ndrag = -1, soloL = -1, soloM = -1;
    float ndx = 0, ndy = 0;
    double dclkT = -10;

    int ty(int b) const { return iround(D.get(ep(b, K_TYPE))); }
    bool dynOn(int b) const { return D.get(ep(b, K_ON)) >= 0.5 && lin == 0 && ty(b) <= 2 && pkDm(D.get(ep(b, K_DYN))) > 0; }
    int soloAct() const { return soloM >= 0 ? soloM : soloL; }
    void select(int b) { if (iround(D.get(P_SEL)) != b) D.setOnce(P_SEL, b); }
    void prep();
    float ey(const R &r, double db) const { return r.y + r.h * 0.5f - (float)(db / dr) * r.h * 0.5f; }
    // band boxes
    bool used(int b) const
    {
        bool u = D.get(ep(b, K_ON)) >= 0.5;
        for (int k = 1; k < EBN; ++k)
            if (std::fabs(D.get(ep(b, k)) - D.T.def(ep(b, k))) > 0.0001) u = true;
        return u;
    }
    int nbox() const
    {
        int n = iclamp(iround(D.get(P_NBOX)), 1, NB);
        for (int b = 0; b < NB; ++b)
            if (D.get(ep(b, K_ON)) >= 0.5) n = std::max(n, b + 1);
        return n;
    }
    void bandOn(int b)
    {
        for (int k = 0; k < EBN; ++k) D.setOnce(ep(b, k), D.T.def(ep(b, k)));
        D.setOnce(ep(b, K_ON), 1);
        select(b);
    }
    void delband(int b)
    {
        if (soloL == b) soloL = -1;
        if (soloM == b) soloM = -1;
        for (int k = 0; k < EBN; ++k) D.setOnce(ep(b, k), D.T.def(ep(b, k)));
        D.setOnce(ep(b, K_ON), 0);
    }
    void addbox(int n)
    {
        int nbx = nbox();
        for (int i = 0; i < n; ++i) {
            if (nbx < NB) {
                D.setOnce(P_NBOX, nbx + 1);
                bandOn(nbx);
                nbx += 1;
            } else {
                int b = -1;
                for (int k = 0; k < NB; ++k)
                    if (b < 0 && !used(k)) b = k;
                if (b >= 0) bandOn(b);
            }
        }
    }
    // double-click on the display: the band after the last one in use (a new box if needed), at the mouse
    int addnext(double f, double gn)
    {
        int last = -1;
        for (int k = 0; k < NB; ++k)
            if (used(k)) last = k;
        int b = last + 1;
        if (b >= NB) {
            b = -1;
            for (int k = 0; k < NB; ++k)
                if (b < 0 && !used(k)) b = k;
        }
        if (b >= 0) {
            if (b >= nbox()) D.setOnce(P_NBOX, b + 1);
            for (int k = 0; k < EBN; ++k) D.setOnce(ep(b, k), D.T.def(ep(b, k)));
            D.setOnce(ep(b, K_TYPE), f < 40 ? 6 : (f > 15000 ? 10 : 0));
            D.setOnce(ep(b, K_FREQ), std::max(20.0, std::min(20000.0, f)));
            D.setOnce(ep(b, K_GAIN), (f < 40 || f > 15000) ? 0 : std::max(-30.0, std::min(30.0, gn)));
            D.setOnce(ep(b, K_Q), (f < 40 || f > 15000) ? 0.71 : 1);
            D.setOnce(ep(b, K_ON), 1);
            select(b);
        }
        return b;
    }
    void delbox(int b)
    {
        int nbx = nbox();
        if (b == nbx - 1 && nbx > 1 && D.get(ep(b, K_ON)) < 0.5) {
            delband(b);
            D.setOnce(P_NBOX, nbx - 1);
            select(nbx - 2);
        } else {
            delband(b);
        }
    }
    // the type menu's entry: 0..4 as they are, 5 = high pass, 6 = low pass (the slope stays)
    void setType(int b, int ui)
    {
        int t = ty(b), sl = t < 5 ? 3 : cutS(t);
        D.setOnce(ep(b, K_TYPE), ui < 5 ? ui : cutTy(ui == 5, sl));
    }
    // a dropdown for one field of a packed parameter: the value is floor(v / dv) % md
    void pdd(Ui &ui, const R &r, int id, int dv, int md, const char *const *lab, const char *prefix, bool en = true)
    {
        int64_t v = (int64_t)std::floor(D.get(id) + 0.5);
        int cv = (int)((v / dv) % md);
        if (!ui.dropdown(r, lab[cv], en, prefix)) return;
        std::vector<MenuItem> m;
        for (int i = 0; i < md; ++i) m.push_back(MenuItem::item(lab[i], i + 1, i == cv));
        EqPlugin *pp = &D;
        int rb = iround(D.get(ep(sel, K_RNG)) * 1000);
        ui.menu(m, r.x, r.b() + 4, [pp, id, v, dv, cv, rb](int k) {
            if (k <= 0) return;
            pp->setOnce(id, (double)(v + (int64_t)(k - 1 - cv) * dv));
            // a band switched to Compress starts at a -3 dB range, to Expand at +3 dB (unless the range was set by hand)
            if ((id - EB0) % EBN == K_DYN && dv == 1 && k - 1 != cv && k - 1 > 0 && (rb == -3000 || rb == 3000 || rb == -6000))
                pp->setOnce(ep((id - EB0) / EBN, K_RNG), k - 1 == 2 ? 3 : -3);
        }, r.w);
    }
    void display(Ui &ui, const R &r);
    void meters(Ui &ui, const R &r);
    void scView(Ui &ui, const R &r);
    void bandsRow(Ui &ui, float y);
    void bandSection(Ui &ui, float y);
    void globalRows(Ui &ui, float y);
};

gui::View *EqPlugin::createView() { return new EqView(*this); }

// every band's filter as the window shows it (a dynamic band at the gain it has right now)
void EqView::prep()
{
    double sr = D.sampleRate() > 0 ? D.sampleRate() : 48000;
    nsc = 0;
    ndyn = 0;
    for (int b = 0; b < NB; ++b) {
        GON[b] = D.get(ep(b, K_ON)) >= 0.5;
        if (GON[b]) {
            int t = ty(b);
            double g = D.get(ep(b, K_GAIN));
            bool dy = lin == 0 && t <= 2 && pkDm(D.get(ep(b, K_DYN))) > 0;
            GEG[b] = dy && live ? g + D.view.edg[b].load() : g;
            GNS[b] = flt::coefBw(t, D.get(ep(b, K_FREQ)), GEG[b], D.get(ep(b, K_Q)), pkQm(D.get(ep(b, K_DSRC))), GCO + b * 40, sr);
            if (dy) {
                double gx = std::max(-30.0, std::min(30.0, g + D.get(ep(b, K_RNG))));
                GRS[b] = flt::coefBw(t, D.get(ep(b, K_FREQ)), g, D.get(ep(b, K_Q)), pkQm(D.get(ep(b, K_DSRC))), GRC + b * 40, sr);
                GXS[b] = flt::coefBw(t, D.get(ep(b, K_FREQ)), gx, D.get(ep(b, K_Q)), pkQm(D.get(ep(b, K_DSRC))), GXC + b * 40, sr);
            } else {
                GRS[b] = GXS[b] = 0;
            }
            if (dy) {
                ndyn++;
                if (pkSrc(D.get(ep(b, K_DSRC)))) nsc++;
            }
        } else {
            GNS[b] = 0;
        }
    }
    D.view.scok.store(nsc > 0 ? 1 : 0, std::memory_order_relaxed);
}

// ---- the display: spectrum behind, the summed curve, one node per band ----
void EqView::display(Ui &ui, const R &r)
{
    FreqScale fs(r);
    for (int b = 0; b < NB; ++b) {
        if (!GON[b]) continue;
        int t = ty(b);
        GNX[b] = (float)fs.x(D.get(ep(b, K_FREQ)));
        GNY[b] = ey(r, t <= 2 ? std::max(-dr, std::min(dr, D.get(ep(b, K_GAIN)))) : 0);
        GIX[b] = std::min(r.r() - 14, GNX[b] + 6);
        GIY[b] = std::max(r.y + 2, GNY[b] - 19);
    }
    const R corner(r.r() - 270, r.y, 270, 30);   // the Analyzer and Range boxes belong to themselves
    const int sa = soloAct();
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        freqGrid(ui, r);
        double stp = dr <= 6 ? 3 : (dr <= 18 ? 6 : 10);
        int nn = (int)std::floor(dr / stp);
        for (int i = -nn; i <= nn; ++i) ui.fill(R(r.x, ey(r, i * stp), r.w, 1 / ui.s), Paint(i == 0 ? th::gridHi() : th::grid()));
        enumDD(ui, R(corner.x + 4, r.y + 4, 132, 22), P_AN, L_AN, 4, "Analyzer");
        enumDD(ui, R(corner.x + 140, r.y + 4, 126, 22), P_DRNG, L_DRNG, 4, "Range");
        // which node / solo icon is under the mouse
        float mx = ui.mx(), my = ui.my();
        bool ind = ui.hit(r) && !ui.hit(corner) && !ui.busy();
        nh = -1;
        ih = -1;
        if (ind) {
            float best = 1e9f;
            for (int b = 0; b < NB; ++b) {
                if (!GON[b]) continue;
                if (mx >= GIX[b] && mx < GIX[b] + 12 && my >= GIY[b] && my < GIY[b] + 12) ih = b;
                float d = std::hypot(mx - GNX[b], my - GNY[b]);
                if (d < 9 && d < best) { best = d; nh = b; }
            }
        }
        if (ndrag >= 0) { nh = ndrag; ih = -1; }
        if (nh >= 0 || ih >= 0) ui.cursor = nh >= 0 ? CUR_MOVE : CUR_HAND;
        if (ind && ui.lclick && !ui.used) {
            ui.used = true;
            if (ih >= 0) {
                select(ih);
                if (ui.mods & M_SHIFT) soloM = ih;
                else soloL = soloL == ih ? -1 : ih;
            } else if (nh >= 0) {
                select(nh);
                ndrag = nh;
                ndx = mx - GNX[nh];
                ndy = my - GNY[nh];
                if (ui.mods & M_SHIFT) soloM = nh;
                D.beginEdit(ep(nh, K_FREQ));
                if (ty(nh) <= 2) D.beginEdit(ep(nh, K_GAIN));
            } else if (ui.now - dclkT < 0.35) {
                addnext(fs.f(mx), (r.y + r.h * 0.5 - my) / (r.h * 0.5) * dr);
                dclkT = -10;
            } else {
                dclkT = ui.now;
            }
        }
        if (ind && ui.rclick && !ui.used) {
            ui.used = true;
            if (nh >= 0) {
                int b = nh;
                select(b);
                std::vector<MenuItem> m = {MenuItem::item("Solo this band", 1, soloL == b), MenuItem::sep()};
                int cur = ty(b) < 5 ? ty(b) : (cutHp(ty(b)) ? 5 : 6);
                for (int i = 0; i < 7; ++i) m.push_back(MenuItem::item(L_UTYPE[i], 2 + i, i == cur));
                m.push_back(MenuItem::sep());
                m.push_back(MenuItem::item("Delete band", 9));
                ui.menu(m, mx, my, [this, b](int k) {
                    if (k == 1) soloL = soloL == b ? -1 : b;
                    if (k >= 2 && k <= 8) setType(b, k - 2);
                    if (k == 9) delband(b);
                });
            } else {
                double f = fs.f(mx), gn = (r.y + r.h * 0.5 - my) / (r.h * 0.5) * dr;
                ui.menu({MenuItem::item("Add a band here", 1)}, mx, my, [this, f, gn](int k) { if (k == 1) addnext(f, gn); });
            }
        }
        if (ind && ui.wheel != 0 && !ui.used && !(ui.mods & M_SHIFT) && nh >= 0) {
            ui.used = true;
            select(nh);
            int t = ty(nh);
            // a high or low pass: the wheel steps its slope; any other band: its Q
            if (t >= 5) D.setOnce(ep(nh, K_TYPE), cutTy(cutHp(t), iclamp(cutS(t) + (ui.wheel > 0 ? 1 : -1), 0, 5)));
            else D.setOnce(ep(nh, K_Q), D.T.clamp(ep(nh, K_Q), D.get(ep(nh, K_Q)) * std::exp((ui.wheel > 0 ? 1 : -1) * 0.12)));
        }
        if (ndrag >= 0) {
            int b = ndrag;
            if (ui.ldown) {
                D.edit(ep(b, K_FREQ), std::max(20.0, std::min(20000.0, fs.f(mx - ndx))));
                if (ty(b) <= 2) D.edit(ep(b, K_GAIN), std::floor(std::max(-30.0, std::min(30.0, (r.y + r.h * 0.5 - (my - ndy)) / (r.h * 0.5) * dr)) * 10 + 0.5) / 10);
            } else {
                D.endEdit(ep(b, K_FREQ));
                if (ty(b) <= 2) D.endEdit(ep(b, K_GAIN));
                ndrag = -1;
            }
        }
        if (!ui.ldown) soloM = -1;
        D.view.solo.store(soloAct(), std::memory_order_relaxed);
    }
    if (!ui.drawD()) return;
    Canvas &c = ui.cv();
    double sr = D.sampleRate() > 0 ? D.sampleRate() : 48000;
    int an = iclamp(iround(D.get(P_AN)), 0, 3);
    if (live && an > 0) {
        int ap = D.view.an.ap.load(std::memory_order_acquire);
        if (an == 1 || an == 2) spi.update(D.view.an.in, AnFeed::N - 1, ap, ui.dt, sr);
        if (an == 1 || an == 3) spo.update(D.view.an.out, AnFeed::N - 1, ap, ui.dt, sr);
    }
    // the summed curve and the selected band's own (filled), sampled every 2 units
    int cn = (int)std::floor(r.w / 2);
    std::vector<Pt> pts(cn + 1), xpts(cn + 1);
    std::vector<float> sy(cn + 1), ry(cn + 1), xy(cn + 1);
    float y0 = ey(r, 0);
    for (int i = 0; i <= cn; ++i) {
        float px = r.x + i * r.w / cn;
        double f = fs.f(px), w = 2 * PI * f / sr, cw = std::cos(w), c2 = std::cos(2 * w), tot = 0, sdb = 0, rest = 0, ext = 0;
        for (int b = 0; b < NB; ++b) {
            if (GNS[b] <= 0) continue;
            double m = 10 * std::log10(std::max(1e-12, flt::mag2(GCO + b * 40, GNS[b], cw, c2)));
            tot += m;
            if (b == sel) sdb = m;
            if (ndyn > 0) {
                // the reach: every dynamic band at rest, and at the end of its range
                rest += GRS[b] > 0 ? 10 * std::log10(std::max(1e-12, flt::mag2(GRC + b * 40, GRS[b], cw, c2))) : m;
                ext += GXS[b] > 0 ? 10 * std::log10(std::max(1e-12, flt::mag2(GXC + b * 40, GXS[b], cw, c2))) : m;
            }
        }
        pts[i] = {ui.X(px), ui.Y(ey(r, std::max(-dr, std::min(dr, tot))))};
        sy[i] = ey(r, std::max(-dr, std::min(dr, sdb)));
        ry[i] = ey(r, std::max(-dr, std::min(dr, rest)));
        xy[i] = ey(r, std::max(-dr, std::min(dr, ext)));
        xpts[i] = {ui.X(px), ui.Y(xy[i])};
    }
    // drawn twice: around the Analyzer and Range boxes in the top right corner
    const R clips[2] = {R(r.x + 1, r.y + 1, corner.x - r.x - 1, r.h - 2), R(corner.x, corner.b(), r.r() - corner.x - 1, r.b() - corner.b() - 1)};
    for (const R &cl : clips) {
        c.pushClip(ui.X(cl.x), ui.Y(cl.y), ui.W(cl.w), ui.W(cl.h));
        if (live && an > 0) {
            if (an == 1 || an == 2) {
                Paint p = Paint::vertical(ui.Y(r.y), Col::hex(0x34343e), ui.Y(r.b()), Col::hex(0x18181d));
                drawSpectrum(ui, fs, spi, sr, &p, nullptr);
            }
            if (an == 1 || an == 3) {
                Paint p = Paint::vertical(ui.Y(r.y), Col::hex(0x40260d), ui.Y(r.b()), Col::hex(0x1a1008));
                Col lc = Col::hex(0xc8c3bb).alpha(0.5f);
                drawSpectrum(ui, fs, spo, sr, an == 3 ? &p : nullptr, &lc, 1.1f);
            }
        }
        if (ndyn > 0) {
            // how far the dynamic bands can take the curve: a faint band from rest to the end of their ranges
            Col rc = th::orange().alpha(0.09f);
            for (int i = 0; i < cn; ++i) {
                float x0 = r.x + i * r.w / cn, x1 = r.x + (i + 1) * r.w / cn;
                float ya = std::min(ry[i], xy[i]), yb = std::max(ry[i], xy[i]);
                if (yb - ya > 0.3f) ui.fill(R(x0, ya, x1 - x0 + 0.05f, yb - ya), Paint(rc));
            }
            c.strokePolyline(xpts.data(), cn + 1, ui.W(1), Paint(th::orange().alpha(0.35f)));
        }
        if (GON[sel]) {
            Col fc = bandCol(sel, 0.22f);
            for (int i = 0; i < cn; ++i) {
                float x0 = r.x + i * r.w / cn, x1 = r.x + (i + 1) * r.w / cn;
                float ya = std::min(y0, sy[i]), yb = std::max(y0, sy[i]);
                if (yb - ya > 0.3f) ui.fill(R(x0, ya, x1 - x0 + 0.05f, yb - ya), Paint(fc));
            }
        }
        c.strokePolyline(pts.data(), cn + 1, ui.W(2), Paint(th::orange()));
        // the nodes (and their solo icons); a dynamic band also shows where its gain is now
        for (int b = 0; b < NB; ++b) {
            if (!GON[b]) continue;
            Col bc = bandCol(b);
            if (dynOn(b)) c.strokeCircle(ui.X(GNX[b]), ui.Y(ey(r, std::max(-dr, std::min(dr, GEG[b])))), ui.W(4), ui.W(1.2f), Paint(bandCol(b, 0.7f)));
            float rad = (b == sel || b == nh) ? 8 : 6.5f;
            c.fillCircle(ui.X(GNX[b]), ui.Y(GNY[b]), ui.W(rad), Paint(bc));
            if (b == sel) c.strokeCircle(ui.X(GNX[b]), ui.Y(GNY[b]), ui.W(8.5f), ui.W(1.2f), Paint(th::text()));
            char s[8];
            snprintf(s, sizeof s, "%d", b + 1);
            ui.text(R(GNX[b] - 8, GNY[b] - 7, 16, 14), s, Col::hex(0x0d0b08), F_BOLD, 10, 1);
            R si(GIX[b], GIY[b], 12, 12);
            bool so = sa == b;
            ui.fill(si, Paint(so ? bc : (ih == b ? th::hover() : th::raised())), 2);
            ui.stroke(si, Paint(so ? bc : th::border()), 2);
            ui.text(si, "S", so ? th::orangeInk() : th::dim(), F_BOLD, 8.5f, 1);
        }
        c.popClip();
    }
    // the gain scale, the frequencies, and the line of help (or the band under the mouse)
    double stp = dr <= 6 ? 3 : (dr <= 18 ? 6 : 10);
    int nn = (int)std::floor(dr / stp);
    for (int i = -nn; i <= nn; ++i) {
        char s[8];
        snprintf(s, sizeof s, i > 0 ? "+%.0f" : "%.0f", i * stp);
        ui.text(R(r.x + 4, std::max(r.y + 2, ey(r, i * stp) - 13), 30, 12), s, th::faint(), F_REGULAR, 9.5f);
    }
    freqLabels(ui, r);
    char hint[160];
    if (nh >= 0) {
        char hf[32];
        f_hz(D.get(ep(nh, K_FREQ)), hf, sizeof hf);
        int t = ty(nh);
        if (t >= 5) snprintf(hint, sizeof hint, "Band %d:  %s   %s", nh + 1, hf, L_SLOPE[cutS(t)]);
        else snprintf(hint, sizeof hint, "Band %d:  %s   %+.1f dB   Q %.2f", nh + 1, hf, D.get(ep(nh, K_GAIN)), D.get(ep(nh, K_Q)));
    } else {
        snprintf(hint, sizeof hint, "Double-click: add a band. Drag a node; wheel: Q or slope; right-click: more; Shift + hold: solo.");
    }
    ui.textFit(R(r.x + 36, r.y + 8, corner.x - r.x - 44, 14), hint, nh >= 0 ? th::text() : th::faint(), F_REGULAR, 10.5f);
    ui.stroke(r, Paint(th::border()), 6);
}

void EqView::meters(Ui &ui, const R &r)
{
    double range = av.meterRange();
    float top = r.y + 22, h = r.h - 22 - 10;
    float x = r.x + 6, ox = r.r() - 6 - 22;
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        ui.stroke(r, Paint(th::border()), 6);
        ui.text(R(x - 2, r.y + 6, 26, 12), "IN", th::faint(), F_SEMIBOLD, 10, 1);
        ui.text(R(r.x + r.w * 0.5f - 14, r.y + 6, 28, 12), "AG", th::faint(), F_SEMIBOLD, 10, 1);
        ui.text(R(ox - 2, r.y + 6, 26, 12), "OUT", th::faint(), F_SEMIBOLD, 10, 1);
        char s[16];
        snprintf(s, sizeof s, "%+.1f", D.get(P_AUTOG) >= 0.5 ? D.view.agdb.load() : 0.0);
        ui.text(R(r.x + r.w * 0.5f - 15, r.y + r.h * 0.5f - 9, 30, 14), s, D.get(P_AUTOG) >= 0.5 ? th::orange() : th::faint(), F_SEMIBOLD, 11, 1);
        ui.text(R(r.x + r.w * 0.5f - 15, r.y + r.h * 0.5f + 5, 30, 14), "dB", th::faint(), F_REGULAR, 10, 1);
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

// ---- the sidechain view: what the sidechain bands' detectors hear; takes no room when it is off ----
void EqView::scView(Ui &ui, const R &r)
{
    FreqScale fs(r);
    int nr = 0;
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        freqGrid(ui, r);
        if (ui.button(R(r.r() - 54, r.y + 4, 48, 20), "Hide")) D.setOnce(P_SCV, 0);
    }
    for (int b = 0; b < NB; ++b) if (dynOn(b) && pkSrc(D.get(ep(b, K_DSRC)))) nr++;
    if (!ui.drawD()) return;
    Canvas &c = ui.cv();
    double sr = D.sampleRate() > 0 ? D.sampleRate() : 48000;
    c.pushClip(ui.X(r.x + 1), ui.Y(r.y + 1), ui.W(r.w - 2), ui.W(r.h - 2));
    if (live) {
        sps.update(D.view.an.sc, AnFeed::N - 1, D.view.an.ap.load(std::memory_order_acquire), ui.dt, sr);
        Paint p = Paint::vertical(ui.Y(r.y), Col::hex(0x34343e), ui.Y(r.b()), Col::hex(0x18181d));
        Col lc = th::orange().alpha(0.8f);
        drawSpectrum(ui, fs, sps, sr, &p, &lc, 1.1f);
    }
    // the bands that listen to the sidechain: what their detector hears
    for (int b = 0; b < NB; ++b) {
        if (!(dynOn(b) && pkSrc(D.get(ep(b, K_DSRC))))) continue;
        double bw = std::pow(2.0, 0.5 / std::max(0.4, std::min(10.0, D.get(ep(b, K_Q))))), f = D.get(ep(b, K_FREQ));
        float x1 = (float)fs.x(f / bw), x2 = (float)fs.x(f * bw);
        ui.fill(R(x1, r.y, x2 - x1, r.h), Paint(th::purple().alpha(0.16f)));
        ui.fill(R(x1, r.y, 1, r.h), Paint(th::purple()));
        ui.fill(R(x2, r.y, 1, r.h), Paint(th::purple()));
        char s[8];
        snprintf(s, sizeof s, "%d", b + 1);
        ui.text(R(x1 + 3, r.y + 3, 14, 12), s, th::purple(), F_BOLD, 10);
    }
    freqLabels(ui, r);
    c.popClip();
    ui.textFit(R(r.x + 8, r.y + 7, r.w - 70, 14),
               nr ? "Sidechain input (3-4): the purple bands are what the sidechain bands' detectors hear."
                  : "Sidechain input (3-4). Set a band to Dynamic with the Sidechain trigger and its detector range shows here.",
               th::dim(), F_REGULAR, 10.5f);
    ui.stroke(r, Paint(th::border()), 6);
}

// ---- the bands: a numbered chip each (+ adds one, right-click + adds several; - deletes the selected band, and the
// last box with it), then solo and delta ----
void EqView::bandsRow(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float h = 26;
    int nbx = nbox();
    label(ui, R(M, y, 48, h), "Bands");
    float x = M + 50;
    for (int b = 0; b < nbx; ++b) {
        R r(x + b * 32, y, 28, h);
        bool on = D.get(ep(b, K_ON)) >= 0.5, sl = b == sel, hv = ui.hit(r);
        ui.fill(r, Paint(hv ? th::hover() : (sl ? th::raised2() : th::panel())), 5);
        if (on) ui.fill(R(r.x + 4, r.b() - 4, r.w - 8, 2), Paint(bandCol(b)), 1);
        ui.stroke(r, Paint(sl ? th::text().alpha(0.8f) : th::border()), 5, sl ? 1.4f : 1);
        char s[16];
        snprintf(s, sizeof s, "%d", b + 1);
        ui.text(R(r.x, r.y - 1, r.w, r.h), s, on ? (sl ? th::text() : th::dim()) : th::faint(), F_SEMIBOLD, 12, 1);
        if (hv && ui.lclick && !ui.used && !ui.busy()) {
            ui.used = true;
            if (!on) D.setOnce(ep(b, K_ON), 1);
            select(b);
        }
    }
    x += nbx * 32;
    if (nbx < NB) {
        R r(x, y, 26, h);
        if (ui.button(r, "+")) addbox(1);
        if (ui.hit(r) && ui.rclick && !ui.used && !ui.busy()) {
            ui.used = true;
            std::vector<MenuItem> m;
            for (int i = 1; i <= NB - nbx; ++i) m.push_back(MenuItem::item("Add " + std::to_string(i) + (i > 1 ? " bands" : " band"), i));
            ui.menu(m, ui.mx(), ui.my(), [this](int k) { if (k > 0) addbox(k); });
        }
        x += 30;
    }
    if (nbx > 1 && ui.button(R(x, y, 26, h), "-")) delbox(sel);
    bool son = D.get(ep(sel, K_ON)) >= 0.5;
    if (son) {
        if (ui.button(R(DW - M - 92 - 8 - 84, y, 84, h), "Solo", soloAct() == sel, true, BS_PURPLE)) {
            if (ui.mods & M_SHIFT) soloM = sel;
            else soloL = soloL == sel ? -1 : sel;
            D.view.solo.store(soloAct(), std::memory_order_relaxed);
        }
    }
    toggle(ui, R(DW - M - 92, y, 92, h), "Delta", P_DELTA, BS_PURPLE, true);
}

// ---- the selected band: on, type, channel, dynamic, trigger, detect; then frequency, gain and Q (a high or low pass:
// its slope) on the left, and on a dynamic band threshold, ratio, knee | attack, hold, release. The rows are always
// the same height, so nothing below moves when a band becomes dynamic ----
void EqView::bandSection(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float h = 26, dy = 32, cw = 318;
    const int b = sel;
    bool son = D.get(ep(b, K_ON)) >= 0.5;
    int t = ty(b);
    bool sdy = lin == 0 && t <= 2, sdon = son && sdy && pkDm(D.get(ep(b, K_DYN))) > 0;
    char s[24];
    snprintf(s, sizeof s, "BAND %d", b + 1);
    ui.text(R(M, y, 70, h), s, son ? bandCol(b) : th::faint(), F_BOLD, 13);
    toggle(ui, R(M + 72, y, 50, h), "On", ep(b, K_ON));
    if (!son) {
        ui.text(R(M + 136, y, 600, h), "This band is off: switch it on, or double-click the display to add a band where you want it.", th::faint(), F_REGULAR, 11.5f);
        return;
    }
    {
        R r(M + 130, y, 130, h);
        int cur = t < 5 ? t : (cutHp(t) ? 5 : 6);
        if (ui.dropdown(r, L_UTYPE[cur])) {
            std::vector<MenuItem> m;
            for (int i = 0; i < 7; ++i) m.push_back(MenuItem::item(L_UTYPE[i], i + 1, i == cur));
            ui.menu(m, r.x, r.b() + 4, [this, b](int k) { if (k > 0) setType(b, k - 1); }, r.w);
        }
    }
    enumDD(ui, R(M + 268, y, 130, h), ep(b, K_PLACE), L_PLACE, 5, "Ch");
    if (sdy) pdd(ui, R(M + 406, y, 170, h), ep(b, K_DYN), 1, 3, L_DM, "Dynamic");
    if (sdon) pdd(ui, R(M + 584, y, 160, h), ep(b, K_DSRC), 1, 2, L_TRIG, nullptr);
    if (sdon) pdd(ui, R(M + 752, y, 128, h), ep(b, K_DYN), 3, 2, L_DET, "Detect");
    if (nsc > 0) toggle(ui, R(DW - M - 84, y, 84, h), "SC view", P_SCV, BS_PURPLE);
    float y0 = y + h + 8, x2 = M + cw + 14, x3 = M + 2 * (cw + 14);
    ui.paramRow(R(M, y0, cw, h), "Frequency", ep(b, K_FREQ), SC_LOG, 72, 84, true, KD_LOG);
    if (t <= 2) ui.paramRow(R(M, y0 + dy, cw, h), "Gain", ep(b, K_GAIN), SC_LIN, 72, 84);
    if (t >= 5) {
        label(ui, R(M, y0 + 2 * dy, 72, h), "Slope");
        R r(M + 72, y0 + 2 * dy, cw - 72, h);
        int sl = cutS(t);
        if (ui.dropdown(r, L_SLOPE[sl])) {
            std::vector<MenuItem> m;
            for (int i = 0; i < 6; ++i) m.push_back(MenuItem::item(L_SLOPE[i], i + 1, i == sl));
            bool hp = cutHp(t);
            EqPlugin *pp = &D;
            ui.menu(m, r.x, r.b() + 4, [pp, b, hp](int k) { if (k > 0) pp->setOnce(ep(b, K_TYPE), cutTy(hp, k - 1)); }, r.w);
        }
    } else {
        // Q: its mode (how much the Q follows the gain, bells and shelves), the bar, the value
        if (t <= 2) pdd(ui, R(M, y0 + 2 * dy, 68, h), ep(b, K_DSRC), 2, 4, L_QM, nullptr);
        else label(ui, R(M, y0 + 2 * dy, 68, h), "Q");
        ui.bar(R(M + 72, y0 + 2 * dy, cw - 72 - 84 - 6, h), ep(b, K_Q), SC_LOG, true, KD_LOG);
        ui.valueBox(R(M + cw - 84, y0 + 2 * dy, 84, h), ep(b, K_Q), true, KD_LOG, std::log(400.0) / 138, std::log(400.0) / 1380);
    }
    if (sdon) {
        ui.paramRow(R(x2, y0, cw, h), "Threshold", ep(b, K_THR), SC_LIN, 72, 84);
        ui.paramRow(R(x2, y0 + dy, cw, h), "Ratio", V_RAT, SC_LOG, 72, 84, true, KD_LOG);
        ui.paramRow(R(x2, y0 + 2 * dy, cw, h), "Knee", V_KNE, SC_LIN, 72, 84);
        ui.paramRow(R(x3, y0, cw, h), "Attack", ep(b, K_ATT), SC_LOG, 72, 84, true, KD_LOG);
        ui.paramRow(R(x3, y0 + dy, cw, h), "Hold", V_HLD, SC_LIN, 72, 84);
        ui.paramRow(R(x3, y0 + 2 * dy, cw, h), "Release", ep(b, K_REL), SC_LOG, 72, 84, true, KD_LOG);
    } else if (lin && t <= 2 && pkDm(D.get(ep(b, K_DYN))) > 0) {
        ui.text(R(x2, y0, 600, h), "Dynamic bands are off with linear or natural phase.", th::faint(), F_REGULAR, 11.5f);
    }
}

// ---- the whole EQ: lookahead and phase, the selected dynamic band's range, Set dynamic thresholds (at the right
// edge); then input, dry/wet, auto gain and its mode ----
void EqView::globalRows(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float h = 26, cw = 318;
    toggle(ui, R(M, y, 96, h), "Lookahead", P_LA);
    if (D.get(P_LA) >= 0.5 && lin == 0) ui.valueBox(R(M + 100, y, 70, h), P_LAMS, true, KD_OTHER, 0.1, 0.01);
    enumDD(ui, R(M + 176, y, 142, h), P_LIN, L_LIN, 8);
    int b = sel;
    bool sdon = D.get(ep(b, K_ON)) >= 0.5 && dynOn(b);
    if (sdon) ui.paramRow(R(M + cw + 14, y, cw, h), "Range", ep(b, K_RNG), SC_LIN, 72, 84);
    int tgs = av.myslot >= 0 ? (int)shm::slot(av.myslot).tgs.load() : 0;
    if (ndyn > 0 || tgs > 0) {
        R tr(DW - M - 186 - 8 - 78, y, 78, h), sr(DW - M - 186, y, 186, h);
        if (tgs > 0) {
            char s[24];
            if (tgs == 1) snprintf(s, sizeof s, "waiting");
            else if (tgs == 2) snprintf(s, sizeof s, "%.1f s", shm::slot(av.myslot).tgel.load());
            else snprintf(s, sizeof s, "working");
            ui.button(tr, s, true, true, BS_PURPLE);
        } else {
            enumDD(ui, tr, P_TGTM, L_TTIME, 7);
        }
        if (ui.button(sr, tgs > 0 ? "Cancel" : "Set dynamic thresholds", tgs > 0, av.myslot >= 0))
            D.act.send(tgs > 0 ? shm::C_TGSTOP : shm::C_TGSTART, 0);
    }
    y += h + 8;
    inputRow(ui, R(M, y, 400, h), P_IN, 72, 84);
    ui.paramRow(R(M + 414, y, 270, h), "Dry/wet", P_MIX, SC_LIN, 62, 76);
    toggle(ui, R(DW - M - 146 - 8 - 100, y, 100, h), "Auto gain", P_AUTOG, BS_NORMAL, true);
    if (D.get(P_AUTOG) >= 0.5) enumDD(ui, R(DW - M - 146, y, 146, h), P_AGM, L_AGM, 2, "Mode");
}

void EqView::frame(Ui &ui)
{
    lin = iclamp(iround(D.get(P_LIN)), 0, 7);
    int t = iclamp(iround(D.get(P_DRNG)), 0, 3);
    dr = t == 0 ? 6 : (t == 1 ? 12 : (t == 2 ? 18 : 30));
    sel = iclamp(iround(D.get(P_SEL)), 0, NB - 1);
    prep();
    bool scv = D.get(P_SCV) >= 0.5 && nsc > 0;
    DH = BASE_H + (scv ? SCV_H : 0);
    double sr = D.sampleRate() > 0 ? D.sampleRate() : 48000;
    int lat = latency(lin, D.get(P_LA) >= 0.5, D.get(P_LAMS), sr);
    R gear = begin(ui, lat / sr * 1000, D.view.side.load() != 0,
                   lat > 0 ? (lin ? (lin >= 5 ? "natural phase" : "linear phase") : "lookahead") : "");
    float y = 66;
    const float dh = 250, metW = 92;
    display(ui, R(M, y, DW - 2 * M - metW - 6, dh));
    meters(ui, R(DW - M - metW, y, metW, dh));
    y += dh + 8;
    if (scv) {
        scView(ui, R(M, y, DW - 2 * M, 110));
        y += 118;
    }
    bandsRow(ui, y);
    y += 26 + 10;
    bandSection(ui, y);
    y += 26 + 8 + 3 * 32 + 10;
    globalRows(ui, y);
    y += 26 + 8 + 26 + 12;
    actCardAt(ui, y);
    finish(ui, gear, [this](Ui &u, const R &in, float yy) {
        label(u, R(in.x, yy, 220, 24), "Set dynamic thresholds: full range");
        u.valueBox(R(in.x + 224, yy, 150, 24), P_DTP, true, KD_OTHER, 0.02, 0.002);
        u.textFit(R(in.x + 384, yy, in.w - 384, 24), "on the loudest moments: compress bands reach their range, expand bands are back to 0", th::faint(),
                  F_REGULAR, 11);
        return yy + 24 + 10;
    });
}

// ---------------------------------------------------------------- the kind
static const char *s_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_EQUALIZER, CLAP_PLUGIN_FEATURE_FILTER,
                                   CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const PluginKind &kind()
{
    static const PluginKind k = {
        {CLAP_VERSION_INIT, "com.twostorms.eq", "TwoStorms EQ", "TwoStorms", "", "", "", TS_VERSION,
         "A 16-band parametric EQ with an Act built in: matched analog-shaped filters that glide, stereo / left / right / mid / side, "
         "dynamic bands (compress or expand, with a sidechain), auto gain, solo and delta. Zero latency, or linear / natural phase or lookahead.",
         s_features},
        &table,
        &factoryPresets,
        "EQ",
        "TWOSTORMS EQ",
        shm::KIND_POINT,
        shm::SK_EQ,
        true,
        true,
        P_AB,
        &create,
        2,
        false,
    };
    return k;
}

} // namespace peq
} // namespace ts
