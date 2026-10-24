// TwoStorms Wah: one to three wah bands (state-variable bandpass, lowpass or highpass, 12 or 24 dB/oct), each swept
// between its own two frequencies by a MIDI pedal (or the Pedal knob), the input's envelope, its transients or an LFO;
// in parallel or in series, with drive, and Voice: the bands become a voice's formants (a fake talk box). "Set
// thresholds" listens and sets the Envelope and Transient bands (also from the Gain Stage). No latency; MIDI passes
// through. Port of twostorms_wah.jsfx.
#include "wah_plugin.h"
#include "../../core/eelrand.h"
#include "../../gui/effect_view.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

namespace ts {
namespace wah {

using namespace gui;

enum { P_MIX = 51, P_EDIT = 52, P_C12 = 53, P_C23 = 54, P_VOICE = 55, P_VPATH = 56, P_VSHIFT = 57, P_B = 58, PBS = 31,
       P_TTIME = 151, P_DTP = 152, P_AB = 153, P_SHOWPED = 154 /* the window: the pedal's controls shown */,
       P_ON0 = 155 /* bands 1, 2, 3 on (155, 156, 157) */ };
enum {
    K_SRC = 0, K_POS, K_FMIN, K_FMAX, K_Q, K_BOOST, K_FTYPE, K_DIR, K_SMOOTH, K_THR, K_SPAN, K_ATT, K_REL, K_TSENS, K_DET, K_HPF,
    K_MCC, K_MCH, K_LEARN, K_LRATE, K_LSYNC, K_LSHAPE, K_LDEPTH, K_LPHASE, K_QTOE, K_DRIVE, K_SLOPE, K_LSTEREO, K_PMIN, K_PMAX, K_PCURVE
};
static inline int bp(int b, int k) { return P_B + PBS * b + k; }
// the Gain Stage's Set row element for the listening time (the wah keeps it at P_TTIME)
constexpr int GS_TTIME = 71;

// ---------------------------------------------------------------- parameters
static void f_pct(double v, char *b, int n) { snprintf(b, n, "%.0f%%", v); }
static void f_dtp(double v, char *b, int n) { snprintf(b, n, "%.1f%% of the time", v); }
static void f_st(double v, char *b, int n) { snprintf(b, n, "%+.1f st", v); }
static void f_hz(double v, char *b, int n)
{
    if (v >= 1000) snprintf(b, n, "%.2f kHz", v / 1000);
    else snprintf(b, n, "%.0f Hz", v);
}
static void f_q(double v, char *b, int n) { snprintf(b, n, "%.2f", v); }
static void f_dbs(double v, char *b, int n) { snprintf(b, n, "%+.1f dB", v); }
static void f_db(double v, char *b, int n) { snprintf(b, n, "%.1f dB", v); }
static void f_att(double v, char *b, int n) { snprintf(b, n, v < 1 ? "%.2f ms" : (v < 10 ? "%.1f ms" : "%.0f ms"), v); }
static void f_ms0(double v, char *b, int n) { snprintf(b, n, "%.0f ms", v); }
static void f_rel(double v, char *b, int n)
{
    if (v < 1000) snprintf(b, n, "%.0f ms", v);
    else snprintf(b, n, "%.2f s", v / 1000);
}
static void f_mcc(double v, char *b, int n)
{
    if (v >= 127.5) snprintf(b, n, "Pitch bend");
    else snprintf(b, n, "CC %d", (int)std::floor(v + 0.5));
}
static void f_mch(double v, char *b, int n)
{
    if (v < 0.5) snprintf(b, n, "all");
    else snprintf(b, n, "%d", (int)std::floor(v + 0.5));
}
static void f_hpf(double v, char *b, int n)
{
    if (v <= 20.5) snprintf(b, n, "off");
    else snprintf(b, n, "%.0f Hz", v);
}
static void f_rate(double v, char *b, int n) { snprintf(b, n, "%.2f Hz", v); }
static void f_spct(double v, char *b, int n) { snprintf(b, n, "%+.0f%%", v); }
static void f_deg(double v, char *b, int n) { snprintf(b, n, "%.0f deg", v); }

static const char *const L_CON[] = {"Parallel", "Series"};
static const char *const L_VOICE[] = {"Off", "Male", "Female", "Child"};
static const char *const L_VPATH[] = {"oo-ah (wah)", "oo-ah-oo (wow)", "ee-eh-ae (yeah)", "oo-ah-ee", "oo oh ah eh ee",
                                      "ee-oo (you)", "ah-ee (eye)", "oo-ee (we)"};
constexpr int NVPATH = 8;
static const char *const L_SRC[] = {"Manual (MIDI)", "Envelope", "Transient", "LFO"};
static const char *const L_FT[] = {"Band pass", "Low pass", "High pass"};
static const char *const L_DIR[] = {"Up", "Down"};
static const char *const L_DET[] = {"Main input", "Sidechain"};
static const char *const L_LSYNC[] = {"Free (Hz)", "4 bars", "2 bars", "1 bar", "1/2", "1/4 dotted", "1/4", "1/4 triplet", "1/8 dotted", "1/8", "1/8 triplet", "1/16"};
static const char *const L_LSHAPE[] = {"Sine", "Triangle", "Ramp up", "Ramp down", "Square", "Random"};
static const char *const L_SLOPE[] = {"12 dB/oct", "24 dB/oct"};
static const char *const L_TTIME[] = {"1 s", "2 s", "3 s", "5 s", "10 s", "20 s", "30 s"};
static const char *const L_VOWEL[] = {"oo", "oh", "aw", "ah", "uh", "eh", "ae", "ee"};

template <size_t N> static std::vector<const char *> labels(const char *const (&a)[N]) { return std::vector<const char *>(a, a + N); }

static const ParamTable &table()
{
    static std::deque<std::string> names;
    static ParamTable t = [] {
        ParamTable t;
        auto nm = [](const std::string &s) { names.push_back(s); return names.back().c_str(); };
        addActParams(t, true);
        const char *M = "Wah";
        const uint32_t H = PF_HOST | PF_AB;
        for (int b = 0; b < 3; ++b) {
            static const char *const on[3] = {"Band 1 on", "Band 2 on", "Band 3 on"};
            t.addEnum(P_ON0 + b, on[b], {"Off", "On"}, b == 0 ? 1 : 0, H | PF_BOOL, M);
        }
        t.add(P_MIX, "Mix", 0, 100, 100, H | PF_INT, f_pct, M).unit = "%";
        t.add(P_EDIT, "Edit band", 0, 2, 0, PF_INT, nullptr, M);
        t.addEnum(P_C12, "Bands 1 and 2", labels(L_CON), 0, H, M);
        t.addEnum(P_C23, "Bands 2 and 3", labels(L_CON), 0, H, M);
        t.addEnum(P_VOICE, "Voice", labels(L_VOICE), 0, H, M);
        t.addEnum(P_VPATH, "Vowel path", labels(L_VPATH), 0, H, M);
        t.add(P_VSHIFT, "Formant shift", -12, 12, 0, H, f_st, M);
        static const double FMIN[3] = {350, 800, 1500}, FMAX[3] = {2200, 4000, 7000}, BOOST[3] = {6, 3, 3};
        for (int b = 0; b < 3; ++b) {
            std::string L = "Band " + std::to_string(b + 1) + " ";
            const char *mod = nm("Band " + std::to_string(b + 1));
            auto n = [&](const char *s) { return nm(L + s); };
            t.addEnum(bp(b, K_SRC), n("control"), labels(L_SRC), 0, H, mod);
            t.add(bp(b, K_POS), n("pedal position"), 0, 100, 50, H, f_pct, mod).unit = "%";
            t.add(bp(b, K_FMIN), n("lowest frequency"), 40, 4000, FMIN[b], H | PF_INT, f_hz, mod).unit = "Hz";
            t.add(bp(b, K_FMAX), n("highest frequency"), 200, 8000, FMAX[b], H | PF_INT, f_hz, mod).unit = "Hz";
            t.add(bp(b, K_Q), n("Q at the heel"), 0.5, 15, 5, H, f_q, mod);
            t.add(bp(b, K_BOOST), n("level"), -12, 18, BOOST[b], H, f_dbs, mod).unit = "dB";
            t.addEnum(bp(b, K_FTYPE), n("filter"), labels(L_FT), 0, H, mod);
            t.addEnum(bp(b, K_DIR), n("sweep direction"), labels(L_DIR), 0, H, mod);
            t.add(bp(b, K_SMOOTH), n("pedal smoothing"), 1, 200, 10, H, f_ms0, mod).unit = "ms";
            t.add(bp(b, K_THR), n("threshold"), -80, 0, -40, H, f_db, mod).unit = "dB";
            t.add(bp(b, K_SPAN), n("envelope trigger"), 6, 60, 24, H, f_dbs, mod).unit = "dB";
            t.add(bp(b, K_ATT), n("attack"), 0.1, 100, 5, H, f_att, mod).unit = "ms";
            t.add(bp(b, K_REL), n("release"), 5, 2000, 150, H | PF_INT, f_rel, mod).unit = "ms";
            t.add(bp(b, K_TSENS), n("transient trigger"), 3, 30, 12, H, f_db, mod).unit = "dB";
            t.addEnum(bp(b, K_DET), n("detector input"), labels(L_DET), 0, H, mod);
            t.add(bp(b, K_HPF), n("detector high-pass"), 20, 2000, 20, H | PF_INT, f_hpf, mod).unit = "Hz";
            t.add(bp(b, K_MCC), n("MIDI controller"), 0, 128, 11, PF_HOST | PF_INT, f_mcc, mod);
            t.add(bp(b, K_MCH), n("MIDI channel"), 0, 16, 0, PF_HOST | PF_INT, f_mch, mod);
            t.addEnum(bp(b, K_LEARN), n("MIDI learn"), {"Off", "On"}, 0, PF_HOST | PF_BOOL, mod);
            t.add(bp(b, K_LRATE), n("LFO rate"), 0.05, 20, 1, H, f_rate, mod).unit = "Hz";
            t.addEnum(bp(b, K_LSYNC), n("LFO sync"), labels(L_LSYNC), 0, H, mod);
            t.addEnum(bp(b, K_LSHAPE), n("LFO shape"), labels(L_LSHAPE), 0, H, mod);
            t.add(bp(b, K_LDEPTH), n("LFO depth"), 0, 100, 100, H | PF_INT, f_pct, mod).unit = "%";
            t.add(bp(b, K_LPHASE), n("LFO phase"), 0, 360, b * 120, H | PF_INT, f_deg, mod).unit = "deg";
            t.add(bp(b, K_QTOE), n("Q at the toe"), 0.5, 15, 5, H, f_q, mod);
            t.add(bp(b, K_DRIVE), n("drive"), 0, 30, 0, H, f_db, mod).unit = "dB";
            t.addEnum(bp(b, K_SLOPE), n("filter slope"), labels(L_SLOPE), 0, H, mod);
            t.add(bp(b, K_LSTEREO), n("LFO stereo offset"), 0, 180, 0, H | PF_INT, f_deg, mod).unit = "deg";
            t.add(bp(b, K_PMIN), n("pedal range heel"), 0, 100, 0, H, f_pct, mod).unit = "%";
            t.add(bp(b, K_PMAX), n("pedal range toe"), 0, 100, 100, H, f_pct, mod).unit = "%";
            t.add(bp(b, K_PCURVE), n("pedal curve"), -100, 100, 0, H | PF_INT, f_spct, mod).unit = "%";
        }
        t.addEnum(P_TTIME, "Set thresholds: listening time", labels(L_TTIME), 3, H, M);
        t.add(P_DTP, "Set thresholds: full range", 0.1, 50, 4.2, H, f_dtp, M).unit = "%";
        t.addEnum(P_AB, "A/B set", {"A", "B"}, 0, PF_HOST, M);
        t.add(P_SHOWPED, "Show the pedal controls", 0, 1, 1, PF_INT | PF_BOOL, nullptr, M);
        return t;
    }();
    return t;
}

// a talk-box preset: Voice (1 male, 2 female, 3 child), its vowel path, every band on the same control (src) with
// the same settings (each "K=v" a band parameter, K as in K_*), then the rest
static std::string tb(int voice, int path, int src, const char *band, const char *rest)
{
    char h[96];
    snprintf(h, sizeof h, "155=1 156=1 157=1 51=100 53=1 54=1 55=%d 56=%d ", voice, path);   // three bands in series: one vocal tract
    std::string s = h;
    for (int b = 0; b < 3; ++b) {
        snprintf(h, sizeof h, "%d=%d %d=6 ", bp(b, K_SRC), src, bp(b, K_Q));
        s += h;
        const char *c = band;
        while (*c) {
            int k = 0;
            double v = 0;
            int used = 0;
            if (sscanf(c, "%d=%lf%n", &k, &v, &used) != 2) break;
            snprintf(h, sizeof h, "%d=%g ", bp(b, k), v);
            s += h;
            c += used;
            while (*c == ' ') ++c;
        }
    }
    return s + rest;
}

// a wah preset written band by band (sources as L_SRC: 0 pedal / MIDI, 1 envelope, 2 transient, 3 LFO)
namespace {
struct WahP {
    std::vector<std::pair<int, double>> v;
    WahP &nb(int n, int c12 = 0, int c23 = 0)
    {
        for (int b = 0; b < 3; ++b) v.push_back({P_ON0 + b, b < n ? 1.0 : 0.0});
        v.push_back({P_C12, (double)c12}); v.push_back({P_C23, (double)c23});
        return *this;
    }
    WahP &band(int b, int src, double fmin, double fmax, double q, double level, double qtoe = -1)
    {
        v.push_back({bp(b, K_SRC), (double)src}); v.push_back({bp(b, K_FMIN), fmin}); v.push_back({bp(b, K_FMAX), fmax});
        v.push_back({bp(b, K_Q), q}); v.push_back({bp(b, K_BOOST), level}); v.push_back({bp(b, K_QTOE), qtoe < 0 ? q : qtoe});
        return *this;
    }
    WahP &env(int b, double thr, double span, double att, double rel) { v.push_back({bp(b, K_THR), thr}); v.push_back({bp(b, K_SPAN), span}); v.push_back({bp(b, K_ATT), att}); v.push_back({bp(b, K_REL), rel}); return *this; }
    WahP &trans(int b, double thr, double att, double rel, double sens) { v.push_back({bp(b, K_THR), thr}); v.push_back({bp(b, K_ATT), att}); v.push_back({bp(b, K_REL), rel}); v.push_back({bp(b, K_TSENS), sens}); return *this; }
    // LFO: sync as L_LSYNC (0 free at rate Hz), shape as L_LSHAPE, depth, phase, stereo offset
    WahP &lfo(int b, int sync, double rate, int shape, double depth, double phase, double stereo = 0)
    {
        v.push_back({bp(b, K_LSYNC), (double)sync}); v.push_back({bp(b, K_LRATE), rate}); v.push_back({bp(b, K_LSHAPE), (double)shape});
        v.push_back({bp(b, K_LDEPTH), depth}); v.push_back({bp(b, K_LPHASE), phase}); v.push_back({bp(b, K_LSTEREO), stereo});
        return *this;
    }
    WahP &drive(int b, double db) { v.push_back({bp(b, K_DRIVE), db}); return *this; }
    WahP &steep(int b) { v.push_back({bp(b, K_SLOPE), 1}); return *this; }
    WahP &dir(int b, int d) { v.push_back({bp(b, K_DIR), (double)d}); return *this; }
    WahP &ft(int b, int t) { v.push_back({bp(b, K_FTYPE), (double)t}); return *this; }
    WahP &set(int id, double x) { v.push_back({id, x}); return *this; }
    operator std::string() const { return presetStr(v); }
};
WahP W() { return WahP(); }
}

static const std::vector<FactoryPreset> &factoryPresets()
{
    // talk box: a distorted guitar (or a synth's saw) into a mouth: the first band's drive for the harmonics, formants a
    // little sharper than spoken (Q 6.5: throatier, more focused) and a little lower (less squeaky), in series (one
    // vocal tract); the mouth moves with the pedal, the playing (envelope, transient) or the tempo (LFO)
    static std::vector<FactoryPreset> v = {
        // ---- guitar
        {"Instruments/Guitar/Clean: envelope quack", "155=1 156=0 157=0 58=1 60=300 61=2000 62=5 63=5 67=-40 68=24 69=4 70=160 82=5"},
        {"Instruments/Guitar/Clean: slow LFO shimmer", "155=1 156=0 157=0 58=3 60=500 61=1800 62=3 63=3 77=0.6 82=3 85=90"},
        {"Instruments/Guitar/Clean: double wah (two bands, parallel)", W().nb(2).band(0, 0, 350, 2000, 5, 4).band(1, 0, 900, 4500, 4, 2)},
        {"Instruments/Guitar/Funk: tight envelope (transient)", "155=1 156=0 157=0 58=2 60=350 61=2500 62=6 63=6 67=-45 69=1 70=150 71=10 82=6"},
        {"Instruments/Guitar/Funk: 16th auto-wah (synced)", "155=1 156=0 157=0 58=3 60=400 61=2200 62=5 63=6 78=11 79=0 82=6"},
        {"Instruments/Guitar/Funk: two-band quack", "155=1 156=1 157=0 58=1 60=300 61=1800 62=5 63=5 67=-42 68=24 69=3 70=140 82=5 89=1 91=900 92=3500 93=5 94=2 98=-42 99=24 100=3 101=140 113=5"},
        {"Instruments/Guitar/Funk: vocal envelope (two bands in series)", W().nb(2, 1).band(0, 1, 300, 1600, 4, 4).env(0, -42, 24, 3, 150).band(1, 1, 900, 3000, 4, 0).env(1, -42, 24, 3, 150)},
        {"Instruments/Guitar/Rock: classic pedal wah", "155=1 156=0 157=0 58=0 60=400 61=2200 62=5 63=6 82=6.5"},
        {"Instruments/Guitar/Rock: half-cocked wah (parked)", "155=1 156=0 157=0 58=0 59=35 60=400 61=2200 62=6 63=6 82=6"},
        {"Instruments/Guitar/Rock: fuzz wah", "155=1 156=0 157=0 58=0 60=350 61=2400 62=6 63=4 82=8 83=18"},
        {"Instruments/Guitar/Rock: triple wah (three bands, parallel)", W().nb(3).band(0, 0, 350, 2000, 5, 4).band(1, 0, 700, 3500, 5, 2).band(2, 0, 1400, 6000, 4, 1)},
        {"Instruments/Guitar/Metal: aggressive wah (24 dB)", "155=1 156=0 157=0 58=0 60=350 61=2400 62=4 63=7 82=9 84=1"},
        {"Instruments/Guitar/Metal: parked wah (60%)", "155=1 156=0 157=0 58=0 59=60 60=400 61=2200 62=7 63=4 82=7"},
        {"Instruments/Guitar/Metal: envelope snarl", "155=1 156=0 157=0 58=1 60=500 61=2800 62=6 63=4 67=-35 68=20 69=2 70=120 82=8 84=1"},
        {"Instruments/Guitar/Metal: triple-band snarl (series, 24 dB)", W().nb(3, 1, 1).band(0, 0, 400, 2400, 3, 6).steep(0).band(1, 0, 800, 3600, 3, 0).band(2, 0, 1500, 6000, 3, 0)},
        // talk box on guitar
        {"Instruments/Guitar/Talk box: 80s anthem 'wow' (pedal)", tb(1, 1, 0, "4=6.5", "57=-1 83=15")},
        {"Instruments/Guitar/Talk box: 70s 'wah-wah' lead (envelope)", tb(1, 0, 1, "4=6.5 9=-42 10=24 11=6 12=220", "57=-1 83=14")},
        {"Instruments/Guitar/Talk box: 'yeah yeah' (pedal)", tb(1, 2, 0, "4=6.5", "57=-1 83=15")},
        {"Instruments/Guitar/Talk box: 'why' (envelope)", tb(1, 3, 1, "4=6.5 9=-42 10=24 11=5 12=200", "57=-1 83=14")},
        {"Instruments/Guitar/Talk box: picked vowels (transient)", tb(1, 0, 2, "4=6.5 9=-48 11=2 12=200 13=10", "57=-1 83=14")},
        {"Instruments/Guitar/Talk box: 'you' (pedal)", tb(1, 5, 0, "4=6.5", "57=-1 83=14")},
        {"Instruments/Guitar/Talk box: slow vowel sweep (LFO)", tb(1, 4, 3, "4=6.5 19=0.25 20=0 21=0 23=0", "57=-1 83=14")},
        // ---- bass
        {"Instruments/Bass/Clean: envelope filter", "155=1 156=0 157=0 51=70 58=1 60=180 61=1400 62=4 63=4 67=-45 68=24 69=5 70=200 82=5"},
        {"Instruments/Bass/Distorted: pedal wah", "155=1 156=0 157=0 58=0 60=250 61=1600 62=5 63=5 82=6"},
        {"Instruments/Bass/Slap: funk envelope", "155=1 156=0 157=0 51=80 58=2 60=200 61=2000 62=5 63=4 67=-50 70=180 71=10 82=6"},
        {"Instruments/Bass/Punk", "155=1 156=0 157=0 58=0 60=300 61=1800 62=5 63=6 82=6 83=10"},
        {"Instruments/Bass/Pop punk", "155=1 156=0 157=0 51=80 58=1 60=250 61=1800 62=4 63=4 67=-45 68=24 70=150 82=5"},
        {"Instruments/Bass/Metal", "155=1 156=0 157=0 58=0 60=300 61=2200 62=6 63=4 82=8 83=20"},
        {"Instruments/Bass/Rock", "155=1 156=0 157=0 58=0 60=300 61=2000 62=5 63=6 82=6.5"},
        {"Instruments/Bass/Jazz", "155=1 156=0 157=0 51=60 58=3 60=250 61=1200 62=3 63=3 77=0.3 80=60 82=3.5"},
        {"Instruments/Bass/Pop", "155=1 156=0 157=0 51=70 58=3 60=300 61=1800 62=4 63=4 78=9 80=100 82=5"},
        {"Instruments/Bass/Double envelope (lows and growl)", W().nb(2).set(P_MIX, 80).band(0, 1, 150, 900, 4, 3).env(0, -45, 24, 5, 200).band(1, 1, 600, 2500, 4, 2).env(1, -45, 24, 5, 200)},
        {"Instruments/Bass/Talk box: synth-bass 'wow' (envelope)", tb(1, 1, 1, "4=6.5 9=-40 10=20 11=3 12=160", "57=-3 83=10")},
        {"Instruments/Bass/Talk box: 'yeah' (transient)", tb(1, 2, 2, "4=6.5 9=-46 11=1.5 12=220 13=9", "57=-3 83=10")},
        // ---- keys and synths
        {"Instruments/Keys & synths/Clavinet: envelope funk", W().nb(1).band(0, 1, 400, 2600, 5, 5).env(0, -42, 24, 2, 120)},
        {"Instruments/Keys & synths/Synth: 1/8 stereo auto-wah", "155=1 156=0 157=0 58=3 60=350 61=2200 62=5 63=6 78=9 79=1 82=6 85=90"},
        {"Instruments/Keys & synths/Synth: triple band sweep (120 deg apart)", W().nb(3).band(0, 3, 300, 1800, 5, 3).lfo(0, 3, 1, 0, 100, 0).band(1, 3, 600, 3500, 5, 2).lfo(1, 3, 1, 0, 100, 120)
                                                                                 .band(2, 3, 1200, 6000, 5, 1).lfo(2, 3, 1, 0, 100, 240)},
        {"Instruments/Keys & synths/Talk box: funk 'wow' 1/8 (synced)", tb(1, 1, 3, "4=6.5 20=9 21=0 23=0", "57=-2")},
        {"Instruments/Keys & synths/Talk box: 16th talk (synced)", tb(1, 3, 3, "4=6.5 20=11 21=1 23=0", "57=-2")},
        {"Instruments/Keys & synths/Talk box: R&B lead, low formants (envelope)", tb(1, 1, 1, "4=7 9=-40 10=20 11=3 12=160", "57=-2 63=3")},
        {"Instruments/Keys & synths/Talk box: 'eye' (envelope)", tb(1, 6, 1, "4=6.5 9=-42 10=22 11=5 12=180", "57=-2")},
        {"Instruments/Keys & synths/Talk box: 'we' (transient)", tb(1, 7, 2, "4=6.5 9=-48 11=2 12=180 13=10", "57=-2")},
        {"Instruments/Keys & synths/Talk box: female (pedal)", tb(2, 0, 0, "4=6.5", "")},
        // ---- drums
        {"Instruments/Drums/Talking drums (transient, child voice)", tb(3, 1, 2, "4=5 9=-40 11=1 12=150 13=8", "")},
        {"Instruments/Drums/Hi-hats: envelope sweep", W().nb(1).band(0, 1, 2000, 8000, 3, 3).env(0, -40, 24, 1, 100)},
        // ---- vocals
        {"Instruments/Vocals/Envelope wah on a voice", W().nb(1).set(P_MIX, 60).band(0, 1, 400, 2500, 4, 3).env(0, -40, 24, 5, 200)},
        // ---- buses
        {"Buses/Instrument bus/Drum bus: filtered breakdown (parked low)", W().nb(1).band(0, 0, 300, 3000, 2, 3).ft(0, 1).set(bp(0, K_POS), 20)},
        {"Buses/Mixbus/Filter sweep (pedal: low pass)", W().nb(1).band(0, 0, 200, 8000, 1, 0).ft(0, 1)},
        {"Buses/Master/Slow LFO sweep (low pass, subtle)", W().nb(1).set(P_MIX, 40).band(0, 3, 800, 8000, 1, 0).ft(0, 1).lfo(0, 1, 1, 0, 100, 0)},
        // ---- creative
        {"Creative/Opposite bands (pedal)", "155=1 156=1 157=0 53=0 58=0 60=350 61=2000 62=5 63=5 82=5 89=0 91=1200 92=5000 93=5 94=3 96=1 113=5"},
        {"Creative/Formants side by side (parallel voice, pedal)", tb(1, 1, 0, "4=6", "57=-1 83=12 53=0 54=0")},
        {"Creative/Child robot 'wow' (envelope)", tb(3, 1, 1, "4=6 9=-42 10=24 11=5 12=200", "83=12")},
        {"Creative/Vowel sequencer 1/16 (random)", tb(1, 4, 3, "4=6.5 20=11 21=5 23=0", "57=-1")},
        {"Creative/Stereo wah (LFO, 90 deg offset)", W().nb(1).band(0, 3, 350, 2200, 5, 5).lfo(0, 0, 0.5, 0, 100, 0, 90)},
        // ---- weird
        {"Weird/Hollow voice (high pass formants)", W().nb(3, 1, 1).band(0, 0, 300, 2000, 6, 3).ft(0, 2).band(1, 0, 600, 3000, 6, 0).ft(1, 2).band(2, 0, 1200, 5000, 6, 0).ft(2, 2)},
        {"Weird/Screaming resonance (Q 15)", W().nb(1).band(0, 3, 300, 4000, 15, 0).lfo(0, 9, 1, 1, 100, 0)},
        {"Weird/Down is up (reverse sweep, envelope)", W().nb(1).band(0, 1, 300, 2500, 6, 5).env(0, -42, 24, 3, 200).dir(0, 1)},
        {"Weird/Giant mouth (formants an octave down)", tb(1, 1, 0, "4=7", "57=-12 63=6")},
    };
    return v;
}

// ---------------------------------------------------------------- shared tables
// LFO cycles per beat for each sync choice (4/4 bars)
static const double WSYNC[12] = {0, 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 2.0 / 3, 1, 3.0 / 2, 4.0 / 3, 2, 3, 4};
// Voice: the bands become the first formants of a vocal tract, one after the other (a cascade of resonances, as in
// formant synthesis: each vowel's formants then have their natural levels), with a fixed fourth formant and the
// lips' radiation. The vowels (oo oh aw ah uh eh ae ee): the first three formants (Hz) of a man, a woman and a child
// (Hillenbrand et al. 1995; "oo" a little rounder, as a talk box's), the fourth, and the bandwidths.
static const double WVF[3][3][8] = {
    {{330, 497, 652, 768, 623, 580, 588, 342}, {900, 910, 997, 1333, 1200, 1799, 1952, 2322}, {2300, 2459, 2538, 2522, 2550, 2605, 2601, 3000}},
    {{410, 555, 781, 936, 753, 731, 669, 437}, {980, 1035, 1136, 1551, 1426, 2058, 2349, 2761}, {2700, 2828, 2824, 2815, 2933, 2979, 2972, 3372}},
    {{450, 597, 803, 1002, 749, 749, 717, 452}, {1150, 1137, 1210, 1688, 1546, 2267, 2501, 3081}, {2950, 2987, 2982, 2950, 3145, 3310, 3289, 3702}},
};
static const double WVF4[3] = {3400, 4000, 4300};
static const double WVBW[3][4] = {{90, 110, 170, 260}, {100, 125, 190, 290}, {110, 140, 210, 310}};
// the vowel paths: how many vowels, and which
static const int WPN[NVPATH] = {2, 3, 3, 3, 5, 2, 2, 2};
static const int WPI[NVPATH * 8] = {0, 3, 0, 0, 0, 0, 0, 0,  0, 3, 0, 0, 0, 0, 0, 0,  7, 5, 6, 0, 0, 0, 0, 0,  0, 3, 7, 0, 0, 0, 0, 0,
                                    0, 1, 3, 5, 7, 0, 0, 0,  7, 0, 0, 0, 0, 0, 0, 0,  3, 7, 0, 0, 0, 0, 0, 0,  0, 7, 0, 0, 0, 0, 0, 0};

// the formant table for a voice setting: WFL[band * 8 + point] = log of that band's formant at each point of the path;
// bw: each formant's bandwidth, f4 the fixed fourth formant
struct VoiceTab {
    double WFL[24] = {};
    double bw[4] = {90, 110, 170, 260}, f4 = 3400;
    int vpath = 0;
    bool von = false;
    void build(double voice, double vpath_, double vshift)
    {
        int v = iclamp((int)std::floor(voice + 0.5), 0, 3);
        von = v > 0;
        vpath = iclamp((int)std::floor(vpath_ + 0.5), 0, NVPATH - 1);
        int vi = std::max(0, v - 1);
        double sc = epow(2, vshift / 12);
        int n = WPN[vpath];
        for (int b = 0; b < 3; ++b)
            for (int i = 0; i < n; ++i) WFL[b * 8 + i] = std::log(WVF[vi][b][WPI[vpath * 8 + i]] * sc);
        for (int b = 0; b < 4; ++b) bw[b] = WVBW[vi][b] * sc;
        f4 = WVF4[vi] * sc;
    }
    // band b's formant frequency (Hz) at position p along the path
    double form(int b, double p) const
    {
        int n = WPN[vpath];
        double t = emax(0, emin(n - 1.000001, p * (n - 1))), fi = std::floor(t), fr = t - fi;
        int i = (int)fi;
        return std::exp(WFL[b * 8 + i] + (WFL[b * 8 + i + 1] - WFL[b * 8 + i]) * fr);
    }
};

// ---------------------------------------------------------------- the engine
// the bands' memory: WSTR numbers each, laid out by these offsets
enum {
    F_MODE = 0, F_DIR = 1, F_FT = 2, F_FMIN = 3, F_LR = 4, F_QH = 5, F_LQ = 6, F_BOOST = 7, F_THR = 8, F_SPAN = 9, F_EA = 10, F_ER = 11,
    F_TSENS = 12, F_TA = 13, F_TD = 14, F_SM = 15, F_SRC = 16, F_HPON = 17, F_B0 = 18, F_B1 = 19, F_A1C = 20, F_A2C = 21, F_LINC = 22,
    F_LSH = 23, F_LDEP = 24, F_LPHO = 25, F_SYNC = 26, F_CPB = 27, F_MCC = 28, F_MCH = 29, F_LEARN = 30, F_BIDX = 31, F_DRV = 32,
    F_DRVON = 33, F_SLOPE = 34, F_LST = 35, F_PMIN = 36, F_PMAX = 37, F_GAM = 38, F_VON = 39, F_MT = 40, F_PM = 41, F_POS = 42,
    F_ENV = 43, F_EF = 44, F_ES = 45, F_ED = 46, F_SD = 47, F_TP = 48, F_USEM = 49, F_LKNOB = 50, F_LCC = 51, F_LVAL = 52,
    F_MTIME = 53, F_BS = 54, F_LPH = 55, F_LR1 = 56, F_LR2 = 57, F_LS = 58, F_LSR = 59, F_INIT = 60, F_KMAX = 61, F_TL = 62,
    F_TR = 68, F_IC = 74, F_BZ = 82, WSTR = 88
};
constexpr int HN = 1024;   // history entries per band (10 ms each)

struct Display {
    std::atomic<float> hk[3 * HN], he[3 * HN], hs[3 * HN], hpos[3 * HN];   // the history: key level, detector, slow follower, position
    std::atomic<int> hp{0};
    std::atomic<float> pos[3], fc[3], lval[3];
    std::atomic<int> usem[3], lcc[3];
    std::atomic<double> mtime[3];
    std::atomic<int> side{0}, anyk{0};
    Display()
    {
        for (int i = 0; i < 3 * HN; ++i) { hk[i] = -200; he[i] = -200; hs[i] = -200; hpos[i] = 0; }
        for (int b = 0; b < 3; ++b) { pos[b] = 0.5f; fc[b] = 1000; lval[b] = 0; usem[b] = 0; lcc[b] = -1; mtime[b] = -100; }
    }
};

class EngineHost {
public:
    virtual ~EngineHost() {}
    virtual double pv(int id) const = 0;
    virtual double abv(int id) const = 0;
    virtual void setAudio(int id, double v) = 0;   // a_setsl: set a parameter from the audio thread (and tell the host)
};

class Engine {
public:
    Engine(EngineHost &h, Act &a, UiLink &u, Display &d) : H(h), A(a), U(u), V(d) {}
    AbMatch ab;
    bool anyk() const { return w_anyk; }
    void tgArm() { w_tgw = -200; }

