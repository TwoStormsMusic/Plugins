// TwoStorms Tremolo: three modes (Tremolo, Auto-pan, Harmonic: the lows and highs in opposition), modulated by an LFO
// (sine, triangle, square, ramps, random; free in Hz or synced to the tempo), by step grids, or by both. One to four
// grids, each with its own depth (and in Tremolo its own trim, pan and pitch); linked grids play together, unlinked
// ones take turns, each group of linked grids with its own number of steps (up to 64). Every step has a level (drag
// it up or down), and a Gap can silence the end of a step that runs into the next. The clock follows the song, runs
// free, or restarts on a MIDI note or a pluck. No latency, except while a Tremolo grid is pitched (the shifter's
// frame). MIDI passes through. Grown from twostorms_tremolo.jsfx: its LFO is the JSFX's, sample for sample, and so
// is one grid at full level.
#include "tremolo_plugin.h"
#include "../../core/eelrand.h"
#include "../../core/pvshift.h"
#include "../../gui/effect_view.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>

namespace ts {
namespace trem {

using namespace gui;

enum {
    P_SRC = 50, P_MODE = 51, P_DEPTH = 52, P_RATE = 53, P_SYNC = 54, P_SHAPE = 55, P_SKEW = 56, P_PHASE = 57, P_STEREO = 58,
    P_RISE = 59, P_FALL = 60, P_CLOCK = 61, P_XOVER = 62, P_MIX = 63, P_GAP = 64, P_PSTEP = 65, P_SWING = 66,
    P_GAPLEN = 67,
    P_GLEN = 68,    // 68-71: each group's number of steps
    P_LINK = 72,    // 72-74: grid g + 1 plays with grid g (linked) or after it
    P_ONSET = 75, P_TRIM = 76, P_PAN = 80,
    P_AB = 84,
    P_NGRID = 87,   // 1-4 grids
    P_PITCH = 88,   // 88-91: each grid's pitch, semitones (Tremolo)
    P_GDEP = 92,    // 92-95: each grid's depth
    P_TSRC = 96,    // the tempo: the project's, tapped, or typed
    P_CTEMPO = 97,  // the tapped or typed tempo
    P_LV = 100,     // 100-143: the steps' levels, 11 words per grid, six steps of 8 bits in each
};
constexpr int NG = 4, GS = 64, LVW = 11, LVPW = 6;
constexpr double LVMAX = 281474976710655.0;   // 2^48 - 1
inline int lvId(int g, int w) { return P_LV + g * LVW + w; }

// ---------------------------------------------------------------- parameters
static void f_pct(double v, char *b, int n) { snprintf(b, n, "%.0f%%", v); }
static void f_rate(double v, char *b, int n) { snprintf(b, n, v < 10 ? "%.2f Hz" : "%.1f Hz", v); }
static void f_deg(double v, char *b, int n) { snprintf(b, n, "%.0f deg", v); }
static void f_rise(double v, char *b, int n) { snprintf(b, n, v < 10 ? "%.1f ms" : "%.0f ms", v); }
static void f_xo(double v, char *b, int n) { if (v >= 1000) snprintf(b, n, "%.2f kHz", v / 1000); else snprintf(b, n, "%.0f Hz", v); }
static void f_int(double v, char *b, int n) { snprintf(b, n, "%.0f", v); }
static void f_trim(double v, char *b, int n) { snprintf(b, n, "%+.1f dB", v); }
static void f_db1(double v, char *b, int n) { snprintf(b, n, "%.1f dB", v); }
static void f_st(double v, char *b, int n)
{
    int s = (int)std::lround(v);
    if (s == 0) snprintf(b, n, "0 st");
    else snprintf(b, n, "%+d st", s);
}
static void f_pan(double v, char *b, int n)
{
    if (v < -0.5) snprintf(b, n, "L %.0f", -v);
    else if (v > 0.5) snprintf(b, n, "R %.0f", v);
    else snprintf(b, n, "centre");
}

static const char *const L_SRC[] = {"LFO", "Pattern", "LFO x Pattern"};
static const char *const L_MODE[] = {"Tremolo", "Auto-pan", "Harmonic"};
static const char *const L_SYNC[] = {"Free (Hz)", "4 bars", "2 bars", "1 bar", "1/2", "1/2 triplet", "1/4", "1/4 dotted",
                                     "1/4 triplet", "1/8", "1/8 dotted", "1/8 triplet", "1/16", "1/16 dotted", "1/16 triplet", "1/32",
                                     "1/32 triplet", "1/64", "1/64 triplet"};
constexpr int NSYNC = 19;
static const char *const L_SHAPE[] = {"Sine", "Triangle", "Square", "Ramp up", "Ramp down", "Random step", "Random smooth"};
static const char *const L_CLOCK[] = {"Song position", "Free-running", "Restart: MIDI note", "Restart: audio onset"};
static const char *const L_PSTEP[] = {"1/4", "1/4 triplet", "1/8", "1/8 triplet", "1/16", "1/16 triplet", "1/32", "1/32 triplet",
                                      "1/64", "1/64 triplet"};
constexpr int NPSTEP = 10;
static const char *const L_NGRID[] = {"1", "2", "3", "4"};
static const char *const L_TSRC[] = {"Project", "Tap", "Custom"};
static void f_bpm(double v, char *b, int n) { snprintf(b, n, "%.1f bpm", v); }

// the default grids: 1 every other step from the first, 2 every other from the second, 3 the beats, 4 the last of each beat
static bool defOn(int g, int i) { return g == 0 ? i % 2 == 0 : (g == 1 ? i % 2 == 1 : (g == 2 ? i % 4 == 0 : i % 4 == 3)); }
static double packWord(const uint8_t *q, int w)
{
    double v = 0, m = 1;
    for (int i = 0; i < LVPW; ++i) {
        int k = w * LVPW + i;
        if (k < GS) v += q[k] * m;
        m *= 256;
    }
    return v;
}

static const ParamTable &table()
{
    static ParamTable t = [] {
        ParamTable t;
        addActParams(t, true);
        const char *M = "Tremolo";
        const uint32_t H = PF_HOST | PF_AB;
        t.addEnum(P_SRC, "Source", {L_SRC[0], L_SRC[1], L_SRC[2]}, 1, H, M);
        t.addEnum(P_MODE, "Mode", {L_MODE[0], L_MODE[1], L_MODE[2]}, 0, H, M);
        t.add(P_DEPTH, "LFO depth", 0, 100, 70, H | PF_INT, f_pct, M).unit = "%";
        t.add(P_RATE, "LFO rate", 0.05, 40, 4, H, f_rate, M).unit = "Hz";
        std::vector<const char *> sy(L_SYNC, L_SYNC + NSYNC);
        t.addEnum(P_SYNC, "LFO sync", sy, 0, H, M);
        std::vector<const char *> sh(L_SHAPE, L_SHAPE + 7);
        t.addEnum(P_SHAPE, "LFO shape", sh, 0, H, M);
        t.add(P_SKEW, "LFO skew", 5, 95, 50, H | PF_INT, f_pct, M).unit = "%";
        t.add(P_PHASE, "LFO phase", 0, 360, 0, H | PF_INT, f_deg, M).unit = "deg";
        t.add(P_STEREO, "Stereo offset", 0, 180, 0, H | PF_INT, f_deg, M).unit = "deg";
        t.add(P_RISE, "Rise", 0.1, 500, 2, H, f_rise, M).unit = "ms";
        t.add(P_FALL, "Fall", 0.1, 500, 2, H, f_rise, M).unit = "ms";
        t.addEnum(P_CLOCK, "Clock", {L_CLOCK[0], L_CLOCK[1], L_CLOCK[2], L_CLOCK[3]}, 0, H, M);
        t.add(P_XOVER, "Harmonic crossover", 100, 4000, 700, H | PF_INT, f_xo, M).unit = "Hz";
        t.add(P_MIX, "Dry/wet", 0, 100, 100, H | PF_INT, f_pct, M).unit = "%";
        t.add(P_GAP, "Gap", 0, 1, 0, H | PF_INT | PF_BOOL, nullptr, M);
        std::vector<const char *> ps(L_PSTEP, L_PSTEP + NPSTEP);
        t.addEnum(P_PSTEP, "Step", ps, 2, H, M);
        t.add(P_SWING, "Swing", 0, 100, 0, H | PF_INT, f_pct, M).unit = "%";
        t.add(P_GAPLEN, "Gap length", 1, 90, 15, H | PF_INT, f_pct, M).unit = "%";
        static const char *gl[4] = {"Group 1 steps", "Group 2 steps", "Group 3 steps", "Group 4 steps"};
        for (int j = 0; j < 4; ++j) t.add(P_GLEN + j, gl[j], 1, GS, 16, H | PF_INT, f_int, M);
        static const char *ln[3] = {"Grid 2 linked to grid 1", "Grid 3 linked to grid 2", "Grid 4 linked to grid 3"};
        for (int i = 0; i < 3; ++i) t.add(P_LINK + i, ln[i], 0, 1, 1, H | PF_INT | PF_BOOL, nullptr, M);
        t.add(P_ONSET, "Onset threshold", -60, -10, -35, H, f_db1, M).unit = "dB";
        static const char *tn[4] = {"Grid 1 trim", "Grid 2 trim", "Grid 3 trim", "Grid 4 trim"};
        static const char *pn[4] = {"Grid 1 pan", "Grid 2 pan", "Grid 3 pan", "Grid 4 pan"};
        for (int g = 0; g < 4; ++g) t.add(P_TRIM + g, tn[g], -24, 12, 0, H, f_trim, M).unit = "dB";
        for (int g = 0; g < 4; ++g) t.add(P_PAN + g, pn[g], -100, 100, 0, H | PF_INT, f_pan, M);
        t.addEnum(P_AB, "A/B set", {"A", "B"}, 0, PF_HOST, M);
        t.addEnum(P_NGRID, "Grids", {L_NGRID[0], L_NGRID[1], L_NGRID[2], L_NGRID[3]}, 0, H, M);
        static const char *qn[4] = {"Grid 1 pitch", "Grid 2 pitch", "Grid 3 pitch", "Grid 4 pitch"};
        for (int g = 0; g < 4; ++g) t.add(P_PITCH + g, qn[g], -24, 24, 0, H | PF_INT, f_st, M).unit = "st";
        static const char *dn[4] = {"Grid 1 depth", "Grid 2 depth", "Grid 3 depth", "Grid 4 depth"};
        for (int g = 0; g < 4; ++g) t.add(P_GDEP + g, dn[g], 0, 100, 100, H | PF_INT, f_pct, M).unit = "%";
        t.addEnum(P_TSRC, "Tempo", {L_TSRC[0], L_TSRC[1], L_TSRC[2]}, 0, H, M);
        t.add(P_CTEMPO, "Tapped / typed tempo", 20, 300, 120, H, f_bpm, M).unit = "bpm";
        // the steps' levels (not host parameters: the window draws and edits them)
        static std::deque<std::string> names;
        for (int g = 0; g < NG; ++g) {
            uint8_t q[GS];
            for (int i = 0; i < GS; ++i) q[i] = defOn(g, i) ? 255 : 0;
            for (int w = 0; w < LVW; ++w) {
                char b[48];
                snprintf(b, sizeof b, "Grid %d steps %d-%d", g + 1, w * LVPW + 1, std::min(GS, w * LVPW + LVPW));
                names.push_back(b);
                t.add(lvId(g, w), names.back().c_str(), 0, LVMAX, packWord(q, w), PF_AB | PF_INT, f_int, M);
            }
        }
        return t;
    }();
    return t;
}

// a grid's steps for a preset: x = on (full level), . = off, 1-9 = 10%-90%; the text repeats to 64 steps
static std::string pat(int g, const char *txt)
{
    std::string s;
    for (const char *c = txt; *c; ++c)
        if (*c != ' ' && *c != '|') s += *c;
    uint8_t q[GS] = {};
    for (int i = 0; i < GS && !s.empty(); ++i) {
        char c = s[i % s.size()];
        q[i] = (c == 'x' || c == 'X') ? 255 : ((c >= '1' && c <= '9') ? (uint8_t)std::lround((c - '0') * 25.5) : 0);
    }
    std::string o;
    for (int w = 0; w < LVW; ++w) {
        char b[48];
        snprintf(b, sizeof b, " %d=%.0f", lvId(g, w), packWord(q, w));
        o += b;
    }
    return o;
}

static const std::vector<FactoryPreset> &factoryPresets()
{
    static std::vector<FactoryPreset> v = {
        // ---- instruments/guitar
        {"Instruments/Guitar/Clean: classic amp tremolo", "50=0 52=55 53=5"},
        {"Instruments/Guitar/Clean: vintage opto", "50=0 52=45 53=3.5 56=40 59=6 60=15"},
        {"Instruments/Guitar/Clean: brownface harmonic", "50=0 51=2 52=75 53=4.5"},
        {"Instruments/Guitar/Rock: surf (fast, deep)", "50=0 52=90 53=9 55=1 59=3 60=3"},
        {"Instruments/Guitar/Rock: square chop", "50=0 52=100 53=12 55=2 59=1.5 60=1.5"},
        {"Instruments/Guitar/Clean: vintage amp tremolo", "50=0 52=50 53=4.5 59=4 60=8"},
        {"Instruments/Guitar/Clean: harmonic shimmer 1/8", "50=0 51=2 52=70 54=9 58=45"},
        {"Instruments/Guitar/Clean: wide auto-pan 1/4", "50=0 51=1 52=70 54=6"},
        {"Instruments/Guitar/Funk: 16th chop", "59=0.5 60=2 64=1 67=20 65=4" + pat(0, "xx.x.xx.x.xx.x.x")},
        {"Instruments/Guitar/Funk: swung 16ths, accents", "59=0.5 60=3 65=4 66=55" + pat(0, "x6x6")},
        {"Instruments/Guitar/Funk: off-beat stabs", "59=0.5 60=2 64=1 67=30 65=4" + pat(0, ".x.x")},
        {"Instruments/Guitar/Rock: 8th note pulse", "50=0 52=70 54=9 55=2 59=4 60=8"},
        {"Instruments/Guitar/Rock: square 1/16", "50=0 52=100 54=12 55=2 59=1 60=2"},
        {"Instruments/Guitar/Rock: stereo ping-pong 1/8", "59=1 60=4 65=2 87=1 80=-100 81=100" + pat(0, "x.") + pat(1, ".x")},
        {"Instruments/Guitar/Metal: tremolo-picked chop 1/32", "59=0.3 60=0.6 65=6" + pat(0, "x.")},
        {"Instruments/Guitar/Metal: djent stutter", "59=0.3 60=0.5 64=1 67=20 65=4" + pat(0, "xxx.xx.x.xxx..x.")},
        {"Instruments/Guitar/Metal: breakdown gallop", "59=0.3 60=0.8 64=1 67=20 65=4" + pat(0, "x.xx")},
        {"Instruments/Guitar/Metal: wall of chops (stereo)", "59=0.3 60=0.6 65=6 87=1 80=-100 81=100" + pat(0, "x.") + pat(1, ".x")},
        // ---- instruments/bass
        {"Instruments/Bass/Clean: gentle pulse 1/8", "50=0 52=35 54=9 55=0 59=8 60=20"},
        {"Instruments/Bass/Distorted: 1/16 chop", "59=1 60=4 65=4"},
        {"Instruments/Bass/Punk", "92=90 59=1 60=3 65=2" + pat(0, "x.xx")},
        {"Instruments/Bass/Pop punk", "59=1 60=4 65=4" + pat(0, "x..x..x.")},
        {"Instruments/Bass/Metal", "59=0.5 60=1.5 65=4" + pat(0, "xxx.")},
        {"Instruments/Bass/Rock", "50=0 52=60 54=9 55=0"},
        {"Instruments/Bass/Jazz", "50=0 52=35 53=3.5 55=0 56=40 59=6 60=15"},
        {"Instruments/Bass/Pop", "92=70 59=1 60=120 65=2" + pat(0, ".xxx")},
        // ---- instruments/vocals
        {"Instruments/Vocals/Lead: wide slow pan (1 bar)", "50=0 51=1 52=100 54=3 57=270"},
        {"Instruments/Vocals/Lead: slow shimmer", "50=0 52=25 53=3 55=0 58=90"},
        {"Instruments/Vocals/Chop: 1/16 stutter with gap", "59=0.5 60=1 64=1 67=25 65=4" + pat(0, "x.xx")},
        // ---- instruments/keys & synths
        {"Instruments/Keys & synths/Pad: slow swell", "50=0 52=65 53=0.35"},
        {"Instruments/Keys & synths/Pad: trance gate 3+3+2", "59=1 60=4 65=4" + pat(0, "x..x..x.")},
        {"Instruments/Keys & synths/Pad: off-beat gate", "59=1 60=8 65=4" + pat(0, "..x.")},
        {"Instruments/Keys & synths/Synth: accented 16ths", "59=1 60=3 65=4" + pat(0, "x575")},
        {"Instruments/Keys & synths/Piano: staccato 8ths (gap)", "59=1 60=2 64=1 65=2 67=45" + pat(0, "x")},
        {"Instruments/Keys & synths/Electric piano: harmonic 1/8", "51=2 92=80 59=4 60=6 65=2"},
        {"Instruments/Keys & synths/Electric piano: stereo auto-pan (suitcase)", "50=0 51=1 52=80 53=4.5 59=4 60=4"},
        {"Instruments/Keys & synths/Wurlitzer: vibrato-style 5.5 Hz", "50=0 52=45 53=5.5 59=3 60=5"},
        // ---- instruments/drums
        {"Instruments/Drums/Shaker and hats: swung gate", "59=1 60=5 65=4 66=66"},
        {"Instruments/Drums/Hi-hats: auto-pan 1/16 steps", "51=1 59=6 60=6 65=4" + pat(0, "x.5.")},
        {"Instruments/Drums/Room: gated to the 8ths", "59=1 60=3 64=1 67=40 65=2" + pat(0, "x")},
        // ---- buses/instrument bus
        {"Buses/Instrument bus/Keys bus: wide stereo tremolo", "50=0 52=60 53=3.5 58=90"},
        {"Buses/Instrument bus/Synth bus: pump (duck on the beat)", "59=120 60=3 65=4 92=85" + pat(0, ".xxx")},
        // ---- buses/mixbus
        {"Buses/Mixbus/Breakdown: tempo tremolo 1/8", "50=0 52=80 54=9"},
        {"Buses/Mixbus/Build-up: fade in over 1 bar", "59=2 60=2 65=4 68=16" + pat(0, "1123445667889xxx")},
        {"Buses/Mixbus/Drop: half-time chop (1/4)", "59=1 60=6 65=0" + pat(0, "x.")},
        // ---- buses/master
        {"Buses/Master/Stutter fill 1/32", "59=0.5 60=0.5 65=6 68=32"},
        {"Buses/Master/Gentle pump (30%)", "59=150 60=3 65=4 92=30" + pat(0, ".xxx")},
        // ---- creative/multi-grid
        {"Creative/Multi-grid/Ping-pong 1-2", "59=1 60=4 65=4 87=1 80=-100 81=100" + pat(0, "x.") + pat(1, ".x")},
        {"Creative/Multi-grid/Four layers", "59=1 60=8 65=4 87=3 77=-3 78=-3 79=-6 81=-60 82=60" + pat(0, "x...") + pat(1, "..x.") +
                                       pat(2, "x.xx") + pat(3, "x..x..x.")},
        {"Creative/Multi-grid/Round-robin sweep (sequence)", "59=1 60=4 65=4 87=3 72=0 73=0 74=0 68=4 69=4 70=4 71=4 80=-100 81=-33 82=33 83=100" +
                                                         pat(0, "xx.x") + pat(1, "xx.x") + pat(2, "xx.x") + pat(3, "xx.x")},
        {"Creative/Multi-grid/Call and response (sequence)", "59=1 60=4 65=4 87=1 72=0 68=8 69=8 80=-40 81=40" + pat(0, "x.x.xx.x") +
                                                        pat(1, ".x.x..xx")},
        {"Creative/Multi-grid/Odd meter 7 + 5 (sequence)", "59=1 60=3 65=4 87=1 72=0 68=7 69=5 80=-50 81=50" + pat(0, "x.x.x.x") + pat(1, "xx.x.")},
        // ---- creative/pitched
        {"Creative/Pitched/Octave pulse", "59=1 60=4 65=4 87=1 89=12" + pat(0, "x.") + pat(1, ".x")},
        {"Creative/Pitched/Arpeggio: root, 5th, octave, 10th", "59=1 60=3 64=1 67=20 65=4 87=3 72=0 73=0 74=0 68=4 69=4 70=4 71=4 89=7 90=12 91=16" +
                                                          pat(0, "x") + pat(1, "x") + pat(2, "x") + pat(3, "x")},
        {"Creative/Pitched/Sub drop", "59=1 60=6 65=4 87=1 89=-12" + pat(0, "xxx.") + pat(1, "...x")},
        {"Creative/Pitched/Shimmer stutter (+12, +19)", "59=1 60=5 65=4 87=2 89=12 90=19 77=-3 78=-6" + pat(0, "x.x.") + pat(1, ".x..") +
                                                   pat(2, "...x")},
        {"Creative/Pitched/Whammy staircase", "59=1 60=4 65=2 87=3 72=0 73=0 74=0 68=2 69=2 70=2 71=2 89=5 90=7 91=12" + pat(0, "x") +
                                         pat(1, "x") + pat(2, "x") + pat(3, "x")},
        // ---- creative
        {"Creative/Triplet chop 1/8T", "50=0 52=100 54=11 55=2 56=60 59=1 60=3"},
        {"Creative/Ping-pong 1/8", "50=0 52=100 54=9 55=2 58=180 59=3 60=3"},
        {"Creative/Random 1/16", "50=0 52=85 54=12 55=5 59=4 60=4"},
        {"Creative/Gallop gate", "59=1 60=4 65=4" + pat(0, "x.xx")},
        {"Creative/Pattern x LFO", "50=2 54=9 60=6 65=4 92=85" + pat(0, "x..x..x.")},
        {"Creative/Long gate (64 x 1/16)", "59=1 60=4 65=4 68=64" +
                                               pat(0, "x..x..x.x..x..x. xx.xx.x.x..xx.x. x..x..x.x..x..x. xxxx.x.x.x.x..x.")},
        {"Creative/Polyrhythm (3 over 4)", "59=1 60=4 65=4 68=16" + pat(0, "x..")},
        {"Creative/Stereo scatter", "59=1 60=3 65=4 58=90 87=3 80=-100 81=100 82=-50 83=50" + pat(0, "x..x...x") + pat(1, ".x...x..") +
                                        pat(2, "..x...x.") + pat(3, "...x.x.x")},
        // ---- weird
        {"Weird/Smooth random wobble", "50=0 54=9 55=6"},
        {"Weird/Helicopter (1/64)", "59=0.3 60=0.6 65=8" + pat(0, "x.")},
        {"Weird/Ring-mod flutter (1/64 triplet LFO)", "50=0 52=100 54=18"},
        {"Weird/Morse code", "59=0.5 60=1 64=1 67=25 65=4 68=32" + pat(0, "x.x.xxx.xxx.x... xxx.x.xxx.x.....")},
        {"Weird/Tape-stop chops", "59=0.5 60=1 65=6 68=16" + pat(0, "x987654321......")},
        {"Weird/Glitch 1/64 triplet", "59=0.3 60=0.5 64=1 67=30 65=9 68=24" + pat(0, "xx.x...x.xx..x.x..xx.x..")},
        {"Weird/Broken radio (harmonic random)", "50=0 51=2 52=90 54=12 55=5 59=2 60=6"},
        {"Weird/Drunk pan", "50=0 51=1 52=100 54=6 55=6 59=30 60=30"},
    };
    return v;
}

// ---------------------------------------------------------------- the grids' groups
// Linked grids play together (a group); the groups take turns, each for its own number of steps
struct Groups {
    int ng = 1, G = 1, tot = 16;
    int gg[NG] = {}, first[NG] = {}, cnt[NG] = {}, len[NG] = {}, base[NG] = {};
    template <class F> void build(F v)
    {
        ng = 1 + iclamp((int)std::floor(v(P_NGRID) + 0.5), 0, NG - 1);
        G = 0;
        for (int g = 0; g < ng; ++g) {
            if (g == 0 || v(P_LINK + g - 1) < 0.5) { first[G] = g; cnt[G] = 0; ++G; }
            gg[g] = G - 1;
            cnt[G - 1] += 1;
        }
        tot = 0;
        for (int j = 0; j < G; ++j) {
            len[j] = iclamp((int)std::floor(v(P_GLEN + j) + 0.5), 1, GS);
            base[j] = tot;
            tot += len[j];
        }
    }
    int groupAt(int cs) const
    {
        int j = 0;
        while (j + 1 < G && cs >= base[j + 1]) ++j;
        return j;
    }
};

// ---------------------------------------------------------------- the engine
static constexpr int HN = 1024;

struct Display {
    std::atomic<float> ha[HN], hb[HN];   // the scope: the two gains every 5 ms
    std::atomic<int> hp{0};
    std::atomic<int> cs{0}, csr{0};      // the step playing (left, right; in the whole sequence)
    std::atomic<double> ph{0};           // the LFO's phase
    std::atomic<int> side{0};
    std::atomic<double> latms{0};        // the latency in use
    Display()
    {
        for (int i = 0; i < HN; ++i) { ha[i] = 1; hb[i] = 1; }
    }
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
        // the instance nonce (the JSFX's first rand in @init), once
        if (!nonceDrawn) { eel_rand(1000000); nonceDrawn = true; }
        for (int i = 0; i < HN; ++i) { V.ha[i] = 1; V.hb[i] = 1; }
        w_h5 = std::max(1.0, std::floor(srate * 0.005 + 0.5)); w_hp = 0; w_hcnt = 0;
        w_src = 0; w_mode = 0; w_dep = 0.7; w_sh = 0; w_sk = 0.5; w_pho = 0; w_sto = 0; w_clk = 0; w_crise = 0.01; w_cfall = 0.01;
        w_linc = 0; w_pinc = 0; w_tot = 16; w_spb = 4; w_sw = 0.5; w_ph = 0; w_pp = 0; w_lastbeat = -1;
        w_sL = 1; w_sR = 1; w_mixs = 1; w_mixt = 1; w_ul = true; w_up = false; w_csm = 0.01; w_cs = 0; w_csr = 0;
        w_nro = 0; w_rp = 0; w_sidx = 0; w_of = 0; w_os = 0; w_ohold = 0; w_othr = 0.01; w_fa = 0.1; w_fr = 0.01; w_sa = 0.01; w_sr2 = 0.001;
        w_xa1 = 0; w_xa2 = 0; w_xa3 = 0;
        memset(WXS, 0, sizeof WXS);
        w_lo = 0; w_hi = 0; w_ap = 0;
        w_gl = 1; w_gr = 1; w_y0 = 0; w_y1 = 0;
        WR[0] = eel_rand(1); WR[1] = eel_rand(1); WR[2] = eel_rand(1);
        for (int s = 0; s < 2; ++s)
            for (int g = 0; g < NG; ++g) { SM[s][g] = 1; PR[s][g] = 1; }
        // the pitch: the shifter, and the delays that keep the rest in time with it
        pvs.init(PvShift::sizeFor(srate));
        for (int c = 0; c < 2; ++c) DL[c].assign(pvs.size(), 0);
        RG.assign((size_t)pvs.size() * RGN, 0);
        w_lat = 0; w_dp = 0;
        V.latms.store(0, std::memory_order_relaxed);
    }
    void reset()
    {
        memset(WXS, 0, sizeof WXS);
        clearShift();
    }

