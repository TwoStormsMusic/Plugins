// TwoStorms Compressor: the CLAP plug-in (engine + built-in Act) and its window.
#include "comp_plugin.h"
#include "../../core/act_view.h"
#include "../../core/clock.h"
#include "../../core/eel.h"
#include "../../core/plugin.h"
#include "../../gui/analyzer.h"
#include "../../gui/common.h"
#include "../../gui/editor.h"
#include "comp_engine.h"
#include "comp_params.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ts {
namespace comp {

using namespace gui;

// ============================================================================ the plug-in
class CompressorPlugin : public Plugin, public EngineHost {
public:
    explicit CompressorPlugin(const clap_host *h) : Plugin(h, kind()), eng(*this, act, uiLink, view) {}
    UiLink uiLink;
    Display view;
    Engine eng;

    // EngineHost and ActHost
    double pv(int id) const override { return P.get(id); }
    double abv(int id) const override { return abValue(id); }
    void setFromAudio(int id, double v) override { Plugin::setFromAudio(id, v); }
    // the Gain Stage's Set row sets the Target GR, its type and the listening time (69..71)
    int hkRset(int x) override { return (x == P_TGR || x == P_TTYPE || x == P_TTIME) ? x : 0; }
    void hkTgStart() override { act.tgs = 1; }
    void hkPublish(shm::Slot &b) override
    {
        b.p[P_TGR].store(P.get(P_TGR), std::memory_order_relaxed);
        b.p[P_TTYPE].store(P.get(P_TTYPE), std::memory_order_relaxed);
        b.p[P_TTIME].store(P.get(P_TTIME), std::memory_order_relaxed);
    }
    gui::View *createView() override;

protected:
    void dspActivate(double sr, uint32_t) override { eng.init(sr); }
    void dspReset() override { eng.reset(); }
    void dspProcess(const double *const *in, const double *const *sc, double *const *out, int n) override
    {
        eng.block(n);
        act.block(n);
        eng.samples(in, sc, out, n);
        act.process(out, 2, n);
        eng.post(out, n);
        act.audioKey(out, 2, n);
    }
    int latencyFromParams(double sr) const override { return Engine::latencyFor(P.get(P_LA), P.get(P_LAMS), sr); }
    void stateSaveExtra(std::string &s) override
    {
        char b[64];
        snprintf(b, sizeof b, "x mkdb %.17g\n", view.mkdb.load());
        s += b;
    }
    void stateLoadExtra(const std::string &k, const std::string &v) override
    {
        if (k == "mkdb") eng.c_mkdb = atof(v.c_str());
    }
};

// ============================================================================ the window
static const char *const L_CHAR[] = {"Custom", "Clean VCA", "Opto", "FET", "Vari-mu"};
static const char *const L_DET[] = {"Peak", "RMS", "Hybrid"};
static const char *const L_MODE[] = {"Mono", "Stereo", "Mid/Side", "Left/Right"};
static const char *const L_ENV[] = {"Raw", "Smooth"};
static const char *const L_RELM[] = {"Classic", "Program", "2-stage"};
static const char *const L_CURVE[] = {"Standard", "Soft", "Opto-like"};
static const char *const L_TTYPE[] = {"Peak GR", "Average GR"};
static const char *const L_TTIME[] = {"1 s", "2 s", "3 s", "5 s", "10 s", "20 s", "30 s"};
static const char *const L_HOLD[] = {"Off", "1 s", "2 s", "5 s", "10 s", "Infinite"};
static const char *const L_RANGE[] = {"24 dB", "36 dB", "48 dB", "60 dB", "72 dB", "96 dB"};
static const char *const L_ZOOM[] = {"100%", "115%", "130%", "150%", "175%", "200%"};
static const char *const L_FIN[] = {"Auto", "Always", "Never"};
static const char *const L_LKL[] = {"Max", "Average (power)"};
static const char *const L_RCMB[] = {"Loudest", "Combined"};
static const char *const L_AMKM[] = {"Average GR", "Static"};
static const char *const L_SCHS[] = {"6 dB/oct", "12 dB/oct", "24 dB/oct"};
static const char *const L_SATT[] = {"Soft", "Warm"};
static const char *const L_SC[] = {"Main input", "Sidechain (3-4)"};
static const char *const L_BASE[] = {"Clean VCA", "Opto", "FET", "Vari-mu"};

// what each Character does (shown in the row Custom uses, so nothing jumps when it changes)
static const char *const CHAR_DESC[] = {
    "",
    "Clean VCA:  standard knee \xC2\xB7 program-dependent release \xC2\xB7 feed-forward \xC2\xB7 no saturation",
    "Opto:  opto-like progressive curve \xC2\xB7 two-stage release \xC2\xB7 50% feedback \xC2\xB7 15% soft saturation",
    "FET:  standard knee \xC2\xB7 program-dependent release \xC2\xB7 60% feedback \xC2\xB7 35% soft saturation",
    "Vari-mu:  soft curve \xC2\xB7 program-dependent release \xC2\xB7 40% feedback \xC2\xB7 30% warm saturation",
};

class CompView : public gui::View {
public:
    explicit CompView(CompressorPlugin &p) : P(p), av(p) {}
    void designSize(float &w, float &h) override { w = DW; h = DH; }
    void frame(Ui &ui) override;
    bool wantsFull() override { return st.settings; }
    uint64_t statusHash() override;
    void opened() override
    {
        spi.reset();
        spo.reset();
    }

private:
    static constexpr float DW = 940, DH = 748, M = 14, SW = 660, SH = 272;
    CompressorPlugin &P;
    ActView av;
    CommonState st;
    bool live = false;
    // analyzer
    gui::Spectrum spi, spo;
    R an;
    int rdrag = 0;
    double rdx = 0, rlo = 0, rhi = 0, anclk = 0;
    // Ctrl + Input moves the output the other way
    double inLast = 1e9;
    bool iol = false;

