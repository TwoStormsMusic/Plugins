// TwoStorms Tuner: a chromatic tuner for guitar, bass and voice. It listens to this track's input, or to the output of
// any TwoStorms plug-in anywhere in the project (a Gain Stage on the guitar's track, while the Tuner sits on a track of
// its own with no audio at all). Mute silences its own output, and the plug-in it listens to (which keeps feeding the
// Tuner). The pitch is found in the window (src/core/pitch.cpp), never on the audio thread, so with the window closed
// it costs nothing; open, it does the work about 30 times a second and draws only when the reading moves. The window
// takes any size and shape (a touch screen across the stage) and everything in it scales.
#include "tuner_plugin.h"
#include "../../core/act_view.h"
#include "../../core/clock.h"
#include "../../core/eel.h"
#include "../../core/pitch.h"
#include "../../gui/common.h"
#include "../../gui/editor.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ts {
namespace tuner {

using namespace gui;

enum {
    P_OWN = 50,     // listens to this track's input
    P_CH = 52,      // 0 left, 1 right, 2 both
    P_MUTE = 53,
    P_REF = 54,     // A4, Hz
    P_RANGE = 55,   // PitchDetector::R_*
    P_NAMES = 56,   // 0 sharps, 1 flats
    P_TOL = 57,     // in tune within: TOLS[]
    P_DISP = 58,    // 0 needle and strobe, 1 needle, 2 strobe
    P_WW = 59, P_WH = 60,   // the window's size (logical pixels; 0: the base size)
    P_MALL = 61,    // Mute silences the plug-ins it listens to as well as its own output
    P_SID0 = 70,    // the TwoStorms plug-ins it listens to: their IDs (0: none), NSRC of them
};
constexpr int NSRC = 16;
static const double TOLS[] = {1, 2, 3, 5};
static double tolOf(double v) { return TOLS[iclamp(iround(v), 0, 3)]; }

// ---------------------------------------------------------------- parameters
static void f_ref(double v, char *b, int n) { snprintf(b, n, "%.1f Hz", v); }

static const ParamTable &table()
{
    static ParamTable t = [] {
        ParamTable t;
        const char *M = "Tuner";
        t.add(P_OWN, "Listen to this track", 0, 1, 1, PF_INT | PF_BOOL, nullptr, M);
        static const char *const sn[NSRC] = {"Listen to 1", "Listen to 2", "Listen to 3", "Listen to 4", "Listen to 5", "Listen to 6",
                                             "Listen to 7", "Listen to 8", "Listen to 9", "Listen to 10", "Listen to 11", "Listen to 12",
                                             "Listen to 13", "Listen to 14", "Listen to 15", "Listen to 16"};
        for (int k = 0; k < NSRC; ++k) t.add(P_SID0 + k, sn[k], 0, 999999, 0, PF_INT, fmt_int, M);
        t.addEnum(P_CH, "Channel", {"Left", "Right", "Both"}, 2, 0, M);
        t.add(P_MUTE, "Mute", 0, 1, 0, PF_HOST | PF_INT | PF_BOOL, nullptr, M);
        // the settings are host parameters too (a footswitch can change the display or the reference)
        const uint32_t H = PF_HOST | PF_PRESET;
        auto &r = t.add(P_REF, "Reference (A4)", 415, 466, 440, H, f_ref, M);
        r.unit = "Hz";
        t.addEnum(P_RANGE, "Range", {"Guitar", "Bass", "Vocals", "Any instrument"}, 3, H, M);
        t.addEnum(P_NAMES, "Note names", {"Sharps", "Flats"}, 0, H, M);
        t.addEnum(P_TOL, "In tune within", {"1 cent", "2 cents", "3 cents", "5 cents"}, 1, H, M);
        t.addEnum(P_DISP, "Display", {"Needle and strobe", "Needle", "Strobe"}, 0, H, M);
        t.add(P_WW, "Window width", 0, 16384, 0, PF_INT, fmt_int, M);
        t.add(P_WH, "Window height", 0, 16384, 0, PF_INT, fmt_int, M);
        t.add(P_MALL, "Mute all tracks heard", 0, 1, 1, PF_INT | PF_BOOL, nullptr, M);
        return t;
    }();
    return t;
}

// factory presets: Instruments, Buses, Creative, Weird, then the reference pitches
static std::string S(std::initializer_list<std::pair<int, double>> kv) { return presetStr(kv); }
static const std::vector<FactoryPreset> &factoryPresets()
{
    static std::vector<FactoryPreset> v = {
        {"Instruments/Guitar/Standard (in tune within 2 cents)", S({{P_RANGE, 0}})},
        {"Instruments/Guitar/Down-tuned, names in flats (Eb, Db ...)", S({{P_RANGE, 0}, {P_NAMES, 1}})},
        {"Instruments/Guitar/Setup: intonation (strobe, within 1 cent)", S({{P_RANGE, 0}, {P_TOL, 0}, {P_DISP, 2}})},
        {"Instruments/Bass/Standard (down to a five-string's low B)", S({{P_RANGE, 1}})},
        {"Instruments/Bass/Down-tuned, names in flats (Eb, Db ...)", S({{P_RANGE, 1}, {P_NAMES, 1}})},
        {"Instruments/Bass/Setup: intonation (strobe, within 1 cent)", S({{P_RANGE, 1}, {P_TOL, 0}, {P_DISP, 2}})},
        {"Instruments/Vocals/Pitch practice (needle, within 5 cents)", S({{P_RANGE, 2}, {P_TOL, 3}, {P_DISP, 1}})},
        {"Instruments/Vocals/Held notes, tight (within 3 cents)", S({{P_RANGE, 2}, {P_TOL, 2}, {P_DISP, 1}})},
        {"Instruments/Drums/Tom and snare heads (tap near the edge)", S({{P_RANGE, 3}, {P_TOL, 3}, {P_DISP, 1}})},
        {"Instruments/Keys & synths/Oscillators (strobe, within 1 cent)", S({{P_TOL, 0}, {P_DISP, 2}})},
        {"Instruments/Strings & brass/Orchestra (A = 442)", S({{P_REF, 442}})},
        {"Instruments/Strings & brass/Brass and winds (names in flats)", S({{P_NAMES, 1}})},
        {"Buses/Instrument bus/Section or choir in unison (within 5 cents)", S({{P_RANGE, 2}, {P_TOL, 3}, {P_DISP, 1}})},
        {"Creative/Strobe only: watch it stand still", S({{P_TOL, 0}, {P_DISP, 2}})},
        {"Creative/Scientific pitch: C = 256 Hz (A = 430.5)", S({{P_REF, 430.54}})},
        {"Weird/Old Philharmonic: A = 452.5 (Victorian high pitch)", S({{P_REF, 452.5}})},
        {"Weird/Diapason normal: A = 435 (Paris, 1859)", S({{P_REF, 435}})},
        {"Reference pitch/A = 415 (baroque)", S({{P_REF, 415}})},
        {"Reference pitch/A = 432", S({{P_REF, 432}})},
        {"Reference pitch/A = 440 (standard)", S({{P_REF, 440}})},
        {"Reference pitch/A = 442 (most orchestras)", S({{P_REF, 442}})},
        {"Reference pitch/A = 444", S({{P_REF, 444}})},
    };
    return v;
}

// ---------------------------------------------------------------- the plug-in
static constexpr int RN = 32768, RMASK = RN - 1;   // this track's input, for the window (the latest 0.7 s at 48 kHz)

class TunerPlugin : public Plugin {
public:
    explicit TunerPlugin(const clap_host *h) : Plugin(h, kind())
    {
        for (auto &v : ring) v.store(0, std::memory_order_relaxed);
    }
    std::atomic<float> ring[RN];
    std::atomic<int64_t> ringW{0};
    std::atomic<double> lastBlockT{0};
    gui::View *createView() override;

protected:
    void dspActivate(double, uint32_t) override { g_ = P.get(P_MUTE) >= 0.5 ? 0 : 1; }
    void dspReset() override {}
    void dspProcess(const double *const *in, const double *const *, double *const *out, int n) override
    {
        double t = now_sec();
        lastBlockT.store(t, std::memory_order_relaxed);
        bool mute = P.get(P_MUTE) >= 0.5;
        // what it hears on this track, for the window (open in the last second), before the mute
        if (P.get(P_OWN) >= 0.5 && t - guiOpenBeat.load(std::memory_order_relaxed) < 1.0) {
            int ch = iclamp(iround(P.get(P_CH)), 0, 2);
            int64_t w = ringW.load(std::memory_order_relaxed);
            for (int i = 0; i < n; ++i) {
                double v = ch == 0 ? in[0][i] : (ch == 1 ? in[1][i] : 0.5 * (in[0][i] + in[1][i]));
                ring[w & RMASK].store((float)v, std::memory_order_relaxed);
                ++w;
            }
            ringW.store(w, std::memory_order_release);
        }
        // the output, muted with 10 ms fades
        double tg = mute ? 0 : 1, step = 1 / std::max(1.0, 0.01 * srate_), g = g_;
        for (int i = 0; i < n; ++i) {
            if (g != tg) g = g < tg ? std::min(tg, g + step) : std::max(tg, g - step);
            out[0][i] = in[0][i] * g;
            out[1][i] = in[1][i] * g;
        }
        g_ = g;
        // the plug-ins it listens to stay quiet while it keeps asking (Mute on, and "Mute all tracks heard"); they play
        // again a quarter second after it stops
        if (mute && P.get(P_MALL) >= 0.5) {
            bool missing = false;
            for (int k = 0; k < NSRC; ++k) {
                int64_t id = (int64_t)P.get(P_SID0 + k);
                if (id < 100000) { hint_[k] = -1; continue; }
                int s = hint_[k];
                if (s < 0 || shm::slot(s).owner.load(std::memory_order_relaxed) != id) { hint_[k] = -1; missing = true; continue; }
                shm::slot(s).tmute.store(t, std::memory_order_relaxed);
            }
            // the ones not found yet: one pass over the slots, twice a second at most
            if (missing && t - scanT_ > 0.5) {
                scanT_ = t;
                int mx = (int)std::min<int64_t>(shm::NSLOT, shm::get()->h.maxslot.load(std::memory_order_relaxed));
                for (int s = 0; s < mx; ++s) {
                    int64_t o = shm::slot(s).owner.load(std::memory_order_relaxed);
                    if (o < 100000) continue;
                    for (int k = 0; k < NSRC; ++k)
                        if (hint_[k] < 0 && (int64_t)P.get(P_SID0 + k) == o) {
                            hint_[k] = s;
                            shm::slot(s).tmute.store(t, std::memory_order_relaxed);
                        }
                }
            }
        }
    }

private:
    double g_ = 1;
    int hint_[NSRC] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    double scanT_ = -10;
};

static Plugin *create(const clap_host *h) { return new TunerPlugin(h); }

// ---------------------------------------------------------------- the window
static const char *const L_RANGE[] = {"Guitar", "Bass", "Vocals", "Any instrument"};
static const char *const L_NAMES[] = {"Sharps (C#, F#)", "Flats (Db, Gb)"};
static const char *const L_TOL[] = {"\xC2\xB1" "1 cent", "\xC2\xB1" "2 cents", "\xC2\xB1" "3 cents", "\xC2\xB1" "5 cents"};
static const char *const L_DISP[] = {"Needle and strobe", "Needle", "Strobe"};
static const char *const L_CH[] = {"Left", "Right", "Both (L + R)"};
// note names: the letter, and its sign (1 sharp, -1 flat)
static const char *const N_SH[] = {"C", "C", "D", "D", "E", "F", "F", "G", "G", "A", "A", "B"};
static const int A_SH[] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
static const char *const N_FL[] = {"C", "D", "D", "E", "E", "F", "G", "G", "A", "A", "B", "B"};
static const int A_FL[] = {0, -1, 0, -1, 0, 0, -1, 0, -1, 0, -1, 0};

// where the audio comes from, as the window last saw it
enum { ST_OK = 0, ST_WAIT, ST_NONE, ST_NOTFOUND, ST_STOPPED, ST_NOAUDIO, ST_BUSY };

class TunerView : public gui::View {
public:
    explicit TunerView(TunerPlugin &p) : P(p), av(p) {}
    void designSize(float &w, float &h) override { w = DW; h = DH; }
    bool freeSize(float &bw, float &bh) override { bw = BW; bh = BH; return true; }
    void savedSize(float &w, float &h) override { w = (float)P.get(P_WW); h = (float)P.get(P_WH); }
    void sizeChanged(float w, float h, float dw, float dh) override
    {
        DW = dw;
        DH = dh;
        int iw = (int)std::lround(w), ih = (int)std::lround(h);
        if (iw != iround(P.get(P_WW)) || ih != iround(P.get(P_WH))) {
            P.putQuiet(P_WW, iw);
            P.putQuiet(P_WH, ih);
            P.markDirty();
        }
    }
    void opened() override { srcs_.clear(); cur_ = -1; have_ = false; }
    bool animating() override { return false; }   // frames come from poll(), when the reading moves
    bool poll(double now) override;
    bool wantsFull() override { return st.settings; }
    uint64_t statusHash() override;
    bool dynamicRegions(std::vector<R> &rs) override;
    void frame(Ui &ui) override;

private:
    static constexpr float BW = 900, BH = 540, HH = 64, M = 16, SW = 600, SH = 380;
    float DW = BW, DH = BH;
    TunerPlugin &P;
    ActView av;
    CommonState st;