    // a pitched grid (Tremolo, with the grids in use) plays through the shifter, and everything else waits for it:
    // its frame is the latency (for either A/B set, so switching between them never moves it)
    template <class F> static bool shifting(F v)
    {
        if (iclamp((int)std::floor(v(P_MODE) + 0.5), 0, 2) != 0) return false;
        if (iclamp((int)std::floor(v(P_SRC) + 0.5), 0, 2) == 0) return false;
        int ng = 1 + iclamp((int)std::floor(v(P_NGRID) + 0.5), 0, NG - 1);
        for (int g = 0; g < ng; ++g)
            if (std::lround(v(P_PITCH + g)) != 0) return true;
        return false;
    }
    static int latencyFor(const EngineHost &h, double sr)
    {
        bool on = shifting([&h](int id) { return h.pv(id); }) || shifting([&h](int id) { return h.abv(id); });
        return on ? PvShift::sizeFor(sr) : 0;
    }

    // @block, the tremolo's part (before the Act's); tr: the host's transport; midi: this block's messages
    void block(int samplesblock, const Transport &tr, const std::vector<MidiEv> &midi)
    {
        double a_now = A.now();
        ab.block(U, a_now, H.pv(P_AB) >= 0.5, samplesblock, srate);
        V.side.store(ab.side ? 1 : 0, std::memory_order_relaxed);
        w_uion = ab.uion;
        w_src = iclamp((int)std::floor(cpv(P_SRC) + 0.5), 0, 2);
        w_mode = iclamp((int)std::floor(cpv(P_MODE) + 0.5), 0, 2);
        w_dep = cpv(P_DEPTH) * 0.01;
        w_sh = iclamp((int)std::floor(cpv(P_SHAPE) + 0.5), 0, 6);
        w_sk = emax(0.05, emin(0.95, cpv(P_SKEW) * 0.01));
        w_pho = cpv(P_PHASE) / 360;
        w_sto = cpv(P_STEREO) / 360;
        w_clk = iclamp((int)std::floor(cpv(P_CLOCK) + 0.5), 0, 3);
        w_crise = 1 - std::exp(-1 / (emax(0.1, cpv(P_RISE)) * 0.001 * srate));
        w_cfall = 1 - std::exp(-1 / (emax(0.1, cpv(P_FALL)) * 0.001 * srate));
        w_csm = 1 - std::exp(-1 / (0.005 * srate));
        w_mixt = cpv(P_MIX) * 0.01;
        // what is in use: the LFO (not with the pattern alone) and the grids (not with the LFO alone)
        w_ul = w_src != 1;
        w_up = w_src != 0;
        // the clock: the tempo, the time signature, the LFO rate and the step rate
        // the project's tempo, or a tapped / typed one (then the clock runs on its own: it can't follow the song)
        int tsrc = iclamp((int)std::floor(cpv(P_TSRC) + 0.5), 0, 2);
        double tempo = tsrc == 0 ? tr.tempo : cpv(P_CTEMPO), w_tm = tempo > 0 ? tempo : 120;
        double w_bpb = (tr.tsNum > 0 && tr.tsDen > 0) ? (double)tr.tsNum * 4 / tr.tsDen : 4;
        WCPB[1] = 1 / (4 * w_bpb); WCPB[2] = 1 / (2 * w_bpb); WCPB[3] = 1 / w_bpb;
        w_sy = iclamp((int)std::floor(cpv(P_SYNC) + 0.5), 0, NSYNC - 1);
        w_linc = w_sy > 0 ? WCPB[w_sy] * w_tm / 60 / srate : cpv(P_RATE) / srate;
        w_spb = WSPB[iclamp((int)std::floor(cpv(P_PSTEP) + 0.5), 0, NPSTEP - 1)];
        w_pinc = w_spb * w_tm / 60 / srate;
        w_sw = 0.5 + 0.25 * cpv(P_SWING) * 0.01;
        // the grids: how they are grouped, every step's level, each grid's depth, trim and pan
        gr.build([this](int id) { return cpv(id); });
        w_tot = gr.tot;
        for (int g = 0; g < NG; ++g) {
            for (int w = 0; w < LVW; ++w) {
                uint64_t a = (uint64_t)std::max(0.0, std::min(LVMAX, std::floor(cpv(lvId(g, w)) + 0.5)));
                for (int i = 0; i < LVPW; ++i) {
                    int k = w * LVPW + i;
                    if (k < GS) LV[g * GS + k] = (double)((a >> (8 * i)) & 255) / 255.0;
                }
            }
            GDP[g] = cpv(P_GDEP + g) * 0.01;
            double at = epow(10, cpv(P_TRIM + g) / 20), au = cpv(P_PAN + g) * 0.01;
            WTL[g] = at * emin(1, 1 - au);
            WTR[g] = at * emin(1, 1 + au);
        }
        // the gap: which groups start and end on a step that sounds (a group's last step runs into the next group's first)
        w_gap = cpv(P_GAP) >= 0.5;
        w_gapf = 1 - cpv(P_GAPLEN) * 0.01;
        for (int j = 0; j < gr.G; ++j) {
            FIRSTON[j] = false;
            for (int g = gr.first[j]; g < gr.first[j] + gr.cnt[j]; ++g)
                if (LV[g * GS] > 0) FIRSTON[j] = true;
        }
        // the clock follows the song when it plays (and the position moved), otherwise it runs on by itself
        double beat = tr.beat;
        bool w_bchg = tsrc == 0 && tr.playing && beat != w_lastbeat;
        w_lastbeat = beat;
        if (w_pp >= w_tot) w_pp -= w_tot * std::floor(w_pp / w_tot);
        if (w_clk == 0 && w_bchg) {
            double t = beat * w_spb;
            w_pp = t - w_tot * std::floor(t / w_tot);
            if (w_sy > 0) { t = beat * WCPB[w_sy]; w_ph = t - std::floor(t); }
        }
        // Harmonic: the crossover (a second-order low-pass; the highs are what it leaves)
        double t = std::tan(PI * std::min(cpv(P_XOVER), srate * 0.45) / srate);
        w_xa1 = 1 / (1 + t * (t + w_xk));
        w_xa2 = t * w_xa1;
        w_xa3 = t * w_xa2;
        // restart on an onset: the detector's two followers and its level
        w_fa = 1 - std::exp(-1 / (0.0003 * srate));
        w_fr = 1 - std::exp(-1 / (0.015 * srate));
        w_sa = 1 - std::exp(-1 / (0.020 * srate));
        w_sr2 = 1 - std::exp(-1 / (0.150 * srate));
        w_othr = epow(10, cpv(P_ONSET) / 20);
        // MIDI: a note-on restarts the clock when asked (everything passes through: the plug-in does that)
        w_nro = 0; w_rp = 0; w_sidx = 0;
        for (const auto &m : midi)
            if ((m.d[0] & 240) == 144 && m.len >= 3 && m.d[2] > 0 && w_nro < 63) WRO[w_nro++] = (int)m.time;
        // the pitch: only while the latency is there for it
        int lat = latencyFor(H, srate);
        if (lat != w_lat) {
            w_lat = lat;
            clearShift();
        }
        for (int g = 0; g < NG; ++g) {
            long st = (w_mode == 0 && w_up && g < gr.ng && w_lat > 0) ? std::lround(cpv(P_PITCH + g)) : 0;
            st = std::max(-24L, std::min(24L, st));
            w_shift[g] = st != 0;
            pvs.setRatio(g, st != 0 ? std::pow(2.0, st / 12.0) : 1.0);
        }
        V.latms.store(w_lat * 1000.0 / srate, std::memory_order_relaxed);
    }