    void init(double sr)
    {
        srate = sr;
        if (!nonceDrawn) { eel_rand(1000000); nonceDrawn = true; }
        memset(WB, 0, sizeof WB);
        for (int b = 0; b < 3; ++b) {
            double *o = WB + b * WSTR;
            o[F_LKNOB] = -1; o[F_TL + 3] = -1; o[F_TR + 3] = -1; o[F_ED] = -200; o[F_SD] = -200;
            o[F_TL + 4] = 1000; o[F_TR + 4] = 1000; o[F_LCC] = -1; o[F_MTIME] = -100; o[F_MT] = 0.5;
            o[F_TL] = 0.5; o[F_TR] = 0.5; o[F_BIDX] = b;
        }
        memset(WRT, 0, sizeof WRT);
        TGLV.assign(6 * 30000, 0.0);
        for (int i = 0; i < 6; ++i) TGMX[i] = 0;
        c_tgn = 0; c_tc = 0; c_tms = 48; c_tgdur = 3000; c_tgi = 0; w_tgw = -200; w_td = -100; w_anyk = false;
        w_nb = 1; w_nev = 0; w_ep = 0; w_sidx = 0; w_hp = 0; w_hcnt = 0; w_uion = false; w_mixs = 1; w_mixt = 1; w_inited = false;
        w_lastbeat = -1; w_y0 = 0; w_y1 = 0; w_i0 = 0; w_i1 = 0; w_d0 = 0; w_d1 = 0;
        w_lvp = 0; w_lvv = 0; w_lvs = 0;
        w_h10 = std::max(1.0, std::floor(srate * 0.01 + 0.5));
    }
    void reset()
    {
        for (int b = 0; b < 3; ++b) {
            double *o = WB + b * WSTR;
            for (int i = 0; i < 8; ++i) o[F_IC + i] = 0;
            for (int i = 0; i < 4; ++i) o[F_BZ + i] = 0;
        }
        memset(VS, 0, sizeof VS);
        memset(VR, 0, sizeof VR);
    }

