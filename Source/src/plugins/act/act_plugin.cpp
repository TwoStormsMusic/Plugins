// TwoStorms Act: a gain stage to drop after any plug-in (up to 8 channels, linked pairs), with
// meters, Auto (Target / Match / Reference), drive links and the finale, plus an optional transient
// shaper before the gain. The Gain Stage lists and remote-controls every Act.
#include "act_plugin.h"
#include "../../core/act_view.h"
#include "../../core/clock.h"
#include "../../core/eel.h"
#include "../../gui/common.h"
#include "../../gui/editor.h"
#include "act_shaper.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ts {
namespace actp {

using namespace gui;

// ---------------------------------------------------------------- parameters
static void fmt_amt(double v, char *b, int n) { snprintf(b, n, "%+.0f%%", v); }
static void fmt_xo(double f, char *b, int n)
{
    if (f >= 1000) snprintf(b, n, "%.2f kHz", f / 1000);
    else snprintf(b, n, "%.0f Hz", f);
}
static void fmt_lams(double v, char *b, int n) { snprintf(b, n, "%.1f ms", v); }

static const ParamTable &table()
{
    static ParamTable t = [] {
        ParamTable t;
        addActParams(t, false);
        const char *M = "Transient shaper";
        t.add(P_TSON, "Transient shaper", 0, 1, 0, PF_HOST | PF_INT | PF_BOOL, nullptr, M);
        static const char *an[3] = {"Low attack", "Mid attack", "High attack"};
        static const char *sn[3] = {"Low sustain", "Mid sustain", "High sustain"};
        for (int b = 0; b < 3; ++b) {
            t.add(P_TSATT + 2 * b, an[b], -100, 100, 0, PF_HOST, fmt_amt, M).unit = "%";
            t.add(P_TSSUS + 2 * b, sn[b], -100, 100, 0, PF_HOST, fmt_amt, M).unit = "%";
        }
        t.add(P_TSX1, "Crossover 1", 20, 20000, 200, PF_HOST, fmt_xo, M).unit = "Hz";
        t.add(P_TSX2, "Crossover 2", 20, 20000, 2500, PF_HOST, fmt_xo, M).unit = "Hz";
        t.add(P_TSXO1, "Crossover 1 used", 0, 1, 0, PF_HOST | PF_INT | PF_BOOL, nullptr, M);
        t.add(P_TSXO2, "Crossover 2 used", 0, 1, 0, PF_HOST | PF_INT | PF_BOOL, nullptr, M);
        t.add(P_TSLA, "Shaper lookahead", 0, 1, 0, PF_HOST | PF_INT | PF_BOOL, nullptr, M);
        t.add(P_TSLAMS, "Shaper lookahead time", 0.1, 10, 2, PF_HOST, fmt_lams, M).unit = "ms";
        return t;
    }();
    return t;
}

static const std::vector<FactoryPreset> &noPresets()
{
    static std::vector<FactoryPreset> v;
    return v;
}

// ---------------------------------------------------------------- the plug-in
class ActPlugin : public Plugin {
public:
    explicit ActPlugin(const clap_host *h) : Plugin(h, kind()) {}
    Shaper shaper;
    std::atomic<double> md[3] = {};      // the shaper's band meters, for the window
    gui::View *createView() override;

