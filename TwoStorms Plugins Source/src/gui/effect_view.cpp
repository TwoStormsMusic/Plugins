#include "effect_view.h"
#include <algorithm>
#include <cstdio>

namespace ts {
namespace gui {

static const char *const L_HOLD[] = {"Off", "1 s", "2 s", "5 s", "10 s", "Infinite"};
static const char *const L_RANGE[] = {"24 dB", "36 dB", "48 dB", "60 dB", "72 dB", "96 dB"};
static const char *const L_ZOOM[] = {"100%", "115%", "130%", "150%", "175%", "200%"};
static const char *const L_FIN[] = {"Auto", "Always", "Never"};

uint64_t EffectView::statusHash()
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
        mix((uint64_t)b.tgs.load());
        mix((uint64_t)(b.tgel.load() * 10));
    }
    mix((uint64_t)P.driveStatus());
    mix(now_sec() - P.presetMsgT < 4);
    mix((uint64_t)P.latencyNow());
    mix((uint64_t)st.holdNow() | (uint64_t)st.settings << 4 | (uint64_t)st.abm << 5);
    mix((uint64_t)shm::get()->h.slotver.load());
    mix(std::hash<std::string>()(st.subtitle));
    hashMore(mix);
    return h;
}

R EffectView::begin(Ui &ui, double latms, bool side, const std::string &extraSub)
{
    av.refresh();
    double t = ui.now;
    U.last.store(t, std::memory_order_relaxed);
    U.beat.store(t, std::memory_order_relaxed);
    U.hold.store(st.holdNow(), std::memory_order_relaxed);
    live = av.myslot >= 0 && av.live(av.myslot);
    if (ui.drawS()) ui.fill(R(0, 0, DW, DH), Paint::vertical(0, th::bg(), DH, th::bg2()));
    std::string sub = av.myslot >= 0 ? av.trackName(av.myslot) : std::string();
    char lt[64];
    if (latms > 0) snprintf(lt, sizeof lt, "latency %.1f ms", latms);
    else snprintf(lt, sizeof lt, "no latency");
    if (!sub.empty()) sub += "   \xC2\xB7   ";
    sub += lt;
    if (P.K.hasAB) sub += side ? "   \xC2\xB7   playing B" : "   \xC2\xB7   playing A";
    if (!extraSub.empty()) sub += "   \xC2\xB7   " + extraSub;
    st.subtitle = sub;
    R gear = gearRect(DW);
    if (st.settings) ui.block(settingsRect(gear, SW, SH));
    topBar(ui, P, av, st, DW, "TWOSTORMS", title_, P.K.hasAB);
    if (st.settings) ui.block(settingsRect(gear, SW, SH));
    return gear;
}

void EffectView::finish(Ui &ui, const R &gear, std::function<float(Ui &, const R &, float)> extra)
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
    y += h + 12;
    if (extra) y = extra(ui, in, y);
    ui.fill(R(in.x, y, in.w, 1), Paint(th::border()));
    y += 8;
    for (const std::string &t : help) { ui.textFit(R(in.x, y, in.w, 14), t.c_str(), th::faint(), F_REGULAR, 10.5f); y += 15; }
    char idb[64];
    snprintf(idb, sizeof idb, "Plugin ID %lld", (long long)av.myid);
    ui.text(R(in.x, y + 2, in.w, 14), idb, th::ghost().lighter(0.3f), F_REGULAR, 10.5f, 2);
    settingsEnd(ui);
}

void EffectView::enumDD(Ui &ui, const R &r, int id, const char *const *labels, int n, const char *prefix, bool en)
{
    int cv = iclamp(iround(P.get(id)), 0, n - 1);
    if (ui.dropdown(r, labels[cv], en, prefix)) {
        std::vector<MenuItem> m;
        for (int i = 0; i < n; ++i) m.push_back(MenuItem::item(labels[i], i + 1, i == cv));
        Plugin *pp = &P;
        ui.menu(m, r.x, r.b() + 4, [pp, id](int r2) { pp->setOnce(id, r2 - 1); }, r.w);
    }
}

void EffectView::toggle(Ui &ui, const R &r, const char *lbl, int id, int style, bool momentary)
{
    bool on = P.get(id) >= 0.5;
    if (ui.button(r, lbl, on, true, style)) {
        if (momentary && (ui.mods & M_SHIFT)) { ui.momKey = id; ui.momPrev = on ? 1 : 0; }
        P.setOnce(id, on ? 0 : 1);
    }
}

void EffectView::inputRow(Ui &ui, const R &r, int id, float labelW, float boxW, const char *lbl)
{
    double vin = P.get(id);
    if (iol_ && inLast_ < 1e8 && vin != inLast_ && P.get(P_GAIN) > GFLOOR)
        P.apply(P_GAIN, std::max(-99.9, std::min(60.0, P.get(P_GAIN) - (vin - inLast_))));
    inLast_ = vin;
    bool act = ui.dragKey == id || ui.hit(r);
    iol_ = act && (ui.mods & M_CTRL);
    bool fineWas = ui.fine;
    if (act) ui.fine = (ui.mods & M_SHIFT) != 0;
    ui.paramRow(r, lbl, id, SC_LIN, labelW, boxW);
    ui.fine = fineWas;
}

void EffectView::card(Ui &ui, const R &r)
{
    ui.shadow(r, 8, 12, Col(0, 0, 0, 0.45f));
    ui.fill(r, Paint::vertical(r.y, Col::hex(0x151417), r.b(), Col::hex(0x101012)), 8);
    ui.stroke(r, Paint(th::border()), 8);
}

} // namespace gui
} // namespace ts