    // @block, the wah's part (before the Act's)
    void block(int samplesblock, const Transport &tr, const std::vector<MidiEv> &midi)
    {
        double a_now = A.now();
        ab.block(U, a_now, H.pv(P_AB) >= 0.5, samplesblock, srate);
        V.side.store(ab.side ? 1 : 0, std::memory_order_relaxed);
        w_uion = ab.uion;
        // the bands that play: any of the three (w_nb: up to the last one on)
        w_nb = 0;
        for (int b = 0; b < 3; ++b) {
            WON[b] = cpv(P_ON0 + b) >= 0.5;
            if (WON[b]) w_nb = b + 1;
        }
        w_mixt = cpv(P_MIX) * 0.01;
        vt.build(H.pv(P_VOICE), H.pv(P_VPATH), H.pv(P_VSHIFT));   // (the sliders themselves, as the JSFX's w_vtab)
        if (vt.von) {
            double k = vt.bw[3] / vt.f4, g = std::tan(PI * emin(vt.f4, srate * 0.45) / srate);
            w_v4a1 = 1 / (1 + g * (g + k));
            w_v4a2 = g * w_v4a1;
            w_v4a3 = g * w_v4a2;
            w_rad = 1 / (2 * std::sin(PI * 1000 / srate));
            if (!w_vwas) { memset(VS, 0, sizeof VS); memset(VR, 0, sizeof VR); }
        }
        w_vwas = vt.von;
        // in series with the band before it only while that one plays too (otherwise side by side, from the input)
        WRT[1] = cpv(P_C12) >= 0.5 && WON[0] && WON[1];
        WRT[2] = cpv(P_C23) >= 0.5 && WON[1] && WON[2];
        // Voice: the groups the bands make (bands in parallel are a group; a band in series starts a new one)
        for (int b = 0; b < 3; ++b) {
            VG0[b] = (b == 0 || truth(WRT[b])) ? b : VG0[b - 1];
            VGN[b] = 0;
        }
        for (int b = 0; b < w_nb; ++b)
            if (WON[b]) VGN[VG0[b]] += 1;
        for (int b = 0; b < w_nb; ++b) VGN[b] = VGN[VG0[b]];
        w_csm = 1 - std::exp(-1 / (0.005 * srate));
        w_lsm = 1 - std::exp(-1 / (0.002 * srate));
        w_fa = 1 - std::exp(-1 / (0.0003 * srate));
        w_fr = 1 - std::exp(-1 / (0.015 * srate));
        w_sa = 1 - std::exp(-1 / (0.020 * srate));
        w_sr = 1 - std::exp(-1 / (0.150 * srate));
        w_h10 = std::max(1.0, std::floor(srate * 0.01 + 0.5));
        double beat = tr.beat, tempo = tr.tempo;
        bool w_bchg = tr.playing && beat != w_lastbeat;
        w_lastbeat = beat;
        // ---- each band's settings ----
        for (int b = 0; b < w_nb; ++b) {
            double *o = WB + b * WSTR;
            int pb = P_B + PBS * b;
            o[F_MODE] = clampd(std::floor(cpv(pb + K_SRC) + 0.5), 0, 3);
            o[F_DIR] = cpv(pb + K_DIR) >= 0.5;
            o[F_FT] = clampd(std::floor(cpv(pb + K_FTYPE) + 0.5), 0, 2);
            double t = emin(cpv(pb + K_FMIN), cpv(pb + K_FMAX)), u = emax(cpv(pb + K_FMIN), cpv(pb + K_FMAX));
            double v = std::log(emax(1.0001, u / t));
            double w = cpv(pb + K_Q), x = std::log(cpv(pb + K_QTOE) / w);
            if (w != o[F_QH] || x != o[F_LQ] || t != o[F_FMIN] || v != o[F_LR] || (double)vt.von != o[F_VON] ||
                (vt.von && (cpv(P_VPATH) != w_lvp || cpv(P_VOICE) != w_lvv || cpv(P_VSHIFT) != w_lvs))) {
                o[F_TL + 3] = -1;
                o[F_TR + 3] = -1;
            }
            o[F_QH] = w; o[F_LQ] = x; o[F_FMIN] = t; o[F_LR] = v; o[F_VON] = vt.von;
            // drive, slope, the pedal's range and curve, the LFO's stereo offset
            t = cpv(pb + K_DRIVE);
            o[F_DRVON] = t > 0.05;
            o[F_DRV] = epow(10, t / 20);
            o[F_SLOPE] = cpv(pb + K_SLOPE) >= 0.5;
            o[F_PMIN] = cpv(pb + K_PMIN) * 0.01;
            o[F_PMAX] = cpv(pb + K_PMAX) * 0.01;
            o[F_GAM] = epow(2, cpv(pb + K_PCURVE) / 50);
            o[F_LST] = cpv(pb + K_LSTEREO) / 360;
            o[F_BOOST] = epow(10, cpv(pb + K_BOOST) / 20);
            o[F_THR] = cpv(pb + K_THR);
            o[F_SPAN] = cpv(pb + K_SPAN);
            o[F_EA] = 1 - std::exp(-1 / (emax(0.05, cpv(pb + K_ATT)) * 0.001 * srate));
            o[F_ER] = 1 - std::exp(-1 / (emax(1, cpv(pb + K_REL)) * 0.001 * srate));
            o[F_TSENS] = cpv(pb + K_TSENS);
            o[F_TA] = o[F_EA];
            o[F_TD] = std::exp(-1 / (emax(1, cpv(pb + K_REL)) * 0.001 * srate));
            o[F_SM] = 1 - std::exp(-1 / (emax(1, cpv(pb + K_SMOOTH)) * 0.001 * srate));
            o[F_SRC] = cpv(pb + K_DET) >= 0.5;
            // the detector's high-pass (12 dB/octave), if it is above its lowest setting
            t = cpv(pb + K_HPF);
            o[F_HPON] = t > 20.5;
            u = 2 * PI * emin(t, srate * 0.49) / srate;
            v = std::cos(u);
            w = std::sin(u) / 1.4142135623730951;
            x = 1 + w;
            o[F_B0] = (1 + v) * 0.5 / x;
            o[F_B1] = -(1 + v) / x;
            o[F_A1C] = -2 * v / x;
            o[F_A2C] = (1 - w) / x;
            // the LFO
            t = std::floor(cpv(pb + K_LSYNC) + 0.5);
            o[F_SYNC] = t > 0;
            o[F_CPB] = WSYNC[(int)clampd(t, 0, 11)];
            o[F_LINC] = t > 0 ? tempo / 60 / srate * o[F_CPB] : cpv(pb + K_LRATE) / srate;
            o[F_LSH] = clampd(std::floor(cpv(pb + K_LSHAPE) + 0.5), 0, 5);
            o[F_LDEP] = cpv(pb + K_LDEPTH) * 0.01;
            o[F_LPHO] = cpv(pb + K_LPHASE) / 360;
            if (truth(o[F_SYNC]) && w_bchg) { t = beat * o[F_CPB]; o[F_LPH] = t - std::floor(t); }
            // the knob moved (by hand or by automation): the knob takes the pedal back from MIDI
            t = cpv(pb + K_POS);
            if (t != o[F_LKNOB]) { o[F_LKNOB] = t; o[F_USEM] = 0; o[F_MT] = t * 0.01; }
            o[F_MCC] = std::floor(cpv(pb + K_MCC) + 0.5);
            o[F_MCH] = std::floor(cpv(pb + K_MCH) + 0.5);
            o[F_LEARN] = cpv(pb + K_LEARN) >= 0.5;
            if (!truth(o[F_INIT])) {
                o[F_BS] = o[F_BOOST];
                o[F_PM] = o[F_MT];
                o[F_LR2] = eel_rand(1);
                o[F_LR1] = eel_rand(1);
                o[F_INIT] = 1;
            }
        }
        w_lvp = cpv(P_VPATH); w_lvv = cpv(P_VOICE); w_lvs = cpv(P_VSHIFT);
        if (!w_inited) { w_mixs = w_mixt; w_inited = true; }
        // ---- "Set thresholds": wait for sound, listen (the loudest of each millisecond), then set every Envelope / Transient band ----
        w_anyk = false;
        for (int b = 0; b < w_nb; ++b) {
            int m = (int)WB[b * WSTR + F_MODE];
            if (WON[b] && (m == 1 || m == 2)) w_anyk = true;
        }
        int &c_tgs = A.tgs;
        if (c_tgs > 0 && !w_anyk) c_tgs = 0;
        if (c_tgs == 1) {
            // wait for sound at a detector (-50 dBFS)
            if (w_tgw > -50) {
                c_tgs = 2; c_tgn = 0; c_tc = 0; c_tms = std::max(1.0, std::floor(srate * 0.001 + 0.5));
                for (int i = 0; i < 6; ++i) TGMX[i] = -200;
                int t3 = (int)std::floor(cpv(P_TTIME) + 0.5);
                c_tgdur = 1000 * (t3 <= 0 ? 1 : (t3 == 1 ? 2 : (t3 == 2 ? 3 : (t3 == 3 ? 5 : (t3 == 4 ? 10 : (t3 == 5 ? 20 : 30))))));
            }
            w_tgw = -200;
        } else if (c_tgs == 2) {
            if (c_tgn >= c_tgdur) { c_tgs = 3; c_tgi = 0; }
        } else if (c_tgs == 3) {
            // one band per block
            if (c_tgi < w_nb) { tgset(c_tgi); c_tgi += 1; }
            else c_tgs = 0;
        }
        A.tgel = c_tgs == 2 ? c_tgn * 0.001 : 0;
        A.tgav = w_anyk ? 1 : 0;
        V.anyk.store(w_anyk ? 1 : 0, std::memory_order_relaxed);
        // ---- MIDI: everything passes through (the plug-in does that); each band reads its own controller or pitch bend ----
        w_nev = 0; w_ep = 0; w_sidx = 0;
        for (const MidiEv &m : midi) {
            int st = m.d[0] & 240, ch = (m.d[0] & 15) + 1;
            if (m.len >= 3 && (st == 176 || st == 224)) {
                bool bend = st == 224;
                int c = bend ? 128 : m.d[1] & 127;
                // 0..1: a controller's 7 bits, or pitch bend's 14
                double v = bend ? ((m.d[1] & 127) + (m.d[2] & 127) * 128) / 16383.0 : (m.d[2] & 127) / 127.0;
                // learn: the next controller (or pitch bend) goes to the first band that is waiting for one
                if (!(!bend && c >= 120)) {
                    bool did = false;
                    for (int b = 0; b < w_nb; ++b) {
                        double *o = WB + b * WSTR;
                        if (truth(o[F_LEARN]) && !did) {
                            H.setAudio(bp(b, K_MCC), c);
                            H.setAudio(bp(b, K_LEARN), 0);
                            o[F_MCC] = c;
                            o[F_LEARN] = 0;
                            did = true;
                        }
                    }
                }
                for (int b = 0; b < w_nb; ++b) {
                    double *o = WB + b * WSTR;
                    if (eq(c, o[F_MCC]) && (eq(o[F_MCH], 0) || eq(o[F_MCH], ch))) {
                        if (w_nev < 255) { WEVO[w_nev] = m.time; WEVV[w_nev] = v; WEVB[w_nev] = b; w_nev += 1; }
                        o[F_USEM] = 1; o[F_MTIME] = a_now; o[F_LCC] = c; o[F_LVAL] = v;
                    }
                }
            }
        }
        for (int b = 0; b < 3; ++b) {
            const double *o = WB + b * WSTR;
            V.usem[b].store(truth(o[F_USEM]), std::memory_order_relaxed);
            V.lcc[b].store((int)o[F_LCC], std::memory_order_relaxed);
            V.lval[b].store((float)o[F_LVAL], std::memory_order_relaxed);
            V.mtime[b].store(o[F_MTIME], std::memory_order_relaxed);
        }
    }