    // ---- reading ----
    PitchDetector det_;
    std::vector<float> buf_;
    double lastDet_ = 0;
    int stat_ = ST_WAIT;
    // the sources it listens to, as the window last saw each
    struct Src {
        int64_t id = 0;              // 0: this track's input
        int slot = -1;
        int64_t lastSeq = -1, lastLane = -2;
        int64_t valid = 0;           // samples written in a row since the window started listening
        int64_t ownW = -1;           // this track: where the ring was when the window started listening
        int64_t lane = -1, end = 0;  // where its newest samples end
        double seqT = 0;             // when it last wrote
        double sr = 48000, level = -200;
        int stat = ST_WAIT;
        void reset(double now) { lastSeq = -1; lastLane = -2; valid = 0; ownW = -1; seqT = now; }
    };
    std::vector<Src> srcs_;
    int cur_ = -1;                   // the one heard (the loudest, with a little hysteresis)
    int nOk_ = 0;                    // how many are coming in
    double levelDb_ = -200;
    bool hd_ = true, tu_ = false;   // held, in tune: as the last look found them (what the frames show)
    // the layout (design units), for the regions that move
    R naR_, mR_, sR_, levR_, readR_;
    double drawnLevel_ = -200, levelT_ = 0;
    bool needleOn_ = true, strobeOn_ = true;
    // ---- the display ----
    bool have_ = false;          // a note has been seen
    int note_ = 69;              // MIDI
    double cents_ = 0;           // smoothed, against note_
    double lastPitch_ = -10;
    double med_[3] = {0, 0, 0};
    int nmed_ = 0;
    int pendNote_ = -1, pend_ = 0;
    double phase_ = 0;           // the strobe, in stripes
    std::string srcLabel_;
    uint64_t labelHash_ = 0;

