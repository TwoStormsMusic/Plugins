// TwoStorms Gate: a noise gate with an Act built in. Threshold with hysteresis, attack, hold, release, range
// (down to silence), optional exact lookahead, peak or RMS detector, key high-pass and low-pass, stereo link and
// key listen. The key can be the main input, the sidechain (inputs 3-4), or the output of a TwoStorms plug-in
// earlier on the same track (read sample for sample with no delay). Port of twostorms_gate.jsfx.
#include "gate_plugin.h"
#include "../../core/abmatch.h"
#include "../../core/act_view.h"
#include "../../core/clock.h"
#include "../../core/eel.h"
#include "../../gui/common.h"
#include "../../gui/editor.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ts {
namespace gate {

using namespace gui;

enum {
    P_THR = 50, P_HYS = 51, P_RNG = 52, P_ATT = 53, P_HLD = 54, P_REL = 55, P_LA = 56,
    P_SRC = 57,   // key: 0 main input, 1 sidechain (3-4), 2 the output of an earlier TwoStorms plug-in on this track
    P_KID = 58,   // that plug-in's ID
    P_DET = 59, P_DT = 60, P_HP = 61, P_LP = 62, P_LINK = 63, P_LISTEN = 64,
    P_AB = 65,    // the set playing: 0 = A, 1 = B
};

// ---------------------------------------------------------------- parameters
static void f_db1(double v, char *b, int n) { snprintf(b, n, "%.1f dB", v); }
static void f_rng(double v, char *b, int n) { if (v >= 89.5) snprintf(b, n, "-inf"); else snprintf(b, n, "%.1f dB", v); }
static void f_att(double v, char *b, int n) { snprintf(b, n, v < 1 ? "%.2f ms" : (v < 10 ? "%.1f ms" : "%.0f ms"), v); }
static void f_ms0(double v, char *b, int n) { snprintf(b, n, "%.0f ms", v); }
static void f_rel(double v, char *b, int n) { if (v < 1000) snprintf(b, n, "%.0f ms", v); else snprintf(b, n, "%.2f s", v / 1000); }
static void f_ms1(double v, char *b, int n) { snprintf(b, n, "%.1f ms", v); }
static void f_pct(double v, char *b, int n) { snprintf(b, n, "%.0f%%", v); }
static void f_hp(double v, char *b, int n) { if (v <= 20.5) snprintf(b, n, "off"); else snprintf(b, n, "%.0f Hz", v); }
static void f_lp(double v, char *b, int n)
{
    if (v >= 19999.5) snprintf(b, n, "off");
    else if (v >= 1000) snprintf(b, n, "%.2f kHz", v / 1000);
    else snprintf(b, n, "%.0f Hz", v);
}

static const ParamTable &table()
{
    static ParamTable t = [] {
        ParamTable t;
        addActParams(t, true);
        const char *M = "Gate";
        const uint32_t H = PF_HOST | PF_AB;
        t.add(P_THR, "Threshold", -90, 0, -45, H, f_db1, M).unit = "dB";
        t.add(P_HYS, "Hysteresis", 0, 24, 4, H, f_db1, M).unit = "dB";
        auto &rg = t.add(P_RNG, "Range", 0, 90, 60, H, f_rng, M);
        rg.unit = "dB";
        rg.maxText = "-inf";
        t.add(P_ATT, "Attack", 0.01, 100, 0.5, H, f_att, M).unit = "ms";
        t.add(P_HLD, "Hold", 0, 2000, 50, H | PF_INT, f_ms0, M).unit = "ms";
        t.add(P_REL, "Release", 1, 5000, 150, H | PF_INT, f_rel, M).unit = "ms";
        auto &la = t.add(P_LA, "Lookahead", 0, 50, 0, H, f_ms1, M);
        la.unit = "ms";
        t.addEnum(P_SRC, "Key source", {"Main input", "Sidechain (3-4)", "TwoStorms plug-in"}, 0, PF_HOST | PF_PRESET, M);
        t.add(P_KID, "Key plug-in ID", 0, 999999, 0, PF_INT, fmt_int, M);
        t.addEnum(P_DET, "Detector", {"Peak", "RMS"}, 0, H, M);
        t.add(P_DT, "Detector time", 0.1, 100, 5, H, f_ms1, M).unit = "ms";
        auto &hp = t.add(P_HP, "Key HPF", 20, 2000, 20, H | PF_INT, f_hp, M);
        hp.unit = "Hz";
        hp.minText = "off";
        auto &lp = t.add(P_LP, "Key LPF", 200, 20000, 20000, H | PF_INT, f_lp, M);
        lp.unit = "Hz";
        lp.maxText = "off";
        t.add(P_LINK, "Stereo link", 0, 100, 100, H | PF_INT, f_pct, M).unit = "%";
        t.add(P_LISTEN, "Listen to the key", 0, 1, 0, PF_HOST | PF_INT | PF_BOOL, nullptr, M);
        t.addEnum(P_AB, "A/B set", {"A", "B"}, 0, PF_HOST, M);
        return t;
    }();
    return t;
}

// factory presets: Instruments, Buses, Creative, Weird
static std::string S(std::initializer_list<std::pair<int, double>> kv) { return presetStr(kv); }
static const std::vector<FactoryPreset> &factoryPresets()
{
    static std::vector<FactoryPreset> v = {
        // ---- guitar
        {"Instruments/Guitar/Clean: soft hum floor", "50=-60 51=6 52=15 53=1 54=80 55=300 56=0 59=0 60=5 61=60 62=20000 63=100"},
        {"Instruments/Guitar/Funk: tight staccato", "50=-45 51=5 52=50 53=0.2 54=10 55=40 56=0 59=0 60=5 61=100 62=20000 63=100"},
        {"Instruments/Guitar/Rock: high-gain hiss", "50=-50 51=6 52=40 53=0.5 54=15 55=60 56=0 59=0 60=5 61=80 62=20000 63=100"},
        {"Instruments/Guitar/Metal: tight chug (live, no latency)", "50=-42 51=6 52=40 53=0.5 54=15 55=60 56=0 59=0 60=5 61=100 62=20000 63=100"},
        {"Instruments/Guitar/Metal: tight chug (studio, 1 ms lookahead)", "50=-42 51=6 52=40 53=0.5 54=15 55=60 56=1 59=0 60=5 61=100 62=20000 63=100"},
        {"Instruments/Guitar/Metal: stop-start djent", "50=-38 51=5 52=80 53=0.05 54=5 55=25 56=1 59=0 60=3 61=150 62=20000 63=100"},
        {"Instruments/Guitar/Acoustic: room noise, gentle", S({{P_THR, -55}, {P_HYS, 6}, {P_RNG, 10}, {P_ATT, 2}, {P_HLD, 120}, {P_REL, 400}, {P_HP, 100}})},
        // ---- bass
        {"Instruments/Bass/Clean: soft noise floor", "50=-58 51=6 52=30 53=1 54=60 55=250 56=0 59=0 60=5 61=20 62=20000 63=100"},
        {"Instruments/Bass/Distorted: hiss killer", "50=-48 51=6 52=60 53=0.5 54=40 55=120 56=0 59=0 60=5 61=60 62=20000 63=100"},
        {"Instruments/Bass/Slap: tight", "50=-50 51=5 52=50 53=0.2 54=20 55=90 56=0 59=0 60=5 61=40 62=20000 63=100"},
        {"Instruments/Bass/Punk", "50=-45 51=5 52=70 53=0.3 54=30 55=100 56=0 59=0 60=5 61=50 62=20000 63=100"},
        {"Instruments/Bass/Pop punk", "50=-48 51=5 52=50 53=0.5 54=40 55=120 56=0 59=0 60=5 61=50 62=20000 63=100"},
        {"Instruments/Bass/Metal: tight chug", "50=-40 51=6 52=90 53=0.05 54=10 55=40 56=0 59=0 60=5 61=80 62=20000 63=100"},
        {"Instruments/Bass/Rock", "50=-50 51=5 52=40 53=0.5 54=50 55=200 56=0 59=0 60=5 61=40 62=20000 63=100"},
        {"Instruments/Bass/Jazz", "50=-60 51=6 52=10 53=2 54=100 55=400 56=0 59=0 60=5 61=20 62=20000 63=100"},
        {"Instruments/Bass/Pop", "50=-52 51=5 52=30 53=0.5 54=40 55=180 56=0 59=0 60=5 61=40 62=20000 63=100"},
        // ---- vocals
        {"Instruments/Vocals/Lead: gentle, between phrases", "50=-52 51=6 52=15 53=2 54=80 55=300 56=0 59=0 60=5 61=100 62=20000 63=100"},
        {"Instruments/Vocals/Lead: mic bleed (stage)", "50=-45 51=8 52=12 53=2 54=100 55=350 56=0 59=0 60=5 61=150 62=20000 63=100"},
        {"Instruments/Vocals/Backing: tight entries and ends", S({{P_THR, -42}, {P_HYS, 6}, {P_RNG, 40}, {P_ATT, 1}, {P_HLD, 30}, {P_REL, 120}, {P_HP, 150}})},
        {"Instruments/Vocals/Spoken word / podcast", "50=-48 51=6 52=20 53=2 54=60 55=250 56=0 59=0 60=5 61=120 62=20000 63=100"},
        // ---- drums
        {"Instruments/Drums/Kick", "50=-35 51=6 52=40 53=0.05 54=20 55=80 56=0 59=0 60=5 61=40 62=2000 63=100"},
        {"Instruments/Drums/Kick: inside mic (key low pass 120)", S({{P_THR, -30}, {P_HYS, 6}, {P_RNG, 60}, {P_ATT, 0.05}, {P_HLD, 15}, {P_REL, 60}, {P_LA, 1}, {P_HP, 30}, {P_LP, 300}})},
        {"Instruments/Drums/Snare", "50=-30 51=6 52=30 53=0.05 54=30 55=120 56=0 59=0 60=5 61=150 62=10000 63=100"},
        {"Instruments/Drums/Snare: keep the ghost notes (12 dB)", S({{P_THR, -36}, {P_HYS, 6}, {P_RNG, 12}, {P_ATT, 0.1}, {P_HLD, 40}, {P_REL, 150}, {P_HP, 150}, {P_LP, 8000}})},
        {"Instruments/Drums/Toms", "50=-38 51=8 52=40 53=0.1 54=80 55=300 56=0 59=0 60=5 61=60 62=6000 63=100"},
        {"Instruments/Drums/Hi-hat: tight (key high pass 5k)", S({{P_THR, -40}, {P_HYS, 4}, {P_RNG, 20}, {P_ATT, 0.1}, {P_HLD, 10}, {P_REL, 50}, {P_HP, 2000}})},
        {"Instruments/Drums/Room: chopped to the hits", S({{P_THR, -26}, {P_HYS, 4}, {P_RNG, 90}, {P_ATT, 0.1}, {P_HLD, 60}, {P_REL, 90}})},
        // ---- others
        {"Instruments/Keys & synths/Synth: tidy note ends", S({{P_THR, -50}, {P_HYS, 6}, {P_RNG, 40}, {P_ATT, 1}, {P_HLD, 20}, {P_REL, 200}})},
        {"Instruments/Keys & synths/Noise floor: soft", "50=-65 51=6 52=12 53=2 54=50 55=300 56=0 59=0 60=5 61=20 62=20000 63=100"},
        // ---- buses
        {"Buses/Instrument bus/Drum bus: tighter room (10 dB)", S({{P_THR, -34}, {P_HYS, 6}, {P_RNG, 10}, {P_ATT, 0.2}, {P_HLD, 50}, {P_REL, 180}})},
        {"Buses/Instrument bus/Guitar bus: high-gain silence", S({{P_THR, -50}, {P_HYS, 6}, {P_RNG, 60}, {P_ATT, 0.5}, {P_HLD, 20}, {P_REL, 80}, {P_HP, 80}})},
        {"Buses/Mixbus/Noise between parts (gentle 6 dB)", S({{P_THR, -60}, {P_HYS, 6}, {P_RNG, 6}, {P_ATT, 5}, {P_HLD, 200}, {P_REL, 800}, {P_DET, 1}, {P_DT, 20}})},
        {"Buses/Master/Silence between songs", S({{P_THR, -72}, {P_HYS, 8}, {P_RNG, 90}, {P_ATT, 10}, {P_HLD, 500}, {P_REL, 2000}, {P_DET, 1}, {P_DT, 30}})},
        // ---- creative
        {"Creative/Gated reverb (after a big reverb)", S({{P_THR, -28}, {P_HYS, 4}, {P_RNG, 90}, {P_ATT, 0.1}, {P_HLD, 250}, {P_REL, 30}})},
        {"Creative/Trance gate (key: a 1/16 pattern on 3-4)", S({{P_THR, -30}, {P_HYS, 2}, {P_RNG, 90}, {P_ATT, 0.5}, {P_HLD, 20}, {P_REL, 40}, {P_SRC, 1}})},
        {"Creative/Bass only with the kick (key: kick on 3-4)", S({{P_THR, -30}, {P_HYS, 4}, {P_RNG, 30}, {P_ATT, 0.2}, {P_HLD, 60}, {P_REL, 120}, {P_SRC, 1}})},
        {"Creative/Stutter chop (only the loud hits)", S({{P_THR, -14}, {P_HYS, 2}, {P_RNG, 90}, {P_ATT, 0.05}, {P_HLD, 0}, {P_REL, 15}})},
        {"Creative/Backing vocals follow the lead (key: lead on 3-4)", S({{P_THR, -40}, {P_HYS, 6}, {P_RNG, 60}, {P_ATT, 2}, {P_HLD, 40}, {P_REL, 150}, {P_SRC, 1}})},
        // ---- weird
        {"Weird/Swell (slow attack: no picking)", S({{P_THR, -40}, {P_HYS, 6}, {P_RNG, 90}, {P_ATT, 100}, {P_HLD, 100}, {P_REL, 300}})},
        {"Weird/Machine gun (chatters on purpose)", S({{P_THR, -24}, {P_HYS, 0}, {P_RNG, 90}, {P_ATT, 0.01}, {P_HLD, 0}, {P_REL, 1}, {P_DT, 0.1}})},
        {"Weird/Only the lows open it (key low pass 200)", S({{P_THR, -36}, {P_HYS, 4}, {P_RNG, 90}, {P_ATT, 0.5}, {P_HLD, 30}, {P_REL, 100}, {P_LP, 200}})},
        {"Weird/Only the highs open it (key high pass 2k)", S({{P_THR, -40}, {P_HYS, 4}, {P_RNG, 90}, {P_ATT, 0.5}, {P_HLD, 30}, {P_REL, 100}, {P_HP, 2000}})},
    };
    return v;
}

// ---------------------------------------------------------------- the engine
static constexpr int HN = 1024;

// written by the audio thread, read by the window
struct Display {
    std::atomic<float> hk[HN], hg[HN], ho[HN];   // history, 10 ms per entry: key level, gate gain, output level (dB)
    std::atomic<int> hp{0};
    std::atomic<int> st{0}, hd{0};               // the gate (left side): open, holding
    std::atomic<double> gdb{0};
    std::atomic<int> kst{0};                     // 0 fine, 1 key plug-in not found, 2 not before the gate, 3 no audio from it
    std::atomic<double> latms{0};
    std::atomic<int> side{0};
    Display()
    {
        for (int i = 0; i < HN; ++i) { hk[i] = -200; hg[i] = 0; ho[i] = -200; }
    }
};

// RBJ biquad, 12 dB/octave high-pass (hp) or low-pass, for the key
struct Bq {
    double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set(double f, double q, bool hp, double srate)
    {
        double w = 2 * PI * std::min(f, srate * 0.49) / srate, cw = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
        if (hp) { b0 = (1 + cw) * 0.5 / a0; b1 = -(1 + cw) / a0; }
        else { b0 = (1 - cw) * 0.5 / a0; b1 = (1 - cw) / a0; }
        b2 = b0;
        a1 = -2 * cw / a0;
        a2 = (1 - al) / a0;
    }
    inline double one(double x)
    {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

class Engine;
// one gate: the state (closed / open, with the hold counter) and the gain, a straight ramp in dB
struct Gt {
    double st = 0, hc = 0, gdb = 0, kk = 1, lastg = 1000, hd = 0;
    inline double run(double e, const Engine &E);
};

class EngineHost {
public:
    virtual ~EngineHost() {}
    virtual double pv(int id) const = 0;
    virtual double abv(int id) const = 0;
};

class Engine {
public:
    Engine(EngineHost &h, Act &a, UiLink &u, Display &d) : H(h), A(a), U(u), V(d), NLA0(16384, 0.0), NLA1(16384, 0.0) {}
    // gt_run reads these
    double n_thro = 0, n_thrc = 0, n_hold = 0, n_ru = 0, n_rd = 0, n_rng = 0;
    bool n_inf = false;
    AbMatch ab;

    void init(double sr)
    {
        srate = sr;
        for (int i = 0; i < HN; ++i) { V.hk[i] = -200; V.hg[i] = 0; V.ho[i] = -200; }
        n_lp = 0; n_hp = 0; n_hcnt = 0; n_kmax = 0; n_omax = 0; n_gmin = 0;
        n_lhp = -1; n_llp = -1; n_inited = false; n_ks = -1; n_kaud = false; n_kst = 0; n_aseq = -1; n_kp = 0; n_kidl = -1;
        n_e0 = n_e1 = n_m0 = n_m1 = 0;
        n_h10 = std::max(1.0, std::floor(srate * 0.01 + 0.5));
        g0.kk = 1; g1.kk = 1; g0.lastg = 1000; g1.lastg = 1000;
    }
    void reset()
    {
        std::fill(NLA0.begin(), NLA0.end(), 0.0);
        std::fill(NLA1.begin(), NLA1.end(), 0.0);
        kh0.z1 = kh0.z2 = kh1.z1 = kh1.z2 = kl0.z1 = kl0.z2 = kl1.z1 = kl1.z2 = 0;
    }
    static int latencyFor(double la, double sr) { return (int)std::max(0.0, std::min(16000.0, std::floor(la * 0.001 * sr + 0.5))); }

    // @block, the gate's part before the Act's
    void block(int samplesblock)
    {
        double a_now = A.now();
        ab.block(U, a_now, H.pv(P_AB) >= 0.5, samplesblock, srate);
        V.side.store(ab.side ? 1 : 0, std::memory_order_relaxed);
        n_uion = ab.uion;
        n_src = iclamp((int)std::floor(cpv(P_SRC) + 0.5), 0, 2);
        n_det = cpv(P_DET) >= 0.5;
        n_listen = cpv(P_LISTEN) >= 0.5;
        n_rng = cpv(P_RNG);
        n_inf = n_rng >= 89.5;
        n_thro = epow(10, cpv(P_THR) / 20);
        n_thrc = epow(10, (cpv(P_THR) - cpv(P_HYS)) / 20);
        n_hold = std::floor(cpv(P_HLD) * 0.001 * srate + 0.5);
        // the gain ramps over the whole range in the attack / release time (dB per sample)
        n_ru = n_rng / emax(1, cpv(P_ATT) * 0.001 * srate);
        n_rd = n_rng / emax(1, cpv(P_REL) * 0.001 * srate);
        if (!n_inited) { g0.gdb = -n_rng; g1.gdb = -n_rng; n_inited = true; }
        // lookahead: the audio waits, the key doesn't
        n_la = latencyFor(cpv(P_LA), srate);
        V.latms.store(n_la / srate * 1000, std::memory_order_relaxed);
        // key filters (12 dB/octave), the detector and the link
        double t = cpv(P_HP);
        if (t != n_lhp) { n_lhp = t; kh0.set(t, 0.7071, true, srate); kh1.set(t, 0.7071, true, srate); }
        n_hpon = t > 20.5;
        t = cpv(P_LP);
        if (t != n_llp) { n_llp = t; kl0.set(t, 0.7071, false, srate); kl1.set(t, 0.7071, false, srate); }
        n_lpon = t < 19999.5;
        n_dec = std::exp(-1 / (cpv(P_DT) * 0.001 * srate));
        n_rk = 1 - std::exp(-1 / (cpv(P_DT) * 0.001 * srate));
        n_lk = cpv(P_LINK) * 0.01;
    }

    // after the Act's @block: the key from an earlier TwoStorms plug-in on this track: its output samples, read
    // straight from shared memory. A plug-in before the gate in the chain finishes its whole block before the gate
    // starts the same block, so the key lines up with the gate's own input sample for sample, with no delay.
    void keyBlock(int samplesblock)
    {
        n_kaud = false;
        n_kst = 0;
        if (n_src == 2 && A.myslot >= 0) {
            double kid = H.pv(P_KID);
            n_ks = A.findSlot((int64_t)kid, n_ks);
            if (kid != n_kidl) { n_kidl = kid; n_aseq = -1; }
            if (n_ks < 0 || n_ks == A.myslot || !A.slotLive(n_ks)) {
                n_kst = 1;
                n_aseq = -1;
            } else if (!A.keyOk(n_ks)) {
                n_kst = 2;
                n_aseq = -1;
            } else {
                shm::Slot &b = shm::slot(n_ks);
                b.areq.fetch_add(1, std::memory_order_relaxed);   // ask it to write its output (it stops soon after)
                int64_t ln = b.alane.load(std::memory_order_relaxed) - 1;
                int64_t sq = b.aseq.load(std::memory_order_acquire);
                // it must have written a block since the gate's last one, of the same length
                if (ln >= 0 && ln < shm::AUD_LANES && sq != n_aseq && b.alen.load(std::memory_order_relaxed) == samplesblock) {
                    n_lane = &shm::get()->lane[ln];
                    n_kp = (int)b.astart.load(std::memory_order_relaxed) & shm::AUD_MASK;
                    n_kaud = true;
                } else {
                    n_kst = 3;
                }
                n_aseq = sq;
            }
        } else {
            n_aseq = -1;
            n_ks = -1;
        }
        V.kst.store(n_kst, std::memory_order_relaxed);
    }

    // @sample, the gate's part (before the Act)
    void samples(const double *const *in, const double *const *sc, double *const *out, int n)
    {
        if ((int)br0.size() < n) { br0.resize(n); br1.resize(n); bq0.resize(n); bq1.resize(n); }
        for (int i = 0; i < n; ++i) {
            // the audio always goes into the lookahead line, so switching lookahead on never plays old samples
            double r0 = in[0][i], r1 = in[1][i];
            NLA0[n_lp] = r0;
            NLA1[n_lp] = r1;
            int rp = (n_lp - n_la) & 16383;
            double d0 = NLA0[rp], d1 = NLA1[rp];
            br0[i] = r0; br1[i] = r1; bq0[i] = d0; bq1[i] = d1;
            n_lp = (n_lp + 1) & 16383;
            // the key: the main input, the sidechain (a mono sidechain on input 3 is used for both sides), or the
            // output of an earlier TwoStorms plug-in on this track (read from its lane; silence while nothing arrives)
            double k0, k1;
            if (n_src == 2) {
                if (n_kaud) {
                    k0 = n_lane->l[n_kp].load(std::memory_order_relaxed);
                    k1 = n_lane->r[n_kp].load(std::memory_order_relaxed);
                    n_kp = (n_kp + 1) & shm::AUD_MASK;
                } else { k0 = 0; k1 = 0; }
            } else if (n_src == 1) {
                double s2 = sc ? sc[0][i] : 0, s3 = sc ? sc[1][i] : 0;
                n_scenv = emax(std::fabs(s2), n_scenv * 0.99995);
                n_screnv = emax(std::fabs(s3), n_screnv * 0.99995);
                k0 = s2;
                k1 = n_screnv > 0.0000001 ? s3 : s2;
            } else { k0 = r0; k1 = r1; }
            if (n_hpon) { k0 = kh0.one(k0); k1 = kh1.one(k1); }
            if (n_lpon) { k0 = kl0.one(k0); k1 = kl1.one(k1); }
            if (n_det) {
                n_m0 += n_rk * (k0 * k0 - n_m0);
                n_m1 += n_rk * (k1 * k1 - n_m1);
                n_e0 = std::sqrt(n_m0) * 1.4142135623730951;
                n_e1 = std::sqrt(n_m1) * 1.4142135623730951;
            } else {
                double t = std::fabs(k0);
                n_e0 = t > n_e0 ? t : n_e0 * n_dec;
                t = std::fabs(k1);
                n_e1 = t > n_e1 ? t : n_e1 * n_dec;
            }
            if (n_e0 < 0.0000000001) n_e0 = 0;
            if (n_e1 < 0.0000000001) n_e1 = 0;
            // stereo link: each side moves toward the louder one
            double t = emax(n_e0, n_e1);
            n_e0 += n_lk * (t - n_e0);
            n_e1 += n_lk * (t - n_e1);
            double kg0 = g0.run(n_e0, *this), kg1;
            if (n_lk >= 0.9999) { kg1 = kg0; g1.gdb = g0.gdb; g1.st = g0.st; }
            else kg1 = g1.run(n_e1, *this);
            double o0 = d0 * kg0, o1 = d1 * kg1;
            if (n_listen) { o0 = k0; o1 = k1; }
            out[0][i] = o0;
            out[1][i] = o1;
            // the history (only while the window is open): the loudest key level, the deepest gain and the loudest output
            if (n_uion) {
                t = emax(n_e0, n_e1);
                if (t > n_kmax) n_kmax = t;
                if (g0.gdb < n_gmin) n_gmin = g0.gdb;
                t = emax(std::fabs(o0), std::fabs(o1));
                if (t > n_omax) n_omax = t;
                if ((n_hcnt += 1) >= n_h10) {
                    n_hcnt = 0;
                    V.hk[n_hp].store(n_kmax > 0.0000000001 ? (float)(20 * std::log10(n_kmax)) : -200.f, std::memory_order_relaxed);
                    V.hg[n_hp].store((float)n_gmin, std::memory_order_relaxed);
                    V.ho[n_hp].store(n_omax > 0.0000000001 ? (float)(20 * std::log10(n_omax)) : -200.f, std::memory_order_relaxed);
                    n_hp = (n_hp + 1) & (HN - 1);
                    V.hp.store(n_hp, std::memory_order_release);
                    n_kmax = 0; n_omax = 0; n_gmin = 0;
                }
            }
        }
        V.st.store(truth(g0.st) ? 1 : 0, std::memory_order_relaxed);
        V.hd.store(truth(g0.hd) ? 1 : 0, std::memory_order_relaxed);
        V.gdb.store(g0.gdb, std::memory_order_relaxed);
    }

    // after the built-in Act: level matching (window open only), held Bypass (crossfaded) and held Swap's trim
    void post(double *const *out, int n)
    {
        for (int i = 0; i < n; ++i) ab.post(br0[i], br1[i], out[0][i], out[1][i], bq0[i], bq1[i]);
    }

private:
    EngineHost &H;
    Act &A;
    UiLink &U;
    Display &V;
    double srate = 44100;
    std::vector<double> NLA0, NLA1, br0, br1, bq0, bq1;
    Bq kh0, kh1, kl0, kl1;
    Gt g0, g1;
    int n_lp = 0, n_hp = 0, n_la = 0, n_src = 0, n_kp = 0, n_ks = -1, n_kst = 0;
    double n_hcnt = 0, n_kmax = 0, n_omax = 0, n_gmin = 0, n_h10 = 1;
    double n_lhp = -1, n_llp = -1, n_kidl = -1, n_dec = 0, n_rk = 0, n_lk = 1;
    double n_e0 = 0, n_e1 = 0, n_m0 = 0, n_m1 = 0, n_scenv = 0, n_screnv = 0;
    int64_t n_aseq = -1;
    bool n_inited = false, n_kaud = false, n_uion = false, n_det = false, n_listen = false, n_hpon = false, n_lpon = false;
    shm::AudioLane *n_lane = nullptr;
    // a setting for the audio: the parameters, or the other A/B set while Swap is held
    double cpv(int p) const
    {
        const ParamDef *d = table().get(p);
        return (ab.swh && d && (d->flags & PF_AB)) ? H.abv(p) : H.pv(p);
    }
};

inline double Gt::run(double e, const Engine &E)
{
    // opens above the threshold, stays open until the level has been below the close threshold (threshold minus
    // hysteresis) for the hold time; the gain ramps over the attack or release time
    if (!truth(st)) {
        if (e >= E.n_thro) { st = 1; hc = E.n_hold; }
    } else {
        if (e >= E.n_thrc) hc = E.n_hold;
        else if ((hc -= 1) < 0) st = 0;
    }
    hd = (truth(st) && e < E.n_thrc) ? 1 : 0;
    double tg = truth(st) ? 0 : -E.n_rng;
    if (gdb < tg) gdb = emin(tg, gdb + E.n_ru);
    else if (gdb > tg) gdb = emax(tg, gdb - E.n_rd);
    if (gdb != lastg) {
        lastg = gdb;
        kk = (E.n_inf && gdb <= -E.n_rng + 0.0001) ? 0 : std::exp(gdb * 0.11512925464970229);
    }
    return kk;
}

// ---------------------------------------------------------------- the plug-in
class GatePlugin : public Plugin, public EngineHost {
public:
    explicit GatePlugin(const clap_host *h) : Plugin(h, kind()), eng(*this, act, uiLink, view) {}
    UiLink uiLink;
    Display view;
    Engine eng;
    double pv(int id) const override { return P.get(id); }
    double abv(int id) const override { return abValue(id); }
    gui::View *createView() override;

protected:
    void dspActivate(double sr, uint32_t) override { eng.init(sr); }
    void dspReset() override { eng.reset(); }
    void dspProcess(const double *const *in, const double *const *sc, double *const *out, int n) override
    {
        eng.block(n);
        act.block(n);
        eng.keyBlock(n);
        eng.samples(in, sc, out, n);
        act.process(out, 2, n);
        eng.post(out, n);
        act.audioKey(out, 2, n);
    }
    int latencyFromParams(double sr) const override { return Engine::latencyFor(P.get(P_LA), sr); }
};

static Plugin *create(const clap_host *h) { return new GatePlugin(h); }

// ---------------------------------------------------------------- the window
static const char *const L_HOLD[] = {"Off", "1 s", "2 s", "5 s", "10 s", "Infinite"};
static const char *const L_RANGE[] = {"24 dB", "36 dB", "48 dB", "60 dB", "72 dB", "96 dB"};
static const char *const L_ZOOM[] = {"100%", "115%", "130%", "150%", "175%", "200%"};
static const char *const L_FIN[] = {"Auto", "Always", "Never"};
static const char *const L_DET[] = {"Peak", "RMS"};

class GateView : public gui::View {
public:
    explicit GateView(GatePlugin &p) : P(p), av(p) {}
    void designSize(float &w, float &h) override { w = DW; h = DH; }
    void frame(Ui &ui) override;
    bool wantsFull() override { return st.settings || tdrag != 0; }
    uint64_t statusHash() override;

private:
    static constexpr float DW = 940, DH = 520, M = 14, SW = 600, SH = 210;
    GatePlugin &P;
    ActView av;
    CommonState st;
    bool live = false;
    int tdrag = 0;
    double kw0 = 0;

    void enumDD(Ui &ui, const R &r, int id, const char *const *labels, int n, const char *prefix = nullptr)
    {
        int cv = iclamp(iround(P.get(id)), 0, n - 1);
        if (ui.dropdown(r, labels[cv], true, prefix)) {
            std::vector<MenuItem> m;
            for (int i = 0; i < n; ++i) m.push_back(MenuItem::item(labels[i], i + 1, i == cv));
            GatePlugin *pp = &P;
            ui.menu(m, r.x, r.b() + 4, [pp, id](int r2) { pp->setOnce(id, r2 - 1); }, r.w);
        }
    }
    void label(Ui &ui, const R &r, const char *t) { ui.text(r, t, th::dim(), F_REGULAR, 12); }
    std::string keyTitle(int s) const;
    bool keyOkUi(int s) const;
    void keyMenu(Ui &ui, float x, float y);
    void drawHistory(Ui &ui, const R &r);
    void drawGain(Ui &ui, const R &r);
    void keyRow(Ui &ui, float y);
    void controls(Ui &ui, float y);
    void settings(Ui &ui, const R &gear);
};

gui::View *GatePlugin::createView() { return new GateView(*this); }

uint64_t GateView::statusHash()
{
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ULL; };
    av.refresh();
    mix((uint64_t)(av.myslot + 1));
    mix(av.myslot >= 0 && av.live(av.myslot));
    mix((uint64_t)av.fin());
    if (av.myslot >= 0) {
        shm::Slot &b = shm::slot(av.myslot);
        mix((uint64_t)b.astate.load());
        mix((uint64_t)(b.atime.load() * 10));
        mix((uint64_t)b.resseq.load());
        mix(b.result.load() == shm::R_NOSIG && av.now - b.donet.load() < 4);
        mix((uint64_t)b.nseq.load());
        mix((uint64_t)b.trackno.load());
    }
    mix((uint64_t)P.driveStatus());
    mix(now_sec() - P.presetMsgT < 4);
    mix((uint64_t)P.view.kst.load());
    mix((uint64_t)P.view.side.load());
    mix((uint64_t)(P.view.latms.load() * 10));
    mix((uint64_t)st.holdNow() | (uint64_t)st.settings << 4 | (uint64_t)st.abm << 5);
    mix(kw0 > 0 && now_sec() - kw0 > 1.5);
    mix((uint64_t)shm::get()->h.slotver.load());
    return h;
}

// ---- the key from another TwoStorms plug-in ----
// a title for the plug-in in slot s: its own name, the track
std::string GateView::keyTitle(int s) const
{
    std::string t1 = shm::slot(s).kind.load() == shm::KIND_MASTER ? "Gain Stage" : av.pointTitle(s);
    return t1 + "   [" + av.trackName(s) + "]";
}
bool GateView::keyOkUi(int s) const
{
    int me = av.myslot;
    if (me < 0 || s == me) return false;
    shm::Slot &b = shm::slot(s), &mb = shm::slot(me);
    return b.track.load() == mb.track.load() && (b.pflags.load() & 7) == (mb.pflags.load() & 7) && b.order.load() < mb.order.load() &&
           !(b.pflags.load() & shm::PL_CONTAINER) && !(mb.pflags.load() & shm::PL_CONTAINER);
}
void GateView::keyMenu(Ui &ui, float x, float y)
{
    std::vector<int> ls;
    std::vector<int64_t> ids;
    int mx = (int)std::min<int64_t>(shm::NSLOT, shm::get()->h.maxslot.load());
    for (int s = 0; s < mx && ls.size() < 200; ++s) {
        shm::Slot &b = shm::slot(s);
        int64_t k = b.kind.load();
        if (s != av.myslot && b.owner.load() >= 100000 && (k == shm::KIND_POINT || k == shm::KIND_MASTER) && av.live(s) && keyOkUi(s)) {
            ls.push_back(s);
            ids.push_back(b.owner.load());
        }
    }
    // in the order they sit in the chain
    std::vector<size_t> ix(ls.size());
    for (size_t i = 0; i < ix.size(); ++i) ix[i] = i;
    auto ord = [&](int s) { return av.named(s) ? (double)shm::slot(s).norder.load() : shm::slot(s).order.load(); };
    std::sort(ix.begin(), ix.end(), [&](size_t a, size_t b) { return ord(ls[a]) < ord(ls[b]); });
    {
        std::vector<int> l2;
        std::vector<int64_t> i2;
        for (size_t k : ix) { l2.push_back(ls[k]); i2.push_back(ids[k]); }
        ls.swap(l2);
        ids.swap(i2);
    }
    int src = iround(P.get(P_SRC));
    double id = P.get(P_KID);
    std::vector<MenuItem> m;
    m.push_back(MenuItem::item("Main input", 1, src == 0));
    m.push_back(MenuItem::item("Sidechain (inputs 3-4)", 2, src == 1));
    m.push_back(MenuItem::sep());
    m.push_back(MenuItem::item(ls.empty() ? "No TwoStorms plug-in before this one on this track" : "Output of an earlier TwoStorms plug-in on this track:",
                               0, false, false));
    for (size_t i = 0; i < ls.size(); ++i) m.push_back(MenuItem::item(keyTitle(ls[i]), 10 + (int)i, src == 2 && (double)ids[i] == id));
    GatePlugin *pp = &P;
    ui.menu(m, x, y, [pp, ids](int r) {
        if (r == 1) pp->setOnce(P_SRC, 0);
        else if (r == 2) pp->setOnce(P_SRC, 1);
        else if (r >= 10 && r - 10 < (int)ids.size()) { pp->setOnce(P_KID, (double)ids[r - 10]); pp->setOnce(P_SRC, 2); }
    }, 420);
}

// ---- history: the key level (grey), the output (orange), the gate (strip on top), the two thresholds ----
void GateView::drawHistory(Ui &ui, const R &r)
{
    const float hdb = 90;
    double to = P.get(P_THR), tc = P.get(P_THR) - P.get(P_HYS);
    float yo = r.y + r.h * (float)(-to / hdb), yc = r.y + r.h * (float)(-tc / hdb);
    // drag a line: orange the threshold, purple the hysteresis (the close threshold)
    if (ui.hit(r) && ui.lclick && !ui.used) {
        float my = ui.my();
        float dO = std::fabs(my - yo), dC = std::fabs(my - yc);
        tdrag = dO <= dC ? (dO < 8 ? 1 : 0) : (dC < 8 ? 2 : 0);
        if (tdrag) { ui.used = true; P.beginEdit(tdrag == 1 ? P_THR : P_HYS); }
    }
    if (tdrag) {
        if (!ui.ldown) {
            P.endEdit(tdrag == 1 ? P_THR : P_HYS);
            tdrag = 0;
        } else {
            double db = std::max(-90.0, std::min(0.0, (double)(-(ui.my() - r.y) / r.h * hdb)));
            db = std::floor(db * 2 + 0.5) / 2;
            if (tdrag == 1) P.edit(P_THR, db);
            else P.edit(P_HYS, std::max(0.0, P.get(P_THR) - db));
        }
        ui.cursor = CUR_HRESIZE;
    } else if (ui.hit(r) && (std::fabs(ui.my() - yo) < 8 || std::fabs(ui.my() - yc) < 8)) {
        ui.cursor = CUR_HRESIZE;
    }
    Canvas &c = ui.cv();
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        for (int d = -10; d >= -80; d -= 10) {
            float gy = r.y + r.h * (-d / hdb);
            ui.fill(R(r.x, gy, r.w, 1 / ui.s), Paint(th::grid()));
            char s[8];
            snprintf(s, sizeof s, "%d", d);
            ui.text(R(r.r() - 30, gy - 13, 26, 12), s, th::faint(), F_REGULAR, 9.5f, 2);
        }
        ui.stroke(r, Paint(th::border()), 6);
    }
    if (ui.drawD()) {
        int hp = P.view.hp.load(std::memory_order_acquire);
        R in(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
        c.pushClip(ui.X(in.x), ui.Y(in.y + 8), ui.W(in.w), ui.W(in.h - 8));
        int px0 = (int)std::floor(ui.X(in.x)), px1 = (int)std::ceil(ui.X(in.r()));
        int np = px1 - px0;
        float per = 1 / ui.s;
        int n = (int)std::ceil(np * per) + 2;
        std::vector<float> tops(np);
        std::vector<Pt> out;
        out.reserve(np);
        float y0 = ui.Y(in.y), H = ui.W(in.h), yb = ui.Y(in.b());
        double rng = std::max(1.0, P.get(P_RNG));
        std::vector<float> gg(np);
        auto flush = [&]() {
            if (out.size() > 1) c.strokeThin(out.data(), (int)out.size(), ui.W(1.6f), th::orange());
            out.clear();
        };
        for (int i = 0; i < np; ++i) {
            int k = n - 1 - (int)((np - 1 - i) * per);
            int e = (hp - n + k) & (HN - 1);
            float v = P.view.hk[e].load(std::memory_order_relaxed), o = P.view.ho[e].load(std::memory_order_relaxed);
            gg[i] = P.view.hg[e].load(std::memory_order_relaxed);
            tops[i] = v > -hdb ? y0 + H * std::min(1.f, -v / hdb) : yb;
            if (o <= -150) { flush(); continue; }
            out.push_back({(float)(px0 + i) + 0.5f, y0 + H * std::min(1.f, -std::max(-hdb, o) / hdb)});
        }
        c.columns(px0, tops.data(), np, yb, Paint::vertical(y0, Col::hex(0x47434f), yb, Col::hex(0x2c2a32)));
        flush();
        c.popClip();
        // the gate along the top: green open, red closed
        c.pushClip(ui.X(in.x), ui.Y(in.y), ui.W(in.w), ui.W(7));
        for (int i = 0; i < np; ++i) {
            float t = (float)std::max(0.0, std::min(1.0, 1 + gg[i] / rng));
            Col cc = Col::hex(0xff4f5e).mix(th::green(), t);
            c.fillRect((float)(px0 + i), ui.Y(in.y), 1, ui.W(6), Paint(cc));
        }
        c.popClip();
    }
    // the thresholds and their labels
    if (ui.drawD()) {
        ui.fill(R(r.x + 1, yc, r.w - 2, 1.2f), Paint(th::purple()));
        ui.fill(R(r.x + 1, yo - 0.5f, r.w - 2, 2), Paint(th::orange()));
        char s[48];
        snprintf(s, sizeof s, "open %.1f dB", to);
        ui.text(R(r.x + 8, yo - 15, 140, 12), s, th::orange(), F_MEDIUM, 10.5f);
        snprintf(s, sizeof s, "close %.1f dB", tc);
        ui.text(R(r.x + 8, yc + 3, 140, 12), s, th::purpleHi(), F_MEDIUM, 10.5f);
        float lx = r.x + 160, ly = r.y + 10;
        auto key = [&](const char *t, const Col &col) {
            ui.fill(R(lx, ly + 4, 8, 3), Paint(col), 1.5f);
            ui.text(R(lx + 12, ly, 200, 11), t, th::dim(), F_REGULAR, 10.5f);
            lx += 12 + ui.textW(F_REGULAR, 10.5f, t) + 14;
        };
        key("key", Col::hex(0x6d6875));
        key("output", th::orange());
        if (iround(P.get(P_SRC)) == 2 && kw0 > 0 && now_sec() - kw0 > 1.5)
            ui.text(R(r.x + 8, r.b() - 20, r.w - 16, 14),
                    "No audio is arriving from the key plug-in: it has to be running (not bypassed), earlier on this track.", th::amber(),
                    F_MEDIUM, 11);
    }
}

// ---- the gate's gain: its state, a bar (red: the gain it takes away), the dB ----
void GateView::drawGain(Ui &ui, const R &r)
{
    double rng = P.get(P_RNG);
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        ui.stroke(r, Paint(th::border()), 6);
        char s[32];
        if (rng >= 89.5) snprintf(s, sizeof s, "range: silence"); else snprintf(s, sizeof s, "range: %.0f dB", rng);
        ui.text(R(r.x, r.b() - 18, r.w, 12), s, th::faint(), F_REGULAR, 10, 1);
    }
    if (!ui.drawD()) return;
    double gdb = live ? P.view.gdb.load() : 0;
    int stt = P.view.st.load(), hd = P.view.hd.load();
    const char *bt = !live ? "GATE" : (stt ? (hd ? "HOLD" : "OPEN") : "CLOSED");
    Col bc = !live ? th::faint() : (stt ? (hd ? th::amber() : th::green()) : th::red());
    float bw = ui.badgeW(bt);
    ui.badgeDyn(r.x + (r.w - bw) * 0.5f, r.y + 8, bt, bc, bc.alpha(0.15f));
    R bar(r.x + (r.w - 30) * 0.5f, r.y + 32, 30, r.h - 32 - 46);
    ui.fill(bar, Paint(th::well()), 3);
    float f = (float)std::min(1.0, std::max(0.0, -gdb / std::max(1.0, rng)));
    if (f > 0) ui.fill(R(bar.x + 1, bar.y + 1, bar.w - 2, (bar.h - 2) * f), Paint::vertical(bar.y, th::red(), bar.b(), th::redLo()), 2);
    if (f < 1) ui.fill(R(bar.x + 1, bar.y + 1 + (bar.h - 2) * f, bar.w - 2, (bar.h - 2) * (1 - f)), Paint::vertical(bar.y, th::green(), bar.b(), th::greenLo()), 2);
    ui.stroke(bar, Paint(th::border()), 3);
    char s[32];
    snprintf(s, sizeof s, "%.1f dB", gdb > -0.05 ? 0.0 : gdb);
    ui.text(R(r.x, r.b() - 38, r.w, 16), s, f >= 0.999 ? th::red() : th::text(), F_SEMIBOLD, 12.5f, 1, true);
}

// ---- the key: where it comes from, the detector, its time, the link, listen ----
void GateView::keyRow(Ui &ui, float y)
{
    int src = iround(P.get(P_SRC));
    int kst = P.view.kst.load();
    if (kst == 3) { if (kw0 == 0) kw0 = now_sec(); } else kw0 = 0;
    if (!ui.drawS()) return;
    const float h = 26;
    std::string kl;
    if (src == 0) kl = "main input";
    else if (src == 1) kl = "sidechain (inputs 3-4)";
    else {
        double kid = P.get(P_KID);
        int ks = av.findId((int64_t)kid);
        if (ks >= 0 && ks != av.myslot && av.live(ks)) {
            kl = keyTitle(ks);
            if (!keyOkUi(ks)) kl += " (not before the gate)";
            else if (kst == 3) kl += " (no audio yet)";
        } else {
            char b[64];
            snprintf(b, sizeof b, "plug-in not found (ID %lld)", (long long)kid);
            kl = b;
        }
    }
    R kr(M, y, 432, h);
    if (ui.dropdown(kr, kl.c_str(), true, "Key")) keyMenu(ui, kr.x, kr.b() + 4);
    if (src == 2) ui.stroke(kr, Paint(th::purple().alpha(0.8f)), 5);
    float x = kr.r() + 10;
    enumDD(ui, R(x, y, 124, h), P_DET, L_DET, 2, "Detect");
    x += 124 + 14;
    label(ui, R(x, y, 34, h), "Time");
    ui.valueBox(R(x + 36, y, 70, h), P_DT, true, KD_TIME, 0.1, 0.01);
    x += 36 + 70 + 14;
    label(ui, R(x, y, 30, h), "Link");
    ui.valueBox(R(x + 32, y, 64, h), P_LINK, true, KD_OTHER, 1, 0.1);
    bool lis = P.get(P_LISTEN) >= 0.5;
    if (ui.button(R(DW - M - 96, y, 96, h), "Listen", lis, true, BS_PURPLE)) {
        if (ui.mods & M_SHIFT) { ui.momKey = P_LISTEN; ui.momPrev = lis ? 1 : 0; }
        P.setOnce(P_LISTEN, lis ? 0 : 1);
    }
}

// ---- the controls: Threshold / Hysteresis / Range | Attack / Hold / Release | Key HPF / Key LPF / Lookahead ----
void GateView::controls(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float cw = (DW - 2 * M - 2 * 20) / 3, h = 26, dy = 32, lw = 80, bw = 84;
    float x1 = M, x2 = M + cw + 20, x3 = M + 2 * (cw + 20);
    ui.paramRow(R(x1, y, cw, h), "Threshold", P_THR, SC_LIN, lw, bw);
    ui.paramRow(R(x1, y + dy, cw, h), "Hysteresis", P_HYS, SC_LIN, lw, bw);
    ui.paramRow(R(x1, y + 2 * dy, cw, h), "Range", P_RNG, SC_LIN, lw, bw);
    ui.paramRow(R(x2, y, cw, h), "Attack", P_ATT, SC_LOG, lw, bw);
    ui.paramRow(R(x2, y + dy, cw, h), "Hold", P_HLD, SC_SQRT, lw, bw);
    ui.paramRow(R(x2, y + 2 * dy, cw, h), "Release", P_REL, SC_LOG, lw, bw);
    ui.paramRow(R(x3, y, cw, h), "Key HPF", P_HP, SC_LOG, lw, bw);
    ui.paramRow(R(x3, y + dy, cw, h), "Key LPF", P_LP, SC_LOG, lw, bw);
    ui.paramRow(R(x3, y + 2 * dy, cw, h), "Lookahead", P_LA, SC_LIN, lw, bw);
}

void GateView::settings(Ui &ui, const R &gear)
{
    if (!ui.drawS()) return;
    R in;
    if (!settingsBegin(ui, st, gear, SW, SH, in)) return;
    const float h = 24;
    float y = in.y;
    ui.text(R(in.x, y, 200, 14), "SETTINGS", th::faint(), F_SEMIBOLD, 10.5f);
    y += 22;
    float x = in.x;
    label(ui, R(x, y, 64, h), "Peak hold");
    enumDD(ui, R(x + 64, y, 86, h), P_HOLD, L_HOLD, 6);
    x += 64 + 86 + 18;
    label(ui, R(x, y, 82, h), "Meter range");
    enumDD(ui, R(x + 82, y, 80, h), P_RANGE, L_RANGE, 6);
    x += 82 + 80 + 18;
    label(ui, R(x, y, 40, h), "Zoom");
    enumDD(ui, R(x + 40, y, 80, h), P_ZOOM, L_ZOOM, 6);
    y += h + 10;
    label(ui, R(in.x, y, 64, h), "Finale");
    enumDD(ui, R(in.x + 64, y, 86, h), P_FINAL, L_FIN, 3);
    bool warn;
    std::string fs = av.finaleStatus(&warn);
    ui.textFit(R(in.x + 164, y, in.r() - in.x - 164, h), fs.c_str(), warn ? th::red() : (av.fin() ? th::orange() : th::dim()), F_REGULAR, 11.5f);
    y += h + 14;
    ui.fill(R(in.x, y, in.w, 1), Paint(th::border()));
    y += 8;
    static const char *help[] = {
        "The key is what the gate listens to: the main input, the sidechain (inputs 3-4) or another TwoStorms plug-in's output.",
        "A TwoStorms plug-in earlier on the same track reaches the gate sample for sample, with no delay.",
        "Bypass (Lvl Match) and Swap (Lvl Match): click to latch, Shift + click to hold. Shift + click Listen to hold it.",
    };
    for (const char *t : help) { ui.textFit(R(in.x, y, in.w, 14), t, th::faint(), F_REGULAR, 10.5f); y += 15; }
    char idb[64];
    snprintf(idb, sizeof idb, "Plugin ID %lld", (long long)av.myid);
    ui.text(R(in.x, y + 2, in.w, 14), idb, th::ghost().lighter(0.3f), F_REGULAR, 10.5f, 2);
    settingsEnd(ui);
}

void GateView::frame(Ui &ui)
{
    av.refresh();
    double t = ui.now;
    P.uiLink.last.store(t, std::memory_order_relaxed);
    P.uiLink.beat.store(t, std::memory_order_relaxed);
    P.uiLink.hold.store(st.holdNow(), std::memory_order_relaxed);
    live = av.myslot >= 0 && av.live(av.myslot);
    if (ui.drawS()) ui.fill(R(0, 0, DW, DH), Paint::vertical(0, th::bg(), DH, th::bg2()));
    std::string sub = av.myslot >= 0 ? av.trackName(av.myslot) : std::string();
    double lm = P.view.latms.load();
    char lt[64];
    if (lm > 0) snprintf(lt, sizeof lt, "latency %.1f ms (lookahead)", lm);
    else snprintf(lt, sizeof lt, "no latency");
    if (!sub.empty()) sub += "   \xC2\xB7   ";
    sub += lt;
    sub += P.view.side.load() ? "   \xC2\xB7   playing B" : "   \xC2\xB7   playing A";
    st.subtitle = sub;

    R gear = gearRect(DW);
    if (st.settings) ui.block(settingsRect(gear, SW, SH));
    topBar(ui, P, av, st, DW, "TWOSTORMS", "GATE", true);
    if (st.settings) ui.block(settingsRect(gear, SW, SH));
    const float y0 = 66, dh = 220, gw = 104;
    drawHistory(ui, R(M, y0, DW - 2 * M - gw - 10, dh));
    drawGain(ui, R(DW - M - gw, y0, gw, dh));
    keyRow(ui, y0 + dh + 12);
    controls(ui, y0 + dh + 12 + 26 + 14);
    float ay = y0 + dh + 12 + 26 + 14 + 3 * 32 + 6;
    actCard(ui, P, av, R(M, ay, DW - 2 * M, 74));
    settings(ui, gear);
}

// ---------------------------------------------------------------- the kind
static const char *s_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_GATE, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const PluginKind &kind()
{
    static const PluginKind k = {
        {CLAP_VERSION_INIT, "com.twostorms.gate", "TwoStorms Gate", "TwoStorms", "", "", "", TS_VERSION,
         "A noise gate with an Act built in: threshold with hysteresis, attack, hold, release, range, optional lookahead, peak or RMS "
         "detector, key filters and link; the key can be the input, the sidechain, or an earlier TwoStorms plug-in on the track.",
         s_features},
        &table,
        &factoryPresets,
        "Gate",
        "TWOSTORMS GATE",
        shm::KIND_POINT,
        shm::SK_GATE,
        true,
        true,
        P_AB,
        &create,
        2,
    };
    return k;
}

} // namespace gate
} // namespace ts