    void afterAct()
    {
        if (!idDrawn && A.myid != 0) { eel_rand(899999); idDrawn = true; }
    }

    // @sample, the wah's part (before the Act); sc: the sidechain (or null)
    void samples(const double *const *in, const double *const *sc, double *const *out, int n)
    {
        if ((int)br0.size() < n) { br0.resize(n); br1.resize(n); }
        const int c_tgs = A.tgs;
        for (int i = 0; i < n; ++i) {
            w_d0 = in[0][i]; w_d1 = in[1][i];
            br0[i] = w_d0; br1[i] = w_d1;
            w_s2 = sc ? sc[0][i] : 0;
            w_s3 = sc ? sc[1][i] : 0;
            // the pedals' MIDI events land on their own sample
            while (w_ep < w_nev && WEVO[w_ep] <= w_sidx) { WB[WEVB[w_ep] * WSTR + F_MT] = WEVV[w_ep]; w_ep += 1; }
            w_sidx += 1;
            w_mixs += w_csm * (w_mixt - w_mixs);
            // bands in parallel share one input and their outputs add; a band in series takes the sum of the bands before it
            // (Voice: bands in series are one vocal tract's formants one after the other, bands in parallel its formants side
            // by side; the first band's drive goes in front of them all)
            double in0 = w_d0, in1 = w_d1, acc0 = 0, acc1 = 0;
            if (vt.von && truth(WB[F_DRVON])) { in0 = sat(in0, WB[F_DRV]); in1 = sat(in1, WB[F_DRV]); }
            if (w_nb == 0) { acc0 = w_d0; acc1 = w_d1; }   // no band on: the input as it is
            for (int b = 0; b < w_nb; ++b) {
                if (!WON[b]) continue;
                if (b > 0 && truth(WRT[b])) { in0 = acc0; in1 = acc1; acc0 = 0; acc1 = 0; }
                w_i0 = in0; w_i1 = in1;
                band(b * WSTR);
                if (c_tgs == 1 || c_tgs == 2) {
                    int t3 = (int)WB[b * WSTR + F_MODE];
                    if (t3 == 1 || t3 == 2) {
                        double ed = WB[b * WSTR + F_ED];
                        if (c_tgs == 1) { if (ed > w_tgw) w_tgw = ed; }
                        else {
                            if (ed > TGMX[b * 2]) TGMX[b * 2] = ed;
                            if (t3 == 2 && w_td > TGMX[b * 2 + 1]) TGMX[b * 2 + 1] = w_td;
                        }
                    }
                }
                acc0 += w_y0; acc1 += w_y1;
            }
            if (vt.von) {
                // the closed vowels (a low first formant) a little louder: oo about 6 dB under ah, as spoken, not 12
                w_vcomp = std::pow(768 / emax(150, WB[F_TL + 4]), 0.6);
                acc0 = voicePost(0, acc0 * WB[F_BS]);   // the first band's level, for the whole voice
                acc1 = voicePost(1, acc1 * WB[F_BS]);
            }
            if (c_tgs == 2) {
                // one entry per millisecond: the largest of each measure
                if ((c_tc += 1) >= c_tms) {
                    if (c_tgn < 30000) {
                        for (int k = 0; k < 6; ++k) { TGLV[k * 30000 + c_tgn] = TGMX[k]; TGMX[k] = -200; }
                        c_tgn += 1;
                    }
                    c_tc = 0;
                }
            }
            out[0][i] = w_d0 + w_mixs * (acc0 - w_d0);
            out[1][i] = w_d1 + w_mixs * (acc1 - w_d1);
            // the history (only while the window is open): per band the loudest key, the detector, the slow follower and the position, each 10 ms
            if (w_uion && (w_hcnt += 1) >= w_h10) {
                w_hcnt = 0;
                for (int b = 0; b < w_nb; ++b) {
                    double *o = WB + b * WSTR;
                    int hi = b * HN + w_hp;
                    double t2 = o[F_KMAX];
                    V.hk[hi].store((float)(t2 > 0.0000000001 ? 20 * std::log10(t2) : -200), std::memory_order_relaxed);
                    o[F_KMAX] = 0;
                    int m2 = (int)o[F_MODE];
                    V.he[hi].store((float)((m2 == 1 || m2 == 2) ? o[F_ED] : -200), std::memory_order_relaxed);
                    V.hs[hi].store((float)(m2 == 2 ? o[F_SD] : -200), std::memory_order_relaxed);
                    V.hpos[hi].store((float)o[F_POS], std::memory_order_relaxed);
                }
                w_hp = (w_hp + 1) & (HN - 1);
                V.hp.store(w_hp, std::memory_order_release);
            }
        }
        for (int b = 0; b < 3; ++b) {
            V.pos[b].store((float)WB[b * WSTR + F_POS], std::memory_order_relaxed);
            V.fc[b].store((float)WB[b * WSTR + F_TL + 4], std::memory_order_relaxed);
        }
    }

    void post(double *const *out, int n)
    {
        for (int i = 0; i < n; ++i) ab.post(br0[i], br1[i], out[0][i], out[1][i], br0[i], br1[i]);
    }

private:
    EngineHost &H;
    Act &A;
    UiLink &U;
    Display &V;
    double srate = 44100;
    bool nonceDrawn = false, idDrawn = false;
    std::vector<double> br0, br1;
    VoiceTab vt;
    double WB[3 * WSTR] = {};
    double WRT[4] = {};
    int VG0[3] = {0, 1, 2}, VGN[3] = {1, 1, 1};   // Voice: each band's group (its first band) and the group's size
    // Voice: a formant in a parallel group is a band pass (peak 1) at the level the group's formants in series would
    // have there (so a parallel group sounds like the same vowel), every other one inverted (no holes between them)
    double vamp(int b, double f) const
    {
        double a = 1;
        for (int j = VG0[b]; j < VG0[b] + VGN[b]; ++j) {
            double fj = WB[j * WSTR + F_TL + 4], kj = WB[j * WSTR + F_TL + 5], x = f / emax(1, fj);
            a /= std::sqrt((1 - x * x) * (1 - x * x) + (kj * x) * (kj * x));
        }
        return ((b - VG0[b]) & 1) ? -a : a;
    }
    uint32_t WEVO[256] = {};
    double WEVV[256] = {};
    int WEVB[256] = {};
    std::vector<double> TGLV;
    double TGMX[6] = {};
    int c_tgn = 0, c_tc = 0, c_tgdur = 3000, c_tgi = 0;
    double c_tms = 48, w_tgw = -200, w_td = -100;
    bool w_anyk = false, w_uion = false, w_inited = false;
    int w_nb = 1, w_nev = 0, w_ep = 0, w_hp = 0;
    bool WON[3] = {true, false, false};
    uint32_t w_sidx = 0;
    double w_hcnt = 0, w_h10 = 1, w_mixs = 1, w_mixt = 1, w_lastbeat = -1;
    double w_y0 = 0, w_y1 = 0, w_i0 = 0, w_i1 = 0, w_d0 = 0, w_d1 = 0, w_s2 = 0, w_s3 = 0;
    double w_csm = 0, w_lsm = 0, w_fa = 0, w_fr = 0, w_sa = 0, w_sr = 0, w_lvp = 0, w_lvv = 0, w_lvs = 0;