    void enumDD(Ui &ui, const R &r, int id, const char *const *labels, int n, const char *prefix = nullptr, bool en = true)
    {
        int cv = iclamp(iround(P.get(id)), 0, n - 1);
        if (ui.dropdown(r, labels[cv], en, prefix)) {
            std::vector<MenuItem> m;
            for (int i = 0; i < n; ++i) m.push_back(MenuItem::item(labels[i], i + 1, i == cv));
            CompressorPlugin *pp = &P;
            ui.menu(m, r.x, r.b() + 4, [pp, id](int r2) { pp->setOnce(id, r2 - 1); }, r.w);
        }
    }
    void toggle(Ui &ui, const R &r, const char *label, int id, int style, bool momentary)
    {
        bool on = P.get(id) >= 0.5;
        if (ui.button(r, label, on, true, style)) {
            if (momentary && (ui.mods & M_SHIFT)) { ui.momKey = id; ui.momPrev = on ? 1 : 0; }
            P.setOnce(id, on ? 0 : 1);
        }
    }
    void label(Ui &ui, const R &r, const char *t, int align = 0) { ui.text(r, t, th::dim(), F_REGULAR, 12, align); }
    void card(Ui &ui, const R &r)
    {
        ui.shadow(r, 8, 12, Col(0, 0, 0, 0.45f));
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x151417), r.b(), Col::hex(0x101012)), 8);
        ui.stroke(r, Paint(th::border()), 8);
    }

    void charLoad(int c);
    double gcgr(double lv);
    void drawCurve(Ui &ui, const R &r);
    void drawHistory(Ui &ui, const R &r);
    void drawMeters(Ui &ui, const R &r);
    void drawAnalyzer(Ui &ui, const R &r);
    void analyzerInput(Ui &ui);
    void triggerRows(Ui &ui, float y);
    void controls(Ui &ui, float y);
    void characterRow(Ui &ui, float y);
    void customRow(Ui &ui, float y);
    void settings(Ui &ui, const R &gear);
    double anX(double f) const { return an.x + an.w * std::log(std::max(20.0, f) / 20) / std::log(1000.0); }
    double anF(double x) const { return 20 * std::pow(1000.0, (x - an.x) / an.w); }
    double anY(double db, double f) const
    {
        return an.y + an.h * std::min(1.0, std::max(0.0, -(db + 4.5 * std::log(f / 1000) / std::log(2.0)) / 90));
    }
};

gui::View *CompressorPlugin::createView() { return new CompView(*this); }

// what the still parts show besides parameters: when any of it changes, they are drawn again
uint64_t CompView::statusHash()
{
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ULL; };
    av.refresh();
    mix((uint64_t)(av.myslot + 1));
    mix(av.myslot >= 0 && av.live(av.myslot));
    mix((uint64_t)av.fin());
    if (av.myslot >= 0) {
        shm::Slot &b = shm::slot(av.myslot);
        mix((uint64_t)b.tgs.load());
        mix((uint64_t)(b.tgel.load() * 10));
        mix((uint64_t)b.astate.load());
        mix((uint64_t)(b.atime.load() * 10));
        mix((uint64_t)b.resseq.load());
        mix(b.result.load() == shm::R_NOSIG && av.now - b.donet.load() < 4);
        mix((uint64_t)b.nseq.load());
        mix((uint64_t)b.trackno.load());
    }
    mix((uint64_t)P.driveStatus());
    mix(now_sec() - P.presetMsgT < 4);
    mix((uint64_t)P.view.ld.load());
    mix((uint64_t)P.view.side.load());
    mix((uint64_t)st.holdNow() | (uint64_t)st.settings << 4 | (uint64_t)st.abm << 5);
    mix((uint64_t)rdrag);
    // other Acts in the chain (drive-link and Match names)
    mix((uint64_t)shm::get()->h.slotver.load());
    return h;
}

// load a Character's voicing (curve, release, feedback, saturation) into the editable parameters
void CompView::charLoad(int c)
{
    int cv = c == 2 ? 2 : (c == 4 ? 1 : 0), rm = c == 2 ? 2 : 1;
    double fbk = c == 2 ? 50 : (c == 3 ? 60 : (c == 4 ? 40 : 0));
    double sa = c == 2 ? 15 : (c == 3 ? 35 : (c == 4 ? 30 : 0));
    P.setOnce(P_CURVE, cv);
    P.setOnce(P_RELM, rm);
    P.setOnce(P_FB, fbk);
    P.setOnce(P_SAT, sa);
    P.setOnce(P_SATT, c == 4 ? 1 : 0);
}

// static gain reduction at input level lv (the curve and the soft Range, as the audio has them)
double CompView::gcgr(double lv)
{
    double kn = std::max(0.0001, P.get(P_KNEE));
    double sl = P.get(P_RAT) >= 99.5 ? -1 : 1 / std::max(1.0, P.get(P_RAT)) - 1;
    double nrg = P.get(P_RNG) >= 59.95 ? -200 : -P.get(P_RNG);
    int chr = iround(P.get(P_CHAR));
    int cvt = chr == 0 ? iround(P.get(P_CURVE)) : (chr == 2 ? 2 : (chr == 4 ? 1 : 0));
    return rs_soft(e_curve(lv - P.get(P_THR), kn, sl, cvt), nrg);
}