    bool held(double now) const { return !have_ || now - lastPitch_ > 0.25; }
    double tol() const { return tolOf(P.get(P_TOL)); }
    bool inTune(double now) const { return !held(now) && std::fabs(cents_) <= tol() + 1e-9; }
    bool gather(double now);
    void readSrc(Src &x, double now, int range);
    std::string srcName(const Src &x, bool full) const;
    bool anyPlugin() const;
    void update(bool ok, double f, double now, double dt);
    std::string sourceLabel(bool *bad);
    bool hasId(int64_t id) const;
    void sourceMenu(Ui &ui, float x, float y, float w);
    // drawing
    void header(Ui &ui);
    bool hButton(Ui &ui, const R &r, const char *label, bool on, const Col &onCol, const Col &onInk);
    struct NoteGeom {
        float C = 0, fs = 0, R1 = 0, rcap = 0, yb = 0, cx = 0, ly = 0;
        R read;   // the line of numbers
    };
    NoteGeom noteGeom(Ui &ui, const R &na) const;
    const char *message(Col &mc) const;
    uint64_t noteKey() const;
    void drawNote(Ui &ui, const R &na);
    void drawMeter(Ui &ui, const R &mr);
    void drawStrobe(Ui &ui, const R &sr);
    void settings(Ui &ui, const R &gear);
    void enumDD(Ui &ui, const R &r, int id, const char *const *labels, int n);
};

gui::View *TunerPlugin::createView() { return new TunerView(*this); }

// ---------------------------------------------------------------- reading the audio
// one source: is it coming in, and where its newest samples end (this track's input: the ring the audio thread fills
// while the window is open; a TwoStorms plug-in: its audio key in shared memory, asked for every time, written while
// asked), its rate, and its level over the last 2048 samples
void TunerView::readSrc(Src &x, double now, int range)
{
    int ch = iclamp(iround(P.get(P_CH)), 0, 2);
    x.level = -200;
    if (x.id == 0) {
        x.sr = P.sampleRate() > 0 ? P.sampleRate() : 48000;
        int64_t w = P.ringW.load(std::memory_order_acquire);
        if (x.ownW < 0) x.ownW = w;
        if (now - P.lastBlockT.load(std::memory_order_relaxed) > 0.5) { x.stat = ST_NOAUDIO; return; }
        det_.setup(x.sr, range);
        // only what was written since the window opened (the ring stops while it's closed)
        if (w - x.ownW < det_.need()) { x.stat = ST_WAIT; return; }
        x.end = w;
        double sq = 0;
        for (int i = 1; i <= 2048; ++i) { double v = P.ring[(w - i) & RMASK].load(std::memory_order_relaxed); sq += v * v; }
        x.level = sq > 1e-30 ? 10 * std::log10(sq / 2048) : -200;
        x.stat = ST_OK;
        return;
    }
    int s = av.findId(x.id, x.slot);
    if (s != x.slot) { x.slot = s; x.reset(now); }
    if (s < 0) { x.stat = ST_NOTFOUND; return; }
    shm::Slot &b = shm::slot(s);
    if (now - b.heart.load(std::memory_order_relaxed) > 1) { x.stat = ST_STOPPED; x.reset(now); return; }
    b.areq.fetch_add(1, std::memory_order_relaxed);   // ask it to write its output (it stops half a second after)
    int64_t ln = b.alane.load(std::memory_order_relaxed) - 1;
    int64_t sq = b.aseq.load(std::memory_order_acquire);
    int64_t len = b.alen.load(std::memory_order_relaxed);
    int64_t start = b.astart.load(std::memory_order_relaxed);
    double ssr = b.srate.load(std::memory_order_relaxed);
    x.sr = ssr > 1000 ? ssr : (P.sampleRate() > 0 ? P.sampleRate() : 48000);
    if (ln == -2) { x.stat = ST_BUSY; x.reset(now); return; }
    if (ln < 0 || ln >= shm::AUD_LANES) { x.stat = now - x.seqT > 0.6 ? ST_NOAUDIO : ST_WAIT; return; }
    if (ln != x.lastLane) { x.lastLane = ln; x.lastSeq = -1; x.valid = 0; }
    if (sq != x.lastSeq) {
        // blocks written since the last look; the first look starts the count
        if (x.lastSeq >= 0 && sq > x.lastSeq && sq - x.lastSeq < 4096) x.valid += (sq - x.lastSeq) * std::max<int64_t>(1, len);
        else x.valid = 0;
        x.lastSeq = sq;
        x.seqT = now;
    } else if (now - x.seqT > 0.6) {
        x.stat = ST_NOAUDIO;
        x.valid = 0;
        return;
    }
    det_.setup(x.sr, range);
    int need = det_.need();
    // the newest whole blocks end where the block being written starts
    if (x.valid - len < need || need > shm::AUD_R - 8192) { x.stat = ST_WAIT; return; }
    x.lane = ln;
    x.end = start;
    shm::AudioLane &L = shm::get()->lane[ln];
    double e = 0;
    for (int i = 1; i <= 2048; ++i) {
        int k = (int)((start - i) & shm::AUD_MASK);
        float l = L.l[k].load(std::memory_order_relaxed), r = L.r[k].load(std::memory_order_relaxed);
        double v = ch == 0 ? l : (ch == 1 ? r : 0.5f * (l + r));
        e += v * v;
    }
    x.level = e > 1e-30 ? 10 * std::log10(e / 2048) : -200;
    x.stat = ST_OK;
}

// Every source it listens to, and the newest samples of the loudest one (the one heard stays until another is 6 dB
// louder) into buf_ for the detector
bool TunerView::gather(double now)
{
    int range = iclamp(iround(P.get(P_RANGE)), 0, 3);
    int ch = iclamp(iround(P.get(P_CH)), 0, 2);
    // the list, keeping what each one already knows
    std::vector<int64_t> want;
    if (P.get(P_OWN) >= 0.5) want.push_back(0);
    for (int k = 0; k < NSRC; ++k) {
        int64_t id = (int64_t)P.get(P_SID0 + k);
        if (id >= 100000 && std::find(want.begin(), want.end(), id) == want.end()) want.push_back(id);
    }
    int64_t curId = cur_ >= 0 && cur_ < (int)srcs_.size() ? srcs_[cur_].id : -1;
    std::vector<Src> next;
    for (int64_t id : want) {
        auto it = std::find_if(srcs_.begin(), srcs_.end(), [&](const Src &x) { return x.id == id; });
        if (it != srcs_.end()) next.push_back(*it);
        else { Src x; x.id = id; x.reset(now); next.push_back(x); }
    }
    srcs_.swap(next);
    cur_ = -1;
    for (int i = 0; i < (int)srcs_.size(); ++i)
        if (srcs_[i].id == curId) cur_ = i;
    levelDb_ = -200;
    nOk_ = 0;
    if (srcs_.empty()) { stat_ = ST_NONE; cur_ = -1; return false; }
    int best = -1;
    for (int i = 0; i < (int)srcs_.size(); ++i) {
        readSrc(srcs_[i], now, range);
        if (srcs_[i].stat != ST_OK) continue;
        ++nOk_;
        if (best < 0 || srcs_[i].level > srcs_[best].level) best = i;
    }
    if (best >= 0 && cur_ >= 0 && srcs_[cur_].stat == ST_OK && srcs_[best].level < srcs_[cur_].level + 6) best = cur_;
    if (best < 0) {
        // nothing coming in: why (none of them found at all: choose again)
        int st = ST_NONE;
        for (const Src &x : srcs_)
            if (x.stat == ST_WAIT) st = ST_WAIT;
        if (st != ST_WAIT)
            for (const Src &x : srcs_)
                if (x.stat != ST_NOTFOUND) { st = x.stat; break; }
        stat_ = st;
        return false;
    }
    cur_ = best;
    Src &x = srcs_[best];
    stat_ = ST_OK;
    levelDb_ = x.level;
    det_.setup(x.sr, range);
    int need = det_.need();
    if ((int)buf_.size() < need) buf_.resize(need);
    if (x.id == 0) {
        for (int i = 0; i < need; ++i) buf_[i] = P.ring[(x.end - need + i) & RMASK].load(std::memory_order_relaxed);
    } else {
        shm::AudioLane &L = shm::get()->lane[x.lane];
        for (int i = 0; i < need; ++i) {
            int k = (int)((x.end - need + i) & shm::AUD_MASK);
            float l = L.l[k].load(std::memory_order_relaxed), r = L.r[k].load(std::memory_order_relaxed);
            buf_[i] = ch == 0 ? l : (ch == 1 ? r : 0.5f * (l + r));
        }
    }
    return true;
}

bool TunerView::poll(double now)
{
    P.guiOpenBeat.store(now, std::memory_order_relaxed);   // this track's input keeps coming while the window is open
    if (now - lastDet_ < 0.030) return false;
    double dt = lastDet_ > 0 ? std::min(0.2, now - lastDet_) : 0.03;
    lastDet_ = now;
    int st0 = stat_, cur0 = cur_;
    bool ok = false;
    double f = 0;
    // nothing to do in silence
    if (gather(now) && levelDb_ > -66) {
        PitchDetector::Result r;
        ok = det_.detect(buf_.data(), r) && r.f > 0;
        f = r.f;
    }
    update(ok, f, now, dt);
    if (stat_ == ST_NONE) have_ = false;   // nothing to listen to: no last note either
    // draw when the reading moved, or the state changed
    bool hd = held(now), tu = inTune(now);
    bool draw = ok || stat_ != st0 || hd != hd_ || tu != tu_ || cur_ != cur0;
    // the level bar, now and then, while nothing else moves
    if (!draw && std::fabs(std::max(-80.0, levelDb_) - std::max(-80.0, drawnLevel_)) > 2 && now - levelT_ > 0.12) draw = true;
    if (draw) levelT_ = now;
    hd_ = hd;
    tu_ = tu;
    return draw;
}

// the note (with a little hysteresis: another note needs two looks in a row past 60 cents), its cents (the median of
// the last three, then smoothed over ~90 ms), and the strobe
void TunerView::update(bool ok, double f, double now, double dt)
{
    if (ok) {
        double ref = P.get(P_REF);
        double midi = 69 + 12 * std::log2(f / ref);
        auto restart = [&](int n) {
            note_ = n;
            cents_ = (midi - n) * 100;
            nmed_ = 0;
            pend_ = 0;
            pendNote_ = -1;
        };
        if (!have_ || now - lastPitch_ > 0.4) {
            restart((int)std::lround(midi));
            have_ = true;
        } else {
            double c = (midi - note_) * 100;
            if (std::fabs(c) > 60) {
                int nn = (int)std::lround(midi);
                if (nn == pendNote_) {
                    if (++pend_ >= 2) restart(nn);
                } else {
                    pendNote_ = nn;
                    pend_ = 1;
                }
            } else {
                pend_ = 0;
                med_[nmed_ % 3] = c;
                ++nmed_;
                double m = c;
                if (nmed_ >= 3) {
                    double a = med_[0], b = med_[1], e = med_[2];
                    m = std::max(std::min(a, b), std::min(std::max(a, b), e));
                }
                double k = 1 - std::exp(-dt / 0.09);
                cents_ += k * (m - cents_);
            }
        }
        lastPitch_ = now;
    }
    // the strobe turns with the error: a quarter stripe a second per cent (at most 12 a second, so it never seems to
    // run backwards), still within the tolerance's tenth
    if (!held(now)) {
        double v = std::max(-12.0, std::min(12.0, cents_ * 0.25));
        phase_ += v * dt;
        phase_ -= std::floor(phase_);
    }
}

// ---------------------------------------------------------------- the sources
bool TunerView::hasId(int64_t id) const
{
    for (int k = 0; k < NSRC; ++k)
        if ((int64_t)P.get(P_SID0 + k) == id) return true;
    return false;
}

bool TunerView::anyPlugin() const
{
    for (int k = 0; k < NSRC; ++k)
        if (P.get(P_SID0 + k) >= 100000) return true;
    return false;
}

// a source's name: full ("Gain Stage · Bass"), or short for a list of them ("Bass")
std::string TunerView::srcName(const Src &x, bool full) const
{
    if (x.id == 0) return full ? "This track's input" : "This track";
    if (x.slot < 0) return "";
    std::string tn = av.trackName(x.slot);
    bool gs = shm::slot(x.slot).kind.load() == shm::KIND_MASTER;
    std::string t = gs ? "Gain Stage" : av.pointTitle(x.slot);
    if (t.compare(0, 10, "TwoStorms ") == 0) t = t.substr(10);
    if (gs && !full && !tn.empty()) return tn;
    if (!tn.empty()) t += " \xC2\xB7 " + tn;
    return t;
}

// what the "Listen to" face says: the source, or the ones there are; nothing chosen, or none of them found: Select Source
std::string TunerView::sourceLabel(bool *bad)
{
    *bad = false;
    std::vector<const Src *> found;
    for (const Src &x : srcs_)
        if (x.id == 0 || x.slot >= 0) found.push_back(&x);
    if (found.empty()) { *bad = true; return "Select Source"; }
    if (found.size() == 1) {
        std::string t = srcName(*found[0], true);
        if (found[0]->id != 0 && !av.live(found[0]->slot)) { *bad = true; t += " (not running)"; }
        return t;
    }
    std::string t;
    for (const Src *x : found) t += (t.empty() ? "" : ", ") + srcName(*x, false);
    return t;
}

// the sources to pick from: tick as many as you like (the menu stays open); Clear all at the bottom
void TunerView::sourceMenu(Ui &ui, float x, float y, float w)
{
    std::vector<int> gs, other;
    int mx = (int)std::min<int64_t>(shm::NSLOT, shm::get()->h.maxslot.load());
    for (int s = 0; s < mx && gs.size() + other.size() < 300; ++s) {
        shm::Slot &b = shm::slot(s);
        int64_t o = b.owner.load();
        // the ones running, and the ones ticked even while they aren't
        if (o < 100000 || (!av.live(s) && !hasId(o))) continue;
        int64_t k = b.kind.load();
        if (k == shm::KIND_MASTER) gs.push_back(s);
        else if (k == shm::KIND_POINT) other.push_back(s);
    }
    auto byTrack = [&](int a, int b) {
        int64_t ta = shm::slot(a).trackno.load(), tb = shm::slot(b).trackno.load();
        if (ta != tb) return (ta < 0 ? 1 << 30 : ta) < (tb < 0 ? 1 << 30 : tb);
        return shm::slot(a).order.load() < shm::slot(b).order.load();
    };
    std::sort(gs.begin(), gs.end(), byTrack);
    std::sort(other.begin(), other.end(), byTrack);
    std::vector<int64_t> ids;
    std::vector<MenuItem> m;
    m.push_back(MenuItem::toggleItem("This track's input", 1, P.get(P_OWN) >= 0.5));
    m.push_back(MenuItem::sep());
    if (gs.empty()) m.push_back(MenuItem::item("No Gain Stage is running", 0, false, false));
    else m.push_back(MenuItem::item("Gain Stages:", 0, false, false));
    for (int s : gs) {
        std::string tn = av.trackName(s);
        int64_t o = shm::slot(s).owner.load();
        ids.push_back(o);
        m.push_back(MenuItem::toggleItem("Gain Stage \xC2\xB7 " + (tn.empty() ? std::string("(no track name)") : tn) + (av.live(s) ? "" : " (not running)"),
                                         10 + (int)ids.size() - 1, hasId(o)));
    }
    if (!other.empty()) {
        std::vector<MenuItem> sub;
        for (int s : other) {
            std::string t = av.pointTitle(s), tn = av.trackName(s);
            int64_t o = shm::slot(s).owner.load();
            ids.push_back(o);
            sub.push_back(MenuItem::toggleItem(t + (tn.empty() ? "" : "   [" + tn + "]") + (av.live(s) ? "" : " (not running)"),
                                               10 + (int)ids.size() - 1, hasId(o)));
        }
        m.push_back(MenuItem::sep());
        m.push_back(MenuItem::submenu("Other TwoStorms plug-ins", sub));
    }
    m.push_back(MenuItem::sep());
    m.push_back(MenuItem::buttonItem("Clear all", 2, true));
    TunerPlugin *pp = &P;
    ui.menu(m, x, y, [pp, ids](int r) {
        if (r == 1) pp->setOnce(P_OWN, pp->get(P_OWN) >= 0.5 ? 0 : 1);
        else if (r == 2) {
            pp->setOnce(P_OWN, 0);
            for (int k = 0; k < NSRC; ++k)
                if (pp->get(P_SID0 + k) != 0) pp->setOnce(P_SID0 + k, 0);
        } else if (r >= 10 && r - 10 < (int)ids.size()) {
            // ticked: off; not: into the first free place
            int64_t id = ids[r - 10];
            int at = -1, free = -1;
            for (int k = 0; k < NSRC; ++k) {
                int64_t v = (int64_t)pp->get(P_SID0 + k);
                if (v == id && at < 0) at = k;
                if (v < 100000 && free < 0) free = k;
            }
            if (at >= 0) pp->setOnce(P_SID0 + at, 0);
            else if (free >= 0) pp->setOnce(P_SID0 + free, (double)id);
        }
    }, w);
}

uint64_t TunerView::statusHash()
{
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ULL; };
    mix((uint64_t)stat_);
    mix(noteKey());
    mix(labelHash_);
    mix(now_sec() - P.presetMsgT < 4);
    mix((uint64_t)st.settings);
    return h;
}