    // the playing set's value (the other set's while Swap is held, for the settings in the A/B set)
    double cpv(int p) const
    {
        const ParamDef *d = table().get(p);
        return (ab.swh && d && (d->flags & PF_AB)) ? H.abv(p) : H.pv(p);
    }

    // Voice, after the formants: the fixed fourth formant, then the lips (+6 dB/oct, unity at 1 kHz) and the level
    double VS[4] = {}, VR[2] = {};
    double w_v4a1 = 0, w_v4a2 = 0, w_v4a3 = 0, w_rad = 1, w_vmk = 0.53, w_vcomp = 1;   // w_vmk: ah, Level 0 dB = the input's level
    bool w_vwas = false;
    inline double voicePost(int c, double x)
    {
        double c1 = VS[2 * c], c2 = VS[2 * c + 1];
        double v3 = x - c2, v1 = w_v4a1 * c1 + w_v4a2 * v3, v2 = c2 + w_v4a2 * c1 + w_v4a3 * v3;
        VS[2 * c] = 2 * v1 - c1;
        VS[2 * c + 1] = 2 * v2 - c2;
        double r = (v2 - VR[c]) * w_rad;
        VR[c] = v2;
        return r * w_vmk * w_vcomp;
    }
    // a state-variable filter (TPT): state at WB[ob], tuning a1 a2 a3 and k = 1 / Q
    inline double svf(int ob, double a1, double a2, double a3, double k, int ft, double x)
    {
        double c1 = WB[ob], c2 = WB[ob + 1];
        double v3 = x - c2, v1 = a1 * c1 + a2 * v3, v2 = c2 + a2 * c1 + a3 * v3;
        WB[ob] = 2 * v1 - c1;
        WB[ob + 1] = 2 * v2 - c2;
        return ft == 0 ? k * v1 : (ft == 1 ? v2 : x - k * v1 - v2);
    }
    // retune one side of a band (tb = its tuning block) to position p, if it has moved
    void tune(int o, int tb, double p)
    {
        if (std::fabs(p - WB[tb + 3]) > 0.00002) {
            WB[tb + 3] = p;
            bool von = truth(WB[o + F_VON]);
            int bi = (int)WB[o + F_BIDX];
            double fc = von ? vt.form(bi, p) : WB[o + F_FMIN] * std::exp(p * WB[o + F_LR]);
            WB[tb + 4] = fc;
            // a formant's Q: its natural bandwidth, sharpened or widened by the band's Q (5 = natural)
            double k = von ? vt.bw[bi] / fc * 5 / emax(0.1, WB[o + F_QH]) : 1 / (WB[o + F_QH] * std::exp(p * WB[o + F_LQ]));
            double g = std::tan(PI * emin(fc, srate * 0.45) / srate);
            double a1 = 1 / (1 + g * (g + k));
            WB[tb] = a1;
            WB[tb + 1] = g * a1;
            WB[tb + 2] = g * g * a1;
            WB[tb + 5] = k;
        }
    }
    // one LFO shape, 0..1: x the phase, ph the phase without the offset (for the random one's glide)
    static double lfov(int t, double x, double ph, double r1, double r2)
    {
        return t == 0 ? 0.5 - 0.5 * std::cos(2 * PI * x)
                      : (t == 1 ? (x < 0.5 ? 2 * x : 2 - 2 * x)
                                : (t == 2 ? x : (t == 3 ? 1 - x : (t == 4 ? (x < 0.5 ? 1 : 0) : (r1 + (r2 - r1) * ph * ph * (3 - 2 * ph))))));
    }
    // soft saturation in front of a band's filter: tanh, level-compensated (g = the drive's gain)
    static double sat(double x, double g) { return (2 / (1 + std::exp(-2 * g * x)) - 1) / epow(g, 0.6); }

    // "Set thresholds": the level the largest values of a measure reach for pct of the counted time (-999 = nothing counted)
    double tgpct(const double *base, int n, double lo, double minv, double pct)
    {
        double hist[1000] = {};
        int c = 0;
        for (int i = 0; i < n; ++i) {
            double t = base[i];
            if (t > minv) { hist[(int)emax(0, emin(999, std::floor((t - lo) * 10)))] += 1; c += 1; }
        }
        if (c <= 0) return -999;
        double lim = c * emax(0.001, emin(0.5, pct)), acc = 0;
        int i = 999;
        for (;;) {
            acc += hist[i];
            if (acc < lim && i > 0) i -= 1;
            else break;
        }
        return lo + i * 0.1;
    }
    // Envelope: the threshold, so the loudest moments just open the wah fully. Transient: the trigger, so the strongest
    // attacks just open it fully, and the threshold 30 dB under the loudest moments.
    void tgset(int b)
    {
        int m = (int)WB[b * WSTR + F_MODE], pb = P_B + PBS * b;
        double pc = H.pv(P_DTP) * 0.01;
        double lv = tgpct(TGLV.data() + (b * 2) * 30000, c_tgn, -70, -60, pc);
        if (m == 1 && lv > -999) H.setAudio(pb + K_THR, emax(-80, emin(0, std::floor((lv - H.pv(pb + K_SPAN)) * 10 + 0.0001) / 10)));
        if (m == 2) {
            double dv = tgpct(TGLV.data() + (b * 2 + 1) * 30000, c_tgn, 0, 0.5, pc);
            if (dv > -999) H.setAudio(pb + K_TSENS, emax(3, emin(30, std::ceil(dv * 10 - 0.0001) / 10)));
            if (lv > -999) H.setAudio(pb + K_THR, emax(-80, emin(0, std::floor((lv - 30) * 10 + 0.0001) / 10)));
        }
    }

    // one band, one sample: a position 0..1 (one per side with an LFO stereo offset) tunes the filter; its output
    // (times the band's level) lands in w_y0 / w_y1. The band filters w_i0 / w_i1; the detectors hear the original input.
    void band(int o)
    {
        int md = (int)WB[o + F_MODE];
        double p = 0, pr = -1;
        if (md == 1 || md == 2) {
            // the detector's key: the main input or the sidechain, high-passed if asked
            double ka, kb;
            if (truth(WB[o + F_SRC])) { ka = w_s2; kb = w_s3; }
            else { ka = w_d0; kb = w_d1; }
            if (truth(WB[o + F_HPON])) {
                double y = WB[o + F_B0] * ka + WB[o + F_BZ];
                WB[o + F_BZ] = WB[o + F_B1] * ka - WB[o + F_A1C] * y + WB[o + F_BZ + 1];
                WB[o + F_BZ + 1] = WB[o + F_B0] * ka - WB[o + F_A2C] * y;
                ka = y;
                y = WB[o + F_B0] * kb + WB[o + F_BZ + 2];
                WB[o + F_BZ + 2] = WB[o + F_B1] * kb - WB[o + F_A1C] * y + WB[o + F_BZ + 3];
                WB[o + F_BZ + 3] = WB[o + F_B0] * kb - WB[o + F_A2C] * y;
                kb = y;
            }
            double a = emax(std::fabs(ka), std::fabs(kb));
            if (a > WB[o + F_KMAX]) WB[o + F_KMAX] = a;
            if (md == 1) {
                // envelope: how loud, followed with an attack and a release
                double x = WB[o + F_ENV];
                x += (a > x ? WB[o + F_EA] : WB[o + F_ER]) * (a - x);
                WB[o + F_ENV] = x;
                double v = x > 0.0000000001 ? 20 * std::log10(x) : -200;
                WB[o + F_ED] = v;
                p = emax(0, emin(1, (v - WB[o + F_THR]) / WB[o + F_SPAN]));
            } else {
                // transient: how far the fast follower is above the slow one, held open and let fall back
                double x = WB[o + F_EF];
                x += (a > x ? w_fa : w_fr) * (a - x);
                WB[o + F_EF] = x;
                double v = x > 0.0000000001 ? 20 * std::log10(x) : -200;
                WB[o + F_ED] = v;
                double y = WB[o + F_ES];
                y += (a > y ? w_sa : w_sr) * (a - y);
                WB[o + F_ES] = y;
                double t = y > 0.0000000001 ? 20 * std::log10(y) : -200;
                WB[o + F_SD] = t;
                w_td = v > -70 ? emin(40, v - t - 1.5) : -100;
                t = v > WB[o + F_THR] ? emax(0, emin(1, (v - t - 1.5) / WB[o + F_TSENS])) : 0;
                x = WB[o + F_TP];
                if (t > x) x += WB[o + F_TA] * (t - x);
                else x = emax(t, x * WB[o + F_TD]);
                WB[o + F_TP] = x;
                p = x;
            }
        } else if (md == 3) {
            // LFO: the phase runs on; a synced LFO is re-locked to the song position each block
            double ph = WB[o + F_LPH] + WB[o + F_LINC];
            if (ph >= 1) { ph -= std::floor(ph); WB[o + F_LR1] = WB[o + F_LR2]; WB[o + F_LR2] = eel_rand(1); }
            WB[o + F_LPH] = ph;
            double x = ph + WB[o + F_LPHO];
            x -= std::floor(x);
            int t = (int)WB[o + F_LSH];
            double v = lfov(t, x, ph, WB[o + F_LR1], WB[o + F_LR2]);
            WB[o + F_LS] += w_lsm * (v - WB[o + F_LS]);
            p = 0.5 + WB[o + F_LDEP] * (WB[o + F_LS] - 0.5);
            if (WB[o + F_LST] > 0) {
                // the right side runs a little behind
                x = ph + WB[o + F_LPHO] + WB[o + F_LST];
                x -= std::floor(x);
                double y = ph + WB[o + F_LST];
                y -= std::floor(y);
                v = lfov(t, x, t == 5 ? y : ph, WB[o + F_LR1], WB[o + F_LR2]);
                WB[o + F_LSR] += w_lsm * (v - WB[o + F_LSR]);
                pr = 0.5 + WB[o + F_LDEP] * (WB[o + F_LSR] - 0.5);
            }
        } else {
            // manual: the pedal, smoothed, then trimmed and shaped
            WB[o + F_PM] += WB[o + F_SM] * (WB[o + F_MT] - WB[o + F_PM]);
            p = WB[o + F_PMIN] + (WB[o + F_PMAX] - WB[o + F_PMIN]) * epow(WB[o + F_PM], WB[o + F_GAM]);
        }
        if (truth(WB[o + F_DIR])) {
            p = 1 - p;
            if (pr >= 0) pr = 1 - pr;
        }
        bool sep = pr >= 0;
        WB[o + F_POS] = p;
        // the filter follows (only when a position has moved)
        tune(o, o + F_TL, p);
        if (sep) tune(o, o + F_TR, pr);
        int tbr = sep ? o + F_TR : o + F_TL;
        WB[o + F_BS] += w_csm * (WB[o + F_BOOST] - WB[o + F_BS]);
        // Voice: a formant is a resonance (a low-pass, 12 dB/oct) in series, a band pass at its level in a parallel group;
        // the drive and the level are the first band's, for the whole voice (see samples)
        bool von = vt.von;
        int bi = (int)WB[o + F_BIDX];
        bool par = von && VGN[bi] > 1;
        int ft = von ? (par ? 0 : 1) : (int)WB[o + F_FT];
        bool sl = !von && truth(WB[o + F_SLOPE]);
        int ob = o + F_IC;
        double i0 = w_i0, i1 = w_i1;
        if (truth(WB[o + F_DRVON]) && !von) { i0 = sat(i0, WB[o + F_DRV]); i1 = sat(i1, WB[o + F_DRV]); }
        double lv = von ? (par ? vamp(bi, WB[o + F_TL + 4]) : 1) : WB[o + F_BS];
        double y = svf(ob, WB[o + F_TL], WB[o + F_TL + 1], WB[o + F_TL + 2], WB[o + F_TL + 5], ft, i0);
        if (sl) y = svf(ob + 4, WB[o + F_TL], WB[o + F_TL + 1], WB[o + F_TL + 2], WB[o + F_TL + 5], ft, y);
        w_y0 = lv * y;
        y = svf(ob + 2, WB[tbr], WB[tbr + 1], WB[tbr + 2], WB[tbr + 5], ft, i1);
        if (sl) y = svf(ob + 6, WB[tbr], WB[tbr + 1], WB[tbr + 2], WB[tbr + 5], ft, y);
        w_y1 = lv * y;
    }
};

// ---------------------------------------------------------------- the plug-in
class WahPlugin : public Plugin, public EngineHost {
public:
    explicit WahPlugin(const clap_host *h) : Plugin(h, kind()), eng(*this, act, uiLink, view) {}
    UiLink uiLink;
    Display view;
    Engine eng;
    double pv(int id) const override { return P.get(id); }
    double abv(int id) const override { return abValue(id); }
    void setAudio(int id, double v) override { setFromAudio(id, v); }
    // the Gain Stage's Set row: its listening time is the wah's P_TTIME
    int hkRset(int x) override { return x == GS_TTIME ? P_TTIME : 0; }
    void hkTgStart() override
    {
        if (eng.anyk()) { act.tgs = 1; eng.tgArm(); }
    }
    void hkPublish(shm::Slot &b) override { b.p[GS_TTIME].store(P.get(P_TTIME), std::memory_order_relaxed); }
    gui::View *createView() override;

protected:
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

static Plugin *create(const clap_host *h) { return new WahPlugin(h); }

// ---------------------------------------------------------------- the window
class WahView : public EffectView {
public:
    explicit WahView(WahPlugin &p) : EffectView(p, p.uiLink, 940, 742, "WAH"), W(p)
    {
        SH = 196 + 34 + 15;
        help = {"No latency: no lookahead and no delay line. Bands joined by + share one input and add; bands joined by > run one after the other.",
                "Each band has its own pedal: a MIDI controller (CC 11 by default) or pitch bend; Learn takes the next one you move. MIDI passes through.",
                "Voice turns the bands into the formants of a voice: band 1 is formant 1, band 2 formant 2, band 3 formant 3.",
                "Set thresholds listens for the time chosen and sets every Envelope and Transient band (the Gain Stage can start it too)."};
    }
    void frame(Ui &ui) override;

protected:
    void hashMore(std::function<void(uint64_t)> mix) override
    {
        mix((uint64_t)W.view.side.load() | (uint64_t)W.view.anyk.load() << 2);
        for (int b = 0; b < 3; ++b) {
            mix((uint64_t)W.view.usem[b].load());
            mix((uint64_t)(W.view.lcc[b].load() + 2));
            mix((uint64_t)(W.view.lval[b].load() * 127));
            mix(ui_now - W.view.mtime[b].load() < 0.6);
        }
        mix((uint64_t)(fdrag + 1) | (uint64_t)pdrag << 4 | (uint64_t)tdrag << 8);
    }
    bool extraFull() override { return fdrag > 0 || pdrag > 0 || tdrag > 0; }

private:
    WahPlugin &W;
    VoiceTab vt;
    int fdrag = 0, pdrag = 0, tdrag = 0;
    double dtop = 0, ui_now = 0;
    int nb = 3, eb = 0, mode = 0;
    bool on[3] = {true, false, false};   // the bands that play
    int non = 1;                         // how many
    double clickT = -10;                 // the last click on a band's tab (a second one soon after: on / off)
    int clickB = -1;
    // in series with the band before it, as the engine has it (only while both play)
    bool ser(int b) const { return b > 0 && on[b - 1] && on[b] && W.get(b == 1 ? P_C12 : P_C23) >= 0.5; }
    // per band: lowest and highest frequency, position, frequency now, tuning g, k, filter type, level, slope
    double GA[40] = {};