// ---------------------------------------------------------------- the curve
void CompView::drawCurve(Ui &ui, const R &r)
{
    Canvas &c = ui.cv();
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        for (int d = -48; d <= -12; d += 12) {
            float f = (d + 60) / 60.f;
            ui.fill(R(r.x + f * r.w, r.y, 1 / ui.s, r.h), Paint(th::grid()));
            ui.fill(R(r.x, r.b() - f * r.h, r.w, 1 / ui.s), Paint(th::grid()));
        }
        ui.line(r.x, r.b(), r.r(), r.y, th::gridHi(), 1);
        // threshold
        double thr = P.get(P_THR);
        float tx = r.x + (float)(thr + 60) / 60 * r.w;
        ui.fill(R(tx - 0.5f, r.y, 1, r.h), Paint(th::purple().alpha(0.85f)));
        char tb[32];
        snprintf(tb, sizeof tb, "%.1f", thr);
        float tw = ui.textW(F_MEDIUM, 10, tb);
        float lx = std::min(std::max(r.x + 4, tx - tw * 0.5f), r.r() - tw - 4);
        ui.text(R(lx, r.y + 4, tw, 12), tb, th::purpleHi(), F_MEDIUM, 10, 0);
        // the curve
        std::vector<Pt> pts;
        int n = 120;
        for (int i = 0; i <= n; ++i) {
            double in = -60 + i * 60.0 / n, out = in + gcgr(in);
            float x = r.x + (float)i / n * r.w, y = r.b() - (float)(std::max(-60.0, out) + 60) / 60 * r.h;
            pts.push_back({ui.X(x), ui.Y(y)});
        }
        c.pushClip(ui.X(r.x), ui.Y(r.y), ui.W(r.w), ui.W(r.h));
        c.strokePolyline(pts.data(), (int)pts.size(), ui.W(6), Paint(th::orange().alpha(0.10f)));
        c.strokePolyline(pts.data(), (int)pts.size(), ui.W(2.2f), Paint(th::orange()));
        c.popClip();
        ui.stroke(r, Paint(th::border()), 6);
        ui.text(R(r.r() - 167, r.b() - 17, 160, 12), "drag across: threshold", th::faint(), F_REGULAR, 10, 2);
        ui.dragArea(r, P_THR, 60 / r.w, 6 / r.w);
    }
    // where the signal is now
    double dlv = P.view.dlv.load();
    if (ui.drawD() && dlv > -60 && live) {
        double in = std::min(0.0, dlv), out = std::min(0.0, in + P.view.ugr.load());
        float x = r.x + (float)(in + 60) / 60 * r.w, y = r.b() - (float)(std::max(-60.0, out) + 60) / 60 * r.h;
        c.pushClip(ui.X(r.x), ui.Y(r.y), ui.W(r.w), ui.W(r.h));
        c.fillCircle(ui.X(x), ui.Y(y), ui.W(7), Paint(Col(1, 1, 1, 0.12f)));
        c.fillCircle(ui.X(x), ui.Y(y), ui.W(3.2f), Paint(Col(1, 1, 1)));
        c.popClip();
    }
}

// ---------------------------------------------------------------- history
void CompView::drawHistory(Ui &ui, const R &r)
{
    Canvas &c = ui.cv();
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        for (int d = -12; d >= -48; d -= 12) ui.fill(R(r.x, r.y + r.h * (-d / 60.f), r.w, 1 / ui.s), Paint(th::grid()));
        ui.stroke(r, Paint(th::border()), 6);
    }
    if (!ui.drawD()) return;
    int hp = P.view.hp.load(std::memory_order_acquire);
    R in(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    c.pushClip(ui.X(in.x), ui.Y(in.y), ui.W(in.w), ui.W(in.h));
    // one entry per pixel column (10 ms each), the newest at the right
    int px0 = (int)std::floor(ui.X(in.x)), px1 = (int)std::ceil(ui.X(in.r()));
    int np = px1 - px0;
    float per = 1 / ui.s;   // entries per pixel
    int n = (int)std::ceil(np * per) + 2;
    auto ent = [&](int k) { return (hp - n + k) & (Display::HN - 1); };
    std::vector<float> tops(np), grb(np);
    std::vector<Pt> out, gr;
    out.reserve(np);
    gr.reserve(np);
    float y0 = ui.Y(in.y), H = ui.W(in.h), yb = ui.Y(in.b());
    auto flush = [&]() {
        if (out.size() > 1) c.strokeThin(out.data(), (int)out.size(), ui.W(1.6f), th::orange());
        out.clear();
    };
    for (int i = 0; i < np; ++i) {
        // the entry under this pixel column
        int k = n - 1 - (int)((np - 1 - i) * per);
        int e = ent(k);
        float v = P.view.hin[e].load(std::memory_order_relaxed), o = P.view.hout[e].load(std::memory_order_relaxed);
        float g = P.view.hgr[e].load(std::memory_order_relaxed);
        tops[i] = v > -60 ? y0 + H * std::min(1.f, -v / 60) : yb;
        if (o <= -150) { grb[i] = y0; flush(); continue; }
        float yg = y0 + H * std::min(1.f, -g / 24);
        grb[i] = yg;
        gr.push_back({(float)(px0 + i) + 0.5f, yg});
        out.push_back({(float)(px0 + i) + 0.5f, y0 + H * std::min(1.f, -std::max(-60.f, o) / 60)});
    }
    c.columns(px0, tops.data(), np, yb, Paint::vertical(y0, Col::hex(0x47434f), yb, Col::hex(0x2c2a32)));
    c.columnsDown(px0, grb.data(), np, y0, Paint::vertical(y0, th::red().alpha(0.26f), y0 + H * 0.6f, th::red().alpha(0.03f)));
    if (gr.size() > 1) c.strokeThin(gr.data(), (int)gr.size(), ui.W(1.5f), th::red());
    flush();
    c.popClip();
    float lx = r.x + 10, ly = r.y + 7;
    auto key = [&](const char *t, const Col &col) {
        ui.fill(R(lx, ly + 4, 8, 3), Paint(col), 1.5f);
        ui.text(R(lx + 12, ly, 200, 11), t, th::dim(), F_REGULAR, 10.5f);
        lx += 12 + ui.textW(F_REGULAR, 10.5f, t) + 14;
    };
    key("input", Col::hex(0x6d6875));
    key("output", th::orange());
    key("gain reduction (0 to 24 dB, from the top)", th::red());
}