    // after the Act's @block: the instance ID's draw (the JSFX's a_new_id), so random shapes follow the same sequence
    void afterAct()
    {
        if (!idDrawn && A.myid != 0) { eel_rand(899999); idDrawn = true; }
    }

    // @sample, the tremolo's part (before the Act)
    void samples(const double *const *in, double *const *out, int n)
    {
        if ((int)br0.size() < n) { br0.resize(n); br1.resize(n); }
        double GV[RGN];
        for (int i = 0; i < n; ++i) {
            double d0 = in[0][i], d1 = in[1][i];
            br0[i] = d0; br1[i] = d1;
            // a restart: a MIDI note on this sample, or an onset in the audio
            bool rs = false;
            if (w_clk == 2) {
                while (w_rp < w_nro && WRO[w_rp] <= w_sidx) { rs = true; w_rp += 1; }
            } else if (w_clk == 3) {
                double ka = emax(std::fabs(d0), std::fabs(d1));
                w_of += (ka > w_of ? w_fa : w_fr) * (ka - w_of);
                w_os += (ka > w_os ? w_sa : w_sr2) * (ka - w_os);
                if (w_ohold > 0) w_ohold -= 1;
                if (w_ohold <= 0 && w_of > w_othr && w_of > w_os * 2) { rs = true; w_ohold = std::floor(0.08 * srate); }
            }
            w_sidx += 1;
            if (rs) { w_ph = 0; w_pp = 0; }
            // the clocks
            w_ph += w_linc;
            if (w_ph >= 1) {
                w_ph -= std::floor(w_ph);
                WR[2] = WR[1]; WR[1] = WR[0]; WR[0] = eel_rand(1);
            }
            w_pp += w_pinc;
            if (w_pp >= w_tot) w_pp -= w_tot * std::floor(w_pp / w_tot);
            // the LFO on each side, 0..1 (1 = full level)
            double mL = 1, mR = 1;
            if (w_ul) {
                if (w_sh < 5) {
                    double xl = w_ph + w_pho;
                    xl -= std::floor(xl);
                    double xr = xl + w_sto;
                    xr -= std::floor(xr);
                    mL = shape(w_sh, skew(xl, w_sk));
                    mR = shape(w_sh, skew(xr, w_sk));
                } else if (w_sh == 5) {
                    mL = WR[0];
                    mR = w_ph >= w_sto ? WR[0] : WR[1];
                } else {
                    mL = WR[1] + (WR[0] - WR[1]) * ss(w_ph);
                    double xr = w_ph - w_sto;
                    mR = xr >= 0 ? WR[1] + (WR[0] - WR[1]) * ss(xr) : WR[2] + (WR[1] - WR[2]) * ss(xr + 1);
                }
            }
            for (int k = 0; k < RGN; ++k) GV[k] = 0;
            if (!w_up) {
                // the LFO alone: one rise and fall per side, the global depth
                w_sL += (mL > w_sL ? w_crise : w_cfall) * (mL - w_sL);
                w_sR += (mR > w_sR ? w_crise : w_cfall) * (mR - w_sR);
                if (w_mode == 0) {
                    GV[0] = 1 - w_dep * (1 - w_sL); GV[4] = 1 - w_dep * (1 - w_sR);
                } else if (w_mode == 1) {
                    // auto-pan: constant power, the position follows the left side's modulation
                    double pan = 0.5 + w_dep * (w_sL - 0.5);
                    GV[0] = std::cos(pan * 0.5 * PI); GV[1] = std::sin(pan * 0.5 * PI);
                } else {
                    GV[0] = 1 - w_dep * (1 - w_sL); GV[1] = 1 - w_dep * w_sL;
                    GV[2] = 1 - w_dep * (1 - w_sR); GV[3] = 1 - w_dep * w_sR;
                }
                GV[8] = 0; GV[9] = 0;
            } else {
                // the grids, on each side (the right one offset by the Stereo amount of the whole sequence)
                for (int s = 0; s < 2; ++s) {
                    double p = w_pp;
                    if (s == 1) {
                        p = w_pp + w_sto * w_tot;
                        if (p >= w_tot) p -= w_tot;
                    }
                    // the step: pairs of steps share a beat-half, swing moves the second one later; f: how far into it
                    double pq = p * 0.5, pa = std::floor(pq), fr = pq - pa, cs, f;
                    if (fr < w_sw) { cs = pa * 2; f = fr / w_sw; }
                    else { cs = pa * 2 + 1; f = (fr - w_sw) / (1 - w_sw); }
                    if ((w_tot & 1) && cs >= w_tot - 1) { cs = w_tot - 1; f = p - (w_tot - 1); }   // an odd length's last step
                    int ics = (int)std::min((double)w_tot - 1, cs);
                    int j = gr.groupAt(ics), k = ics - gr.base[j];
                    double lfo = w_src == 2 ? (s ? mR : mL) : 1;
                    for (int g = 0; g < gr.ng; ++g) {
                        bool act = gr.gg[g] == j;
                        double v = act ? LV[g * GS + k] : 0;
                        // the gap: the end of a step that runs into another one that sounds
                        if (w_gap && v > 0 && f > w_gapf) {
                            bool nx = k + 1 < gr.len[j] ? LV[g * GS + k + 1] > 0 : FIRSTON[(j + 1) % gr.G];
                            if (nx) v = 0;
                        }
                        double m = v * lfo, pr = act ? 1 : 0;
                        double &S = SM[s][g], &Q = PR[s][g];
                        S += (m > S ? w_crise : w_cfall) * (m - S);
                        Q += (pr > Q ? w_crise : w_cfall) * (pr - Q);
                    }
                    if (s == 0) w_cs = ics;
                    else w_csr = ics;
                }
                if (w_mode == 0) {
                    // each grid: its gate (its depth), faded in and out with its group's turn
                    for (int g = 0; g < gr.ng; ++g) {
                        GV[g] = PR[0][g] * (1 - GDP[g] * (1 - SM[0][g]));
                        GV[4 + g] = PR[1][g] * (1 - GDP[g] * (1 - SM[1][g]));
                    }
                } else if (w_mode == 1) {
                    // auto-pan: the playing grids' positions, averaged (the left side's)
                    double num = 0, den = 0;
                    for (int g = 0; g < gr.ng; ++g) {
                        num += PR[0][g] * (0.5 + GDP[g] * (SM[0][g] - 0.5));
                        den += PR[0][g];
                    }
                    double pan = den > 1e-9 ? num / den : 0.5;
                    GV[0] = std::cos(pan * 0.5 * PI); GV[1] = std::sin(pan * 0.5 * PI);
                } else {
                    // harmonic: the playing grids' levels for the lows and the highs, averaged
                    for (int s = 0; s < 2; ++s) {
                        double lo = 0, hi = 0, den = 0;
                        for (int g = 0; g < gr.ng; ++g) {
                            lo += PR[s][g] * (1 - GDP[g] * (1 - SM[s][g]));
                            hi += PR[s][g] * (1 - GDP[g] * SM[s][g]);
                            den += PR[s][g];
                        }
                        GV[2 * s] = den > 1e-9 ? lo / den : 1;
                        GV[2 * s + 1] = den > 1e-9 ? hi / den : 1;
                    }
                }
                GV[8] = w_cs; GV[9] = w_csr;
            }
            // with the latency (a pitched grid, in either A/B set): the sound comes out of the shifter, or a delay as
            // long, w_lat samples later, and the levels wait as long
            if (w_lat > 0) {
                pvs.push(d0, d1);
                double x0 = DL[0][w_dp], x1 = DL[1][w_dp];
                DL[0][w_dp] = d0; DL[1][w_dp] = d1;
                double *rg = &RG[(size_t)w_dp * RGN];
                for (int k = 0; k < RGN; ++k) std::swap(rg[k], GV[k]);
                if (++w_dp >= w_lat) w_dp = 0;
                d0 = x0; d1 = x1;
                br0[i] = x0; br1[i] = x1;
            }
            w_shownL = (int)GV[8]; w_shownR = (int)GV[9];
            // the gains
            if (w_mode == 0) {
                if (!w_up) {
                    w_gl = GV[0]; w_gr = GV[4];
                    w_y0 = d0 * w_gl; w_y1 = d1 * w_gr;
                } else {
                    // each grid is a gate on the whole sound, with its own trim, pan and pitch; they add up
                    w_gl = 0; w_gr = 0; w_y0 = 0; w_y1 = 0;
                    for (int g = 0; g < gr.ng; ++g) {
                        double a = GV[g] * WTL[g], b = GV[4 + g] * WTR[g];
                        w_gl += a;
                        w_gr += b;
                        if (w_shift[g]) { w_y0 += pvs.out(g, 0) * a; w_y1 += pvs.out(g, 1) * b; }
                        else { w_y0 += d0 * a; w_y1 += d1 * b; }
                    }
                }
            } else if (w_mode == 1) {
                w_gl = GV[0]; w_gr = GV[1];
                w_y0 = d0 * w_gl; w_y1 = d1 * w_gr;
            } else {
                // harmonic: the lows and the highs (adding back to the input's allpass) in opposition
                split(d0, 0);
                w_gl = GV[0]; w_gr = GV[1];
                w_y0 = w_lo * GV[0] + w_hi * GV[1];
                split(d1, 4);
                w_y1 = w_lo * GV[2] + w_hi * GV[3];
            }
            w_mixs += w_csm * (w_mixt - w_mixs);
            out[0][i] = d0 + w_mixs * (w_y0 - d0);
            out[1][i] = d1 + w_mixs * (w_y1 - d1);
            // the scope (only while the window is open): the two gains every 5 ms
            if (w_uion) {
                if ((w_hcnt += 1) >= w_h5) {
                    w_hcnt = 0;
                    V.ha[w_hp].store((float)w_gl, std::memory_order_relaxed);
                    V.hb[w_hp].store((float)w_gr, std::memory_order_relaxed);
                    w_hp = (w_hp + 1) & (HN - 1);
                    V.hp.store(w_hp, std::memory_order_release);
                }
            }
        }
        V.cs.store(w_shownL, std::memory_order_relaxed);
        V.csr.store(w_shownR, std::memory_order_relaxed);
        V.ph.store(w_ph, std::memory_order_relaxed);
    }