    static Col bandCol(int b, float a = 1)
    {
        return (b == 0 ? Col(1.0f, 0.58f, 0.14f) : (b == 1 ? Col(0.72f, 0.56f, 1.0f) : Col(0.35f, 0.80f, 0.74f))).alpha(a);
    }
    // the response of a state-variable filter (g its tuning, k = 1 / Q) at f
    void hf(double f, double g, double k, int ft, bool sl, double sr, double &hr, double &hi) const
    {
        double r = std::tan(PI * std::min(f, sr * 0.499) / sr) / g;
        double dr = 1 - r * r, di = r * k, nr, ni;
        if (ft == 0) { nr = 0; ni = di; }
        else if (ft == 1) { nr = 1; ni = 0; }
        else { nr = -r * r; ni = 0; }
        double d2 = dr * dr + di * di;
        hr = (nr * dr + ni * di) / d2;
        hi = (ni * dr - nr * di) / d2;
        if (sl) { double t = hr * hr - hi * hi; hi = 2 * hr * hi; hr = t; }
    }
    void bandsNow();
    void sweep(Ui &ui, const R &r);
    void pedals(Ui &ui, const R &r);
    void bandsRow(Ui &ui, float y);
    void bandHeader(Ui &ui, float y);
    void history(Ui &ui, const R &r);
    void controls(Ui &ui, float y);
};

gui::View *WahPlugin::createView() { return new WahView(*this); }

void WahView::bandsNow()
{
    double sr = W.sampleRate();
    for (int b = 0; b < nb; ++b) {
        int q = bp(b, 0);
        if (vt.von) {
            int n = WPN[vt.vpath];
            GA[b] = 100000; GA[4 + b] = 0;
            for (int k = 0; k < n; ++k) {
                double f = std::exp(vt.WFL[b * 8 + k]);
                GA[b] = std::min(GA[b], f);
                GA[4 + b] = std::max(GA[4 + b], f);
            }
        } else {
            GA[b] = std::min(W.get(q + K_FMIN), W.get(q + K_FMAX));
            GA[4 + b] = std::max(W.get(q + K_FMIN), W.get(q + K_FMAX));
        }
        GA[8 + b] = live ? W.view.pos[b].load() : (W.get(q + K_DIR) >= 0.5 ? 1 - W.get(q + K_POS) * 0.01 : W.get(q + K_POS) * 0.01);
        GA[12 + b] = vt.von ? vt.form(b, GA[8 + b]) : GA[b] * std::exp(GA[8 + b] * std::log(std::max(1.0001, GA[4 + b] / GA[b])));
        GA[16 + b] = std::tan(PI * std::min(GA[12 + b], sr * 0.45) / sr);
        if (vt.von) {
            // a formant: a resonance at its natural bandwidth (Q 5); in a parallel group a band pass at its level there
            GA[20 + b] = vt.bw[b] / GA[12 + b] * 5 / std::max(0.1, W.get(q + K_Q));
            GA[24 + b] = 1;
            GA[28 + b] = 1;
            GA[32 + b] = 0;
        } else {
            GA[20 + b] = 1 / (W.get(q + K_Q) * std::exp(GA[8 + b] * std::log(W.get(q + K_QTOE) / W.get(q + K_Q))));
            GA[24 + b] = iround(W.get(q + K_FTYPE));
            GA[28 + b] = std::pow(10.0, W.get(q + K_BOOST) / 20);
            GA[32 + b] = W.get(q + K_SLOPE) >= 0.5;
        }
    }
}

// the sweep: every band's response, where each wah sits now, the edited band's lowest and highest frequency (drag them)
void WahView::sweep(Ui &ui, const R &r)
{
    const double dmax = 30, dmin = -40, lg = std::log(1000.0);
    auto xOf = [&](double f) { return r.x + r.w * (float)(std::log(std::max(20.0, std::min(20000.0, f)) / 20) / lg); };
    auto yOf = [&](double db) { return r.y + r.h * (float)((dmax - db) / (dmax - dmin)); };
    float x0 = xOf(GA[eb]), x1 = xOf(GA[4 + eb]);
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        // the ranges the wahs sweep: the edited band's across the whole plot, the others as a strip along the top
        for (int b = 0; b < nb; ++b) {
            if (!on[b] && b != eb) continue;
            float a0 = xOf(GA[b]), a1 = xOf(GA[4 + b]);
            if (b == eb) ui.fill(R(a0, r.y, std::max(1.f, a1 - a0), r.h), Paint(bandCol(b, 0.10f)));
            else ui.fill(R(a0, r.y + 3 + b * 4, std::max(1.f, a1 - a0), 2.5f), Paint(bandCol(b, 0.55f)));
        }
        for (int d = -40; d < 30; d += 10) {
            float yy = yOf(d);
            ui.fill(R(r.x, yy, r.w, 1 / ui.s), Paint(d == 0 ? th::gridHi() : th::grid()));
            char s[8];
            snprintf(s, sizeof s, "%+d", d);
            ui.text(R(r.x + 6, yy - 12, 30, 11), s, th::faint(), F_REGULAR, 9.5f);
        }
        static const int FL[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000};
        for (int f : FL) {
            float xx = xOf(f);
            ui.fill(R(xx, r.y, 1 / ui.s, r.h), Paint(th::grid()));
            if (f == 100 || f == 1000 || f == 10000) {
                char s[8];
                snprintf(s, sizeof s, f >= 1000 ? "%dk" : "%d", f >= 1000 ? f / 1000 : f);
                ui.text(R(xx + 3, r.b() - 14, 30, 11), s, th::faint(), F_REGULAR, 9.5f);
            }
        }
        if (!vt.von) {
            Col c = bandCol(eb);
            ui.fill(R(x0 - 1, r.y, 2, r.h), Paint(c));
            ui.fill(R(x1 - 1, r.y, 2, r.h), Paint(c));
            ui.text(R(x0 + 5, r.b() - 30, 60, 12), "lowest", c, F_REGULAR, 10);
            ui.text(R(x1 - 65, r.b() - 30, 60, 12), "highest", c, F_REGULAR, 10, 2);
            // drag the edited band's lowest and highest frequency
            bool in = ui.hit(r);
            float mx = ui.mx();
            int nearEdge = std::fabs(mx - x0) <= std::fabs(mx - x1) ? (std::fabs(mx - x0) < 8 ? 1 : 0) : (std::fabs(mx - x1) < 8 ? 2 : 0);
            if (in && (nearEdge || fdrag)) ui.cursor = CUR_HRESIZE;
            if (in && ui.lclick && !ui.used && nearEdge && !ui.busy()) {
                ui.used = true;
                fdrag = nearEdge;
                W.beginEdit(bp(eb, fdrag == 1 ? K_FMIN : K_FMAX));
            }
            if (fdrag) {
                int id = bp(eb, fdrag == 1 ? K_FMIN : K_FMAX);
                if (ui.ldown) {
                    double f = 20 * std::exp(lg * std::max(0.0, std::min((double)r.w, (double)(mx - r.x))) / r.w);
                    f = f >= 1000 ? std::floor(f / 10 + 0.5) * 10 : std::floor(f / 5 + 0.5) * 5;
                    W.edit(id, W.T.clamp(id, f));
                } else {
                    W.endEdit(id);
                    fdrag = 0;
                }
            }
        } else {
            fdrag = 0;
        }
        ui.stroke(r, Paint(th::border()), 6);
    }
    if (!ui.drawD()) return;
    // the curves (they follow the wahs): each band alone (with the dry), and all of them as they are connected
    double sr = W.sampleRate(), mx2 = W.get(P_MIX) * 0.01;
    int np = (int)std::floor(r.w / 3) + 1;
    std::vector<float> cv((size_t)np * 4);
    if (vt.von) {
        // the groups (as the engine's): a band in a parallel group is a band pass at the level the group's formants in
        // series have at its frequency, every other one inverted
        int g0[3], gn[3] = {0, 0, 0};
        for (int b = 0; b < 3; ++b) g0[b] = (b == 0 || ser(b)) ? b : g0[b - 1];
        for (int b = 0; b < nb; ++b)
            if (on[b]) gn[g0[b]] += 1;
        for (int b = 0; b < nb; ++b) {
            int n = gn[g0[b]];
            if (n < 2) continue;
            double a = 1, f = GA[12 + b];
            for (int j = g0[b]; j < g0[b] + n; ++j) {
                double x = f / GA[12 + j];
                a /= std::sqrt((1 - x * x) * (1 - x * x) + (GA[20 + j] * x) * (GA[20 + j] * x));
            }
            GA[24 + b] = 0;
            GA[28 + b] = ((b - g0[b]) & 1) ? -a : a;
        }
    }
    const double vlv = vt.von ? std::pow(10.0, W.get(bp(0, K_BOOST)) / 20) : 1;
    // Voice: after the formants, the fixed fourth one, the lips and the level (as the engine's voicePost)
    double g4 = std::tan(PI * std::min(vt.f4, sr * 0.45) / sr), k4 = vt.bw[3] / vt.f4;
    double vcomp = 0.53 * std::pow(768 / std::max(150.0, GA[12]), 0.6), rad = 1 / (2 * std::sin(PI * 1000 / sr));
    for (int i = 0; i < np; ++i) {
        double f = 20 * std::exp(lg * std::min((double)i * 3, (double)r.w) / r.w);
        double cr = 1, ci = 0, sre = 0, sim = 0;
        for (int b = 0; b < nb; ++b) {
            double hr, hi;
            hf(f, GA[16 + b], GA[20 + b], (int)GA[24 + b], GA[32 + b] > 0.5, sr, hr, hi);
            double xr = (1 - mx2) + mx2 * GA[28 + b] * hr, xi = mx2 * GA[28 + b] * hi;
            cv[(size_t)b * np + i] = (float)std::max(dmin, std::min(dmax, 10 * std::log10(std::max(1e-10, xr * xr + xi * xi))));
            if (!on[b]) continue;
            // a band in series closes the group before it: the chain takes that group's sum
            if (ser(b)) {
                double tr = cr * sre - ci * sim, ti = cr * sim + ci * sre;
                cr = tr; ci = ti; sre = 0; sim = 0;
            }
            sre += GA[28 + b] * hr;
            sim += GA[28 + b] * hi;
        }
        double tr = cr * sre - ci * sim, ti = cr * sim + ci * sre;
        if (non == 0) { tr = 1; ti = 0; }   // no band on: the dry
        if (vt.von) {
            tr *= vlv;
            ti *= vlv;
            double hr, hi, w = 2 * PI * std::min(f, sr * 0.499) / sr;
            hf(f, g4, k4, 1, false, sr, hr, hi);
            double ar = hr * tr - hi * ti, ai = hr * ti + hi * tr;
            double rr = (1 - std::cos(w)) * rad * vcomp, ri = std::sin(w) * rad * vcomp;
            tr = ar * rr - ai * ri;
            ti = ar * ri + ai * rr;
        }
        double xr = (1 - mx2) + mx2 * tr, xi = mx2 * ti;
        cv[(size_t)3 * np + i] = (float)std::max(dmin, std::min(dmax, 10 * std::log10(std::max(1e-10, xr * xr + xi * xi))));
    }
    Canvas &c = ui.cv();
    c.pushClip(ui.X(r.x + 1), ui.Y(r.y + 1), ui.W(r.w - 2), ui.W(r.h - 2));
    auto line = [&](int which, const Col &col, float lw) {
        std::vector<Pt> pts(np);
        for (int i = 0; i < np; ++i) pts[i] = {ui.X(r.x + std::min((float)i * 3, r.w)), ui.Y(yOf(cv[(size_t)which * np + i]))};
        c.strokePolyline(pts.data(), np, ui.W(lw), Paint(col));
    };
    // the edited band's response, filled to the 0 dB line
    {
        float y0 = ui.Y(yOf(0));
        std::vector<float> tops(np);
        for (int i = 0; i < np; ++i) tops[i] = ui.Y(yOf(cv[(size_t)eb * np + i]));
        int px0 = (int)std::floor(ui.X(r.x)), px1 = (int)std::floor(ui.X(r.r()));
        std::vector<float> ct(std::max(0, px1 - px0)), cb(std::max(0, px1 - px0));
        for (int px = px0; px < px1; ++px) {
            float fi = (px + 0.5f - ui.X(r.x)) / ui.W(3);
            int i = std::max(0, std::min(np - 2, (int)fi));
            float t = std::max(0.f, std::min(1.f, fi - i)), y = tops[i] + (tops[i + 1] - tops[i]) * t;
            ct[px - px0] = std::min(y, y0);
            cb[px - px0] = std::max(y, y0);
        }
        for (int px = px0; px < px1; ++px) c.fillRect((float)px, ct[px - px0], 1, std::max(1.f, cb[px - px0] - ct[px - px0]), Paint(bandCol(eb, ct[px - px0] < y0 - 0.5f ? 0.22f : 0.09f)));
    }
    for (int b = 0; b < nb; ++b)
        if (b != eb && on[b]) line(b, bandCol(b, 0.62f), 1.2f);
    if (non > 1 || (non == 1 && !on[eb])) line(3, Col(0.94f, 0.92f, 0.89f, 0.9f), 1.3f);
    line(eb, on[eb] ? bandCol(eb) : bandCol(eb, 0.45f), 2);
    // where each wah is now
    for (int b = 0; b < nb; ++b) {
        if (!on[b] && b != eb) continue;
        float cx = xOf(GA[12 + b]);
        if (b == eb) ui.fill(R(cx - 1, r.y, 2, r.h), Paint(Col(1.0f, 0.95f, 0.88f, 0.9f)));
        else ui.fill(R(cx - 0.5f, r.y, 1, r.h), Paint(bandCol(b, 0.8f)));
    }
    c.popClip();
    float cx = xOf(GA[12 + eb]);
    char s[24];
    if (GA[12 + eb] >= 1000) snprintf(s, sizeof s, "%.2f kHz", GA[12 + eb] / 1000);
    else snprintf(s, sizeof s, "%.0f Hz", GA[12 + eb]);
    bool right = cx > r.r() - 90;
    ui.text(R(right ? cx - 86 : cx + 6, r.y + 20, 80, 14), s, th::text(), F_SEMIBOLD, 12, right ? 2 : 0, true);
    // Voice: the vowel the first band sits nearest to
    if (vt.von) {
        int vi = WPI[vt.vpath * 8 + (int)std::floor(std::max(0.0, std::min(1.0, GA[8])) * (WPN[vt.vpath] - 1) + 0.5)];
        ui.text(R(r.x + 40, r.y + 10, 80, 26), L_VOWEL[vi], th::text(), F_BOLD, 22);
        ui.text(R(r.x + 40, r.y + 38, 80, 12), "vowel", th::faint(), F_REGULAR, 10);
    }
}

// the pedals: each band's position (click to edit that band; drag it in Manual mode)
void WahView::pedals(Ui &ui, const R &r)
{
    float by = r.y + 22, bh = r.h - 22 - 48;
    float bx0 = r.x + (r.w - (nb * 26 - 4)) / 2;
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        ui.stroke(r, Paint(th::border()), 6);
        // the title is the pedal controls' switch: lit while they are shown
        bool shown = W.get(P_SHOWPED) >= 0.5;
        if (ui.button(R(r.x + 12, r.y + 3, r.w - 24, 17), "PEDAL", shown, true, BS_NORMAL)) W.setOnce(P_SHOWPED, shown ? 0 : 1);
        for (int b = 0; b < nb; ++b) {
            float bx = bx0 + b * 26;
            R br(bx, by, 22, bh);
            ui.fill(br, Paint(th::well()), 3);
            ui.stroke(br, Paint(b == eb ? th::text() : th::border()), 3);
            char s[12];
            snprintf(s, sizeof s, "%d", b + 1);
            ui.text(R(bx, by + bh + 4, 22, 12), s, b == eb ? th::text() : th::faint(), F_SEMIBOLD, 10, 1);
            bool in = ui.hit(R(bx - 2, by - 4, 26, bh + 8));
            if (in) ui.cursor = CUR_HAND;
            if (in && ui.lclick && !ui.used && !ui.busy()) {
                ui.used = true;
                if (b != eb) W.setOnce(P_EDIT, b);
                if (iround(W.get(bp(b, K_SRC))) == 0) { pdrag = b + 1; W.beginEdit(bp(b, K_POS)); }
            }
            if (pdrag == b + 1) {
                if (ui.ldown) {
                    double t = std::max(0.0, std::min(1.0, (double)(1 - (ui.my() - by) / bh)));
                    W.edit(bp(b, K_POS), std::floor((W.get(bp(b, K_DIR)) >= 0.5 ? 1 - t : t) * 1000 + 0.5) / 10);
                } else {
                    W.endEdit(bp(b, K_POS));
                    pdrag = 0;
                }
            }
        }
        int md = iround(W.get(bp(eb, K_SRC)));
        const char *how = md == 0 ? (W.view.usem[eb].load() ? "MIDI pedal" : "knob / mouse") : (md == 1 ? "envelope" : (md == 2 ? "transient" : "LFO"));
        ui.text(R(r.x, r.b() - 16, r.w, 12), how, th::faint(), F_REGULAR, 10, 1);
    }
    if (ui.drawD()) {
        for (int b = 0; b < nb; ++b) {
            float bx = bx0 + b * 26, f = (float)std::max(0.0, std::min(1.0, GA[8 + b]));
            ui.fill(R(bx + 1, by + bh * (1 - f), 20, std::max(1.f, bh * f)), Paint(bandCol(b, on[b] ? 0.85f : 0.2f)), 2);
            ui.fill(R(bx - 2, by + bh * (1 - f) - 1.5f, 26, 3), Paint(Col(1.0f, 0.95f, 0.88f)), 1.5f);
        }
        char s[16];
        snprintf(s, sizeof s, "%.0f%%", GA[8 + eb] * 100);
        ui.text(R(r.x, r.b() - 34, r.w, 16), s, th::text(), F_SEMIBOLD, 12.5f, 1, true);
    }
}

// the bands: tabs like the Delay's lines (a dot says a band plays; click: edit it; double-click: on / off), and between
// them how two neighbours connect (side by side or one into the other; greyed while either is off)
void WahView::bandsRow(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float h = 26, tw = 110, cw = 46, g = 6;
    float x = M;
    for (int b = 0; b < 3; ++b) {
        R r(x, y, tw, h);
        bool hv = ui.hit(r), act = eb == b;
        ui.fill(r, Paint(act ? th::raised2() : (hv ? th::hover() : th::panel())), 6);
        ui.stroke(r, Paint(act ? th::borderHi() : th::border()), 6);
        if (act) ui.fill(R(r.x + 8, r.b() - 2.5f, r.w - 16, 2.5f), Paint(th::orange()), 1.25f);
        if (hv) ui.cursor = CUR_HAND;
        if (on[b]) ui.fill(R(r.x + 10, y + h / 2 - 4, 8, 8), Paint(bandCol(b)), 4);
        else ui.stroke(R(r.x + 10, y + h / 2 - 4, 8, 8), Paint(bandCol(b, 0.7f)), 4);
        char s[20];
        snprintf(s, sizeof s, "BAND %d", b + 1);
        ui.text(R(r.x + 26, y, r.w - 30, h), s, act ? th::text() : (on[b] ? th::dim() : th::faint()), F_SEMIBOLD, 11.5f);
        if (hv && ui.lclick && !ui.used && !ui.busy()) {
            ui.used = true;
            bool dbl = ui.now - clickT < 0.35 && clickB == b;
            clickT = ui.now;
            clickB = b;
            if (eb != b) W.setOnce(P_EDIT, b);
            if (dbl) { W.setOnce(P_ON0 + b, on[b] ? 0 : 1); clickT = -10; }
        }
        x += tw + g;
        if (b < 2) {
            // side by side or one into the other: purple while both play, greyed while either is off
            bool both = on[b] && on[b + 1], sr = W.get(b == 0 ? P_C12 : P_C23) >= 0.5;
            // the Delay's symbols: // side by side, -- one into the other
            R cr(x, y, cw, h);
            if (ui.button(cr, "", both, both, BS_PURPLE)) W.setOnce(b == 0 ? P_C12 : P_C23, sr ? 0 : 1);
            ui.icon(sr ? IC_SER : IC_PAR, cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f, 18, both ? Col(1, 1, 1) : th::faint());
            x += cw + g;
        }
    }
    // how they play, in words
    std::string rt;
    if (non == 0) rt = "no band on: the dry signal";
    else if (vt.von && non > 1) {
        bool any = false, all = true;
        for (int b = 1; b < 3; ++b)
            if (on[b] && on[b - 1]) { if (ser(b)) any = true; else all = false; }
        rt = all ? "Voice: formants one into the next" : (!any ? "Voice: formants side by side" : "Voice: formants mixed");
    } else if (non > 1) {
        for (int b = 0; b < 3; ++b) {
            if (!on[b]) continue;
            if (!rt.empty()) rt += ser(b) ? " > " : " + ";
            rt += "Band " + std::to_string(b + 1);
        }
    }
    ui.textFit(R(x + 4, y, DW - M - x - 4 - 60, h), rt.c_str(), th::dim(), F_REGULAR, 11.5f);
    bool midi = false;
    for (int b = 0; b < nb; ++b) {
        double t = W.view.mtime[b].load();
        if (t > 0 && ui_now - t < 0.6) midi = true;
    }
    if (midi) ui.badge(DW - M - ui.badgeW("MIDI"), y + 6, "MIDI", Col(1, 1, 1), th::purple());
}

// the edited band: how it is moved, Voice, and Set thresholds
void WahView::bandHeader(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    int q = bp(eb, 0);
    char s[24];
    snprintf(s, sizeof s, "BAND %d", eb + 1);
    ui.text(R(M, y, 70, 26), s, bandCol(eb), F_BOLD, 13);
    // this band on or off (also: double-click its tab)
    if (ui.button(R(M + 70, y, 46, 26), "On", on[eb])) W.setOnce(P_ON0 + eb, on[eb] ? 0 : 1);
    enumDD(ui, R(M + 124, y, 190, 26), q + K_SRC, L_SRC, 4, "Control");
    enumDD(ui, R(M + 322, y, 128, 26), P_VOICE, L_VOICE, 4, "Voice");
    if (vt.von) enumDD(ui, R(M + 458, y, 196, 26), P_VPATH, L_VPATH, NVPATH, "Vowels");
    int tgs = av.myslot >= 0 ? (int)shm::slot(av.myslot).tgs.load() : 0;
    bool anyk = W.view.anyk.load() != 0;
    if (anyk || tgs > 0) {
        R tr(DW - M - 150 - 8 - 78, y, 78, 26), sr(DW - M - 150, y, 150, 26);
        if (tgs > 0) {
            char b[24];
            if (tgs == 1) snprintf(b, sizeof b, "waiting");
            else if (tgs == 2) snprintf(b, sizeof b, "%.1f s", shm::slot(av.myslot).tgel.load());
            else snprintf(b, sizeof b, "working");
            ui.button(tr, b, true, true, BS_PURPLE);
        } else {
            enumDD(ui, tr, P_TTIME, L_TTIME, 7, nullptr, anyk);
        }
        if (ui.button(sr, tgs > 0 ? "Cancel" : "Set thresholds", tgs > 0, av.myslot >= 0))
            W.act.send(tgs > 0 ? shm::C_TGSTOP : shm::C_TGSTART, 0);
    }
}

// the edited band's detector (Envelope, Transient) or its position over time (LFO, Manual)
void WahView::history(Ui &ui, const R &r)
{
    int q = bp(eb, 0);
    const float db = 80;
    bool det = mode == 1 || mode == 2;
    auto yOfDb = [&](double v) { return r.y + r.h * (float)std::min(1.0, -std::max(-(double)db, v) / db); };
    float t1 = (float)W.get(q + K_THR), t2 = (float)(W.get(q + K_THR) + W.get(q + K_SPAN));
    float y1 = yOfDb(t1), y2 = yOfDb(std::min(0.f, t2));
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        if (det) {
            for (int d = -20; d >= -60; d -= 20) {
                float yy = yOfDb(d);
                ui.fill(R(r.x, yy, r.w, 1 / ui.s), Paint(th::grid()));
                char s[8];
                snprintf(s, sizeof s, "%d", d);
                ui.text(R(r.r() - 32, yy - 12, 26, 11), s, th::faint(), F_REGULAR, 9.5f, 2);
            }
            // the thresholds (drag a line)
            bool in = ui.hit(r);
            float my = ui.my();
            if (in && ui.lclick && !ui.used && !ui.busy()) {
                int td = (mode == 1 && std::fabs(my - y2) < std::fabs(my - y1)) ? (std::fabs(my - y2) < 6 ? 2 : 0) : (std::fabs(my - y1) < 6 ? 1 : 0);
                if (td) {
                    ui.used = true;
                    tdrag = td;
                    dtop = t2;
                    W.beginEdit(q + K_THR);
                    W.beginEdit(q + K_SPAN);
                }
            }
            if (in && (std::fabs(my - y1) < 6 || (mode == 1 && std::fabs(my - y2) < 6))) ui.cursor = CUR_MOVE;
            if (tdrag) {
                if (ui.ldown) {
                    double v = std::max(-80.0, std::min(0.0, (double)(-(my - r.y) / r.h * db)));
                    v = std::floor(v * 2 + 0.5) / 2;
                    if (tdrag == 1) {
                        W.edit(q + K_THR, v);
                        if (mode == 1) W.edit(q + K_SPAN, std::max(6.0, std::min(60.0, dtop - v)));
                    } else {
                        W.edit(q + K_SPAN, std::max(6.0, std::min(60.0, v - W.get(q + K_THR))));
                    }
                } else {
                    W.endEdit(q + K_THR);
                    W.endEdit(q + K_SPAN);
                    tdrag = 0;
                }
            }
            float lx = r.x + 200;
            ui.text(R(lx, r.y + 6, 40, 12), "input", Col(0.55f, 0.55f, 0.58f), F_REGULAR, 10.5f);
            ui.text(R(lx + 40, r.y + 6, 60, 12), mode == 1 ? "envelope" : "fast", bandCol(eb), F_REGULAR, 10.5f);
            if (mode == 2) ui.text(R(lx + 100, r.y + 6, 40, 12), "slow", th::text(), F_REGULAR, 10.5f);
            ui.text(R(lx + 150, r.y + 6, 300, 12), "drag the lines   10 ms per step", th::faint(), F_REGULAR, 10.5f);
        } else {
            tdrag = 0;
            char s[256];
            if (mode == 3) snprintf(s, sizeof s, "position of the wah   10 ms per step");
            else {
                int mcc = iround(W.get(q + K_MCC)), mch = iround(W.get(q + K_MCH)), lcc = W.view.lcc[eb].load();
                char c1[24], c2[24], c3[48];
                if (mcc >= 128) snprintf(c1, sizeof c1, "pitch bend");
                else snprintf(c1, sizeof c1, "CC %d", mcc);
                if (mch < 1) snprintf(c2, sizeof c2, "any channel");
                else snprintf(c2, sizeof c2, "channel %d", mch);
                if (lcc < 0) snprintf(c3, sizeof c3, "none yet");
                else if (lcc >= 128) snprintf(c3, sizeof c3, "pitch bend = %d", (int)std::floor(W.view.lval[eb].load() * 127 + 0.5));
                else snprintf(c3, sizeof c3, "CC %d = %d", lcc, (int)std::floor(W.view.lval[eb].load() * 127 + 0.5));
                snprintf(s, sizeof s, "pedal: %s on %s   last seen: %s   %s", c1, c2, c3,
                         W.view.usem[eb].load() ? "(the MIDI pedal moves this band)" : "(the Pedal knob sets it until a MIDI pedal moves)");
            }
            ui.textFit(R(r.x + 10, r.y + 6, r.w - 20, 12), s, th::faint(), F_REGULAR, 10.5f);
        }
        ui.stroke(r, Paint(th::border()), 6);
    }
    if (!ui.drawD()) return;
    Canvas &c = ui.cv();
    c.pushClip(ui.X(r.x + 1), ui.Y(r.y + 1), ui.W(r.w - 2), ui.W(r.h - 2));
    int hp = W.view.hp.load(std::memory_order_acquire), hb = eb * HN;
    int px0 = (int)std::floor(ui.X(r.x)), px1 = (int)std::ceil(ui.X(r.r())), np = std::max(1, px1 - px0);
    float per = (float)std::min(HN - 1, (int)r.w) / np;   // one entry per design unit, as the JSFX
    auto ent = [&](int i) { return hb + ((hp - (int)std::min((float)HN - 1, r.w) + (int)(i * per)) & (HN - 1)); };
    if (live) {
        if (det) {
            std::vector<float> tops(np);
            std::vector<Pt> le(np), ls(np);
            float yb = ui.Y(r.b());
            for (int i = 0; i < np; ++i) {
                int e = ent(i);
                double v = W.view.hk[e].load(std::memory_order_relaxed);
                tops[i] = v > -db ? ui.Y(yOfDb(v)) : yb;
                le[i] = {(float)(px0 + i) + 0.5f, ui.Y(yOfDb(W.view.he[e].load(std::memory_order_relaxed)))};
                ls[i] = {(float)(px0 + i) + 0.5f, ui.Y(yOfDb(W.view.hs[e].load(std::memory_order_relaxed)))};
                float t = std::max(0.f, std::min(1.f, W.view.hpos[e].load(std::memory_order_relaxed)));
                c.fillRect((float)(px0 + i), ui.Y(r.y), 1, ui.W(5), Paint(bandCol(eb, 0.22f + 0.78f * t)));
            }
            c.columns(px0, tops.data(), np, yb, Paint(Col(0.24f, 0.24f, 0.27f)));
            c.strokeThin(le.data(), np, ui.W(1.4f), bandCol(eb));
            if (mode == 2) c.strokeThin(ls.data(), np, ui.W(1.2f), th::text());
        } else {
            std::vector<Pt> lp(np);
            for (int i = 0; i < np; ++i) {
                int e = ent(i);
                float t = std::max(0.f, std::min(1.f, W.view.hpos[e].load(std::memory_order_relaxed)));
                lp[i] = {(float)(px0 + i) + 0.5f, ui.Y(r.y + 22 + (r.h - 30) * (1 - t))};
            }
            c.strokeThin(lp.data(), np, ui.W(1.6f), bandCol(eb));
        }
    }
    if (det) {
        if (mode == 1) ui.fill(R(r.x, y2, r.w, 1.2f), Paint(th::purple()));
        ui.fill(R(r.x, y1, r.w, 2), Paint(th::orange()));
        char s[40];
        snprintf(s, sizeof s, "threshold %.1f dB", t1);
        ui.text(R(r.x + 8, std::min(y1 + 4, r.b() - 14), 180, 12), s, th::orange(), F_REGULAR, 10.5f);
        if (mode == 1) {
            snprintf(s, sizeof s, "fully open %.1f dB", t2);
            ui.text(R(r.x + 8, std::max(y2 - 15, r.y + 20), 180, 12), s, th::purple(), F_REGULAR, 10.5f);
        }
    }
    c.popClip();
}