    ShaperIn shaperIn() const
    {
        ShaperIn s;
        s.sw = P.get(P_TSON) >= 0.5;
        s.xo1 = P.get(P_TSXO1) >= 0.5;
        s.xo2 = P.get(P_TSXO2) >= 0.5;
        for (int b = 0; b < 3; ++b) { s.att[b] = P.get(P_TSATT + 2 * b); s.sus[b] = P.get(P_TSSUS + 2 * b); }
        s.x1 = P.get(P_TSX1);
        s.x2 = P.get(P_TSX2);
        s.la = P.get(P_TSLA) >= 0.5;
        s.lams = P.get(P_TSLAMS);
        return s;
    }

protected:
    void dspActivate(double, uint32_t) override {}
    void dspReset() override {}
    void dspProcess(const double *const *in, const double *const *, double *const *out, int n) override
    {
        const int NC = K.nch();
        for (int c = 0; c < NC; ++c)
            if (out[c] != in[c]) memcpy(out[c], in[c], sizeof(double) * n);
        act.block(n);
        int ga[8], gn[8];
        int ng = act.groups(ga, gn);
        shaper.block(shaperIn(), sampleRate(), act.nchs, ng, ga, gn);
        for (int b = 0; b < 3; ++b) md[b].store(shaper.TSMD[b], std::memory_order_relaxed);
        shaper.process(out, act.nchs, n);
        act.process(out, NC, n);
        act.audioKey(out, NC, n);
    }
    int latencyFromParams(double sr) const override { return Shaper::latency(shaperIn(), sr); }
};

static Plugin *create(const clap_host *h) { return new ActPlugin(h); }

// ---------------------------------------------------------------- the window
static const char *const L_HOLD[] = {"Off", "1 s", "2 s", "5 s", "10 s", "Infinite"};
static const char *const L_RANGE[] = {"24 dB", "36 dB", "48 dB", "60 dB", "72 dB", "96 dB"};
static const char *const L_ZOOM[] = {"100%", "115%", "130%", "150%", "175%", "200%"};
static const char *const L_FIN[] = {"Auto", "Always", "Never"};

class ActWin : public gui::View {
public:
    explicit ActWin(ActPlugin &p) : P(p), av(p) {}
    void designSize(float &w, float &h) override
    {
        w = DW;
        h = height();
    }
    void frame(Ui &ui) override;
    bool wantsFull() override { return st.settings; }
    uint64_t statusHash() override;

private:
    static constexpr float DW = 940, M = 14, SW = 560, SH = 196;
    ActPlugin &P;
    ActView av;
    CommonState st;

    int bands() const { return 1 + (P.get(P_TSXO1) >= 0.5) + (P.get(P_TSXO2) >= 0.5); }
    float shaperH() const { return 16 + 26 + 10 + bands() * 30 + 6; }
    float height() { return 66 + shaperH() + 12 + actCardHeight(av) + 12 + 38; }
    void enumDD(Ui &ui, const R &r, int id, const char *const *labels, int n)
    {
        int cv = iclamp(iround(P.get(id)), 0, n - 1);
        if (ui.dropdown(r, labels[cv])) {
            std::vector<MenuItem> m;
            for (int i = 0; i < n; ++i) m.push_back(MenuItem::item(labels[i], i + 1, i == cv));
            ActPlugin *pp = &P;
            ui.menu(m, r.x, r.b() + 4, [pp, id](int r2) { pp->setOnce(id, r2 - 1); }, r.w);
        }
    }
    void label(Ui &ui, const R &r, const char *t, bool on = true) { ui.text(r, t, on ? th::dim() : th::faint(), F_REGULAR, 12); }
    void drawShaper(Ui &ui, const R &r);
    void settings(Ui &ui, const R &gear);
};

gui::View *ActPlugin::createView() { return new ActWin(*this); }

uint64_t ActWin::statusHash()
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
    mix((uint64_t)st.settings);
    mix((uint64_t)P.latencyNow());
    mix((uint64_t)shm::get()->h.slotver.load());
    return h;
}

void ActWin::settings(Ui &ui, const R &gear)
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
    ui.textFit(R(in.x + 64 + 86 + 14, y, in.r() - in.x - 64 - 86 - 14, h), fs.c_str(), warn ? th::red() : (av.fin() ? th::orange() : th::dim()),
               F_REGULAR, 11.5f);
    y += h + 14;
    ui.fill(R(in.x, y, in.w, 1), Paint(th::border()));
    y += 8;
    static const char *help[] = {
        "Auto modes: Target = the output hits Target. Reference = equals the reference. Match = equals the previous plug-in.",
        "Drive link: when this gain changes, the linked Act or plug-in parameter moves the opposite way.",
        "Channels, links and the finale: the \xE2\x80\xA6 menu. Shift (or Ctrl) while dragging adjusts finely.",
    };
    for (const char *t : help) { ui.textFit(R(in.x, y, in.w, 14), t, th::faint(), F_REGULAR, 10.5f); y += 15; }
    char idb[64];
    snprintf(idb, sizeof idb, "Plugin ID %lld", (long long)av.myid);
    ui.text(R(in.x, y + 2, in.w, 14), idb, th::ghost().lighter(0.3f), F_REGULAR, 10.5f, 2);
    settingsEnd(ui);
}

