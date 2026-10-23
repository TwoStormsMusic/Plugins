// TwoStorms Delay: the plug-in (parameters, presets, the window). The sound is in delay_engine.cpp.
// Port of twostorms_delay.jsfx.
#include "delay_plugin.h"
#include "delay_engine.h"
#include "../../core/filters.h"
#include "../../gui/effect_view.h"
#include "../../gui/modslots.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

namespace ts {
namespace delay {

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
static void f_ms(double v, char *b, int n)
{
    if (v >= 1000) snprintf(b, n, "%.2f s", v / 1000);
    else if (v >= 100) snprintf(b, n, "%.0f ms", v);
    else snprintf(b, n, "%.1f ms", v);
}
static void f_hz(double v, char *b, int n)
{
    if (v >= 10000) snprintf(b, n, "%.1f kHz", v / 1000);
    else if (v >= 1000) snprintf(b, n, "%.2f kHz", v / 1000);
    else snprintf(b, n, "%.0f Hz", v);
}
static void f_sec(double v, char *b, int n) { snprintf(b, n, "%.2f s", v); }
static void f_st(double v, char *b, int n) { snprintf(b, n, "%+.2f st", v); }
static void f_q(double v, char *b, int n) { snprintf(b, n, "%.2f", v); }
static void f_int(double v, char *b, int n) { snprintf(b, n, "%.0f", v); }
static void f_rate(double v, char *b, int n) { snprintf(b, n, v < 10 ? "%.2f" : "%.1f", v); }
static void f_rel(double v, char *b, int n)
{
    if (v < 1000) snprintf(b, n, "%.0f ms", v);
    else snprintf(b, n, "%.2f s", v / 1000);
}

static const char *const L_CON[] = {"Parallel", "Series", "Linked"};
static const char *const L_DSRC[] = {"Input", "Sidechain"};
static const char *const L_AN[] = {"Off", "Pre", "Post", "Both"};
static const char *const L_SHP[] = {"Octave up", "Fifth up", "Octave + fifth", "Octave down"};
static const char *const L_HQ[] = {"Standard", "High"};
static const char *const L_SYNC[] = {"Free", "1/32T", "1/32", "1/16T", "1/16", "1/16.", "1/8T", "1/8", "1/8.",
                                     "1/4T", "1/4", "1/4.", "1/2T", "1/2", "1/2.", "1 bar", "2 bars"};
static const char *const L_MODE[] = {"Stereo", "Ping-pong L first", "Ping-pong R first"};
static const char *const L_CHAR[] = {"Clean", "Tape", "BBD", "Lo-Fi"};
static const char *const L_SAT[] = {"Clean Tube", "Warm Tube", "Hot Tube", "Broken Tube", "Clean Tape", "Warm Tape", "Hot Tape",
                                    "Extreme Tape", "Subtle Transformer", "Warm Transformer", "Lush Transformer", "American Amp",
                                    "British Amp", "Smooth Amp", "Crunch Amp", "Scream Amp", "Even Harmonics", "Inflator", "Fuzz",
                                    "Foldback", "Sine Fold", "Rectify", "Soft Clip", "Hard Clip"};
static const char *const L_SH[] = {"Off", "Pitch", "Reverse", "Reverse + pitch"};
static const char *const L_RCH[] = {"Delay time", "1/32T", "1/32", "1/16T", "1/16", "1/16.", "1/8T", "1/8", "1/8.",
                                    "1/4T", "1/4", "1/4.", "1/2T", "1/2", "1/2.", "1 bar", "2 bars"};
static const char *const L_TAPS[] = {"1 tap", "2 taps", "3 taps", "4 taps"};
static const char *const L_TPAT[] = {"Even", "Accelerating", "Decelerating", "Gallop"};
static const char *const L_MTYPE[] = {"Off", "LFO", "Follower", "MIDI"};
static const char *const L_MLINE[] = {"Global", "Line A", "Line B", "Line C", "All lines"};
static const char *const L_MGLOB[] = {"Wet level", "Input", "Delay ducking", "Freeze", "Reverb"};
static const char *const L_MPAR[] = {"Time", "Feedback", "Level", "Pan", "Cross", "High pass", "Low pass", "Drive", "Pitch", "Diffusion", "Width", "Bell gain"};
static const char *const L_MSYNC[] = {"Free", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars", "1/16T", "1/8T", "1/4T", "1/8."};
static const char *const L_MSRC[] = {"Input", "Wet", "Sidechain"};

template <size_t N> static std::vector<const char *> labels(const char *const (&a)[N]) { return std::vector<const char *>(a, a + N); }

static const ParamTable &table()
{
    static std::deque<std::string> names;   // the line and slot names, kept for the table's lifetime
    static ParamTable t = [] {
        ParamTable t;
        auto nm = [](const std::string &s) { names.push_back(s); return names.back().c_str(); };
        addActParams(t, true);
        const char *M = "Delay";
        const uint32_t H = PF_HOST | PF_AB;
        t.add(P_SEL, "Selected line", 0, 2, 0, PF_INT, f_int, M);
        t.addEnum(P_C12, "Lines A and B", labels(L_CON), 0, H, M);
        t.addEnum(P_C23, "Lines B and C", labels(L_CON), 0, H, M);
        t.add(P_LK12, "Link A and B", 0, 100, 50, H, f_pct, M).unit = "%";
        t.add(P_LK23, "Link B and C", 0, 100, 50, H, f_pct, M).unit = "%";
        t.add(P_IN, "Input", -24, 24, 0, H, f_dbs, M).unit = "dB";
        t.add(P_DRY, "Dry level", -60, 12, 0, H, f_lvl, M).unit = "dB";
        t.add(P_WET, "Wet level", -60, 12, 0, H, f_lvl, M).unit = "dB";
        t.addEnum(P_AB, "A/B set", {"A", "B"}, 0, PF_HOST, M);
        t.add(P_DUCK, "Delay ducking", 0, 100, 0, H, f_pct, "Ducking").unit = "%";
        t.add(P_RDUCK, "Reverb ducking", 0, 100, 0, H, f_pct, "Ducking").unit = "%";
        t.add(P_DTHR, "Ducking threshold", -60, 0, -30, H, f_dbs, "Ducking").unit = "dB";
        t.add(P_DATT, "Ducking attack", 1, 200, 10, H, f_ms, "Ducking").unit = "ms";
        t.add(P_DREL, "Ducking release", 10, 2000, 300, H, f_ms, "Ducking").unit = "ms";
        t.addEnum(P_DSRC, "Ducking source", labels(L_DSRC), 0, H, "Ducking");
        t.addEnum(P_FREEZE, "Freeze", {"Off", "On"}, 0, PF_HOST | PF_BOOL, M);
        t.add(P_MSEL, "Selected modulation slot", 0, 7, 0, PF_INT, f_int, M);
        t.addEnum(P_MODOPEN, "Modulation section", {"Closed", "Open"}, 0, 0, M);
        t.addEnum(P_AN, "Analyzer", labels(L_AN), 3, 0, M);
        t.add(P_RVB, "Reverb", 0, 100, 0, H, f_pct, "Reverb").unit = "%";
        t.add(P_RDEC, "Reverb decay", 0.2, 10, 2, H, f_sec, "Reverb").unit = "s";
        t.add(P_RSIZ, "Reverb size", 50, 200, 100, H, f_pct, "Reverb").unit = "%";
        t.add(P_RDMP, "Reverb damping", 1000, 20000, 8000, H | PF_INT, f_hz, "Reverb").unit = "Hz";
        t.add(P_RSHM, "Shimmer", 0, 100, 0, H, f_pct, "Reverb").unit = "%";
        t.addEnum(P_RSHP, "Shimmer interval", labels(L_SHP), 0, H, "Reverb");
        t.add(P_RPRE, "Reverb pre-delay", 0, 200, 15, H, f_ms, "Reverb").unit = "ms";
        t.addEnum(P_HQ, "Quality", labels(L_HQ), 1, H, M);
        t.add(P_GRP, "Open groups", 0, 1023, 1, PF_INT, f_int, M);   // the window: which of the list's groups are open
        static const int SYD[NL] = {7, 10, 13};      // 1/8, 1/4, 1/2
        static const double LVD[NL] = {0, -6, -9};
        for (int b = 0; b < NL; ++b) {
            std::string L = std::string("Line ") + (char)('A' + b) + " ";
            const char *mod = nm(std::string("Line ") + (char)('A' + b));
            auto n = [&](const char *s) { return nm(L + s); };
            t.addEnum(ep(b, K_ON), n("on"), {"Off", "On"}, b == 0 ? 1 : 0, H | PF_BOOL, mod);
            t.add(ep(b, K_TL), n("time L"), 1, 8000, 375, H, f_ms, mod).unit = "ms";
            t.addEnum(ep(b, K_SL), n("sync L"), labels(L_SYNC), SYD[b], H, mod);
            t.add(ep(b, K_TR), n("time R"), 1, 8000, 375, H, f_ms, mod).unit = "ms";
            t.addEnum(ep(b, K_SR), n("sync R"), labels(L_SYNC), SYD[b], H, mod);
            t.addEnum(ep(b, K_LINK), n("R follows L"), {"Off", "On"}, 1, H | PF_BOOL, mod);
            t.add(ep(b, K_FB), n("feedback"), 0, 120, 40, H, f_pct, mod).unit = "%";
            t.add(ep(b, K_CROSS), n("cross feedback"), 0, 100, 0, H, f_pct, mod).unit = "%";
            t.addEnum(ep(b, K_MODE), n("mode"), labels(L_MODE), 0, H, mod);
            t.add(ep(b, K_LEVEL), n("level"), -60, 12, LVD[b], H, f_lvl, mod).unit = "dB";
            t.add(ep(b, K_PAN), n("pan"), -100, 100, 0, H, f_spct, mod).unit = "%";
            t.add(ep(b, K_WIDTH), n("width"), 0, 200, 100, H, f_pct, mod).unit = "%";
            t.addEnum(ep(b, K_CHAR), n("character"), labels(L_CHAR), 0, H, mod);
            t.add(ep(b, K_AGE), n("age"), 0, 100, 30, H, f_pct, mod).unit = "%";
            t.add(ep(b, K_WOW), n("wow"), 0, 100, 0, H, f_pct, mod).unit = "%";
            t.add(ep(b, K_FLUT), n("flutter"), 0, 100, 0, H, f_pct, mod).unit = "%";
            t.add(ep(b, K_GLIDE), n("time glide"), 1, 2000, 150, H, f_ms, mod).unit = "ms";
            t.add(ep(b, K_DRIVE), n("drive"), 0, 100, 0, H, f_pct, mod).unit = "%";
            t.addEnum(ep(b, K_SAT), n("saturation style"), labels(L_SAT), 1, H, mod);
            t.add(ep(b, K_HP), n("high pass"), 20, 2000, 20, H, f_hz, mod).unit = "Hz";
            t.add(ep(b, K_LP), n("low pass"), 200, 20000, 20000, H, f_hz, mod).unit = "Hz";
            t.add(ep(b, K_BF), n("bell frequency"), 20, 20000, 1000, H, f_hz, mod).unit = "Hz";
            t.add(ep(b, K_BG), n("bell gain"), -18, 18, 0, H, f_dbs, mod).unit = "dB";
            t.add(ep(b, K_BQ), n("bell Q"), 0.2, 8, 1, H, f_q, mod);
            t.add(ep(b, K_DIFF), n("diffusion"), 0, 100, 0, H, f_pct, mod).unit = "%";
            t.add(ep(b, K_DSZ), n("diffusion size"), 5, 80, 30, H, f_ms, mod).unit = "ms";
            t.addEnum(ep(b, K_SH), n("shift mode"), labels(L_SH), 0, H, mod);
            t.add(ep(b, K_SEMI), n("pitch"), -12, 12, 0, H, f_st, mod);
            t.add(ep(b, K_SHSZ), n("pitch window"), 20, 300, 80, H, f_ms, mod).unit = "ms";
            t.addEnum(ep(b, K_TAPS), n("taps"), labels(L_TAPS), 0, H, mod);
            t.addEnum(ep(b, K_TPAT), n("tap pattern"), labels(L_TPAT), 0, H, mod);
            t.add(ep(b, K_TLVL), n("tap level"), 0, 100, 60, H, f_pct, mod).unit = "%";
            t.addEnum(ep(b, K_SHSY), n("reverse chunk"), labels(L_RCH), 0, H, mod);
            t.add(ep(b, K_RSND), n("reverb send"), 0, 100, 100, H, f_pct, mod).unit = "%";
        }
        for (int s = 0; s < NMOD; ++s) {
            std::string L = "Mod " + std::to_string(s + 1) + " ";
            const char *mod = nm("Modulation " + std::to_string(s + 1));
            auto n = [&](const char *x) { return nm(L + x); };
            t.addEnum(mp(s, M_TYPE), n("type"), labels(L_MTYPE), 0, H, mod);
            t.addEnum(mp(s, M_TLINE), n("target line"), labels(L_MLINE), 1, H, mod);
            t.add(mp(s, M_TPAR), n("target parameter"), 0, 11, 0, H | PF_INT, f_int, mod);
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

// factory presets: Instruments, Buses, Creative, Weird (lines A 70.., B 110.., C 150..; the modulation slots 190..;
// 47 / 69 the dry and wet levels)
static const std::vector<FactoryPreset> &factoryPresets()
{
    static std::vector<FactoryPreset> v = {
        // ---- instruments/guitar
        {"Instruments/Guitar/Clean: tape echo (space echo style)", "45=0 50=0 60=0 61=2 64=0 72=10 76=45 82=1 83=40 84=20 85=15 87=20 47=-1 69=-6.86"},
        {"Instruments/Guitar/Clean: dotted 1/8 + 1/4 (stereo)", "45=0 50=0 60=0 61=2 64=0 72=8 74=10 75=0 76=22 89=300 90=5500 190=1 191=1 192=0 193=1.5 194=0.6 47=-0.35 69=-11.09"},
        {"Instruments/Guitar/Clean: ambient swell", "45=0 50=0 60=30 61=4 64=0 72=8 76=45 89=250 90=6000 94=40 190=1 191=1 192=0 193=1.2 194=0.4 47=-1 69=-6.86"},
        {"Instruments/Guitar/Clean: tape echo shimmer", "45=0 50=0 60=20 61=3 64=25 72=10 76=35 82=1 83=30 84=15 85=10 89=300 90=5000 47=-0.53 69=-9.4"},
        {"Instruments/Guitar/Funk: tight slap (1/16)", "45=0 50=0 60=0 61=2 64=0 72=4 76=10 89=300 90=4000 47=-0.24 69=-12.64"},
        {"Instruments/Guitar/Funk: ping-pong 1/16", "45=0 50=0 60=0 61=2 64=0 72=4 76=25 78=1 89=300 90=5000 47=-0.44 69=-10.2"},
        {"Instruments/Guitar/Funk: triplet bounce (ducked)", "45=0 50=50 51=-30 60=0 61=2 64=0 72=6 76=25 78=1 89=300 90=5000 47=-0.53 69=-9.4"},
        {"Instruments/Guitar/Rock: dotted eighth lead", "45=0 50=30 60=0 61=2 64=0 72=8 76=40 89=200 90=7000 47=-1 69=-6.86"},
        {"Instruments/Guitar/Rock: analog BBD echo", "45=0 50=0 60=0 61=2 64=0 72=10 76=45 82=2 83=50 47=-1 69=-6.86"},
        {"Instruments/Guitar/Rock: lead quarter (20%)", "45=0 50=0 60=0 61=2 64=0 72=10 76=30 82=1 83=25 89=200 90=6000 47=-0.44 69=-10.2"},
        {"Instruments/Guitar/Rock: slapback", "45=0 50=0 60=0 61=2 64=0 71=110 72=0 76=10 82=1 89=150 90=6000 47=-0.69 69=-8.34"},
        {"Instruments/Guitar/Rock: arena dotted 1/8 + quarter", "45=0 50=0 60=10 61=2 64=0 72=8 76=35 80=-40 89=200 90=6000 110=1 112=10 116=25 119=-6 120=40 129=200 130=5000 47=-0.69 69=-8.34"},
        {"Instruments/Guitar/Metal: tight lead quarter (ducked)", "45=0 50=60 51=-30 60=0 61=2 64=0 72=10 76=30 89=300 90=5000 47=-0.53 69=-9.4"},
        {"Instruments/Guitar/Metal: solo dotted 1/8 + reverb (ducked)", "45=0 50=50 51=-30 60=20 61=3 64=0 72=8 76=40 89=300 90=6000 47=-0.69 69=-8.34"},
        {"Instruments/Guitar/Metal: rhythm thickener", "45=0 50=0 60=0 61=2 64=0 71=14 73=21 72=0 74=0 75=0 76=0 89=400 90=8000 47=-0.24 69=-12.64"},
        // ---- instruments/bass
        {"Instruments/Bass/Clean: dotted eighth, clean lows", "45=0 50=40 60=0 61=2 64=0 72=8 76=25 89=250 90=6000 47=-0.24 69=-12.64"},
        {"Instruments/Bass/Distorted: slapback", "45=0 50=0 60=0 61=2 64=0 71=120 72=0 76=10 82=1 89=300 90=4000 47=-0.24 69=-12.64"},
        {"Instruments/Bass/Slap: short room echo", "45=0 50=0 60=0 61=2 64=0 71=80 72=0 76=5 89=300 90=5000 47=-0.11 69=-16.11"},
        {"Instruments/Bass/Punk", "45=0 50=0 60=0 61=2 64=0 71=110 72=0 76=15 82=1 89=250 90=5000 47=-0.44 69=-10.2"},
        {"Instruments/Bass/Pop punk", "45=0 50=50 60=0 61=2 64=0 72=7 76=30 78=1 89=250 90=7000 47=-0.44 69=-10.2"},
        {"Instruments/Bass/Metal", "45=0 50=60 60=0 61=2 64=0 72=11 76=35 89=300 90=4000 47=-0.44 69=-10.2"},
        {"Instruments/Bass/Rock", "45=0 50=0 60=0 61=2 64=0 72=8 76=35 82=1 89=200 90=5500 47=-0.69 69=-8.34"},
        {"Instruments/Bass/Jazz", "45=0 50=0 60=0 61=2 64=0 72=10 76=20 82=2 89=200 90=3500 47=-0.24 69=-12.64"},
        {"Instruments/Bass/Pop", "45=0 50=40 60=20 61=2 64=0 72=10 76=25 89=250 90=8000 110=1 112=8 114=8 116=25 119=-6 120=50 129=300 130=8000 47=-0.53 69=-9.4"},
        // ---- instruments/vocals
        {"Instruments/Vocals/Lead: quarter-note tape", "45=0 50=50 60=15 61=2 64=0 72=10 76=30 82=1 83=25 89=250 90=6000 47=-0.35 69=-11.09"},
        {"Instruments/Vocals/Spoken: slap", "45=0 50=0 60=0 61=2 64=0 71=90 72=0 76=5 89=300 90=7000 47=-0.11 69=-16.11"},
        {"Instruments/Vocals/Rockabilly slapback", "45=0 50=0 60=0 61=2 64=0 71=110 72=0 76=10 82=1 89=150 90=6000 47=-0.69 69=-8.34"},
        {"Instruments/Vocals/Lead: quarter-note echo", "45=0 50=0 60=0 61=2 64=0 72=10 76=35 89=150 90=8000 47=-0.69 69=-8.34"},
        {"Instruments/Vocals/Rap: eighth-note echo", "45=0 50=0 60=0 61=2 64=0 72=7 76=30 89=150 90=8000 47=-0.69 69=-8.34"},
        {"Instruments/Vocals/Doubler (short, wide)", "45=0 50=0 60=0 61=2 64=0 71=18 73=27 72=0 74=0 75=0 76=0 89=150 47=-0.44 69=-10.2"},
        {"Instruments/Vocals/Lead: slap (100 ms, tape)", "47=-0.69 69=-8.34 72=0 71=100 76=10 82=1 83=30 89=150 90=6000"},
        {"Instruments/Vocals/Lead: 1/4 throw (ducked)", "47=-1 69=-6.86 50=60 51=-30 72=10 76=35 89=250 90=6000 103=100 60=20 61=2.5"},
        {"Instruments/Vocals/Lead: dotted 1/8 ping-pong (ducked)", "47=-0.87 69=-7.42 50=55 51=-30 72=8 76=40 78=1 89=300 90=6500"},
        {"Instruments/Vocals/Rap: eighth echo, heavily ducked", "47=-1 69=-6.86 50=90 51=-34 53=150 72=7 76=30 89=300 90=5000"},
        {"Instruments/Vocals/Backing: wide 1/16 + 1/8 (two lines)", "47=-0.69 69=-8.34 72=4 76=10 80=-60 89=300 110=1 112=7 116=15 119=-4 120=60 129=300"},
        // ---- instruments/drums
        {"Instruments/Drums/Snare: dub echo", "45=0 50=0 60=0 61=2 64=0 72=11 76=80 82=2 87=30 89=300 90=2000 47=-1.38 69=-5.64"},
        {"Instruments/Drums/Percussion: multi-tap rhythm", "45=0 50=0 60=0 61=2 64=0 72=10 76=30 99=3 100=0 101=70 47=-0.87 69=-7.42"},
        {"Instruments/Drums/Kick: flam (30 ms)", "47=-0.44 69=-10.2 72=0 71=30 76=0 90=4000"},
        {"Instruments/Drums/Hi-hat: 1/16 ping-pong", "47=-0.69 69=-8.34 72=4 76=30 78=1 89=2000"},
        {"Instruments/Drums/Percussion: dotted 1/8 swirl", "47=-1 69=-6.86 72=8 76=45 78=1 94=30 89=400 90=8000"},
        {"Instruments/Drums/Toms: big 1/4 dub throw", "47=-1.38 69=-5.64 72=10 76=55 82=2 83=50 89=150 90=2500 87=20"},
        // ---- instruments/keys & synths
        {"Instruments/Keys & synths/Synth: ping-pong eighths", "45=0 50=0 60=0 61=2 64=0 72=7 76=45 78=1 89=200 90=8000 47=-1 69=-6.86"},
        {"Instruments/Keys & synths/Pad: ambient wash", "45=0 50=0 60=40 61=4 64=0 72=10 76=50 89=200 90=6000 94=60 47=-1.84 69=-4.62"},
        {"Instruments/Keys & synths/Pad: half-note ambience", "45=0 50=0 60=25 61=3 64=0 72=13 76=50 89=250 90=5000 94=30 47=-1 69=-6.86"},
        {"Instruments/Keys & synths/Synth lead: three lines (1/8, 1/8., 1/4)", "47=-1 69=-6.86 72=7 76=25 80=-70 89=250 110=1 112=8 116=25 119=-3 120=70 129=250 150=1 152=10 156=35 159=-6 169=400 170=5000"},
        {"Instruments/Keys & synths/Piano: subtle 1/4 tape", "47=-0.35 69=-11.09 72=10 76=25 82=1 83=40 84=10 89=200 90=5000"},
        // ---- buses/instrument bus
        {"Buses/Instrument bus/Guitar bus: stereo ambience (1/8. left, 1/4 right)", "47=-0.24 69=-12.64 72=8 75=0 74=10 76=25 89=300 90=5000 94=20"},
        {"Buses/Instrument bus/Vocal bus: thickening slap", "47=-0.24 69=-12.64 72=0 71=70 75=0 74=0 73=95 76=5 89=250 90=7000 82=1 83=20"},
        {"Buses/Instrument bus/Drum bus: short multi-tap room", "47=-0.24 69=-12.64 72=0 71=60 76=20 99=3 100=0 101=70 94=60 95=20 89=200 90=7000"},
        // ---- buses/mixbus
        {"Buses/Mixbus/Breakdown: 1/4 dub throws (automate the wet)", "47=0 69=-60 72=10 76=60 82=1 83=40 89=300 90=3000 87=15"},
        {"Buses/Mixbus/Half-note ambience (10%)", "47=-0.11 69=-16.11 72=13 76=35 94=50 89=300 90=5000 60=30 61=3"},
        // ---- buses/master
        {"Buses/Master/Last hit: long half-note fade", "47=-0.69 69=-8.34 72=13 76=70 94=40 89=200 90=6000 60=40 61=5"},
        // ---- creative
        {"Creative/Shimmer swell", "45=0 50=0 60=50 61=6 64=40 72=10 76=40 89=300 94=40 47=-1.84 69=-4.62"},
        {"Creative/Reverse echoes", "45=0 50=0 60=0 61=2 64=0 72=10 76=35 96=2 47=-1.38 69=-5.64"},
        {"Creative/Octave-up repeats", "45=0 50=0 60=0 61=2 64=0 72=8 76=40 89=300 96=1 97=12 47=-1 69=-6.86"},
        {"Creative/Reverse swell", "45=0 50=0 60=35 61=5 64=0 72=10 76=40 96=2 89=250 47=-1.84 69=-4.62"},
        {"Creative/Fifth-up staircase", "45=0 50=0 60=0 61=2 64=0 72=7 76=60 89=300 90=7000 96=1 97=7 47=-1.38 69=-5.64"},
        {"Creative/Downward spiral (octave down)", "45=0 50=0 60=10 61=3 64=0 72=8 76=55 89=80 90=5000 96=1 97=-12 47=-1.38 69=-5.64"},
        {"Creative/Lo-fi cassette wobble", "45=0 50=0 60=0 61=2 64=0 72=10 76=45 82=3 83=70 84=60 85=40 90=3500 47=-1.38 69=-5.64"},
        {"Creative/Granular cloud", "45=0 50=0 60=25 61=4 64=0 72=10 76=50 94=70 95=60 99=3 100=1 101=80 47=-1.84 69=-4.62"},
        {"Creative/Gallop taps", "45=0 50=0 60=0 61=2 64=0 72=10 76=25 99=3 100=3 101=75 47=-1 69=-6.86"},
        {"Creative/Shimmer heaven", "45=0 50=0 60=60 61=8 64=60 65=0 72=13 76=45 89=300 94=50 47=-1.84 69=-4.62"},
        {"Creative/Pitch ping-pong (fourths)", "45=0 50=0 60=0 61=2 64=0 72=6 76=50 78=1 96=1 97=5 89=250 47=-1 69=-6.86"},
        {"Creative/Three-line cascade (series A > B > C)", "47=-1.38 69=-5.64 45=1 44=1 72=7 76=20 89=300 110=1 112=8 116=20 119=0 120=-50 129=400 130=6000 150=1 152=10 156=20 159=0 160=50 169=500 170=4000"},
        {"Creative/Bouncing between A and B (linked)", "47=-1.38 69=-5.64 45=2 58=80 72=7 76=60 80=-80 90=7000 110=1 112=8 116=60 119=0 120=80 122=2 123=50 130=4000"},
        {"Creative/Reverse chunks on the beat (1/2)", "47=-1.84 69=-4.62 72=13 76=40 96=2 102=13 89=200 60=30 61=3"},
        {"Creative/Reverse octave-up shimmer", "47=-1.84 69=-4.62 72=10 76=45 96=3 97=12 89=400 60=50 61=5 64=40"},
        // ---- weird
        {"Weird/Self-oscillating BBD (careful)", "45=0 50=0 60=0 61=2 64=0 72=8 76=104 82=2 83=50 87=40 89=150 90=4000 47=-1 69=-6.86"},
        {"Weird/Radio from next door", "45=0 50=0 60=0 61=2 64=0 72=7 76=40 87=55 88=22 89=500 90=2500 91=1200 92=6 47=-2.38 69=-3.75"},
        {"Weird/Seasick (time LFO)", "45=0 50=0 60=0 61=2 64=0 72=10 76=45 82=1 190=1 191=1 192=0 193=35 194=0.25 47=-1.38 69=-5.64"},
        {"Weird/Infinite drone (freeze me)", "45=0 50=0 60=40 61=10 64=30 72=13 76=95 94=80 95=70 89=100 90=6000 47=-3.01 69=-3.01"},
        {"Weird/Karplus pluck (5 ms, 97% feedback)", "47=-3.01 69=-3.01 72=0 71=5 76=97 90=6000"},
        {"Weird/Linked triangle (A, B, C all linked)", "47=-1.84 69=-4.62 45=2 44=2 58=60 48=60 72=0 71=110 76=80 80=-90 90=6000 110=1 112=0 111=170 116=80 119=0 122=2 123=60 150=1 152=0 151=290 156=80 159=0 160=90 162=3 163=40 169=500"},
        {"Weird/Pitch spiral down (three lines, series)", "47=-1.84 69=-4.62 45=1 44=1 72=7 76=30 96=1 97=-5 110=1 112=7 116=30 119=0 136=1 137=-7 150=1 152=7 156=30 159=0 176=1 177=-12 170=3000"},
    };
    return v;
}

// ---------------------------------------------------------------- the plug-in
class DelayPlugin : public Plugin, public EngineHost {
public:
    explicit DelayPlugin(const clap_host *h) : Plugin(h, kind()), eng(*this, act, uiLink, view) {}
    UiLink uiLink;
    Display view;
    Engine eng;
    double pv(int id) const override { return P.get(id); }
    double abv(int id) const override { return abValue(id); }
    double plo(int id) const override { const ParamDef *d = T.get(id); return d ? d->min : 0; }
    double phi(int id) const override { const ParamDef *d = T.get(id); return d ? d->max : 1; }
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

static Plugin *create(const clap_host *h) { return new DelayPlugin(h); }

// ---------------------------------------------------------------- the window
class DelayView : public EffectView {
public:
    explicit DelayView(DelayPlugin &p) : EffectView(p, p.uiLink, 1000, 730, "DELAY"), D(p)
    {
        SH = 196 + 34 + 34 + 30;
        help = {"Three lines, each with its own times (free or at the tempo), feedback, taps, filters, character and saturation in the loop.",
                "Double-click a line's tab to turn it on or off. Between the tabs: parallel, series (the first's repeats into the second)",
                "or linked (their repeats cross by the Link amount, set in the line's top row). Freeze holds the repeats.",
                "Ducking lowers the repeats and the reverb (each by its own amount) while the source is loud. Reverse: every repeat backwards.",
                "Ctrl while changing Input moves the output the other way. Shift + click Freeze: only while the mouse is held."};
    }
    void frame(Ui &ui) override;
    void opened() override { spi.reset(); spo.reset(); }

protected:
    void hashMore(std::function<void(uint64_t)> mix) override
    {
        mix((uint64_t)(D.view.tempo.load() * 100));
        mix((uint64_t)D.view.side.load());
        mix((uint64_t)tab);
        mix((uint64_t)(scroll * 4));
        mix((uint64_t)(edrag + 2) | (uint64_t)(fdrag + 2) << 4 | (uint64_t)(ghover + 2) << 8 | (uint64_t)(nhover + 2) << 12);
    }
    bool extraFull() override { return edrag >= 0 || fdrag >= 0 || sdrag; }

private:
    DelayPlugin &D;
    Spectrum spi, spo;
    int tab = 0;   // 0: the selected line, 1: modulation, 2: reverb, 3: ducking
    double clickT = -10;   // the last click on a line's tab (a second one soon after: on / off)
    int clickK = -1;
    float scroll = 0, contentH = 0;   // the list's scroll position and its height
    R listR;                           // the list's area
    int sdrag = 0;                     // the scrollbar is dragged
    float sdy = 0;
    int sel = 0;
    int edrag = -1, ghover = -1, fdrag = -1, nhover = -1;
    double dspan = 1000;
    // the bars a modulation slot can move (drawn with a marker where the modulation has them now)
    struct Mark { int id; R bar; int sc; bool inList; };
    std::vector<Mark> marks;
    bool modsOn = false;

    double tempo() const { return D.view.tempo.load(); }
    // a line's time for side c in ms (a note value at the tempo, or the free time), as the window shows it
    double tms(int k, int c) const
    {
        int u = (c == 1 && D.get(ep(k, K_LINK)) >= 0.5) ? 0 : c;
        int s = iclamp(iround(D.get(ep(k, u ? K_SR : K_SL))), 0, 16);
        return std::max(1.0, std::min(4000.0, s > 0 ? YBEATS[s] * 60000 / tempo() : D.get(ep(k, u ? K_TR : K_TL))));
    }
    // the echo map's simulation: seeds (time, side, level) -> events
    struct Ev { double t; int c; double a; };
    std::vector<Ev> seeds, events;
    void seedInput(int k);
    void seedFromEvents(int k);
    void simulate(int k, double span);
    void echoMap(Ui &ui, const R &r);
    void meters(Ui &ui, const R &r);
    void filters(Ui &ui, const R &r);
    void tabs(Ui &ui, float y);
    void lineTab(Ui &ui, float y, float h);
    void reverbTab(Ui &ui, float y);
    void duckTab(Ui &ui, float y);
    void markSlide(Ui &ui, const R &r, int id, int sc, const Col &c, bool en, bool centered = false)
    {
        ui.slideBox(r, id, sc, c, centered, en);
        marks.push_back({id, r, sc, ui.areaOn});
    }
    bool group(Ui &ui, int g, float &y, float w, const char *name, const std::string &sum, int rows, bool global = false, bool lit = false);
    void modTab(Ui &ui, float y);
    void markRow(Ui &ui, const R &r, const char *label, int id, int sc, bool en, float lw = 72, float bw = 84)
    {
        ui.paramRow(r, label, id, sc, lw, bw, en);
        marks.push_back({id, R(r.x + lw, r.y, r.w - lw - bw - 6, r.h), sc, ui.areaOn});
    }
    void timeRow(Ui &ui, const R &r, const char *label, int pT, int pS, bool en);
    void lineCol(Ui &ui, int k, float a, Col &c) const { c = (k == 0 ? th::orange() : (k == 1 ? th::purple() : th::green())).alpha(a); }
    static char letter(int k) { return (char)('A' + k); }
    bool reverbOn() const { return D.get(P_RVB) >= 0.5; }
};

gui::View *DelayPlugin::createView() { return new DelayView(*this); }

// ---- the echo map ----
void DelayView::seedInput(int k)
{
    int m = iround(D.get(ep(k, K_MODE)));
    seeds.clear();
    if (m == 0 || m == 1) seeds.push_back({0, 0, 1});
    if (m == 0 || m == 2) seeds.push_back({0, 1, 1});
}
// the repeats of the line just simulated become the seeds of the line after it (series routing)
void DelayView::seedFromEvents(int k)
{
    int m = iround(D.get(ep(k, K_MODE)));
    seeds.clear();
    for (const Ev &e : events) {
        if (seeds.size() >= 280) break;
        seeds.push_back({e.t, m == 0 ? e.c : (m == 1 ? 0 : 1), e.a * (m == 0 ? 1 : 0.5)});
    }
}
void DelayView::simulate(int k, double span)
{
    int n = 1 + iclamp(iround(D.get(ep(k, K_TAPS))), 0, 3);
    int pe = iround(D.get(ep(k, K_TPAT)));
    double e = pe == 0 ? 1 : (pe == 1 ? 2 : (pe == 2 ? 0.5 : 0.6));
    double tl = D.get(ep(k, K_TLVL)) * 0.01, fb = D.get(ep(k, K_FB)) * 0.01;
    double x = iround(D.get(ep(k, K_MODE))) > 0 ? 1 : D.get(ep(k, K_CROSS)) * 0.01;
    double lv = D.get(ep(k, K_LEVEL)) <= -59.9 ? 0 : std::pow(10.0, D.get(ep(k, K_LEVEL)) / 20);
    std::vector<Ev> q = seeds;
    events.clear();
    for (size_t qi = 0; qi < q.size() && events.size() < 240; ++qi) {
        Ev s = q[qi];
        double tt = tms(k, s.c);
        for (int j = 0; j < n; ++j) {
            double p = j == n - 1 ? 1 : std::pow((double)(j + 1) / n, e);
            if (s.t + tt * p <= span * 1.01 && events.size() < 290) events.push_back({s.t + tt * p, s.c, s.a * lv * std::pow(tl, n - 1 - j)});
        }
        if (s.t + tt <= span * 1.01 && q.size() < 280) {
            double ml = s.a * fb * (1 - x);
            if (ml > 0.02) q.push_back({s.t + tt, s.c, ml});
            ml = s.a * fb * x;
            if (ml > 0.02) q.push_back({s.t + tt, 1 - s.c, ml});
        }
    }
}

void DelayView::echoMap(Ui &ui, const R &r)
{
    int con[NL - 1] = {iround(D.get(P_C12)), iround(D.get(P_C23))};
    // how much time to show: about six of the longest delay, in a round number
    double tmx = 100;
    for (int k = 0; k < NL; ++k)
        if (D.get(ep(k, K_ON)) >= 0.5) tmx = std::max(tmx, std::max(tms(k, 0), tms(k, 1)));
    tmx *= 6;
    double span = tmx <= 500 ? 500 : (tmx <= 1000 ? 1000 : (tmx <= 1500 ? 1500 : (tmx <= 2000 ? 2000 : (tmx <= 3000 ? 3000 : (tmx <= 4000 ? 4000 : (tmx <= 6000 ? 6000 : 8000))))));
    if (edrag >= 0) span = dspan;
    else dspan = span;
    const float top = r.y + 22, bot = r.b() - 16;
    float cy = (top + bot) * 0.5f, hh = (bot - top) * 0.5f - 4;
    auto xOf = [&](double t) { return r.x + (float)(t / span) * r.w; };
    if (!ui.drawS()) return;
    ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
    double stp = span <= 500 ? 50 : (span <= 1000 ? 100 : (span <= 2000 ? 250 : (span <= 4000 ? 500 : 1000)));
    for (double t = 0; t <= span + 0.001; t += stp) {
        float px = xOf(t);
        ui.fill(R(px, top - 4, 1 / ui.s, bot - top + 8), Paint(th::grid()));
        if (t > 0 && px < r.r() - 36) {
            char b[16];
            if (t >= 1000) snprintf(b, sizeof b, std::fmod(t, 1000) == 0 ? "%.0f s" : "%.1f s", t / 1000);
            else snprintf(b, sizeof b, "%.0f ms", t);
            ui.text(R(px + 3, r.b() - 14, 50, 11), b, th::faint(), F_REGULAR, 9.5f);
        }
    }
    ui.fill(R(r.x, cy, r.w, 1 / ui.s), Paint(th::gridHi()));
    ui.text(R(r.x + 6, top, 12, 12), "L", th::faint(), F_SEMIBOLD, 10);
    ui.text(R(r.x + 6, bot - 12, 12, 12), "R", th::faint(), F_SEMIBOLD, 10);
    ui.fill(R(r.x + 1, top, 2, bot - top), Paint(Col(1.0f, 0.95f, 0.88f, 0.9f)));
    // the repeats (in series a line's repeats feed the next one)
    auto draw = [&](int k) {
        Col c;
        lineCol(ui, k, k == sel ? 1.f : 0.5f, c);
        for (const Ev &e : events) {
            float x = xOf(e.t), h = std::max(2.f, std::min(hh, (float)e.a * hh));
            ui.fill(e.c == 0 ? R(x - 1, cy - h, 3, h) : R(x - 1, cy, 3, h), Paint(c), 1);
        }
    };
    ui.beginArea(R(r.x + 1, r.y + 1, r.w - 2, r.h - 2));
    events.clear();
    for (int k = 0; k < NL; ++k) {
        bool fromPrev = k > 0 && con[k - 1] == 1 && D.get(ep(k - 1, K_ON)) >= 0.5;
        if (fromPrev) seedFromEvents(k);
        else seedInput(k);
        if (D.get(ep(k, K_ON)) >= 0.5) {
            simulate(k, span);
            draw(k);
        } else {
            events.clear();
        }
    }
    // the first repeat of the selected line can be grabbed: sideways changes its time
    ghover = -1;
    bool ind = ui.hit(r);
    float mx = ui.mx(), my = ui.my();
    if (D.get(ep(sel, K_ON)) >= 0.5) {
        float gx0 = xOf(tms(sel, 0)), gx1 = xOf(tms(sel, 1));
        if (ind && my < cy && std::fabs(mx - gx0) < 7) ghover = 0;
        if (ind && my >= cy && std::fabs(mx - gx1) < 7) ghover = 1;
        if (edrag >= 0) ghover = edrag;
        if (ghover >= 0) {
            Col c;
            lineCol(ui, sel, 1, c);
            ui.cv().fillCircle(ui.X(ghover == 0 ? gx0 : gx1), ui.Y(ghover == 0 ? cy - 7 : cy + 7), ui.W(5), Paint(c));
            ui.cursor = CUR_HRESIZE;
        }
    }
    ui.endArea();
    if (ind && ui.lclick && !ui.used && ghover >= 0 && !ui.busy()) {
        ui.used = true;
        edrag = ghover;
        int c = (edrag == 1 && D.get(ep(sel, K_LINK)) < 0.5) ? 1 : 0;
        D.beginEdit(ep(sel, c ? K_SR : K_SL));
        D.beginEdit(ep(sel, c ? K_TR : K_TL));
    }
    if (edrag >= 0) {
        int c = (edrag == 1 && D.get(ep(sel, K_LINK)) < 0.5) ? 1 : 0;
        int ps = ep(sel, c ? K_SR : K_SL), pt = ep(sel, c ? K_TR : K_TL);
        if (ui.ldown) {
            double nt = std::max(1.0, std::min(8000.0, (mx - r.x) / r.w * span));
            if (iround(D.get(ps)) == 0) D.edit(pt, nt);
            else {
                int bj = 1;
                double bd = 1e9;
                for (int j = 1; j <= 16; ++j) {
                    double d1 = std::fabs(std::log(YBEATS[j] * 60000 / tempo() / nt));
                    if (d1 < bd) { bd = d1; bj = j; }
                }
                D.edit(ps, bj);
            }
        } else {
            D.endEdit(ps);
            D.endEdit(pt);
            edrag = -1;
        }
    }
    char hint[200];
    if (ghover >= 0) {
        char b[32];
        f_ms(tms(sel, ghover), b, sizeof b);
        snprintf(hint, sizeof hint, "Line %c, %s: %s  -  drag sideways to change the time", letter(sel), ghover == 0 ? "left" : "right", b);
    } else {
        snprintf(hint, sizeof hint, "repeats over time (left above, right below); drag the selected line's first repeat");
    }
    ui.textFit(R(r.x + 22, r.y + 5, r.w - 30, 12), hint, ghover >= 0 ? th::text() : th::faint(), F_REGULAR, 10.5f);
    ui.stroke(r, Paint(th::border()), 6);
}

void DelayView::meters(Ui &ui, const R &r)
{
    double range = av.meterRange();
    float top = r.y + 22, h = r.h - 22 - 16;
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

// ---- the selected line's filters (high pass, low pass, bell) over the spectrum ----
void DelayView::filters(Ui &ui, const R &r)
{
    FreqScale fs(r);
    auto ey = [&](double db) { return r.y + r.h * 0.5f - (float)(db / 18) * r.h * 0.5f; };
    int o = EB0 + sel * EBN;
    double hpf = D.get(o + K_HP), lpf = D.get(o + K_LP), bf = D.get(o + K_BF), bg = D.get(o + K_BG), bqv = D.get(o + K_BQ);
    float nx[3] = {(float)fs.x(hpf), (float)fs.x(lpf), (float)fs.x(bf)}, ny[3] = {ey(0), ey(0), ey(bg)};
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        freqGrid(ui, r);
        for (int d = -12; d <= 12; d += 6) {
            ui.fill(R(r.x, ey(d), r.w, 1 / ui.s), Paint(d == 0 ? th::gridHi() : th::grid()));
            char s[8];
            snprintf(s, sizeof s, d > 0 ? "+%d" : "%d", d);
            ui.text(R(r.x + 6, ey(d) - 12, 30, 11), s, th::faint(), F_REGULAR, 9.5f);
        }
        float mx = ui.mx(), my = ui.my();
        bool ind = ui.hit(r);
        nhover = -1;
        if (ind) {
            float best = 1e9f;
            for (int i = 0; i < 3; ++i) {
                float d = std::hypot(mx - nx[i], my - ny[i]);
                if (d < 10 && (i == 2 ? d <= best : d < best)) { best = d; nhover = i; }
            }
        }
        if (fdrag >= 0) nhover = fdrag;
        if (nhover >= 0) ui.cursor = nhover == 2 ? CUR_MOVE : CUR_HRESIZE;
        if (ind && ui.lclick && !ui.used && nhover >= 0 && !ui.busy()) {
            ui.used = true;
            fdrag = nhover;
            if (fdrag == 0) D.beginEdit(o + K_HP);
            if (fdrag == 1) D.beginEdit(o + K_LP);
            if (fdrag == 2) { D.beginEdit(o + K_BF); D.beginEdit(o + K_BG); }
        }
        if (ind && ui.rclick && !ui.used && nhover >= 0 && !ui.busy()) {
            ui.used = true;
            int which = nhover;
            DelayPlugin *pp = &D;
            ui.menu({MenuItem::item("Reset this filter", 1)}, mx, my, [pp, which, o](int k) {
                if (k != 1) return;
                if (which == 0) pp->setOnce(o + K_HP, pp->T.def(o + K_HP));
                if (which == 1) pp->setOnce(o + K_LP, pp->T.def(o + K_LP));
                if (which == 2) pp->setOnce(o + K_BG, 0);
            });
        }
        if (ind && ui.wheel != 0 && !ui.used && !(ui.mods & M_SHIFT) && nhover == 2) {
            ui.used = true;
            D.setOnce(o + K_BQ, std::max(0.2, std::min(8.0, bqv * std::exp((ui.wheel > 0 ? 1 : -1) * 0.12))));
        }
        if (fdrag >= 0) {
            if (ui.ldown) {
                double fv = std::max(20.0, std::min(20000.0, fs.f(mx)));
                if (fdrag == 0) D.edit(o + K_HP, std::max(20.0, std::min(2000.0, fv)));
                if (fdrag == 1) D.edit(o + K_LP, std::max(200.0, std::min(20000.0, fv)));
                if (fdrag == 2) {
                    D.edit(o + K_BF, fv);
                    D.edit(o + K_BG, std::floor(std::max(-18.0, std::min(18.0, (r.y + r.h * 0.5 - my) / (r.h * 0.5) * 18)) * 10 + 0.5) / 10);
                }
            } else {
                if (fdrag == 0) D.endEdit(o + K_HP);
                if (fdrag == 1) D.endEdit(o + K_LP);
                if (fdrag == 2) { D.endEdit(o + K_BF); D.endEdit(o + K_BG); }
                fdrag = -1;
            }
        }
        char hint[160], b[32];
        if (nhover == 0) { f_hz(hpf, b, sizeof b); snprintf(hint, sizeof hint, "High pass: %s", b); }
        else if (nhover == 1) { f_hz(lpf, b, sizeof b); snprintf(hint, sizeof hint, "Low pass: %s", b); }
        else if (nhover == 2) { f_hz(bf, b, sizeof b); snprintf(hint, sizeof hint, "Bell: %s  %+.1f dB  Q %.2f  (wheel: Q)", b, bg, bqv); }
        else snprintf(hint, sizeof hint, "Line %c's loop: H high pass, L low pass, B bell", letter(sel));
        ui.textFit(R(r.x + 40, r.y + 5, r.w - 46, 12), hint, nhover >= 0 ? th::text() : th::faint(), F_REGULAR, 10.5f);
        ui.stroke(r, Paint(th::border()), 6);
    }
    if (!ui.drawD()) return;
    Canvas &c = ui.cv();
    double sr = D.sampleRate();
    c.pushClip(ui.X(r.x + 1), ui.Y(r.y + 1), ui.W(r.w - 2), ui.W(r.h - 2));
    int an = iclamp(iround(D.get(P_AN)), 0, 3);
    if (live && an > 0) {
        int ap = D.view.an.ap.load(std::memory_order_acquire);
        if (an == 1 || an == 3) {
            spi.update(D.view.an.in, AnFeed::N - 1, ap, ui.dt, sr);
            Paint p = Paint::vertical(ui.Y(r.y), Col::hex(0x3a3a46), ui.Y(r.b()), Col::hex(0x1c1c22));
            drawSpectrum(ui, fs, spi, sr, &p, nullptr);
        }
        if (an == 2 || an == 3) {
            spo.update(D.view.an.out, AnFeed::N - 1, ap, ui.dt, sr);
            Paint p = Paint::vertical(ui.Y(r.y), Col::hex(0x40260d), ui.Y(r.b()), Col::hex(0x1a1008));
            Col lc = Col::hex(0xc8c3bb).alpha(0.5f);
            drawSpectrum(ui, fs, spo, sr, an == 2 ? &p : nullptr, &lc, 1.1f);
        }
    }
    freqLabels(ui, r);
    // the three filters as the loop has them
    double co[60];
    int ns[3];
    ns[0] = hpf > 20.5 ? flt::coef(6, hpf, 0, 0.7071, 0, co, sr) : 0;
    ns[1] = lpf < 19500 ? flt::coef(10, lpf, 0, 0.7071, 0, co + 20, sr) : 0;
    ns[2] = std::fabs(bg) >= 0.05 ? flt::coef(0, bf, bg, bqv, 0, co + 40, sr) : 0;
    int cn = (int)std::floor(r.w / 2);
    std::vector<Pt> pts(cn + 1);
    for (int i = 0; i <= cn; ++i) {
        float px = r.x + i * r.w / cn;
        double f = fs.f(px), w = 2 * PI * f / sr, cw = std::cos(w), c2 = std::cos(2 * w), tot = 0;
        for (int j = 0; j < 3; ++j)
            if (ns[j]) tot += 10 * std::log10(std::max(1e-12, flt::mag2(co + j * 20, ns[j], cw, c2)));
        pts[i] = {ui.X(px), ui.Y(ey(std::max(-18.0, std::min(18.0, tot))))};
    }
    Col lc;
    lineCol(ui, sel, 1, lc);
    c.strokePolyline(pts.data(), cn + 1, ui.W(1.8f), Paint(lc));
    static const char *nl[3] = {"H", "L", "B"};
    for (int i = 0; i < 3; ++i) {
        float rad = i == nhover ? 8 : 6.5f;
        c.fillCircle(ui.X(nx[i]), ui.Y(ny[i]), ui.W(rad), Paint(i == nhover ? lc.lighter(0.2f) : lc));
        ui.text(R(nx[i] - 8, ny[i] - 7, 16, 14), nl[i], Col::hex(0x0d0b08), F_BOLD, 10, 1);
    }
    c.popClip();
}

// ---- the tabs: the three lines (a dot says a line is on; double-click: on / off) with how each pair connects
// between them, the reverb, the ducking, Freeze, the modulation ----
void DelayView::tabs(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float h = 28, cw = 46, fw = 84, g = 6;
    const float tw = (DW - 2 * M - 2 * cw - fw - 8 * g) / 6;
    int nmod = 0;
    for (int s = 0; s < NMOD; ++s)
        if (D.get(mp(s, M_TYPE)) >= 0.5) nmod++;
    // a tab: its box, its text, the bar under it while it's the one shown
    auto box = [&](const R &r, bool active, const Col &bar) {
        bool hv = ui.hit(r);
        ui.fill(r, Paint(active ? th::raised2() : (hv ? th::hover() : th::panel())), 6);
        ui.stroke(r, Paint(active ? th::borderHi() : th::border()), 6);
        if (active) ui.fill(R(r.x + 8, r.b() - 2.5f, r.w - 16, 2.5f), Paint(bar), 1.25f);
        if (hv) ui.cursor = CUR_HAND;
        return hv && ui.lclick && !ui.used && !ui.busy();
    };
    float x = M;
    for (int k = 0; k < NL; ++k) {
        R r(x, y, tw, h);
        bool on = D.get(ep(k, K_ON)) >= 0.5, active = tab == 0 && sel == k;
        Col c;
        lineCol(ui, k, on ? 1.f : 0.22f, c);
        bool clk = box(r, active, th::orange());
        if (on) ui.fill(R(r.x + 10, r.y + h / 2 - 4, 8, 8), Paint(c), 4);
        else ui.stroke(R(r.x + 10, r.y + h / 2 - 4, 8, 8), Paint(c.alpha(3)), 4);
        char lb[16];
        snprintf(lb, sizeof lb, "LINE %c", letter(k));
        ui.text(R(r.x + 26, r.y, r.w - 30, h), lb, active ? th::text() : (on ? th::dim() : th::faint()), F_SEMIBOLD, 11.5f);
        if (clk) {
            ui.used = true;
            bool dbl = ui.now - clickT < 0.35 && clickK == k;
            clickT = ui.now;
            clickK = k;
            tab = 0;
            if (sel != k) D.setOnce(P_SEL, k);
            sel = k;
            if (dbl) { D.setOnce(ep(k, K_ON), on ? 0 : 1); clickT = -10; }
        }
        x += tw + g;
        if (k < NL - 1) {
            // how this line and the next connect
            int id = k == 0 ? P_C12 : P_C23, cv = iclamp(iround(D.get(id)), 0, 2);
            R cr(x, y + 2, cw, h - 4);
            static const Icon CI[3] = {IC_PAR, IC_SER, IC_CHAIN};   // parallel //, series --, linked (a chain)
            // purple while both lines play (whichever way they connect), greyed while either is off
            bool both = on && D.get(ep(k + 1, K_ON)) >= 0.5;
            bool clk = ui.button(cr, "", both, both, BS_PURPLE);
            ui.icon(CI[cv], cr.x + cr.w * 0.5f, cr.y + cr.h * 0.5f, 18, both ? Col(1, 1, 1) : th::faint());
            if (clk) {
                std::vector<MenuItem> m;
                for (int i = 0; i < 3; ++i) m.push_back(MenuItem::item(L_CON[i], i + 1, i == cv));
                DelayPlugin *pp = &D;
                ui.menu(m, cr.x, cr.b() + 4, [pp, id](int r2) { if (r2 > 0) pp->setOnce(id, r2 - 1); }, 120);
            }
            x += cw + g;
        }
    }
    // the reverb and the ducking (a dot while they do something)
    for (int t = 0; t < 2; ++t) {
        R r(x, y, tw, h);
        int me = t == 0 ? 2 : 3;
        bool lit = t == 0 ? reverbOn() : (D.get(P_DUCK) >= 0.5 || D.get(P_RDUCK) >= 0.5);
        if (box(r, tab == me, th::orange())) { ui.used = true; tab = me; }
        if (lit) ui.fill(R(r.x + 10, r.y + h / 2 - 3, 6, 6), Paint(th::green()), 3);
        ui.text(R(r.x + 22, r.y, r.w - 26, h), t == 0 ? "REVERB" : "DUCKING", tab == me ? th::text() : th::dim(), F_SEMIBOLD, 11.5f);
        x += tw + g;
    }
    toggle(ui, R(x, y + 1, fw, h - 2), "Freeze", P_FREEZE, BS_PURPLE, true);
    x += fw + g;
    {
        R r(x, y, DW - M - x, h);
        if (box(r, tab == 1, th::purple())) { ui.used = true; tab = 1; }
        ui.text(R(r.x + 12, r.y, r.w - 40, h), "MODULATION", tab == 1 ? th::text() : th::dim(), F_SEMIBOLD, 11.5f);
        if (nmod) {
            char b[8];
            snprintf(b, sizeof b, "%d", nmod);
            ui.badge(r.r() - 10 - ui.badgeW(b), r.y + 7, b, Col(1, 1, 1), th::purple());
        }
    }
}

// a time control: label, the note value (or Free) and the time itself
void DelayView::timeRow(Ui &ui, const R &r, const char *lbl, int pT, int pS, bool en)
{
    const float lw = 56;
    label(ui, R(r.x, r.y, lw, r.h), lbl, en);
    enumDD(ui, R(r.x + lw, r.y, 92, r.h), pS, L_SYNC, 17, nullptr, en);
    int s = iclamp(iround(D.get(pS)), 0, 16);
    double ms = s > 0 ? YBEATS[s] * 60000 / tempo() : D.get(pT);
    char b[32];
    f_ms(ms, b, sizeof b);
    ui.valueBox(R(r.x + lw + 100, r.y + 2, r.w - lw - 100, r.h - 4), pT, en && s == 0, KD_LOG, std::log(8000.0) / 138, std::log(8000.0) / 1380, 2,
                s > 0 ? b : nullptr);
}

// one of the list's groups: its header (click to open or close it; the summary says what is set while it is closed)
// and, when open, the space for its rows. Returns whether it is open; y moves past it.
bool DelayView::group(Ui &ui, int g, float &y, float w, const char *name, const std::string &sum, int rows, bool global, bool lit)
{
    int bits = iround(D.get(P_GRP));
    bool open = (bits >> g) & 1;
    R hr(M, y, w, 26);
    bool hv = ui.hit(hr);
    if (ui.drawS()) {
        ui.fill(hr, Paint(hv ? th::hover() : th::panel()), 5);
        ui.stroke(hr, Paint(open ? th::borderHi() : th::border()), 5);
        // the chevron: down when open, right when closed
        float cx = hr.x + 14, cy = hr.y + 13;
        Col cc = open ? th::orange() : th::dim();
        if (open) ui.icon(IC_CHEVRON, cx, cy, 11, cc);
        else { ui.line(cx - 2, cy - 4, cx + 2, cy, cc, 1.5f); ui.line(cx + 2, cy, cx - 2, cy + 4, cc, 1.5f); }
        ui.text(R(hr.x + 28, hr.y, 160, 26), name, open ? th::text() : th::dim(), F_BOLD, 11.5f);
        float sx = hr.x + 28 + ui.textW(F_BOLD, 11.5f, name) + 14;
        if (lit) { ui.fill(R(sx - 4, cy - 3, 6, 6), Paint(th::green()), 3); sx += 10; }
        if (global) {
            ui.text(R(sx, hr.y, 90, 26), "all lines", th::purple(), F_SEMIBOLD, 10.5f);
            sx += 74;
        }
        if (!open) ui.textFit(R(sx, hr.y, hr.r() - sx - 10, 26), sum.c_str(), th::faint(), F_REGULAR, 11);
        if (hv) ui.cursor = CUR_HAND;
        if (hv && ui.lclick && !ui.used && !ui.busy()) {
            ui.used = true;
            D.setOnce(P_GRP, bits ^ (1 << g));
        }
    }
    y += 26 + 6;
    if (open) y += rows * 32 + 4;
    return open;
}

// the selected line: its top row (on, mode, R = L, level, reverb send, the links), then its settings as groups to open
// and close, in a list that scrolls; which groups are open is the same for all three lines
void DelayView::lineTab(Ui &ui, float y, float h)
{
    int k = sel, o = EB0 + k * EBN;
    bool on = D.get(o + K_ON) >= 0.5;
    int mode = iround(D.get(o + K_MODE)), chr = iround(D.get(o + K_CHAR)), shm = iround(D.get(o + K_SH)), taps = iround(D.get(o + K_TAPS));
    bool rev = (shm & 2) != 0;
    auto fmt = [&](int id) {
        char b[48];
        D.T.format(id, D.get(id), b, sizeof b);
        return std::string(b);
    };
    Col lc;
    lineCol(ui, k, 1, lc);
    if (ui.drawS()) {
        toggle(ui, R(M, y, 64, 26), "On", o + K_ON, BS_NORMAL);
        int cv = iclamp(mode, 0, 2);
        R r(M + 72, y, 190, 26);
        if (ui.dropdown(r, L_MODE[cv], true, "Mode")) {
            std::vector<MenuItem> m;
            for (int i = 0; i < 3; ++i) m.push_back(MenuItem::item(L_MODE[i], i + 1, i == cv));
            DelayPlugin *pp = &D;
            int id = o + K_MODE;
            ui.menu(m, r.x, r.b() + 4, [pp, id](int r2) { if (r2 > 0) pp->setOnce(id, r2 - 1); }, r.w);
        }
        toggle(ui, R(M + 270, y, 72, 26), "R = L", o + K_LINK, BS_NORMAL);
        // four places: level, reverb send (while the reverb is on), the link with the line before, with the line after
        const float sx = M + 352, sw = (DW - M - sx) / 4;
        auto slot = [&](int i, const char *lbl, int id, bool en) {
            float xx = sx + i * sw;
            label(ui, R(xx + 6, y, 62, 26), lbl, en);
            markSlide(ui, R(xx + 68, y + 1, sw - 72, 24), id, SC_LIN, lc, en);
        };
        slot(0, "Level", o + K_LEVEL, on);
        if (reverbOn()) slot(1, "Rev send", o + K_RSND, on);
        char lb[16];
        if (k > 0 && iround(D.get(k == 1 ? P_C12 : P_C23)) == 2) {
            snprintf(lb, sizeof lb, "Link %c", letter(k - 1));
            slot(2, lb, k == 1 ? P_LK12 : P_LK23, true);
        }
        if (k < NL - 1 && iround(D.get(k == 0 ? P_C12 : P_C23)) == 2) {
            snprintf(lb, sizeof lb, "Link %c", letter(k + 1));
            slot(3, lb, k == 0 ? P_LK12 : P_LK23, true);
        }
    }
    y += 34;
    listR = R(M, y, DW - 2 * M, h - 34);
    const float sbw = 10, lw = DW - 2 * M - sbw - 6;   // the list's width (the scrollbar on the right)
    const float cw = (lw - 2 * 20) / 3, rh = 26, dy = 32;
    float x[3] = {M, M + cw + 20, M + 2 * (cw + 20)};
    if (ui.drawS()) ui.beginArea(listR);
    float yy = y - scroll;
    auto row = [&](int col, float ry, int r, const char *lbl, int kk, int sc) { markRow(ui, R(x[col], ry + r * dy, cw, rh), lbl, o + kk, sc, on); };
    auto prow = [&](int col, float ry, int r, const char *lbl, int id, int sc, bool en = true) {
        ui.paramRow(R(x[col], ry + r * dy, cw, rh), lbl, id, sc, 72, 84, en);
    };
    char sb[240];
    float gy;
    bool rl = D.get(o + K_LINK) >= 0.5;
    // time: left, right (unless it follows the left), glide; the taps
    snprintf(sb, sizeof sb, "L %s%s%s   glide %s   %s%s%s", fmt(o + K_SL).c_str(), rl ? "" : "   R ", rl ? "" : fmt(o + K_SR).c_str(),
             fmt(o + K_GLIDE).c_str(), L_TAPS[iclamp(taps, 0, 3)], taps >= 1 ? ", " : "", taps >= 1 ? L_TPAT[iclamp(iround(D.get(o + K_TPAT)), 0, 3)] : "");
    gy = yy + 32;
    if (group(ui, 0, yy, lw, "TIME", sb, 2) && ui.drawS()) {
        timeRow(ui, R(x[0], gy, cw, rh), "Time L", o + K_TL, o + K_SL, on);
        if (!rl) timeRow(ui, R(x[1], gy, cw, rh), "Time R", o + K_TR, o + K_SR, on);
        prow(rl ? 1 : 2, gy, 0, "Glide", o + K_GLIDE, SC_LOG, on);
        if (rev) {
            ui.text(R(x[0], gy + dy, lw, rh), "No taps while the line reverses (each repeat plays backwards).", th::faint(), F_REGULAR, 11.5f);
        } else {
            enumDD(ui, R(x[0], gy + dy, cw, rh), o + K_TAPS, L_TAPS, 4, "Taps", on);
            if (taps >= 1) {
                enumDD(ui, R(x[1], gy + dy, cw, rh), o + K_TPAT, L_TPAT, 4, "Tap pattern", on);
                prow(2, gy, 1, "Tap level", o + K_TLVL, SC_LIN, on);
            }
        }
    }
    // feedback, character, saturation, tape movement
    snprintf(sb, sizeof sb, "feedback %s   %s   %s   drive %s", fmt(o + K_FB).c_str(), L_CHAR[iclamp(chr, 0, 3)], fmt(o + K_SAT).c_str(),
             fmt(o + K_DRIVE).c_str());
    gy = yy + 32;
    if (group(ui, 3, yy, lw, "CHARACTER", sb, 3) && ui.drawS()) {
        row(0, gy, 0, "Feedback", K_FB, SC_LIN);
        enumDD(ui, R(x[1], gy, cw, rh), o + K_CHAR, L_CHAR, 4, "Character", on);
        if (chr > 0) row(2, gy, 0, "Age", K_AGE, SC_LIN);
        enumDD(ui, R(x[0], gy + dy, cw, rh), o + K_SAT, L_SAT, 24, "Saturation", on);
        row(1, gy, 1, "Drive", K_DRIVE, SC_LIN);
        row(2, gy, 1, "Wow", K_WOW, SC_LIN);
        row(2, gy, 2, "Flutter", K_FLUT, SC_LIN);
    }
    // stereo
    snprintf(sb, sizeof sb, "pan %s   width %s%s%s", fmt(o + K_PAN).c_str(), fmt(o + K_WIDTH).c_str(), mode == 0 ? "   cross " : "",
             mode == 0 ? fmt(o + K_CROSS).c_str() : "");
    gy = yy + 32;
    if (group(ui, 1, yy, lw, "STEREO", sb, 1) && ui.drawS()) {
        row(0, gy, 0, "Pan", K_PAN, SC_LIN);
        row(1, gy, 0, "Width", K_WIDTH, SC_LIN);
        if (mode == 0) row(2, gy, 0, "Cross", K_CROSS, SC_LIN);
    }
    // the filters in the loop
    snprintf(sb, sizeof sb, "high pass %s   low pass %s   bell %s at %s", fmt(o + K_HP).c_str(), fmt(o + K_LP).c_str(), fmt(o + K_BG).c_str(),
             fmt(o + K_BF).c_str());
    gy = yy + 32;
    if (group(ui, 2, yy, lw, "EQ", sb, 2) && ui.drawS()) {
        row(0, gy, 0, "High pass", K_HP, SC_LOG);
        row(1, gy, 0, "Low pass", K_LP, SC_LOG);
        prow(0, gy, 1, "Bell freq", o + K_BF, SC_LOG, on);
        row(1, gy, 1, "Bell gain", K_BG, SC_LIN);
        prow(2, gy, 1, "Bell Q", o + K_BQ, SC_LOG, on);
    }
    // pitch and reverse
    snprintf(sb, sizeof sb, "%s%s%s", L_SH[iclamp(shm, 0, 3)], (shm & 1) ? "   " : "", (shm & 1) ? fmt(o + K_SEMI).c_str() : "");
    gy = yy + 32;
    if (group(ui, 5, yy, lw, "PITCH & REVERSE", sb, 2) && ui.drawS()) {
        enumDD(ui, R(x[0], gy, cw, rh), o + K_SH, L_SH, 4, "Shift", on);
        if (shm & 1) row(1, gy, 0, "Pitch", K_SEMI, SC_LIN);
        if (shm == 1) prow(2, gy, 0, "Window", o + K_SHSZ, SC_LIN, on);
        if (rev) {
            enumDD(ui, R(x[0], gy + dy, cw, rh), o + K_SHSY, L_RCH, 17, "Chunk", on);
            ui.text(R(x[1], gy + dy, lw - (x[1] - x[0]), rh),
                    D.get(o + K_SHSY) >= 0.5 ? "Each chunk (on the beat) plays backwards, every repeat too."
                                             : "Each repeat plays backwards, a chunk of the delay time at a time.",
                    th::faint(), F_REGULAR, 11.5f);
        } else if (shm == 1) {
            ui.text(R(x[0], gy + dy, lw, rh), "Pitch in the loop: each repeat is shifted again (a staircase).", th::faint(), F_REGULAR, 11.5f);
        }
    }
    // diffusion
    snprintf(sb, sizeof sb, "diffusion %s   size %s", fmt(o + K_DIFF).c_str(), fmt(o + K_DSZ).c_str());
    gy = yy + 32;
    if (group(ui, 4, yy, lw, "DIFFUSION", sb, 1) && ui.drawS()) {
        row(0, gy, 0, "Diffusion", K_DIFF, SC_LIN);
        prow(1, gy, 0, "Diff size", o + K_DSZ, SC_LIN, on);
    }
    contentH = yy + scroll - y;
    if (!ui.drawS()) return;
    // the wheel scrolls where it moves no value; the scrollbar on the right
    float maxs = std::max(0.f, contentH - listR.h);
    if (ui.wheel != 0 && !ui.used && ui.hit(listR)) {
        ui.used = true;
        scroll -= ui.wheel * 48;
    }
    ui.endArea();
    R tr(listR.r() - sbw, listR.y, sbw, listR.h);
    if (maxs > 0) {
        float th2 = std::max(24.f, tr.h * listR.h / contentH), ty = tr.y + (tr.h - th2) * (scroll / maxs);
        R thr(tr.x + 2, ty, sbw - 4, th2);
        bool hv = ui.hit(tr);
        ui.fill(tr, Paint(th::well()), 4);
        ui.fill(thr, Paint(sdrag || ui.hit(thr) ? th::borderHi() : th::raised()), 3);
        if (hv && ui.lclick && !ui.used && !ui.busy()) {
            ui.used = true;
            sdrag = 1;
            sdy = ui.hit(thr) ? ui.my() - ty : th2 / 2;
        }
        if (sdrag) {
            if (ui.ldown) scroll = (ui.my() - sdy - tr.y) / std::max(1.f, tr.h - th2) * maxs;
            else sdrag = 0;
        }
    } else {
        sdrag = 0;
    }
    scroll = std::max(0.f, std::min(maxs, scroll));
}

// the reverb, after the three lines (each line's send sets how much of it goes in)
void DelayView::reverbTab(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float cw = (DW - 2 * M - 2 * 20) / 3, rh = 26, dy = 32;
    float x[3] = {M, M + cw + 20, M + 2 * (cw + 20)};
    bool ron = reverbOn();
    ui.text(R(M, y, DW - 2 * M, rh), "A shared reverb after the three lines: each line's send sets how much of its repeats go in.", th::faint(),
            F_REGULAR, 11.5f);
    y += dy;
    auto prow = [&](int col, int r, const char *lbl, int id, int sc, bool en = true) {
        ui.paramRow(R(x[col], y + r * dy, cw, rh), lbl, id, sc, 72, 84, en);
    };
    markRow(ui, R(x[0], y, cw, rh), "Reverb", P_RVB, SC_LIN, true);
    if (!ron) return;
    prow(1, 0, "Decay", P_RDEC, SC_LOG);
    prow(2, 0, "Size", P_RSIZ, SC_LIN);
    prow(0, 1, "Damping", P_RDMP, SC_LOG);
    prow(1, 1, "Pre-delay", P_RPRE, SC_LIN);
    prow(2, 1, "Shimmer", P_RSHM, SC_LIN);
    if (D.get(P_RSHM) >= 0.5) enumDD(ui, R(x[0], y + 2 * dy, cw, rh), P_RSHP, L_SHP, 4, "Shimmer");
    // the sends
    for (int k = 0; k < NL; ++k) {
        char lb[24];
        snprintf(lb, sizeof lb, "Line %c send", letter(k));
        ui.paramRow(R(x[k], y + 3 * dy + 8, cw, rh), lb, ep(k, K_RSND), SC_LIN, 86, 70, D.get(ep(k, K_ON)) >= 0.5);
    }
}

// ducking: the repeats and the reverb, each by its own amount, while the source is loud
void DelayView::duckTab(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float cw = (DW - 2 * M - 2 * 20) / 3, rh = 26, dy = 32;
    float x[3] = {M, M + cw + 20, M + 2 * (cw + 20)};
    ui.text(R(M, y, DW - 2 * M, rh), "Lowers the repeats and the reverb (each by its own amount) while the source is over the threshold.", th::faint(),
            F_REGULAR, 11.5f);
    y += dy;
    markRow(ui, R(x[0], y, cw, rh), "Delay ducking", P_DUCK, SC_LIN, true, 100, 72);
    ui.paramRow(R(x[1], y, cw, rh), "Reverb ducking", P_RDUCK, SC_LIN, 100, 72, reverbOn());
    if (D.get(P_DUCK) < 0.5 && D.get(P_RDUCK) < 0.5) return;
    ui.paramRow(R(x[2], y, cw, rh), "Threshold", P_DTHR, SC_LIN, 100, 72);
    ui.paramRow(R(x[0], y + dy, cw, rh), "Attack", P_DATT, SC_LOG, 100, 72);
    ui.paramRow(R(x[1], y + dy, cw, rh), "Release", P_DREL, SC_LOG, 100, 72);
    enumDD(ui, R(x[2], y + dy, cw, rh), P_DSRC, L_DSRC, 2, "From");
}

void DelayView::modTab(Ui &ui, float y)
{
    static const gui::ModSlotLabels L = {L_MLINE, 5, L_MGLOB, 5, L_MPAR, 12, L_MSRC, MB0};
    gui::modSlots(ui, D, M, y, DW - 2 * M, 37, L, D.view.mv, live);
}

void DelayView::frame(Ui &ui)
{
    sel = iclamp(iround(D.get(P_SEL)), 0, NL - 1);
    modsOn = false;
    for (int s = 0; s < NMOD; ++s)
        if (D.get(mp(s, M_TYPE)) >= 0.5) modsOn = true;
    R gear = begin(ui, 0, D.view.side.load() != 0);
    if (ui.drawS()) marks.clear();
    float y = 66;
    const float dh = 156, mapW = 520, metW = 58;
    echoMap(ui, R(M, y, mapW, dh));
    meters(ui, R(M + mapW + 6, y, metW, dh));
    filters(ui, R(M + mapW + metW + 16, y, DW - 2 * M - mapW - metW - 16, dh));
    y += dh + 10;
    tabs(ui, y);
    y += 28 + 10;
    const float LH = 2 * 32 + 8 * 32 + 2;   // the tab's area (the list scrolls in it)
    if (tab == 0) lineTab(ui, y, LH);
    else if (tab == 1) modTab(ui, y);
    else if (tab == 2) reverbTab(ui, y);
    else duckTab(ui, y);
    y += LH;
    // the whole delay: Input (Ctrl moves the output back), the dry and the wet levels
    if (ui.drawS()) {
        ui.fill(R(M, y, DW - 2 * M, 1), Paint(th::border()));
        const float w3 = (DW - 2 * M - 2 * 20) / 3;
        inputRow(ui, R(M, y + 8, w3, 26), P_IN, 56, 84);
        markRow(ui, R(M + w3 + 20, y + 8, w3, 26), "Dry", P_DRY, SC_LIN, true, 56, 84);
        markRow(ui, R(M + 2 * (w3 + 20), y + 8, w3, 26), "Wet", P_WET, SC_LIN, true, 56, 84);
    }
    y += 8 + 26 + 12;
    actCardAt(ui, y);
    // where the modulation has the bars it moves now
    if (ui.drawD() && live && modsOn) {
        Canvas &cv = ui.cv();
        for (const Mark &m : marks) {
            const ParamDef *d = D.T.get(m.id);
            if (!d) continue;
            if (m.inList && (m.bar.b() <= listR.y || m.bar.y >= listR.b())) continue;
            double e = D.view.eff[m.id].load(), v = D.get(m.id);
            if (std::fabs(e - v) <= 0.0002 * (d->max - d->min)) continue;
            double f = std::max(0.0, std::min(1.0, barFrac(m.sc, e, d->min, d->max)));
            if (m.inList) cv.pushClip(ui.X(listR.x), ui.Y(listR.y), ui.W(listR.w), ui.W(listR.h));
            ui.fill(R(m.bar.x + (float)f * m.bar.w - 1.5f, m.bar.y - 1, 3, m.bar.h + 2), Paint(th::purple()), 1.5f);
            if (m.inList) cv.popClip();
        }
    }
    finish(ui, gear, [this](Ui &u, const R &in, float yy) {
        label(u, R(in.x, yy, 64, 24), "Quality");
        enumDD(u, R(in.x + 64, yy, 110, 24), P_HQ, L_HQ, 2);
        u.text(R(in.x + 188, yy, in.w - 188, 24), "High: interpolation and saturation at twice the rate (7 samples the times allow for).", th::faint(),
               F_REGULAR, 11);
        yy += 24 + 10;
        label(u, R(in.x, yy, 64, 24), "Analyzer");
        enumDD(u, R(in.x + 64, yy, 110, 24), P_AN, L_AN, 4);
        u.text(R(in.x + 188, yy, in.w - 188, 24), "Pre: the input, filled. Post: the output, as a line. Only while the window is open.", th::faint(),
               F_REGULAR, 11);
        return yy + 24 + 10;
    });
}

// ---------------------------------------------------------------- the kind
static const char *s_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DELAY, CLAP_PLUGIN_FEATURE_REVERB,
                                   CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const PluginKind &kind()
{
    static const PluginKind k = {
        {CLAP_VERSION_INIT, "com.twostorms.delay", "TwoStorms Delay", "TwoStorms", "", "", "", TS_VERSION,
         "A three-line delay with an Act built in: free or synced times, taps, ping-pong, parallel / series / linked lines, pitch and reverse, "
         "diffusion, filters, tape / BBD / lo-fi character and saturation in the loop, ducking, freeze, a shared reverb with shimmer and eight "
         "modulation slots. No latency.",
         s_features},
        &table,
        &factoryPresets,
        "Delay",
        "TWOSTORMS DELAY",
        shm::KIND_POINT,
        shm::SK_DELAY,
        true,
        true,
        P_AB,
        &create,
        2,
        true,
    };
    return k;
}

} // namespace delay
} // namespace ts