// ---------------------------------------------------------------- drawing
// a header button, larger than the suite's (a touch screen): lit in onCol when on
bool TunerView::hButton(Ui &ui, const R &r, const char *label, bool on, const Col &onCol, const Col &onInk)
{
    if (!ui.drawS()) return false;
    bool hv = ui.hit(r);
    if (hv) ui.cursor = CUR_HAND;
    float rad = 7;
    if (on) {
        ui.shadow(r, rad, 10, onCol.alpha(0.35f));
        ui.fill(r, Paint::vertical(r.y, hv ? onCol.lighter(0.1f) : onCol, r.b(), onCol.darker(0.25f)), rad);
        ui.stroke(r, Paint(onCol.lighter(0.3f).alpha(0.7f)), rad);
    } else {
        ui.fill(r, Paint::vertical(r.y, hv ? th::hover() : th::raised(), r.b(), hv ? th::raised() : th::raised2()), rad);
        ui.stroke(r, Paint(hv ? th::borderHi() : th::border()), rad);
    }
    ui.text(r, label, on ? onInk : th::text(), F_SEMIBOLD, 14, 1);
    bool clk = hv && ui.lclick && !ui.used;
    if (clk) { ui.used = true; ui.changed = true; }
    return clk;
}

void TunerView::header(Ui &ui)
{
    bool bad = false;
    srcLabel_ = sourceLabel(&bad);
    uint64_t lh = 1469598103934665603ULL;
    for (char c : srcLabel_) lh = (lh ^ (uint8_t)c) * 1099511628211ULL;
    labelHash_ = lh ^ (uint64_t)bad;
    if (!ui.drawS()) return;
    ui.fill(R(0, 0, DW, HH), Paint::vertical(0, Col::hex(0x141317), HH, Col::hex(0x0d0d10)));
    ui.fill(R(0, HH - 1, DW, 1), Paint(th::border()));
    ui.fill(R(0, 0, DW, 2), Paint::linear(0, 0, th::orange().alpha(0.0f), DW * 0.5f, 0, th::orange().alpha(0.55f)));
    logo(ui, 16, 14, 34);
    float tx = 60;
    ui.textAt(tx, 31, "TWOSTORMS", th::orange(), F_BOLD, 17);
    float w1 = ui.textW(F_BOLD, 17, "TWOSTORMS");
    ui.textAt(tx + w1 + 7, 31, "TUNER", th::text(), F_BOLD, 17);
    char sub[128];
    snprintf(sub, sizeof sub, "A4 = %g Hz  \xC2\xB7  %s  \xC2\xB7  \xC2\xB1%g\xC2\xA2", std::round(P.get(P_REF) * 100) / 100,
             L_RANGE[iclamp(iround(P.get(P_RANGE)), 0, 3)], tol());
    // the right side, from the right edge: Settings, Presets, Mute, the source
    const float y = 13, h = 38;
    float x = DW - M;
    R gear(x - 40, y, 40, h);
    x -= 40 + 8;
    R pre(x - 112, y, 112, h);
    x -= 112 + 8;
    R mute(x - 118, y, 118, h);
    x -= 118 + 12;
    float sw = std::min(430.f, x - 320);
    R srcR(x - sw, y, sw, h);
    ui.textFit(R(tx, 38, srcR.x - tx - 12, 16), sub, th::faint(), F_REGULAR, 11.5f);
    // the source: a big dropdown
    {
        bool hv = ui.hit(srcR);
        if (hv) ui.cursor = CUR_HAND;
        ui.fill(srcR, Paint::vertical(srcR.y, hv ? th::hover() : th::raised(), srcR.b(), hv ? th::raised() : th::raised2()), 7);
        ui.stroke(srcR, Paint(anyPlugin() ? th::purple().alpha(0.85f) : (hv ? th::borderHi() : th::border())), 7);
        ui.text(R(srcR.x + 12, srcR.y + 1, 60, 14), "LISTEN TO", th::faint(), F_SEMIBOLD, 9);
        ui.textFit(R(srcR.x + 12, srcR.y + 14, srcR.w - 40, 20), srcLabel_.c_str(), bad ? th::amber() : th::text(), F_SEMIBOLD, 14);
        ui.icon(IC_CHEVRON, srcR.r() - 16, srcR.y + srcR.h * 0.5f, 12, th::dim());
        if (hv && ui.lclick && !ui.used) {
            ui.used = true;
            ui.changed = true;
            sourceMenu(ui, srcR.x, srcR.b() + 4, srcR.w);
        }
    }
    bool mu = P.get(P_MUTE) >= 0.5;
    if (hButton(ui, mute, mu ? "MUTED" : "MUTE", mu, th::red(), Col(1, 1, 1))) P.setOnce(P_MUTE, mu ? 0 : 1);
    if (hButton(ui, pre, "Presets", false, th::orange(), th::orangeInk())) presetsMenu(ui, P, pre.x, pre.b() + 4);
    {
        bool hv = ui.hit(gear);
        if (hButton(ui, gear, "", st.settings, th::orange(), th::orangeInk())) st.settings = !st.settings;
        ui.icon(IC_GEAR, gear.x + gear.w * 0.5f, gear.y + gear.h * 0.5f, 17, st.settings ? th::orangeInk() : (hv ? th::text() : th::dim()));
    }
    if (now_sec() - P.presetMsgT < 4) ui.text(R(pre.x - 230, HH - 14, 340, 12), P.presetMsg.c_str(), th::orange(), F_MEDIUM, 11, 2);
}