// the transient shaper: a row for the switch, the crossovers (a tick in front of each: ticked, it splits there)
// and lookahead, then one row per band: its range, Attack, Sustain, and what it is doing now
void ActWin::drawShaper(Ui &ui, const R &r)
{
    bool on = P.get(P_TSON) >= 0.5;
    int nb = bands();
    float x = r.x + 14, y = r.y + 14;
    if (ui.drawS()) {
        ui.shadow(r, 8, 12, Col(0, 0, 0, 0.45f));
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x151417), r.b(), Col::hex(0x101012)), 8);
        ui.stroke(r, Paint(th::border()), 8);
        if (ui.button(R(x, y, 172, 26), on ? "Transient shaper: on" : "Transient shaper: off", on)) P.setOnce(P_TSON, on ? 0 : 1);
        float cx = x + 172 + 22;
        label(ui, R(cx, y, 80, 26), "Crossovers");
        cx += 82;
        for (int c = 0; c < 2; ++c) {
            bool used = P.get(P_TSXO1 + c) >= 0.5;
            if (ui.checkbox(R(cx, y + 6, 14, 14), used)) P.setOnce(P_TSXO1 + c, used ? 0 : 1);
            if (used) ui.valueBox(R(cx + 22, y, 92, 26), P_TSX1 + c, true, KD_LOG, std::log(10.0) / 120, std::log(10.0) / 1200);
            cx += 22 + 92 + 18;
        }
        bool la = P.get(P_TSLA) >= 0.5;
        R lab(r.r() - 14 - 84, y, 84, 26);
        R lb(lab.x - 6 - 104, y, 104, 26);
        if (ui.button(lb, "Lookahead", la)) P.setOnce(P_TSLA, la ? 0 : 1);
        if (la) ui.valueBox(lab, P_TSLAMS, true, KD_TIME, 0.1, 0.01);
    }
    y += 26 + 12;
    // the bands, lowest first (the crossovers in order, whichever box holds which)
    double x1 = P.get(P_TSX1), x2 = P.get(P_TSX2);
    double fa = nb == 3 ? std::min(x1, x2) : (P.get(P_TSXO1) >= 0.5 ? x1 : x2);
    double fb = std::max(std::max(x1, x2), fa * 1.26);
    char f1[32], f2[32], tl[96];
    fmt_xo(fa, f1, sizeof f1);
    fmt_xo(fb, f2, sizeof f2);
    float rowsTop = y;
    for (int b = 0; b < nb; ++b) {
        if (nb == 1) snprintf(tl, sizeof tl, "Full band");
        else if (b == 0) snprintf(tl, sizeof tl, "Low  < %s", f1);
        else if (b == nb - 1) snprintf(tl, sizeof tl, "High  > %s", nb == 2 ? f1 : f2);
        else snprintf(tl, sizeof tl, "Mid  %s \xE2\x80\x93 %s", f1, f2);
        float bx = x;
        R meter(r.r() - 14 - 120, y + 5, 120, 14);
        if (ui.drawS()) {
            ui.textFit(R(bx, y, 146, 24), tl, on ? th::text() : th::faint(), F_MEDIUM, 12);
            bx += 152;
            // Attack and Sustain share what is left before the meter
            float cw = (meter.x - 44 - bx - 22) * 0.5f, bw = cw - 56 - 6 - 66;
            for (int k = 0; k < 2; ++k) {
                int id = (k == 0 ? P_TSATT : P_TSSUS) + 2 * b;
                label(ui, R(bx, y, 56, 24), k == 0 ? "Attack" : "Sustain", on);
                ui.bar(R(bx + 56, y + 1, bw, 22), id, SC_LIN, true, KD_OTHER, true);
                ui.valueBox(R(bx + 56 + bw + 6, y + 1, 66, 22), id, true, KD_OTHER, 1, 0.1);
                bx += cw + 22;
            }
            ui.text(R(meter.x - 34, y, 30, 24), "now", th::faint(), F_REGULAR, 11, 2);
            ui.fill(meter, Paint(th::well()), 3);
            ui.stroke(meter, Paint(th::border()), 3);
            ui.fill(R(meter.x + meter.w * 0.5f - 0.5f, meter.y + 1, 1, meter.h - 2), Paint(th::borderHi()));
        }
        if (ui.drawD()) {
            // what the band is doing now: the most it went up (orange) or down (purple) lately, +-12 dB
            double d = (on && av.myslot >= 0 && av.live(av.myslot)) ? std::max(-12.0, std::min(12.0, P.md[b].load())) : 0;
            float half = meter.w * 0.5f - 2, cxm = meter.x + meter.w * 0.5f;
            float w = (float)(std::fabs(d) / 12) * half;
            if (w > 0.4f) {
                R bar = d >= 0 ? R(cxm, meter.y + 3, w, meter.h - 6) : R(cxm - w, meter.y + 3, w, meter.h - 6);
                ui.fill(bar, Paint(d >= 0 ? th::orange() : th::purple()), 2);
            }
        }
        y += 30;
    }
    // switched off: the bands stay where they are (editable), dimmed
    if (!on && ui.drawS()) ui.fill(R(r.x + 1, rowsTop - 4, r.w - 2, y - rowsTop + 2), Paint(th::bg().alpha(0.45f)));
}