// ---------------------------------------------------------------- meters: in, gain reduction, out
void CompView::drawMeters(Ui &ui, const R &r)
{
    double range = av.meterRange();
    float top = r.y + 22, h = r.h - 22 - 24;
    float x = r.x + 10, gx = x + 34, ox = gx + 26;
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        ui.stroke(r, Paint(th::border()), 6);
        ui.text(R(x, r.y + 6, 22, 12), "IN", th::faint(), F_SEMIBOLD, 10, 1);
        ui.text(R(gx - 4, r.y + 6, 22, 12), "GR", th::faint(), F_SEMIBOLD, 10, 1);
        ui.text(R(ox, r.y + 6, 22, 12), "OUT", th::faint(), F_SEMIBOLD, 10, 1);
    }
    if (!ui.drawD()) return;
    vMeter(ui, R(x, top, 10, h), P.view.mi0.load(), live, range);
    vMeter(ui, R(x + 12, top, 10, h), P.view.mi1.load(), live, range);
    double ugr = live ? P.view.ugr.load() : 0;
    grMeter(ui, R(gx, top, 14, h), ugr);
    double o0 = -200, o1 = -200, h0 = -200, h1 = -200;
    if (av.myslot >= 0) {
        shm::Slot &b = shm::slot(av.myslot);
        o0 = b.pkd[0].load(); o1 = b.pkd[1].load(); h0 = b.pkh[0].load(); h1 = b.pkh[1].load();
    }
    vMeter(ui, R(ox, top, 10, h), o0, live, range, h0);
    vMeter(ui, R(ox + 12, top, 10, h), o1, live, range, h1);
    char b[16];
    snprintf(b, sizeof b, "%.1f", -ugr);
    ui.text(R(gx - 14, r.b() - 21, 42, 16), b, ugr < -0.05 ? th::red() : th::faint(), F_SEMIBOLD, 11.5f, 1, true);
}