    void post(double *const *out, int n)
    {
        for (int i = 0; i < n; ++i) ab.post(br0[i], br1[i], out[0][i], out[1][i], br0[i], br1[i]);
    }

    // the shapes, 0..1 (1 = full level), at phase x; the skew bends the phase
    static double skew(double x, double s) { return x < s ? 0.5 * x / s : 0.5 + 0.5 * (x - s) / (1 - s); }
    static double shape(int t, double x)
    {
        return t == 0 ? 0.5 + 0.5 * std::cos(2 * PI * x)
                      : (t == 1 ? (x < 0.5 ? 1 - 2 * x : 2 * x - 1) : (t == 2 ? (x < 0.5 ? 1 : 0) : (t == 3 ? x : 1 - x)));
    }
    static double ss(double t) { return t * t * (3 - 2 * t); }

private:
    static constexpr int RGN = 10;   // what waits with the latency: eight gains and the two steps shown
    EngineHost &H;
    Act &A;
    UiLink &U;
    Display &V;
    double srate = 44100;
    bool nonceDrawn = false, idDrawn = false;
    std::vector<double> br0, br1;
    double WCPB[NSYNC] = {0, 0, 0, 0, 1.0 / 2, 3.0 / 4, 1, 2.0 / 3, 3.0 / 2, 2, 4.0 / 3, 3, 4, 8.0 / 3, 6, 8, 12, 16, 24};
    const double WSPB[NPSTEP] = {1, 1.5, 2, 3, 4, 6, 8, 12, 16, 24};
    double WTL[NG] = {}, WTR[NG] = {}, WR[4] = {};
    int WRO[64] = {};
    double WXS[8] = {};
    // the grids
    Groups gr;
    double LV[NG * GS] = {}, GDP[NG] = {}, SM[2][NG] = {}, PR[2][NG] = {};
    bool FIRSTON[NG] = {}, w_gap = false;
    double w_gapf = 0.85;
    int w_tot = 16, w_shownL = 0, w_shownR = 0;
    // the pitch
    PvShift pvs;
    std::vector<double> DL[2], RG;   // the input, and what waits with it
    int w_lat = 0, w_dp = 0;
    bool w_shift[NG] = {};
    void clearShift()
    {
        pvs.reset();
        for (int c = 0; c < 2; ++c) std::fill(DL[c].begin(), DL[c].end(), 0.0);
        std::fill(RG.begin(), RG.end(), 0.0);
        w_dp = 0;
    }
    double w_h5 = 1, w_hcnt = 0;
    int w_hp = 0;
    int w_src = 0, w_mode = 0, w_sh = 0, w_clk = 0, w_sy = 0, w_nro = 0, w_rp = 0;
    double w_dep = 0.7, w_sk = 0.5, w_pho = 0, w_sto = 0, w_crise = 0.01, w_cfall = 0.01;
    double w_linc = 0, w_pinc = 0, w_spb = 4, w_sw = 0.5, w_ph = 0, w_pp = 0, w_lastbeat = -1;
    double w_sL = 1, w_sR = 1, w_mixs = 1, w_mixt = 1, w_csm = 0.01, w_cs = 0, w_csr = 0;
    bool w_ul = true, w_up = false, w_uion = false;
    double w_sidx = 0, w_of = 0, w_os = 0, w_ohold = 0, w_othr = 0.01, w_fa = 0.1, w_fr = 0.01, w_sa = 0.01, w_sr2 = 0.001;
    double w_xa1 = 0, w_xa2 = 0, w_xa3 = 0;
    const double w_xk = 1.4142135623730951;
    double w_lo = 0, w_hi = 0, w_ap = 0, w_gl = 1, w_gr = 1, w_y0 = 0, w_y1 = 0;