// a sharp or a flat, drawn (in pixels): top-left x, y, height h
static void drawSharp(Canvas &c, float x, float y, float h, const Col &col)
{
    float w = h * 0.62f, sw = h * 0.085f, bt = h * 0.13f, sl = h * 0.10f;
    float sx[2] = {x + w * 0.33f, x + w * 0.67f};
    for (int k = 0; k < 2; ++k) {
        float y0 = y + (k == 0 ? h * 0.06f : 0), y1 = y + h - (k == 0 ? 0 : h * 0.06f);
        c.fillRect(sx[k] - sw * 0.5f, y0, sw, y1 - y0, Paint(col));
    }
    for (int k = 0; k < 2; ++k) {
        float yc = y + h * (k == 0 ? 0.36f : 0.64f);
        c.beginPath();
        c.moveTo(x, yc + sl * 0.5f);
        c.lineTo(x + w, yc - sl * 0.5f);
        c.lineTo(x + w, yc - sl * 0.5f + bt);
        c.lineTo(x, yc + sl * 0.5f + bt);
        c.closePath();
        c.fillPath(Paint(col));
    }
}

static void bez(std::vector<Pt> &v, Pt a, Pt b, Pt c, Pt d, int n)
{
    for (int i = 1; i <= n; ++i) {
        float t = (float)i / n, u = 1 - t;
        v.push_back({u * u * u * a.x + 3 * u * u * t * b.x + 3 * u * t * t * c.x + t * t * t * d.x,
                     u * u * u * a.y + 3 * u * u * t * b.y + 3 * u * t * t * c.y + t * t * t * d.y});
    }
}

static void drawFlat(Canvas &c, float x, float y, float h, const Col &col)
{
    // the stem and the bowl as one outline, the bowl's hole as a second one the other way round (nonzero fill)
    auto P = [&](float u, float v) { return Pt{x + u * h, y + v * h}; };
    std::vector<Pt> o, in;
    o.push_back(P(0, 0));
    o.push_back(P(0.09f, 0));
    o.push_back(P(0.09f, 0.5f));
    bez(o, P(0.09f, 0.5f), P(0.25f, 0.38f), P(0.52f, 0.42f), P(0.47f, 0.6f), 14);
    bez(o, P(0.47f, 0.6f), P(0.43f, 0.76f), P(0.22f, 0.88f), P(0.09f, 1.0f), 14);
    o.push_back(P(0, 1.0f));
    in.push_back(P(0.09f, 0.62f));
    in.push_back(P(0.09f, 0.89f));
    bez(in, P(0.09f, 0.89f), P(0.18f, 0.82f), P(0.31f, 0.72f), P(0.33f, 0.62f), 12);
    bez(in, P(0.33f, 0.62f), P(0.36f, 0.52f), P(0.18f, 0.53f), P(0.09f, 0.62f), 12);
    c.beginPath();
    for (size_t i = 0; i < o.size(); ++i) i ? c.lineTo(o[i].x, o[i].y) : c.moveTo(o[i].x, o[i].y);
    c.closePath();
    for (size_t i = 0; i < in.size(); ++i) i ? c.lineTo(in[i].x, in[i].y) : c.moveTo(in[i].x, in[i].y);
    c.closePath();
    c.fillPath(Paint(col));
}