// ---------------------------------------------------------------- analyzer
void CompView::drawAnalyzer(Ui &ui, const R &r)
{
    an = r;
    Canvas &c = ui.cv();
    static const int fr[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000};
    if (ui.drawS()) {
        ui.fill(r, Paint::vertical(r.y, Col::hex(0x0b0b0d), r.b(), Col::hex(0x070708)), 6);
        for (int f : fr) ui.fill(R((float)anX(f), r.y, 1 / ui.s, r.h), Paint(th::grid()));
        ui.stroke(r, Paint(th::border()), 6);
        analyzerInput(ui);
    }
    if (!ui.drawD()) return;
    double sr = P.sampleRate();
    R in(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
    c.pushClip(ui.X(in.x), ui.Y(in.y), ui.W(in.w), ui.W(in.h));
    if (live) {
        // the shared analyzer: an FFT of about the newest 170 ms, the lows drawn as a smooth line
        int ap = P.view.ap.load(std::memory_order_acquire);
        spi.update(P.view.ani, Display::AN - 1, ap, ui.dt, sr);
        spo.update(P.view.ano, Display::AN - 1, ap, ui.dt, sr);
        gui::FreqScale fs(r);
        Paint fill = Paint::vertical(ui.Y(r.y), Col::hex(0x5a4f78), ui.Y(r.b()), Col::hex(0x231f2e));
        Col line = th::orange();
        gui::drawSpectrum(ui, fs, spi, sr, &fill, nullptr);
        gui::drawSpectrum(ui, fs, spo, sr, nullptr, &line);
    }
    // the trigger ranges
    int nr = 0;
    for (int k = 0; k < 4; ++k) {
        if (P.get(P_R1 + 3 * k) < 0.5) continue;
        nr++;
        double lo = std::min(P.get(P_R1 + 3 * k + 1), P.get(P_R1 + 3 * k + 2)), hi = std::max(P.get(P_R1 + 3 * k + 1), P.get(P_R1 + 3 * k + 2));
        float x1 = (float)anX(lo), x2 = (float)anX(hi);
        bool hot = rdrag > 0 && rdrag / 4 == k;
        ui.fill(R(x1, r.y, x2 - x1, r.h), Paint::vertical(r.y, th::purple().alpha(hot ? 0.30f : 0.20f), r.b(), th::purple().alpha(0.06f)));
        ui.fill(R(x1 - 0.75f, r.y, 1.5f, r.h), Paint(th::purple()));
        ui.fill(R(x2 - 0.75f, r.y, 1.5f, r.h), Paint(th::purple()));
        char b2[8];
        snprintf(b2, sizeof b2, "%d", k + 1);
        ui.fill(R(x1 + 4, r.y + 4, 14, 14), Paint(th::purple()), 7);
        ui.text(R(x1 + 4, r.y + 4, 14, 14), b2, Col(1, 1, 1), F_BOLD, 9.5f, 1);
    }
    c.popClip();
    for (int f : fr) {
        char b2[16];
        if (f >= 1000) snprintf(b2, sizeof b2, "%dk", f / 1000);
        else snprintf(b2, sizeof b2, "%d", f);
        ui.text(R((float)anX(f) + 3, r.b() - 14, 30, 11), b2, th::faint(), F_REGULAR, 10);
    }
    const char *hint = nr ? (P.get(P_RCMB) >= 0.5 ? "Trigger: the purple ranges (they combine). Drag an edge or a range; right-click to remove."
                                                   : "Trigger: the purple ranges (the loudest one counts). Drag an edge or a range; right-click to remove.")
                          : "Trigger: the whole signal. Double-click here to add a frequency range.";
    ui.text(R(r.r() - 10 - 560, r.y + 6, 560, 12), hint, th::faint(), F_REGULAR, 10.5f, 2);
}

// drag a range's edge or the whole range; double-click to add one; right-click to remove
void CompView::analyzerInput(Ui &ui)
{
    float mxd = ui.mx();
    bool inan = ui.hit(an);
    auto hitRange = [&](int &edge) {
        int hr = -1;
        edge = 0;
        for (int k = 0; k < 4; ++k) {
            if (P.get(P_R1 + 3 * k) < 0.5) continue;
            double lo = std::min(P.get(P_R1 + 3 * k + 1), P.get(P_R1 + 3 * k + 2)), hi = std::max(P.get(P_R1 + 3 * k + 1), P.get(P_R1 + 3 * k + 2));
            float x1 = (float)anX(lo), x2 = (float)anX(hi);
            if (std::fabs(mxd - x1) < 5) { hr = k; edge = 1; }
            else if (std::fabs(mxd - x2) < 5) { hr = k; edge = 2; }
            else if (mxd > x1 && mxd < x2 && edge == 0) { hr = k; edge = 3; }
        }
        return hr;
    };
    if (inan && !rdrag) {
        int e;
        int hr = hitRange(e);
        if (hr >= 0) ui.cursor = e == 3 ? CUR_MOVE : CUR_HRESIZE;
    }
    if (inan && (ui.lclick || ui.rclick) && !ui.used) {
        ui.used = true;
        int e;
        int hr = hitRange(e);
        if (ui.rclick) {
            if (hr >= 0) {
                CompressorPlugin *pp = &P;
                ui.menu({MenuItem::item("Remove this range", 1)}, ui.mx(), ui.my(), [pp, hr](int r) {
                    if (r == 1) pp->setOnce(P_R1 + 3 * hr, 0);
                });
            }
        } else if (hr < 0 && ui.now - anclk < 0.35) {
            // double-click: a new range, one octave around here
            int frr = -1;
            for (int k = 0; k < 4 && frr < 0; ++k)
                if (P.get(P_R1 + 3 * k) < 0.5) frr = k;
            if (frr >= 0) {
                double f = anF(mxd);
                P.setOnce(P_R1 + 3 * frr + 1, std::max(20.0, f / std::sqrt(2.0)));
                P.setOnce(P_R1 + 3 * frr + 2, std::min(20000.0, f * std::sqrt(2.0)));
                P.setOnce(P_R1 + 3 * frr, 1);
            }
            anclk = 0;
        } else {
            anclk = ui.now;
            if (hr >= 0) {
                rdrag = hr * 4 + e;
                rdx = mxd;
                rlo = std::min(P.get(P_R1 + 3 * hr + 1), P.get(P_R1 + 3 * hr + 2));
                rhi = std::max(P.get(P_R1 + 3 * hr + 1), P.get(P_R1 + 3 * hr + 2));
                P.beginEdit(P_R1 + 3 * hr + 1);
                P.beginEdit(P_R1 + 3 * hr + 2);
                // a range kept low/high swapped is put in order first
                P.edit(P_R1 + 3 * hr + 1, rlo);
                P.edit(P_R1 + 3 * hr + 2, rhi);
            }
        }
    }
    if (rdrag > 0 && ui.ldown) {
        int k = rdrag / 4, e = rdrag - k * 4;
        double f = std::max(20.0, std::min(20000.0, anF(mxd)));
        ui.cursor = e == 3 ? CUR_MOVE : CUR_HRESIZE;
        if (e == 1) P.edit(P_R1 + 3 * k + 1, std::min(f, rhi / 1.05));
        if (e == 2) P.edit(P_R1 + 3 * k + 2, std::max(f, rlo * 1.05));
        if (e == 3) {
            double t = anF(mxd) / anF(rdx);
            t = std::max(20 / rlo, std::min(20000 / rhi, t));
            P.edit(P_R1 + 3 * k + 1, rlo * t);
            P.edit(P_R1 + 3 * k + 2, rhi * t);
        }
    }
    if (rdrag > 0 && !ui.ldown) {
        int k = rdrag / 4;
        P.endEdit(P_R1 + 3 * k + 1);
        P.endEdit(P_R1 + 3 * k + 2);
        rdrag = 0;
    }
}

// ---------------------------------------------------------------- trigger rows
void CompView::triggerRows(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float h = 26;
    label(ui, R(M, y, 54, h), "Trigger");
    enumDD(ui, R(M + 54, y, 150, h), P_SC, L_SC, 2);
    float x = M + 54 + 156;
    enumDD(ui, R(x, y, 128, h), P_DET, L_DET, 3, "Detect");
    x += 134;
    if (P.get(P_DET) >= 0.5) {
        ui.valueBox(R(x, y, 76, h), P_RMST, true, KD_LOG, std::log(2000000.0) / 140, std::log(2000000.0) / 1400);
        x += 82;
    }
    int frr = -1, nr = 0;
    for (int k = 0; k < 4; ++k) {
        if (P.get(P_R1 + 3 * k) < 0.5) { if (frr < 0) frr = k; }
        else nr++;
    }
    if (ui.button(R(x, y, 84, h), "Range", false, frr >= 0, BS_NORMAL, IC_PLUS)) P.setOnce(P_R1 + 3 * frr, 1);
    x += 90;
    enumDD(ui, R(x, y, 160, h), P_CMODE, L_MODE, 4, "Mode");
    x += 166;
    if (P.get(P_CMODE) >= 0.5) {
        label(ui, R(x, y, 32, h), "Link");
        ui.valueBox(R(x + 32, y, 70, h), P_SLINK, true, KD_OTHER, 1, 0.1);
    }
    toggle(ui, R(DW - M - 84, y, 84, h), "Listen", P_LISTEN, BS_PURPLE, true);
    toggle(ui, R(DW - M - 84 - 6 - 76, y, 76, h), "Delta", P_DELTA, BS_PURPLE, true);

    // the ranges (this row is always there, so nothing below moves when one is added)
    float y2 = y + h + 6;
    if (nr == 0) {
        ui.text(R(M, y2, 800, 24), "No trigger ranges: the whole signal drives the detector. Add one with + Range, or double-click the analyzer.",
                th::faint(), F_REGULAR, 11);
        return;
    }
    label(ui, R(M, y2, 54, 24), "Ranges");
    float rx = M + 54;
    for (int k = 0; k < 4; ++k) {
        if (P.get(P_R1 + 3 * k) < 0.5) continue;
        char b[8];
        snprintf(b, sizeof b, "%d", k + 1);
        ui.fill(R(rx, y2 + 5, 14, 14), Paint(th::purple()), 7);
        ui.text(R(rx, y2 + 5, 14, 14), b, Col(1, 1, 1), F_BOLD, 9.5f, 1);
        ui.valueBox(R(rx + 20, y2, 68, 24), P_R1 + 3 * k + 1, true, KD_LOG, std::log(1000.0) / 140, std::log(1000.0) / 1400);
        ui.text(R(rx + 88, y2, 12, 24), "\xE2\x80\x93", th::faint(), F_REGULAR, 12, 1);
        ui.valueBox(R(rx + 100, y2, 68, 24), P_R1 + 3 * k + 2, true, KD_LOG, std::log(1000.0) / 140, std::log(1000.0) / 1400);
        if (ui.button(R(rx + 172, y2, 24, 24), "", false, true, BS_GHOST, IC_X)) P.setOnce(P_R1 + 3 * k, 0);
        rx += 212;
    }
}

// ---------------------------------------------------------------- the dynamics controls
void CompView::controls(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float cw = (DW - 2 * M - 2 * 20) / 3, h = 26, dy = 32, lw = 72, bw = 84;
    float x1 = M, x2 = M + cw + 20, x3 = M + 2 * (cw + 20);
    // Ctrl + Input: its change moves the output the other way (Shift is the fine adjustment there)
    R inRow(x3, y, cw, h);
    double vin = P.get(P_IN);
    if (iol && inLast < 1e8 && vin != inLast && P.get(P_GAIN) > GFLOOR)
        P.apply(P_GAIN, std::max(-99.9, std::min(60.0, P.get(P_GAIN) - (vin - inLast))));
    inLast = vin;
    bool inAct = ui.dragKey == P_IN || ui.hit(inRow);
    iol = inAct && (ui.mods & M_CTRL);
    bool fineWas = ui.fine;
    ui.paramRow(R(x1, y, cw, h), "Threshold", P_THR, SC_LIN, lw, bw);
    ui.paramRow(R(x1, y + dy, cw, h), "Ratio", P_RAT, SC_LOG, lw, bw);
    ui.paramRow(R(x1, y + 2 * dy, cw, h), "Knee", P_KNEE, SC_LIN, lw, bw);
    ui.paramRow(R(x2, y, cw, h), "Attack", P_ATT, SC_LOG, lw, bw);
    ui.paramRow(R(x2, y + dy, cw, h), "Hold", P_HLD, SC_LIN, lw, bw);
    ui.paramRow(R(x2, y + 2 * dy, cw, h), "Release", P_REL, SC_LOG, lw, bw);
    if (inAct) ui.fine = (ui.mods & M_SHIFT) != 0;
    ui.paramRow(inRow, "Input", P_IN, SC_LIN, lw, bw);
    ui.fine = fineWas;
    ui.paramRow(R(x3, y + dy, cw, h), "Range", P_RNG, SC_LIN, lw, bw);
    ui.paramRow(R(x3, y + 2 * dy, cw, h), "Dry/wet", P_MIX, SC_LIN, lw, bw);
}

// ---------------------------------------------------------------- Character, makeup, lookahead, Set threshold
void CompView::characterRow(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float h = 26;
    int cv = iclamp(iround(P.get(P_CHAR)), 0, 4);
    R cr(M, y, 182, h);
    if (ui.dropdown(cr, L_CHAR[cv], true, "Character")) {
        // the four voicings, then Custom last; choosing Custom keeps the voicing you were on as its base
        std::vector<MenuItem> m;
        for (int i = 1; i <= 5; ++i) {
            int v = i % 5;
            if (v == 0) m.push_back(MenuItem::sep());
            m.push_back(MenuItem::item(L_CHAR[v], v + 1, v == cv));
        }
        CompView *self = this;
        ui.menu(m, cr.x, cr.b() + 4, [self, cv](int r) {
            int nv = r - 1;
            if (nv == 0 && cv > 0) { self->P.setOnce(P_BASE, cv - 1); self->charLoad(cv); }
            self->P.setOnce(P_CHAR, nv);
        }, cr.w);
    }
    toggle(ui, R(M + 188, y, 104, h), "Auto makeup", P_AMK, BS_NORMAL, true);
    toggle(ui, R(M + 298, y, 92, h), "Lookahead", P_LA, BS_NORMAL, false);
    if (P.get(P_LA) >= 0.5) ui.valueBox(R(M + 396, y, 66, h), P_LAMS, true, KD_OTHER, 0.1, 0.01);
    // Set threshold, hugging the right edge, with what it aims for to its left
    int tgs = av.myslot >= 0 ? (int)shm::slot(av.myslot).tgs.load() : 0;
    R set(DW - M - 118, y, 118, h);
    if (ui.button(set, tgs > 0 ? "Cancel" : "Set threshold", tgs > 0, av.myslot >= 0))
        P.act.send(tgs > 0 ? shm::C_TGSTOP : shm::C_TGSTART, 0);
    R tim(set.x - 6 - 66, y, 66, h);
    if (tgs > 0) {
        char b[32];
        if (tgs == 1) snprintf(b, sizeof b, "waiting");
        else if (tgs == 2) snprintf(b, sizeof b, "%.1f s", shm::slot(av.myslot).tgel.load());
        else snprintf(b, sizeof b, "working");
        ui.button(tim, b, true);
    } else {
        enumDD(ui, tim, P_TTIME, L_TTIME, 7);
    }
    R typ(tim.x - 6 - 104, y, 104, h);
    enumDD(ui, typ, P_TTYPE, L_TTYPE, 2);
    R tb(typ.x - 6 - 138, y, 138, h);
    ui.valueBox(tb, P_TGR, true, KD_OTHER, 0.1, 0.01, 2, nullptr, "Target GR");
}

// ---------------------------------------------------------------- Custom: base, envelope, release, curve, feedback, saturation
void CompView::customRow(Ui &ui, float y)
{
    if (!ui.drawS()) return;
    const float h = 26;
    int cv = iclamp(iround(P.get(P_CHAR)), 0, 4);
    if (cv != 0) {
        ui.fill(R(M, y, DW - 2 * M, h), Paint(Col::hex(0x0f0f11)), 5);
        ui.stroke(R(M, y, DW - 2 * M, h), Paint(th::border().alpha(0.6f)), 5);
        ui.text(R(M + 12, y, DW - 2 * M - 24, h), CHAR_DESC[cv], th::faint(), F_REGULAR, 11.5f, 0);
        ui.text(R(M + 12, y, DW - 2 * M - 24, h), "choose Custom to shape it", th::ghost().lighter(0.25f), F_REGULAR, 11, 2);
        return;
    }
    int b = iclamp(iround(P.get(P_BASE)), 0, 3);
    R br(M, y, 136, h);
    if (ui.dropdown(br, L_BASE[b], true, "Base")) {
        std::vector<MenuItem> m;
        for (int i = 0; i < 4; ++i) m.push_back(MenuItem::item(L_BASE[i], i + 1, i == b));
        CompView *self = this;
        ui.menu(m, br.x, br.b() + 4, [self](int r) { self->P.setOnce(P_BASE, r - 1); self->charLoad(r); }, br.w);
    }
    enumDD(ui, R(M + 142, y, 134, h), P_ENV, L_ENV, 2, "Envelope");
    enumDD(ui, R(M + 282, y, 130, h), P_RELM, L_RELM, 3, "Release");
    enumDD(ui, R(M + 418, y, 130, h), P_CURVE, L_CURVE, 3, "Curve");
    // feedback can't look ahead: hidden while lookahead is on
    if (P.get(P_LA) < 0.5) {
        label(ui, R(M + 560, y, 58, h), "Feedback");
        ui.valueBox(R(M + 618, y, 54, h), P_FB, true, KD_OTHER, 1, 0.1);
    }
    label(ui, R(M + 684, y, 66, h), "Saturation");
    ui.valueBox(R(M + 750, y, 52, h), P_SAT, true, KD_OTHER, 1, 0.1);
    enumDD(ui, R(M + 808, y, DW - M - (M + 808), h), P_SATT, L_SATT, 2);
}

// ---------------------------------------------------------------- Settings popover
void CompView::settings(Ui &ui, const R &gear)
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
    label(ui, R(in.r() - 86 - 48, y, 48, h), "Finale");
    enumDD(ui, R(in.r() - 86, y, 86, h), P_FINAL, L_FIN, 3);
    y += h + 6;
    bool warn;
    std::string fs = av.finaleStatus(&warn);
    ui.text(R(in.x, y, in.w, 16), fs.c_str(), warn ? th::red() : (av.fin() ? th::orange() : th::dim()), F_REGULAR, 11, 2);
    y += 22;
    ui.text(R(in.x, y, 200, 14), "DETECTOR", th::faint(), F_SEMIBOLD, 10.5f);
    y += 20;
    x = in.x;
    if (P.get(P_CMODE) >= 0.5) enumDD(ui, R(x, y, 186, h), P_LKL, L_LKL, 2, "Link law");
    x += 192;
    enumDD(ui, R(x, y, 160, h), P_RCMB, L_RCMB, 2, "Ranges");
    x += 166;
    enumDD(ui, R(x, y, 180, h), P_AMKM, L_AMKM, 2, "Makeup");
    x += 192;
    if (P.get(P_DET) >= 1.5) {
        label(ui, R(x, y, 70, h), "RMS share");
        ui.valueBox(R(x + 70, y, in.r() - x - 70, h), P_BLEND, true, KD_OTHER, 1, 0.1);
    }
    y += h + 12;
    ui.text(R(in.x, y, 200, 14), "SIDECHAIN", th::faint(), F_SEMIBOLD, 10.5f);
    y += 20;
    label(ui, R(in.x, y, 34, h), "HPF");
    ui.valueBox(R(in.x + 34, y, 74, h), P_SCHP, true, KD_LOG, std::log(50.0) / 140, std::log(50.0) / 1400);
    if (P.get(P_SCHP) > 20.5) enumDD(ui, R(in.x + 114, y, 104, h), P_SCHS, L_SCHS, 3);
    label(ui, R(in.x + 236, y, 30, h), "Tilt");
    ui.valueBox(R(in.x + 266, y, 76, h), P_SCTL, true, KD_OTHER, 0.2, 0.02);
    if (P.get(P_SC) >= 0.5) {
        label(ui, R(in.x + 360, y, 34, h), "Trim");
        ui.valueBox(R(in.x + 394, y, 76, h), P_SCT, true, KD_OTHER, 0.2, 0.02);
    } else {
        ui.text(R(in.x + 360, y, in.r() - in.x - 360, h), "Trim: with the sidechain as the trigger", th::faint(), F_REGULAR, 10.5f);
    }
    y += h + 14;
    ui.fill(R(in.x, y, in.w, 1), Paint(th::border()));
    y += 8;
    static const char *help[] = {
        "Bypass (Lvl Match): the input at the same loudness. Swap (Lvl Match): the other A/B set at this one's loudness.",
        "Click to latch Bypass, Swap, A/B, Delta, Listen and Auto makeup; Shift + click and hold to use them while held.",
        "Ctrl + Input moves the output the other way. Shift (or Ctrl) while dragging adjusts finely.",
    };
    for (const char *t : help) { ui.textFit(R(in.x, y, in.w, 14), t, th::faint(), F_REGULAR, 10.5f); y += 15; }
    char idb[64];
    snprintf(idb, sizeof idb, "Plugin ID %lld", (long long)av.myid);
    ui.text(R(in.x, y - 15, in.w, 14), idb, th::ghost().lighter(0.3f), F_REGULAR, 10.5f, 2);
    settingsEnd(ui);
}