    double cpv(int p) const
    {
        const ParamDef *d = table().get(p);
        return (ab.swh && d && (d->flags & PF_AB)) ? H.abv(p) : H.pv(p);
    }
    // Harmonic: one channel through two second-order low-passes in a row (the lows) and what is left (the highs);
    // o = 0 left, 4 right. The allpass of the first stage (x - 2 k v1) minus the lows is the highs.
    void split(double x, int o)
    {
        double v3 = x - WXS[o + 1];
        double v1 = w_xa1 * WXS[o] + w_xa2 * v3;
        double v2 = WXS[o + 1] + w_xa2 * WXS[o] + w_xa3 * v3;
        WXS[o] = 2 * v1 - WXS[o];
        WXS[o + 1] = 2 * v2 - WXS[o + 1];
        w_ap = x - 2 * w_xk * v1;
        v3 = v2 - WXS[o + 3];
        v1 = w_xa1 * WXS[o + 2] + w_xa2 * v3;
        v2 = WXS[o + 3] + w_xa2 * WXS[o + 2] + w_xa3 * v3;
        WXS[o + 2] = 2 * v1 - WXS[o + 2];
        WXS[o + 3] = 2 * v2 - WXS[o + 3];
        w_lo = v2;
        w_hi = w_ap - v2;
    }
};

// ---------------------------------------------------------------- the plug-in
class TremoloPlugin : public Plugin, public EngineHost {
public:
    explicit TremoloPlugin(const clap_host *h) : Plugin(h, kind()), eng(*this, act, uiLink, view) {}
    UiLink uiLink;
    Display view;
    Engine eng;
    double pv(int id) const override { return P.get(id); }
    double abv(int id) const override { return abValue(id); }
    gui::View *createView() override;
    double tempoNow() const { return lastTempo.load(std::memory_order_relaxed); }
    bool playingNow() const { return lastPlaying.load(std::memory_order_relaxed); }

protected:
    std::atomic<double> lastTempo{0};
    std::atomic<bool> lastPlaying{false};
    void dspActivate(double sr, uint32_t) override { eng.init(sr); }
    void dspReset() override { eng.reset(); }
    void dspProcess(const double *const *in, const double *const *, double *const *out, int n) override
    {
        lastTempo.store(P.get(P_TSRC) >= 0.5 ? P.get(P_CTEMPO) : tr.tempo, std::memory_order_relaxed);
        lastPlaying.store(tr.playing, std::memory_order_relaxed);
        eng.block(n, tr, midiIn);
        act.block(n);
        eng.afterAct();
        eng.samples(in, out, n);
        act.process(out, 2, n);
        eng.post(out, n);
        act.audioKey(out, 2, n);
    }
    int latencyFromParams(double sr) const override { return Engine::latencyFor(*this, sr); }
};

static Plugin *create(const clap_host *h) { return new TremoloPlugin(h); }

// ---------------------------------------------------------------- the window
class TremView : public EffectView {
public:
    explicit TremView(TremoloPlugin &p) : EffectView(p, p.uiLink, 940, 750, "TREMOLO"), T(p)
    {
        help = {"No latency, unless a Tremolo grid is pitched: then the shifter's frame (4096 samples at 44.1 / 48 kHz), and all of it waits.",
                "Grids: click a box to switch it, drag across to paint, drag up or down on one to set its level. Linked grids play together;",
                "unlinked ones take turns, each group with its own steps. The Gap reaches silence when it is over 2.3 x the Fall (90%)."};
    }
    void frame(Ui &ui) override;

protected:
    void hashMore(std::function<void(uint64_t)> mix) override
    {
        mix((uint64_t)(T.tempoNow() * 10));
        mix(T.playingNow());
        mix((uint64_t)dmode);
        mix(std::hash<std::string>()(clockText));
    }
    bool extraFull() override { return dmode != 0; }

private:
    TremoloPlugin &T;
    double LV[NG * GS] = {};
    Groups gr;
    std::string clockText;
    // the mouse on the boxes: 1 pressed (not yet a paint or a level), 2 painting across, 3 setting one box's level
    int dmode = 0, dgrid = 0, dstep = 0;
    float dx0 = 0, dy0 = 0, dsens = 60;
    double dlv0 = 0, paint = 0;
    bool dirty = false;