// a chevron pointing right (dir 1) or left (-1), centered at (cx, cy) in pixels
static void chevron(Canvas &c, float cx, float cy, float w, float h, int dir, float lw, const Col &col)
{
    Pt p[3] = {{cx - dir * w * 0.5f, cy - h * 0.5f}, {cx + dir * w * 0.5f, cy}, {cx - dir * w * 0.5f, cy + h * 0.5f}};
    c.strokePolyline(p, 3, lw, Paint(col));
}

// the note's size and place in the area (both passes)
TunerView::NoteGeom TunerView::noteGeom(Ui &ui, const R &na) const
{
    NoteGeom g;
    Font &FB = font(F_BOLD);
    const float capR = FB.capHeight(100) / 100;
    // the note's cap height: as big as the area allows (the note with its sign, the chevrons on both sides, the line
    // of numbers under it)
    float C = std::min(na.h * 0.92f / 1.36f, na.w * 0.94f / 3.4f);
    C = std::max(C, 20.f);
    float ps = C / capR * ui.s;
    ps = std::max(8.f, std::round(ps / 4) * 4);   // in steps (the glyph cache keeps every size it draws)
    g.fs = ps / ui.s;
    g.C = g.fs * capR;
    g.R1 = std::max(15.f, std::min(76.f, g.C * 0.24f));
    g.rcap = capR * g.R1;
    float blockH = g.C + g.C * 0.17f + g.rcap;
    float top = na.y + (na.h - blockH) * 0.5f;
    g.yb = top + g.C;
    g.cx = na.x + na.w * 0.5f;
    g.ly = g.yb + g.C * 0.17f + g.rcap;
    g.read = R(na.x, g.ly - g.rcap - g.R1 * 0.3f, na.w, g.rcap + g.R1 * 0.6f);
    return g;
}

const char *TunerView::message(Col &mc) const
{
    mc = th::amber();
    switch (stat_) {
    case ST_NONE: case ST_NOTFOUND: return "Select Source";
    case ST_STOPPED: return "Not running (bypassed, offline, or FX not running)";
    case ST_BUSY: return "Too many TwoStorms plug-ins are being listened to at once";
    case ST_NOAUDIO: return "No audio is coming in";
    default: mc = th::faint(); return nullptr;
    }
}

// what the still layer shows of the note: when it changes, the still layer is drawn again
uint64_t TunerView::noteKey() const
{
    if (!have_) return 1;
    double ac = std::fabs(cents_);
    int lit = hd_ || ac <= tol() ? 0 : (ac > 20 ? 3 : (ac > 8 ? 2 : 1));
    return 2 + (uint64_t)(note_ + 200) * 64 + (uint64_t)(hd_ ? 0 : (tu_ ? 1 : 2)) * 8 + (uint64_t)lit * 2 + (cents_ < 0 ? 1 : 0);
}

// the note, its sign, its octave and the chevrons (still layer); the numbers under it (moving)
void TunerView::drawNote(Ui &ui, const R &na)
{
    Canvas &c = ui.cv();
    bool hd = hd_, tune = tu_;
    NoteGeom g = noteGeom(ui, na);
    const float C = g.C, fs = g.fs, yb = g.yb, cx = g.cx;
    Col mc;
    const char *msg = message(mc);
    if (!have_) {
        if (ui.drawS()) {
            const char *t = msg ? msg : "Play a note";
            float sz = std::max(14.f, std::min(46.f, C * 0.2f));
            ui.textFit(R(na.x, na.y, na.w, na.h), t, msg ? mc : th::faint(), F_SEMIBOLD, sz, 1);
        }
        return;
    }
    // the colors: green in tune, white out of it (the signs orange), dim once the note has gone
    Col dimc = Col::hex(0x55504b);
    Col nc = hd ? dimc : (tune ? th::green() : th::text());
    if (ui.drawS()) {
        int pc = ((note_ % 12) + 12) % 12, oct = note_ / 12 - 1;
        bool flats = iround(P.get(P_NAMES)) == 1;
        const char *L = flats ? N_FL[pc] : N_SH[pc];
        int sign = flats ? A_FL[pc] : A_SH[pc];
        float lw = ui.textW(F_BOLD, fs, L);
        float accH = C * 0.48f, accW = sign > 0 ? accH * 0.62f : (sign < 0 ? accH * 0.5f : 0);
        char os[16];
        snprintf(os, sizeof os, "%d", oct);
        float ofs = fs * 0.34f, ow = ui.textW(F_SEMIBOLD, ofs, os);
        float gap = C * 0.06f;
        float gw = lw + gap + std::max(accW, ow);
        float gx = cx - gw * 0.5f;
        // a soft glow behind the note when it's in tune
        if (tune) {
            R gr(gx - C * 0.08f, yb - C * 1.0f, gw + C * 0.16f, C * 1.0f);
            c.shadow(ui.X(gr.x), ui.Y(gr.y), ui.W(gr.w), ui.W(gr.h), ui.W(C * 0.25f), ui.W(C * 0.4f), th::green().alpha(0.12f), true);
        }
        ui.textAt(gx, yb, L, nc, F_BOLD, fs);
        float ax = gx + lw + gap;
        if (sign > 0) drawSharp(c, ui.X(ax), ui.Y(yb - C), ui.W(accH), nc);
        else if (sign < 0) drawFlat(c, ui.X(ax), ui.Y(yb - C), ui.W(accH), nc);
        ui.textAt(ax, yb, os, hd ? dimc : th::dim(), F_SEMIBOLD, ofs);
        // the chevrons: lit on the side it's off to (more of them the further off), both inner ones green in tune
        float chH = C * 0.42f, chW = C * 0.2f, chS = C * 0.22f, cy = yb - C * 0.5f, clw = ui.W(C * 0.065f);
        double ac = std::fabs(cents_);
        int lit = hd || ac <= tol() ? 0 : (ac > 20 ? 3 : (ac > 8 ? 2 : 1));
        for (int side = 0; side < 2; ++side) {
            bool on = cents_ < 0 ? side == 0 : side == 1;
            for (int k = 0; k < 3; ++k) {
                float x0 = side == 0 ? gx - C * 0.33f - chW * 0.5f - k * chS : gx + gw + C * 0.33f + chW * 0.5f + k * chS;
                Col col = Col::hex(0x1f1d22);
                if (tune && k == 0) col = th::green();
                else if (on && k < lit) col = k == 2 ? th::red() : th::orange();
                chevron(c, ui.X(x0), ui.Y(cy), ui.W(chW), ui.W(chH), side == 0 ? 1 : -1, clw, col);
            }
        }
        if (msg) ui.text(R(na.x, na.y + 4, na.w, 18), msg, mc, F_MEDIUM, 13, 1);
    }
    if (!ui.drawD()) return;
    // the numbers: cents (big, colored) and the frequency
    char cs[32], hz[32];
    double cv = std::fabs(cents_) < 0.05 ? 0.0 : cents_;
    if (cv > 0) snprintf(cs, sizeof cs, "+%.1f\xC2\xA2", cv);
    else if (cv < 0) snprintf(cs, sizeof cs, "\xE2\x88\x92%.1f\xC2\xA2", -cv);
    else snprintf(cs, sizeof cs, "0.0\xC2\xA2");
    double f = P.get(P_REF) * std::pow(2.0, (note_ + cents_ / 100 - 69) / 12);
    snprintf(hz, sizeof hz, f < 1000 ? "%.2f Hz" : "%.1f Hz", f);
    float R1 = g.R1, hs = R1 * 0.6f;
    float w1 = ui.textW(F_BOLD, R1, cs, true), w2 = ui.textW(F_MEDIUM, hs, hz, true), sp = R1 * 0.9f;
    float lx = cx - (w1 + sp + w2) * 0.5f;
    ui.textAt(lx, g.ly, cs, hd ? dimc : (tune ? th::green() : th::orange()), F_BOLD, R1, true);
    ui.textAt(lx + w1 + sp, g.ly, hz, hd ? dimc : th::dim(), F_MEDIUM, hs, true);
}