void WahView::controls(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float cw = (DW - 2 * M - 2 * 20) / 3, h = 26, dy = 32, lw = 72, bw = 84;
    float x[3] = {M, M + cw + 20, M + 2 * (cw + 20)};
    int q = bp(eb, 0);
    auto row = [&](int col, int r, const char *lbl, int id, int sc, bool en = true) { ui.paramRow(R(x[col], y + r * dy, cw, h), lbl, id, sc, lw, bw, en); };
    float hw = (cw - 8) / 2;
    if (vt.von) {
        // Voice: the band is a formant (its own resonance; the first band drives the voice and sets its level)
        row(0, 0, "Shift", P_VSHIFT, SC_LIN);
        char s[80];
        snprintf(s, sizeof s, "Band %d is formant %d of the voice", eb + 1, eb + 1);
        ui.text(R(x[1], y, cw, h), s, th::dim(), F_REGULAR, 12);
        row(2, 0, "Resonance", q + K_Q, SC_LOG);
        if (eb == 0) { row(0, 1, "Level", q + K_BOOST, SC_LIN); row(1, 1, "Drive", q + K_DRIVE, SC_LIN); }
        ui.text(R(x[2], y + dy, cw, h), "Resonance 5: the voice's own", th::faint(), F_REGULAR, 11);
        enumDD(ui, R(x[1] + hw + 8, y + 2 * dy, hw, h), q + K_DIR, L_DIR, 2, "Sweep");
    } else {
        row(0, 0, "Min freq", q + K_FMIN, SC_LOG);
        row(1, 0, "Max freq", q + K_FMAX, SC_LOG);
        row(2, 0, "Q heel", q + K_Q, SC_LOG);
        row(0, 1, "Level", q + K_BOOST, SC_LIN);
        row(1, 1, "Drive", q + K_DRIVE, SC_LIN);
        row(2, 1, "Q toe", q + K_QTOE, SC_LOG);
        enumDD(ui, R(x[0], y + 2 * dy, cw, h), q + K_FTYPE, L_FT, 3, "Filter");
        enumDD(ui, R(x[1], y + 2 * dy, hw, h), q + K_SLOPE, L_SLOPE, 2, "Slope");
        enumDD(ui, R(x[1] + hw + 8, y + 2 * dy, hw, h), q + K_DIR, L_DIR, 2, "Sweep");
    }
    row(2, 2, "Dry/wet", P_MIX, SC_LIN);
    float y3 = y + 3 * dy + 8;
    auto row3 = [&](int col, int r, const char *lbl, int id, int sc, bool en = true) { ui.paramRow(R(x[col], y3 + r * dy, cw, h), lbl, id, sc, lw, bw, en); };
    ui.fill(R(M, y3 - 5, DW - 2 * M, 1), Paint(th::border()));
    if (mode == 0 && W.get(P_SHOWPED) < 0.5) {
        ui.text(R(x[0], y3, DW - 2 * M, h), "The pedal's controls are hidden: click PEDAL (top right) to show them.", th::faint(), F_REGULAR, 11.5f);
    } else if (mode == 0) {
        row3(0, 0, "Pedal", q + K_POS, SC_LIN);
        row3(1, 0, "Controller", q + K_MCC, SC_LIN);
        row3(2, 0, "Channel", q + K_MCH, SC_LIN);
        row3(0, 1, "Smoothing", q + K_SMOOTH, SC_LOG);
        bool learn = W.get(q + K_LEARN) >= 0.5;
        if (ui.button(R(x[1], y3 + dy, cw, h), learn ? "Move the pedal..." : "Learn the pedal", learn, true, BS_PURPLE)) W.setOnce(q + K_LEARN, learn ? 0 : 1);
        row3(0, 2, "Heel", q + K_PMIN, SC_LIN);
        row3(1, 2, "Toe", q + K_PMAX, SC_LIN);
        row3(2, 2, "Curve", q + K_PCURVE, SC_LIN);
    } else if (mode == 1 || mode == 2) {
        row3(0, 0, "Threshold", q + K_THR, SC_LIN);
        row3(1, 0, "Attack", q + K_ATT, SC_LOG);
        enumDD(ui, R(x[2], y3, cw, h), q + K_DET, L_DET, 2, "Detect");
        row3(0, 1, "Trigger", q + (mode == 1 ? K_SPAN : K_TSENS), SC_LIN);
        row3(1, 1, "Release", q + K_REL, SC_LOG);
        row3(2, 1, "Det. HPF", q + K_HPF, SC_LOG);
    } else {
        int sy = iround(W.get(q + K_LSYNC));
        row3(0, 0, "Rate", q + K_LRATE, SC_LOG, sy == 0);
        enumDD(ui, R(x[1], y3, cw, h), q + K_LSYNC, L_LSYNC, 12, "Sync");
        enumDD(ui, R(x[2], y3, cw, h), q + K_LSHAPE, L_LSHAPE, 6, "Shape");
        row3(0, 1, "Depth", q + K_LDEPTH, SC_LIN);
        row3(1, 1, "Phase", q + K_LPHASE, SC_LIN);
        row3(2, 1, "Stereo", q + K_LSTEREO, SC_LIN);
    }
}