    int mode() const { return iclamp(iround(T.get(P_MODE)), 0, 2); }
    int src() const { return iclamp(iround(T.get(P_SRC)), 0, 2); }
    int gs() const { return (iround(T.get(P_PSTEP)) % 2) == 1 ? 3 : 4; }   // boxes per beat group: 3 for a triplet step
    void load()
    {
        for (int g = 0; g < NG; ++g)
            for (int w = 0; w < LVW; ++w) {
                uint64_t a = (uint64_t)std::max(0.0, std::min(LVMAX, std::floor(T.get(lvId(g, w)) + 0.5)));
                for (int i = 0; i < LVPW; ++i) {
                    int k = w * LVPW + i;
                    if (k < GS) LV[g * GS + k] = (double)((a >> (8 * i)) & 255) / 255.0;
                }
            }
    }
    void store(int g, bool gesture)
    {
        uint8_t q[GS];
        for (int i = 0; i < GS; ++i) q[i] = (uint8_t)std::lround(std::max(0.0, std::min(1.0, LV[g * GS + i])) * 255);
        for (int w = 0; w < LVW; ++w) {
            double v = packWord(q, w);
            if (gesture) T.setOnce(lvId(g, w), v);
            else if (v != T.get(lvId(g, w))) T.edit(lvId(g, w), v);
        }
    }
    void beginGrid(int g) { for (int w = 0; w < LVW; ++w) T.beginEdit(lvId(g, w)); }
    void endGrid(int g) { for (int w = 0; w < LVW; ++w) T.endEdit(lvId(g, w)); }
    // does step k of grid g run into another that sounds (the gap is drawn there)
    bool runsOn(int g, int k) const
    {
        int j = gr.gg[g];
        if (LV[g * GS + k] <= 0) return false;
        if (k + 1 < gr.len[j]) return LV[g * GS + k + 1] > 0;
        int nj = (j + 1) % gr.G;
        for (int h = gr.first[nj]; h < gr.first[nj] + gr.cnt[nj]; ++h)
            if (LV[h * GS] > 0) return true;
        return false;
    }
    void fill(int g, int k);
    void tools(Ui &ui, float x, float y, int g);
    Col laneCol(int g) const
    {
        return g == 0 ? th::orange() : (g == 1 ? th::purple() : (g == 2 ? Col::hex(0x40c7b0) : Col::hex(0xee6b9e)));
    }
    void header(Ui &ui, float y);
    void gridRow(Ui &ui, int g, float y, float h);
    void boxes(Ui &ui, int g, float x0, float y0, float w, float bw, float bh, int rows);
    static constexpr float AH = 266;   // the grids' area (or the LFO's cycle)
    std::vector<double> taps;          // Tap tempo: when the box was tapped
    void scope(Ui &ui, const R &r);
    void preview(Ui &ui, const R &r);
    void controls(Ui &ui, float y);
};

gui::View *TremoloPlugin::createView() { return new TremView(*this); }

// the pattern tools: k as in the menu
void TremView::fill(int g, int k)
{
    int s = gs(), n = gr.len[gr.gg[g]], o = g * GS;
    double *L = LV + o;
    if (k == 3) { for (int i = 0; i < n; ++i) L[i] = L[i] > 0 ? 0 : 1; return; }
    if (k == 4) { double t = L[0]; for (int i = 0; i < n - 1; ++i) L[i] = L[i + 1]; L[n - 1] = t; return; }
    if (k == 5) { double t = L[n - 1]; for (int i = n - 1; i > 0; --i) L[i] = L[i - 1]; L[0] = t; return; }
    if (k >= 13 && k <= 17) {
        // the levels of the steps that sound
        for (int i = 0; i < n; ++i) {
            if (L[i] <= 0) continue;
            if (k == 13) L[i] = (i % s) == 0 ? 1 : 0.6;
            if (k == 14) L[i] = (double)(i + 1) / n;
            if (k == 15) L[i] = (double)(n - i) / n;
            if (k == 16) L[i] = 0.3 + 0.7 * eel_rand(1);
            if (k == 17) L[i] = 1;
        }
        return;
    }
    if (k == 18) {
        for (int h = 0; h < NG; ++h)
            if (h != g)
                for (int i = 0; i < GS; ++i) LV[h * GS + i] = L[i];
        return;
    }
    for (int i = 0; i < GS; ++i) {
        int v = 1;
        if (k == 2) v = 0;
        if (k == 6) v = eel_rand(1) < 0.5;
        if (k == 7) v = (i % 2) == 0;
        if (k == 8) v = (i % s) == 0;
        if (k == 9) v = (i % s) == s / 2;
        if (k == 10) v = (i % s) != 1;
        if (k == 11) v = s == 3 ? ((i % 12) % 4) == 0 : ((i % 8) == 0 || (i % 8) == 3 || (i % 8) == 6);
        if (k == 12) v = (i % s) != 0;
        L[i] = v;
    }
}

void TremView::tools(Ui &ui, float x, float y, int g)
{
    static const char *items[] = {"All on", "Clear", "Invert", "Shift left", "Shift right", "Random", "Every 2nd",
                                  "On the beat (first of each group)", "Off-beats", "Gallop", "3 + 3 + 2 (4 + 4 + 4 in triplets)",
                                  "Pump (all but the first of each group)", "Accent the beats (others 60%)", "Levels: ramp up",
                                  "Levels: ramp down", "Levels: random", "Levels: all 100%", "Copy to the other grids"};
    std::vector<MenuItem> m;
    for (int i = 0; i < 18; ++i) {
        if (i == 12 || i == 17) m.push_back(MenuItem::sep());
        m.push_back(MenuItem::item(items[i], i + 1));
    }
    TremView *self = this;
    ui.menu(m, x, y, [self, g](int r) {
        if (r <= 0) return;
        self->load();
        self->fill(g, r);
        if (r == 18) { for (int h = 0; h < NG; ++h) self->store(h, true); }
        else self->store(g, true);
    }, 300);
}

// one grid's boxes in a width w from x0, in one row or two (more than 32 steps)
void TremView::boxes(Ui &ui, int g, float x0, float y0, float w, float bw, float bh, int rows)
{
    int j = gr.gg[g], len = gr.len[j], s = gs();
    // left-aligned, the same spacing in every grid: a step sits at the same place in each (two rows: 1-32 and 33-64)
    (void)w;
    int per = rows == 1 ? len : 32;
    float gx = x0;
    int o = g * GS;
    int cs = T.view.cs.load(), csr = T.view.csr.load();
    bool offs = T.get(P_STEREO) > 0.5;
    bool gap = T.get(P_GAP) >= 0.5;
    float gapw = (float)(T.get(P_GAPLEN) * 0.01);
    Col lc = laneCol(g);
    for (int r = 0; r < rows; ++r) {
        float by = y0 + r * (bh + 6);
        for (int c = 0; c < std::min(per, len - r * per); ++c) {
            int ix = r * per + c;
            float bx = gx + c * (bw + 3) + (c / s) * 6;
            R b(bx, by, bw, bh);
            double lv = LV[o + ix];
            bool on = lv > 0;
            bool inb = ui.hitRaw(R(bx - 1.5f, by, bw + 3, bh));
            if (ui.drawS()) {
                bool hot = inb && !ui.busy() && (dmode == 0 || dgrid == g);
                ui.fill(b, Paint(hot ? th::hover() : ((c / s) % 2 ? Col::hex(0x19181c) : Col::hex(0x131215))), 4);
                if (on) {
                    // the level: the color fills the box from the bottom
                    float fh = std::max(2.f, (float)(bh * lv));
                    R f(bx, by + bh - fh, bw, fh);
                    ui.fill(f, Paint::vertical(f.y, lc.lighter(hot ? 0.25f : 0.1f), f.b(), lc.darker(0.15f)), lv > 0.97 ? 4 : 2);
                    // the gap: where the step goes quiet before the next one
                    if (gap && runsOn(g, ix)) {
                        float gx0 = bx + bw * (1 - gapw);
                        ui.fill(R(gx0, f.y, bx + bw - gx0, fh), Paint(Col(0.03f, 0.03f, 0.04f, 0.72f)), 2);
                    }
                }
                ui.stroke(b, Paint(on ? lc.lighter(0.3f).alpha(0.4f + 0.6f * (float)lv) : th::border()), 4);
                if ((c % s) == 0 && bw >= 14 && bh >= 16) {
                    char t[12];
                    snprintf(t, sizeof t, "%d", ix + 1);
                    ui.text(R(bx + 3, by + 3, bw - 4, 11), t, lv > 0.85 ? th::orangeInk() : th::faint(), F_MEDIUM, 9.5f);
                }
            }
            // the step playing now (and the right side's, when it is offset)
            if (ui.drawD() && live) {
                if (cs - gr.base[j] == ix && gr.groupAt(cs) == j)
                    ui.stroke(R(bx - 1.5f, by - 1.5f, bw + 3, bh + 3), Paint(Col(1, 1, 1, 0.95f)), 5, 1.5f);
                if (offs && csr - gr.base[j] == ix && gr.groupAt(csr) == j) ui.fill(R(bx, by + bh + 2, bw, 2.5f), Paint(th::purple()), 1.2f);
            }
            if (!ui.drawS()) continue;
            // the mouse: a click switches the box, a drag across paints, a drag up or down sets this box's level
            if (inb && ui.lclick && !ui.used && !ui.busy()) {
                ui.used = true;
                dmode = 1;
                dgrid = g;
                dstep = ix;
                dx0 = ui.mx();
                dy0 = ui.my();
                dlv0 = lv;
                dsens = std::max(60.f, bh);
                paint = on ? 0 : 1;
                beginGrid(g);
            }
            if (dmode == 2 && dgrid == g && inb && ui.ldown && LV[o + ix] != paint) {
                LV[o + ix] = paint;
                dirty = true;
            }
            if (dmode == 1 && dgrid == g && inb && ix != dstep && ui.ldown) {
                dmode = 2;
                LV[o + dstep] = paint;
                LV[o + ix] = paint;
                dirty = true;
            }
        }
    }
}

// a grid's row: its color, its controls (Depth; Trim, Pan and Pitch in Tremolo), its tools, its boxes
void TremView::gridRow(Ui &ui, int g, float y, float h)
{
    int md = mode();
    float cy = y + h / 2;
    if (ui.drawS()) {
        Col lc = laneCol(g);
        ui.fill(R(M + 9, cy - 6, 12, 12), Paint(lc), 6);
        float l1 = cy - 25, l2 = cy + 1;
        label(ui, R(M + 34, l1, 40, 24), "Depth");
        ui.slideBox(R(M + 74, l1, 58, 24), P_GDEP + g, SC_LIN, lc, false, true, 1, 1);
        if (md == 0) {
            label(ui, R(M + 140, l1, 32, 24), "Trim");
            ui.slideBox(R(M + 172, l1, 64, 24), P_TRIM + g, SC_LIN, lc, true, true, 0.1, 0.01);
            label(ui, R(M + 34, l2, 40, 24), "Pan");
            ui.slideBox(R(M + 74, l2, 58, 24), P_PAN + g, SC_LIN, lc, true, true, 1, 0.1);
            label(ui, R(M + 140, l2, 32, 24), "Pitch");
            ui.slideBox(R(M + 172, l2, 64, 24), P_PITCH + g, SC_LIN, lc, true, true, 1, 1);
        }
        R tb(M + 242, l1, 24, 24);
        if (ui.button(tb, "", false, true, BS_NORMAL, IC_CHEVRON)) tools(ui, tb.x, tb.b() + 4, g);
    }
    // the boxes: never bigger than with four grids of 16 steps; the longest group sets the size for all (up to 16,
    // up to 32, or two rows of 32), from the left (each step in the same place in every grid), centered vertically
    int len = gr.len[gr.gg[g]], mx = 1;
    for (int j = 0; j < gr.G; ++j) mx = std::max(mx, gr.len[j]);
    int ref = mx <= 16 ? 16 : 32, s = gs();
    bool two = mx > 32;
    const float rh4 = (AH - 34) / 4, x0 = M + 276, w = DW - M - x0;
    float bh = two ? (rh4 - 16) / 2 : rh4 - 10;
    int grp = (ref + s - 1) / s;
    float bw = std::min(56.f, (w - (ref - 1) * 3 - (grp - 1) * 6) / ref);
    int rows = (two && len > 32) ? 2 : 1;
    float th = rows * bh + (rows - 1) * 6;
    boxes(ui, g, x0, cy - th / 2, w, bw, bh, rows);
}

// above the grids: how many, the step, the swing, the gap, and each group's steps
void TremView::header(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    enumDD(ui, R(M, y, 100, 26), P_NGRID, L_NGRID, 4, "Grids");
    enumDD(ui, R(M + 108, y, 150, 26), P_PSTEP, L_PSTEP, NPSTEP, "Step");
    label(ui, R(M + 268, y, 40, 26), "Swing");
    ui.valueBox(R(M + 306, y + 2, 56, 22), P_SWING, true, KD_OTHER, 1, 0.1);
    toggle(ui, R(M + 372, y, 50, 26), "Gap", P_GAP);
    if (T.get(P_GAP) >= 0.5) ui.valueBox(R(M + 428, y + 2, 56, 22), P_GAPLEN, true, KD_OTHER, 1, 1);
    label(ui, R(M + 500, y, 40, 26), "Steps");
    for (int j = 0; j < gr.G; ++j) {
        char t[16];
        int a = gr.first[j] + 1, b = gr.first[j] + gr.cnt[j];
        if (a == b) snprintf(t, sizeof t, "%d", a);
        else snprintf(t, sizeof t, "%d-%d", a, b);
        float x = M + 542 + j * 94;
        if (gr.G > 1) ui.text(R(x, y, 34, 26), t, th::dim(), F_SEMIBOLD, 11.5f, 2);
        ui.slideBox(R(x + 38, y + 2, 50, 22), P_GLEN + j, SC_LIN, laneCol(gr.first[j]), false, true, 1, 1, 5);
    }
}

// the scope: what the tremolo did to the level over the last five seconds (left / lows orange, right / highs purple)
void TremView::scope(Ui &ui, const R &r)
{
    int md = mode();
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        float y1 = r.y + 24, yh = r.h - 32;
        for (int k = 0; k < 3; ++k) {
            float v = 1 - k * 0.5f, yy = y1 + yh * (1 - v);
            ui.fill(R(r.x, yy, r.w, 1 / ui.s), Paint(k == 0 ? th::gridHi() : th::grid()));
            char s[8];
            snprintf(s, sizeof s, "%.0f%%", v * 100);
            ui.text(R(r.x + 6, v > 0.01f ? yy + 3 : yy - 13, 40, 11), s, th::faint(), F_REGULAR, 9.5f);
        }
        ui.text(R(r.x + 10, r.y + 6, 200, 12), "level, last 5 seconds", th::faint(), F_REGULAR, 10.5f);
        ui.text(R(r.x + 170, r.y + 6, r.w - 340, 12), clockText.c_str(), th::dim(), F_REGULAR, 10.5f, 1);
        float lx = r.r() - 150;
        ui.fill(R(lx, r.y + 9, 8, 3), Paint(th::orange()), 1.5f);
        ui.text(R(lx + 12, r.y + 5, 50, 12), md == 2 ? "lows" : "left", th::dim(), F_REGULAR, 10.5f);
        ui.fill(R(lx + 62, r.y + 9, 8, 3), Paint(th::purple()), 1.5f);
        ui.text(R(lx + 74, r.y + 5, 50, 12), md == 2 ? "highs" : "right", th::dim(), F_REGULAR, 10.5f);
        ui.stroke(r, Paint(th::border()), 6);
    }
    if (!ui.drawD()) return;
    Canvas &c = ui.cv();
    R in(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    c.pushClip(ui.X(in.x), ui.Y(in.y), ui.W(in.w), ui.W(in.h));
    int hp = T.view.hp.load(std::memory_order_acquire);
    int px0 = (int)std::floor(ui.X(in.x)), px1 = (int)std::ceil(ui.X(in.r()));
    int np = px1 - px0;
    float per = 1000.f / np;   // the 1000 newest entries across the width
    float y1 = ui.Y(r.y + 24), yh = ui.W(r.h - 32), yb = y1 + yh;
    std::vector<float> tops(np);
    std::vector<Pt> la(np), lb(np);
    for (int i = 0; i < np; ++i) {
        int e = (hp - 1000 + (int)(i * per)) & (HN - 1);
        float a = std::max(0.f, std::min(1.f, T.view.ha[e].load(std::memory_order_relaxed)));
        float b = std::max(0.f, std::min(1.f, T.view.hb[e].load(std::memory_order_relaxed)));
        tops[i] = y1 + yh * (1 - a);
        la[i] = {(float)(px0 + i) + 0.5f, tops[i]};
        lb[i] = {(float)(px0 + i) + 0.5f, y1 + yh * (1 - b)};
    }
    c.columns(px0, tops.data(), np, yb, Paint::vertical(y1, th::orange().alpha(0.28f), yb, th::orange().alpha(0.05f)));
    c.strokeThin(lb.data(), np, ui.W(1.4f), th::purple());
    c.strokeThin(la.data(), np, ui.W(1.6f), th::orange());
    c.popClip();
}

// the LFO alone: one cycle of it, with where it is now
void TremView::preview(Ui &ui, const R &r)
{
    int shp = iround(T.get(P_SHAPE));
    double dp = T.get(P_DEPTH) * 0.01, pho = T.get(P_PHASE) / 360, sk = std::max(0.05, std::min(0.95, T.get(P_SKEW) * 0.01));
    float y1 = r.y + 24, yh = r.h - 44;
    auto level = [&](double x) {
        double xx = x + pho;
        xx -= std::floor(xx);
        double m = Engine::shape(shp, Engine::skew(xx, sk));
        return y1 + yh * (float)(1 - (1 - dp * (1 - m)));
    };
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        for (int k = 0; k < 3; ++k) ui.fill(R(r.x, y1 + yh * k * 0.5f, r.w, 1 / ui.s), Paint(k == 0 ? th::gridHi() : th::grid()));
        ui.text(R(r.x + 10, r.y + 6, 400, 12), "one cycle of the LFO (the grids appear with the Pattern or LFO x Pattern source)", th::faint(),
                F_REGULAR, 10.5f);
        if (shp >= 5) {
            ui.text(R(r.x, r.y + 40, r.w, r.h - 80), shp == 5 ? "random: a new level every cycle, held" : "random: glides to a new level every cycle",
                    th::dim(), F_REGULAR, 12.5f, 1);
        } else {
            Canvas &c = ui.cv();
            int np = (int)ui.W(r.w - 2);
            std::vector<float> tops(np);
            std::vector<Pt> pts(np);
            int px0 = (int)std::floor(ui.X(r.x + 1));
            for (int i = 0; i < np; ++i) {
                float y = ui.Y(level((double)i / np));
                tops[i] = y;
                pts[i] = {(float)(px0 + i) + 0.5f, y};
            }
            c.pushClip(ui.X(r.x + 1), ui.Y(r.y + 1), ui.W(r.w - 2), ui.W(r.h - 2));
            c.columns(px0, tops.data(), np, ui.Y(y1 + yh), Paint::vertical(ui.Y(y1), th::orange().alpha(0.25f), ui.Y(y1 + yh), th::orange().alpha(0.04f)));
            c.strokePolyline(pts.data(), np, ui.W(2), Paint(th::orange()));
            c.popClip();
        }
        ui.stroke(r, Paint(th::border()), 6);
    }
    if (ui.drawD() && live && shp < 5) {
        double ph = T.view.ph.load();
        float x = r.x + 1 + (float)ph * (r.w - 2), y = level(ph);
        ui.fill(R(x - 5, y - 5, 10, 10), Paint(Col(1, 1, 1, 1)), 5);
    }
}