void TunerView::drawMeter(Ui &ui, const R &mr)
{
    float pad = std::max(18.f, mr.h * 0.28f);
    R tr(mr.x + pad, mr.y + mr.h * 0.36f, mr.w - 2 * pad, mr.h * 0.42f);   // the track
    float cx = tr.x + tr.w * 0.5f, hw = tr.w * 0.5f;
    double tl = tol();
    if (ui.drawS()) {
        ui.fill(mr, Paint::vertical(mr.y, Col::hex(0x0d0c0f), mr.b(), Col::hex(0x09090b)), 10);
        ui.stroke(mr, Paint(th::border()), 10);
        ui.fill(tr, Paint(th::well()), tr.h * 0.25f);
        // the tolerance, green
        float zw = (float)(tl / 50) * hw;
        ui.fill(R(cx - zw, tr.y, 2 * zw, tr.h), Paint(th::green().alpha(0.18f)), 2);
        // ticks every 5 cents, numbers every 10
        float ls = std::max(10.f, std::min(30.f, mr.h * 0.17f));
        for (int k = -50; k <= 50; k += 5) {
            float x = cx + k / 50.f * hw;
            bool major = k % 10 == 0;
            float th_ = major ? tr.h * 0.55f : tr.h * 0.3f;
            Col tc = k == 0 ? th::green() : (major ? Col::hex(0x5a5550) : Col::hex(0x34313a));
            ui.fill(R(x - (k == 0 ? 1.5f : 0.75f), tr.b() - th_, k == 0 ? 3.f : 1.5f, th_), Paint(tc));
            if (major) {
                char s[8];
                if (k > 0) snprintf(s, sizeof s, "+%d", k);
                else if (k < 0) snprintf(s, sizeof s, "\xE2\x88\x92%d", -k);
                else snprintf(s, sizeof s, "0");
                ui.text(R(x - 40, mr.y + mr.h * 0.06f, 80, ls), s, k == 0 ? th::green() : th::faint(), F_SEMIBOLD, ls, 1);
            }
        }
        // flat and sharp at the ends
        Canvas &c = ui.cv();
        float gh = std::min(tr.h * 0.9f, pad * 0.9f);
        drawFlat(c, ui.X(mr.x + pad * 0.5f - gh * 0.22f), ui.Y(tr.y + (tr.h - gh) * 0.5f), ui.W(gh), Col::hex(0x6a645f));
        drawSharp(c, ui.X(mr.r() - pad * 0.5f - gh * 0.31f), ui.Y(tr.y + (tr.h - gh) * 0.5f), ui.W(gh), Col::hex(0x6a645f));
    }
    if (!ui.drawD() || !have_) return;
    bool hd = hd_, tune = tu_;
    double cv = std::max(-50.0, std::min(50.0, cents_));
    float x = cx + (float)(cv / 50) * hw;
    Col col = hd ? Col::hex(0x4a4642) : (tune ? th::green() : th::orange());
    // the trail from the middle
    if (!hd) {
        float a = std::min(cx, x), b = std::max(cx, x);
        ui.fill(R(a, tr.y + tr.h * 0.2f, b - a, tr.h * 0.6f), Paint(col.alpha(0.35f)), 2);
    }
    // the needle
    float nw = std::max(4.f, mr.h * 0.07f);
    R nr(x - nw * 0.5f, mr.y + mr.h * 0.26f, nw, mr.h * 0.64f);
    if (!hd) ui.shadow(nr, nw * 0.5f, nw * 2.2f, col.alpha(0.55f));
    ui.fill(nr, Paint(col), nw * 0.5f);
    if (std::fabs(cents_) > 50 && !hd) {
        // off the scale: an arrow at the end
        Canvas &c = ui.cv();
        float ex = cents_ > 0 ? tr.r() : tr.x, d = cents_ > 0 ? 1.f : -1.f;
        c.beginPath();
        c.moveTo(ui.X(ex + d * pad * 0.1f), ui.Y(tr.y - tr.h * 0.25f));
        c.lineTo(ui.X(ex + d * pad * 0.75f), ui.Y(tr.y + tr.h * 0.5f));
        c.lineTo(ui.X(ex + d * pad * 0.1f), ui.Y(tr.b() + tr.h * 0.25f));
        c.closePath();
        c.fillPath(Paint(th::red()));
    }
}

void TunerView::drawStrobe(Ui &ui, const R &sr)
{
    if (ui.drawS()) {
        ui.fill(sr, Paint(th::well()), 8);
        ui.stroke(sr, Paint(th::border()), 8);
    }
    if (!ui.drawD() || !have_) return;
    bool hd = hd_, tune = tu_;
    Col col = hd ? Col::hex(0x2c2a2e) : (tune ? th::green() : th::purple());
    Canvas &c = ui.cv();
    R in = sr.inset(3);
    c.pushClip(ui.X(in.x), ui.Y(in.y), ui.W(in.w), ui.W(in.h));
    // two rows: the lower one at twice the speed (the second partial, like a strobe tuner's second ring)
    for (int row = 0; row < 2; ++row) {
        float rh = in.h * 0.5f, ry = in.y + row * rh;
        float per = rh * 1.6f;
        double ph = phase_ * (row + 1);
        ph -= std::floor(ph);
        float off = (float)ph * per;
        Col rc = row == 0 ? col : col.alpha(0.75f);
        for (float x = in.x - per + off; x < in.r(); x += per)
            ui.fill(R(x, ry + (row == 0 ? 0 : 1), per * 0.5f, rh - 1), Paint(rc));
    }
    c.popClip();
    // the middle mark
    ui.fill(R(sr.x + sr.w * 0.5f - 1, sr.y - 4, 2, sr.h + 8), Paint(Col(1, 1, 1, 0.5f)));
}

void TunerView::enumDD(Ui &ui, const R &r, int id, const char *const *labels, int n)
{
    int cv = iclamp(iround(P.get(id)), 0, n - 1);
    if (ui.dropdown(r, labels[cv])) {
        std::vector<MenuItem> m;
        for (int i = 0; i < n; ++i) m.push_back(MenuItem::item(labels[i], i + 1, i == cv));
        TunerPlugin *pp = &P;
        ui.menu(m, r.x, r.b() + 4, [pp, id](int r2) { pp->setOnce(id, r2 - 1); }, r.w);
    }
}