void WahView::frame(Ui &ui)
{
    ui_now = ui.now;
    vt.build(W.get(P_VOICE), W.get(P_VPATH), W.get(P_VSHIFT));
    nb = 3;
    non = 0;
    for (int b = 0; b < 3; ++b) {
        on[b] = W.get(P_ON0 + b) >= 0.5;
        non += on[b];
    }
    eb = iclamp(iround(W.get(P_EDIT)), 0, 2);
    mode = iclamp(iround(W.get(bp(eb, K_SRC))), 0, 3);
    R gear = begin(ui, 0, W.view.side.load() != 0);
    bandsNow();
    float y = 66;
    const float sh = 186;
    sweep(ui, R(M, y, DW - 2 * M - 112, sh));
    pedals(ui, R(DW - M - 104, y, 104, sh));
    y += sh + 10;
    bandsRow(ui, y);
    y += 26 + 10;
    bandHeader(ui, y);
    y += 26 + 10;
    history(ui, R(M, y, DW - 2 * M, 92));
    y += 92 + 12;
    controls(ui, y);
    y += 6 * 32 + 8 + 4;
    actCardAt(ui, y);
    finish(ui, gear, [this](Ui &u, const R &in, float yy) {
        label(u, R(in.x, yy, 150, 24), "Set thresholds: full range");
        u.valueBox(R(in.x + 156, yy, 150, 24), P_DTP, true, KD_OTHER, 0.02, 0.002);
        u.textFit(R(in.x + 316, yy, in.w - 316, 24), "Envelope: the loudest moments just open it fully. Transient: the strongest attacks.", th::faint(),
                  F_REGULAR, 11);
        return yy + 24 + 10;
    });
}

// ---------------------------------------------------------------- the kind
static const char *s_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_FILTER, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const PluginKind &kind()
{
    static const PluginKind k = {
        {CLAP_VERSION_INIT, "com.twostorms.wah", "TwoStorms Wah", "TwoStorms", "", "", "", TS_VERSION,
         "A wah with an Act built in: up to three bands, each moved by a MIDI pedal, the envelope, the transients or an LFO; "
         "in parallel or in series, with drive, and Voice for a talk-box sound. Set thresholds listens and sets the detectors. No latency.",
         s_features},
        &table,
        &factoryPresets,
        "Wah",
        "TWOSTORMS WAH",
        shm::KIND_POINT,
        shm::SK_WAH,
        true,
        true,
        P_AB,
        &create,
        2,
        true,
    };
    return k;
}

} // namespace wah
} // namespace ts