void ActWin::frame(Ui &ui)
{
    av.refresh();
    const float H = height();
    if (ui.drawS()) ui.fill(R(0, 0, DW, H), Paint::vertical(0, th::bg(), H, th::bg2()));
    // the subtitle: what this Act measures (OUT: the plug-in before it), the track, the latency
    std::string sub;
    if (av.myslot >= 0) {
        sub = av.pointTitle(av.myslot);
        std::string tn = av.trackName(av.myslot);
        if (!tn.empty()) sub += "   \xC2\xB7   " + tn;
    }
    int lat = P.latencyNow();
    char lt[64];
    if (lat > 0) snprintf(lt, sizeof lt, "latency %.1f ms (shaper lookahead)", lat * 1000.0 / P.sampleRate());
    else snprintf(lt, sizeof lt, "no latency");
    if (!sub.empty()) sub += "   \xC2\xB7   ";
    sub += lt;
    st.subtitle = sub;

    R gear = gearRect(DW);
    if (st.settings) ui.block(settingsRect(gear, SW, SH));
    topBar(ui, P, av, st, DW, "TWOSTORMS", "ACT", false, 0);
    if (st.settings) ui.block(settingsRect(gear, SW, SH));
    float y = 66;
    drawShaper(ui, R(M, y, DW - 2 * M, shaperH()));
    y += shaperH() + 12;
    float ch = actCardHeight(av);
    actCard(ui, P, av, R(M, y, DW - 2 * M, ch));
    y += ch + 12;
    if (ui.drawS()) {
        ui.textFit(R(M + 4, y, DW - 2 * M - 8, 14),
                   "The transient shaper works before the gain, on each gain group: Attack lifts or softens hits, Sustain what rings after them.",
                   th::faint(), F_REGULAR, 11);
        ui.textFit(R(M + 4, y + 16, DW - 2 * M - 8, 14),
                   "Off, it costs nothing. Lookahead delays the audio by its time (reported to the host) so the shaping lands with the hit.",
                   th::faint(), F_REGULAR, 11);
    }
    settings(ui, gear);
}

// ---------------------------------------------------------------- the kind
static const char *s_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, CLAP_PLUGIN_FEATURE_ANALYZER,
                                   CLAP_PLUGIN_FEATURE_SURROUND, nullptr};

const PluginKind &kind()
{
    static const PluginKind k = {
        {CLAP_VERSION_INIT, "com.twostorms.act", "TwoStorms Act", "TwoStorms", "", "", "", TS_VERSION,
         "A gain stage to drop after any plug-in: up to 8 channels in linked pairs, meters, Auto (Target, Match, Reference), "
         "drive links, the finale, and an optional transient shaper. TwoStorms Gain Stage lists and remote-controls every Act.",
         s_features},
        &table,
        &noPresets,
        "Act",
        "TWOSTORMS ACT",
        shm::KIND_POINT,
        shm::SK_ACT,
        false,
        false,
        0,
        &create,
        8,
    };
    return k;
}

} // namespace actp
} // namespace ts