void TunerView::settings(Ui &ui, const R &gear)
{
    if (!ui.drawS()) return;
    R in;
    if (!settingsBegin(ui, st, gear, SW, SH, in)) return;
    const float h = 32, lw = 130, dy = h + 10;
    float y = in.y;
    ui.text(R(in.x, y, 200, 14), "SETTINGS", th::faint(), F_SEMIBOLD, 10.5f);
    y += 24;
    auto label = [&](float yy, const char *t) { ui.text(R(in.x, yy, lw, h), t, th::dim(), F_REGULAR, 13); };
    // the reference: - / + by 1 Hz for a finger, the box for a mouse (drag, wheel, type)
    label(y, "Reference A4");
    float x = in.x + lw;
    if (ui.button(R(x, y, 40, h), "\xE2\x88\x92")) P.setOnce(P_REF, std::max(415.0, std::round(P.get(P_REF)) - 1));
    ui.valueBox(R(x + 46, y, 110, h), P_REF, true, KD_OTHER, 1, 0.1, 1);
    if (ui.button(R(x + 162, y, 40, h), "+")) P.setOnce(P_REF, std::min(466.0, std::round(P.get(P_REF)) + 1));
    if (ui.button(R(x + 214, y, 70, h), "440", std::fabs(P.get(P_REF) - 440) < 0.01)) P.setOnce(P_REF, 440);
    y += dy;
    label(y, "Range");
    enumDD(ui, R(in.x + lw, y, 200, h), P_RANGE, L_RANGE, 4);
    ui.text(R(in.x + lw + 212, y, in.r() - in.x - lw - 212, h),
            iround(P.get(P_RANGE)) == 1 ? "27 to 700 Hz" : (iround(P.get(P_RANGE)) == 0 ? "50 to 1500 Hz" : (iround(P.get(P_RANGE)) == 2 ? "65 to 1400 Hz" : "27 to 2000 Hz")),
            th::faint(), F_REGULAR, 11.5f);
    y += dy;
    label(y, "In tune within");
    enumDD(ui, R(in.x + lw, y, 200, h), P_TOL, L_TOL, 4);
    y += dy;
    label(y, "Note names");
    enumDD(ui, R(in.x + lw, y, 200, h), P_NAMES, L_NAMES, 2);
    y += dy;
    label(y, "Display");
    enumDD(ui, R(in.x + lw, y, 200, h), P_DISP, L_DISP, 3);
    float x2 = in.x + lw + 212;
    ui.text(R(x2, y, 70, h), "Channel", th::dim(), F_REGULAR, 13);
    enumDD(ui, R(x2 + 70, y, in.r() - x2 - 70, h), P_CH, L_CH, 3);
    y += dy;
    // what Mute silences: every track it hears too (the TwoStorms plug-ins ticked under Listen to), or only its own
    // output
    {
        bool ma = P.get(P_MALL) >= 0.5;
        R cb(in.x + lw, y + (h - 20) * 0.5f, 20, 20);
        bool clk = ui.checkbox(cb, ma);
        R lr(cb.r() + 10, y, in.r() - cb.r() - 10, h);
        if (!clk && ui.hit(lr) && ui.lclick && !ui.used) { ui.used = true; clk = true; }
        if (clk) P.setOnce(P_MALL, ma ? 0 : 1);
        label(y, "Mute");
        ui.text(R(lr.x, y, 170, h), "Mute all tracks heard", th::text(), F_REGULAR, 13);
        ui.textFit(R(lr.x + 162, y, lr.r() - lr.x - 162, h), ma ? "(the plug-ins it listens to too)" : "(off: only the Tuner's own output)",
                   th::faint(), F_REGULAR, 11.5f);
    }
    y += dy + 4;
    ui.fill(R(in.x, y, in.w, 1), Paint(th::border()));
    y += 10;
    static const char *help[] = {
        "Listen to Gain Stages anywhere in the project: the Tuner can sit on a track of its own.",
        "Tick several sources and it hears the loudest. Mute can silence them all (it still hears them).",
        "Drag the window's edges to any size and shape, or make it fill a screen.",
    };
    for (const char *t : help) { ui.textFit(R(in.x, y, in.w, 14), t, th::faint(), F_REGULAR, 11); y += 16; }
    settingsEnd(ui);
}

void TunerView::frame(Ui &ui)
{
    av.refresh();
    if (ui.drawS()) ui.fill(R(0, 0, DW, DH), Paint::vertical(0, th::bg(), DH, th::bg2()));
    R gear(DW - M - 40, 13, 40, 38);
    if (st.settings) ui.block(settingsRect(gear, SW, SH));
    header(ui);
    if (st.settings) ui.block(settingsRect(gear, SW, SH));
    // the display: the note, then the needle and the strobe across the width
    int disp = iclamp(iround(P.get(P_DISP)), 0, 2);
    bool needle = disp != 2, strobe = disp != 1;
    R pane(M, HH + 12, DW - 2 * M, DH - HH - 12 - M);
    float CH = pane.h - 24;
    float mh = needle ? std::max(84.f, std::min(190.f, CH * 0.22f)) : 0;
    float sh = strobe ? std::max(34.f, std::min(needle ? 90.f : 160.f, CH * (needle ? 0.11f : 0.2f))) : 0;
    float gaps = (needle ? 12.f : 0.f) + (strobe ? 12.f : 0.f);
    R na(pane.x + 12, pane.y + 12, pane.w - 24, CH - mh - sh - gaps);
    // a tall window: the note is as big as the width allows, and the whole stack sits in the middle
    {
        float cw = na.w * 0.94f / 3.4f, chh = na.h * 0.92f / 1.36f;
        if (cw < chh) {
            float need = cw * 1.36f / 0.92f + 24;
            float extra = na.h - need;
            na.y += extra * 0.5f;
            na.h = need;
        }
    }
    bool mu = P.get(P_MUTE) >= 0.5;
    if (ui.drawS()) {
        ui.fill(pane, Paint::vertical(pane.y, Col::hex(0x111013), pane.b(), Col::hex(0x0b0b0d)), 14);
        ui.stroke(pane, Paint(mu ? th::red() : th::border()), 14, mu ? 3.f : 1.f);
        if (mu) {
            // from across the stage: a red frame and a red tag
            const char *t = P.get(P_MALL) >= 0.5 && anyPlugin() ? "MUTED  (and every track it hears)" : "MUTED";
            float tw = ui.textW(F_BOLD, 15, t) + 24;
            R tag(pane.x + 14, pane.y + 12, tw, 28);
            ui.fill(tag, Paint(th::red()), 6);
            ui.text(tag, t, Col(1, 1, 1), F_BOLD, 15, 1);
        }
    }
    // the level, top right: is anything arriving
    float lw2 = 90, lx = pane.r() - 14 - lw2, ly = pane.y + 18;
    levR_ = R(lx - 140, ly - 5, 140 + lw2, 18);
    naR_ = na;
    readR_ = noteGeom(ui, na).read;
    needleOn_ = needle;
    strobeOn_ = strobe;
    if (ui.drawD()) {
        drawnLevel_ = levelDb_;
        ui.fill(R(lx, ly, lw2, 8), Paint(th::well()), 4);
        float fl = (float)std::max(0.0, std::min(1.0, (levelDb_ + 66) / 60));
        if (fl > 0) ui.fill(R(lx, ly, lw2 * fl, 8), Paint(stat_ == ST_OK ? th::green().alpha(0.8f) : th::faint()), 4);
        // more than one coming in: which one is heard
        std::string lt = stat_ != ST_OK ? "" : (nOk_ > 1 && cur_ >= 0 && cur_ < (int)srcs_.size() ? srcName(srcs_[cur_], false)
                                                                                                  : (levelDb_ > -66 ? "signal" : "quiet"));
        ui.textFit(R(lx - 140, ly - 4, 132, 16), lt.c_str(), nOk_ > 1 ? th::dim() : th::faint(), F_REGULAR, 11, 2);
    }
    drawNote(ui, na);
    float y = na.b() + 12;
    mR_ = R(pane.x + 12, y, pane.w - 24, mh);
    if (needle) {
        drawMeter(ui, mR_);
        y += mh + 12;
    }
    sR_ = R(pane.x + 12, y, pane.w - 24, sh);
    if (strobe) drawStrobe(ui, sR_);
    settings(ui, gear);
}

// what moves: the numbers under the note, the needle, the strobe, the level
bool TunerView::dynamicRegions(std::vector<R> &rs)
{
    rs.push_back(levR_);
    if (!have_) return true;
    rs.push_back(readR_);
    if (needleOn_) rs.push_back(mR_);
    if (strobeOn_) rs.push_back(R(sR_.x - 2, sR_.y - 6, sR_.w + 4, sR_.h + 12));
    return true;
}

// ---------------------------------------------------------------- the kind
static const char *s_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_ANALYZER, CLAP_PLUGIN_FEATURE_UTILITY,
                                   CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const PluginKind &kind()
{
    static const PluginKind k = {
        {CLAP_VERSION_INIT, "com.twostorms.tuner", "TwoStorms Tuner", "TwoStorms", "", "", "", TS_VERSION,
         "A chromatic tuner for guitar, bass and voice: listens to this track or to any TwoStorms Gain Stage in the project (it can sit "
         "on a track of its own), mutes, and fills any window or screen.",
         s_features},
        &table,
        &factoryPresets,
        "Tuner",
        "TWOSTORMS TUNER",
        shm::KIND_POINT,
        shm::SK_TUNER,
        false,
        false,
        0,
        &create,
        2,
    };
    return k;
}

} // namespace tuner
} // namespace ts