// the controls: every one has a fixed place; what does not apply is not drawn
void TremView::controls(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float cw = (DW - 2 * M - 2 * 20) / 3, h = 26, dy = 32, lw = 72, bw = 84;
    float x[3] = {M, M + cw + 20, M + 2 * (cw + 20)};
    int md = mode(), sr = src();
    int sy = iround(T.get(P_SYNC)), clk = iround(T.get(P_CLOCK)), shp = iround(T.get(P_SHAPE));
    bool ul = sr != 1;
    auto row = [&](int col, int r, const char *lbl, int id, int sc) { ui.paramRow(R(x[col], y + r * dy, cw, h), lbl, id, sc, lw, bw); };
    auto dd = [&](int col, int r, int id, const char *const *labels, int n, const char *pre) { enumDD(ui, R(x[col], y + r * dy, cw, h), id, labels, n, pre); };
    // Rise | Fall | Dry/wet; Clock | Stereo | Crossover; Depth | Rate | Shape; Skew | Sync | Phase; Onset under Clock
    row(0, 0, "Rise", P_RISE, SC_LOG); row(1, 0, "Fall", P_FALL, SC_LOG); row(2, 0, "Dry/wet", P_MIX, SC_LIN);
    dd(0, 1, P_CLOCK, L_CLOCK, 4, "Clock");
    if (md != 1) row(1, 1, "Stereo", P_STEREO, SC_LIN);   // (Auto-pan moves the whole sound: no sides to offset)
    if (md == 2) row(2, 1, "Crossover", P_XOVER, SC_LOG);
    if (clk == 3) row(0, 4, "Onset", P_ONSET, SC_LIN);
    if (sr == 0) row(0, 2, "Depth", P_DEPTH, SC_LIN);
    if (!ul) {
        ui.text(R(x[0], y + 2 * dy, DW - 2 * M, h), "Rate, Shape, Skew, Sync and Phase appear when the Source is LFO or LFO x Pattern.", th::faint(),
                F_REGULAR, 11.5f);
        return;
    }
    if (sy == 0) row(1, 2, "Rate", P_RATE, SC_LOG);
    dd(2, 2, P_SHAPE, L_SHAPE, 7, "Shape");
    dd(1, 3, P_SYNC, L_SYNC, NSYNC, "Sync");
    if (shp < 5) { row(0, 3, "Skew", P_SKEW, SC_LIN); row(2, 3, "Phase", P_PHASE, SC_LIN); }
}