// ---------------------------------------------------------------- the frame
void CompView::frame(Ui &ui)
{
    av.refresh();
    double t = ui.now;
    P.uiLink.last.store(t, std::memory_order_relaxed);
    P.uiLink.beat.store(t, std::memory_order_relaxed);
    P.uiLink.hold.store(st.holdNow(), std::memory_order_relaxed);
    live = av.myslot >= 0 && av.live(av.myslot);

    // background
    if (ui.drawS()) ui.fill(R(0, 0, DW, DH), Paint::vertical(0, th::bg(), DH, th::bg2()));
    // the subtitle: track, latency, the set playing
    std::string sub = av.myslot >= 0 ? av.trackName(av.myslot) : std::string();
    int ld = P.view.ld.load();
    char lt[64];
    if (ld > 0) snprintf(lt, sizeof lt, "latency %.1f ms (lookahead)", P.view.latms.load());
    else snprintf(lt, sizeof lt, "no latency");
    if (!sub.empty()) sub += "   \xC2\xB7   ";
    sub += lt;
    sub += P.view.side.load() ? "   \xC2\xB7   playing B" : "   \xC2\xB7   playing A";
    st.subtitle = sub;

    static const bool prof = getenv("TS_PROFILE") != nullptr;
    static double acc[12] = {};
    static int cnt = 0;
    double tp = now_sec();
    int sec = 0;
    auto mark = [&]() { if (prof) { double n = now_sec(); acc[sec++] += n - tp; tp = n; } };
    R gear = gearRect(DW);
    if (st.settings) ui.block(settingsRect(gear, SW, SH));
    topBar(ui, P, av, st, DW, "TWOSTORMS", "COMPRESSOR", true);
    if (st.settings) ui.block(settingsRect(gear, SW, SH));
    mark();
    // ---- displays: curve, history, meters
    const float y0 = 66, dh = 210;
    drawCurve(ui, R(M, y0, dh, dh));
    mark();
    float mw = 100;
    float hx = M + dh + 10, hw = DW - M - mw - 10 - hx;
    drawHistory(ui, R(hx, y0, hw, dh));
    mark();
    drawMeters(ui, R(DW - M - mw, y0, mw, dh));
    mark();
    // ---- analyzer and the trigger
    drawAnalyzer(ui, R(M, y0 + dh + 10, DW - 2 * M, 116));
    mark();
    triggerRows(ui, y0 + dh + 10 + 116 + 10);
    mark();
    float yc = 488;
    if (ui.drawS()) ui.fill(R(M, yc - 10, DW - 2 * M, 1), Paint(th::border().alpha(0.7f)));
    controls(ui, yc);
    mark();
    characterRow(ui, 590);
    customRow(ui, 622);
    mark();
    // ---- the built-in Act
    actCard(ui, P, av, R(M, 660, DW - 2 * M, 74));
    mark();
    // ---- the Settings popover, on top
    settings(ui, gear);
    mark();
    if (prof && ++cnt == 60) {
        static const char *nm[] = {"top", "curve", "history", "meters", "analyzer", "trigger", "controls", "char", "act", "settings"};
        fprintf(stderr, "[ts]");
        for (int i = 0; i < sec; ++i) { fprintf(stderr, " %s %.2f", nm[i], acc[i] / cnt * 1000); acc[i] = 0; }
        fprintf(stderr, "\n");
        cnt = 0;
    }
}

// ============================================================================ registration
static const char *const s_features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_COMPRESSOR, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

static Plugin *create(const clap_host *h) { return new CompressorPlugin(h); }

const PluginKind &kind()
{
    static const PluginKind k = {
        {CLAP_VERSION_INIT, "com.twostorms.compressor", "TwoStorms Compressor", "TwoStorms", "", "", "", TS_VERSION,
         "A compressor with an Act built in: threshold, ratio, knee, attack, hold, release, range; peak, RMS or hybrid detection; "
         "characters (Clean VCA, Opto, FET, Vari-mu) or Custom; sidechain, trigger ranges, lookahead (optional), Set threshold.",
         s_features},
        &table,
        &factoryPresets,
        "Compressor",
        "TWOSTORMS COMPRESSOR",
        shm::KIND_POINT,
        shm::SK_COMPRESSOR,
        true,
        true,
        P_AB,
        &create,
        2,
    };
    return k;
}

} // namespace comp
} // namespace ts