void TremView::frame(Ui &ui)
{
    int clk = iround(T.get(P_CLOCK));
    double tempo = T.tempoNow() > 0 ? T.tempoNow() : 120;
    gr.build([this](int id) { return T.get(id); });
    char ex[200];
    bool own = T.get(P_TSRC) >= 0.5;
    int n = snprintf(ex, sizeof ex, "%.1f bpm, %s", tempo,
                     clk == 0 ? (own ? "runs on its own (a tapped or typed tempo can't follow the song)"
                                     : (T.playingNow() ? "following the song" : "song position (stopped: runs on)"))
                              : (clk == 1 ? "free-running" : (clk == 2 ? "restarts on a MIDI note" : "restarts on a pluck")));
    if (src() != 0 && T.get(P_GAP) >= 0.5 && n > 0 && n < (int)sizeof ex) {
        // how long the gap is now, and whether the Fall lets it reach silence
        static const double spb[NPSTEP] = {1, 1.5, 2, 3, 4, 6, 8, 12, 16, 24};
        double stepMs = 60000.0 / tempo / spb[iclamp(iround(T.get(P_PSTEP)), 0, NPSTEP - 1)];
        double gapMs = stepMs * T.get(P_GAPLEN) * 0.01, fall = T.get(P_FALL);
        if (gapMs < 2.3 * fall) snprintf(ex + n, sizeof ex - n, "  |  gap %.0f ms: under 2.3 x the Fall, it won't reach silence", gapMs);
        else snprintf(ex + n, sizeof ex - n, "  |  gap %.0f ms", gapMs);
    }
    R gear = begin(ui, T.view.latms.load(), T.view.side.load() != 0);
    clockText = ex;
    int sr = src();
    float y = 66;
    if (ui.drawS()) {
        enumDD(ui, R(M, y, 290, 26), P_MODE, L_MODE, 3, "Mode");
        enumDD(ui, R(M + 300, y, 290, 26), P_SRC, L_SRC, 3, "Source");
        // the tempo: the project's, tapped (tap the box), or typed
        int ts = iclamp(iround(T.get(P_TSRC)), 0, 2);
        enumDD(ui, R(M + 600, y, 150, 26), P_TSRC, L_TSRC, 3, "Tempo");
        R tb(M + 758, y, DW - M - (M + 758), 26);
        char bt[48];
        if (ts == 0) {
            snprintf(bt, sizeof bt, "%.1f bpm", T.tempoNow() > 0 ? T.tempoNow() : 120.0);
            ui.valueBox(tb, P_CTEMPO, false, KD_OTHER, 0, 0, 2, bt);
        } else if (ts == 2) {
            ui.valueBox(tb, P_CTEMPO, true, KD_OTHER, 1, 0.1);
        } else {
            snprintf(bt, sizeof bt, "%.1f bpm   tap", T.get(P_CTEMPO));
            if (ui.button(tb, bt, false, true, BS_PURPLE)) {
                // a tap: the tempo from the last few taps (a pause of 2 s starts over)
                if (!taps.empty() && ui.now - taps.back() > 2.0) taps.clear();
                taps.push_back(ui.now);
                if (taps.size() > 6) taps.erase(taps.begin());
                if (taps.size() >= 2) {
                    double bpm = 60.0 * (taps.size() - 1) / (taps.back() - taps.front());
                    T.setOnce(P_CTEMPO, std::max(20.0, std::min(300.0, std::floor(bpm * 10 + 0.5) / 10)));
                }
            }
        }
    }
    y += 36;
    scope(ui, R(M, y, DW - 2 * M, 104));
    y += 104 + 12;
    if (sr != 0) {
        load();
        header(ui, y);
        float gy = y + 34, gh = AH - 34, rh = gh / gr.ng;
        for (int g = 0; g < gr.ng; ++g) gridRow(ui, g, gy + g * rh, rh);
        // the links between the grids: lit = they play together; unlit = the next one plays after
        if (ui.drawS())
            for (int g = 0; g + 1 < gr.ng; ++g) {
                bool on = T.get(P_LINK + g) >= 0.5;
                float by = gy + (g + 1) * rh;
                if (on) ui.fill(R(M + 14, gy + g * rh + rh / 2 + 6, 2, rh - 12), Paint(th::purple().alpha(0.6f)));
                R lb(M, by - 9, 30, 18);
                if (ui.button(lb, "", on, true, BS_PURPLE, IC_LINK)) T.setOnce(P_LINK + g, on ? 0 : 1);
            }
        // the mouse on the boxes, after every row has seen it
        if (dmode != 0 && ui.drawS()) {
            if (dmode == 1 && ui.ldown && std::fabs(ui.my() - dy0) > 4 && std::fabs(ui.my() - dy0) > std::fabs(ui.mx() - dx0)) dmode = 3;
            if (dmode == 3) {
                double sens = (ui.mods & M_SHIFT) ? dsens * 4 : dsens;
                double v = std::max(0.0, std::min(1.0, dlv0 - (ui.my() - dy0) / sens));
                if (LV[dgrid * GS + dstep] != v) { LV[dgrid * GS + dstep] = v; dirty = true; }
            }
            if (!ui.ldown) {
                if (dmode == 1) { LV[dgrid * GS + dstep] = paint; dirty = true; }
                if (dirty) { store(dgrid, false); dirty = false; }
                endGrid(dgrid);
                dmode = 0;
            } else if (dirty) {
                store(dgrid, false);
                dirty = false;
            }
        }
    } else {
        preview(ui, R(M, y, DW - 2 * M, AH));
    }
    y += AH + 12;
    controls(ui, y);
    y += 5 * 32 + 6;
    actCardAt(ui, y);
    finish(ui, gear);
}

// ---------------------------------------------------------------- the kind
static const char *s_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_TREMOLO, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const PluginKind &kind()
{
    static const PluginKind k = {
        {CLAP_VERSION_INIT, "com.twostorms.tremolo", "TwoStorms Tremolo", "TwoStorms", "", "", "", TS_VERSION,
         "A tremolo with an Act built in: Tremolo, Auto-pan and Harmonic modes, an LFO free or synced to the tempo, one to four step "
         "grids (linked or taking turns, every step with its own level, a gap between steps, a pitch per grid), and a clock that "
         "follows the song, runs free or restarts on a MIDI note or a pluck. No latency unless a grid is pitched.",
         s_features},
        &table,
        &factoryPresets,
        "Tremolo",
        "TWOSTORMS TREMOLO",
        shm::KIND_POINT,
        shm::SK_TREMOLO,
        false,
        true,
        P_AB,
        &create,
        2,
        true,
    };
    return k;
}

} // namespace trem
} // namespace ts
