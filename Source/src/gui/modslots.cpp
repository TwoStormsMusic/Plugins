#include "modslots.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

namespace ts {
namespace gui {

static const char *const L_TYPE[] = {"Off", "LFO", "Follower", "MIDI"};
static const char *const L_SHAPE[] = {"Sine", "Triangle", "Saw up", "Saw down", "Square", "Random"};
static const char *const L_SYNC[] = {"Free", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars", "1/16T", "1/8T", "1/4T", "1/8."};
static const char *const L_RANGE[] = {"24 dB", "48 dB", "72 dB"};
static const char *const L_MIDI[] = {"CC", "Velocity", "Pitch bend"};

int modSlotsOn(const Plugin &P, int base)
{
    int n = 0;
    for (int s = 0; s < MOD_N; ++s)
        if (P.get(modParamB(base, s, MS_TYPE)) >= 0.5) n++;
    return n;
}

// a dropdown that picks one of n labels and sets parameter id to the index (or calls set)
static void pick(Ui &ui, Plugin &P, const R &r, int cv, const char *const *labels, int n, std::function<void(int)> set, bool en = true)
{
    cv = std::max(0, std::min(n - 1, cv));
    if (!ui.dropdown(r, labels[cv], en)) return;
    std::vector<MenuItem> m;
    for (int i = 0; i < n; ++i) m.push_back(MenuItem::item(labels[i], i + 1, i == cv));
    ui.menu(m, r.x, r.b() + 4, [set](int k) { if (k > 0) set(k - 1); }, r.w);
    (void)P;
}

static void row(Ui &ui, Plugin &P, float x, float y, float w, int s, const ModSlotLabels &L, const std::atomic<float> *mv, bool live)
{
    const float h = 26;
    const int B = L.base;
    int ty = std::max(0, std::min(3, iround(P.get(modParamB(B, s, MS_TYPE)))));
    const float nowW = 60, nowX = x + w - nowW;
    Plugin *pp = &P;
    if (ui.drawS()) {
        char nb[12];
        snprintf(nb, sizeof nb, "%d", s + 1);
        ui.text(R(x, y, 16, h), nb, ty > 0 ? th::purple() : th::faint(), F_BOLD, 12, 1);
        pick(ui, P, R(x + 20, y, 106, h), ty, L_TYPE, 4, [pp, B, s](int nt) {
            pp->setOnce(modParamB(B, s, MS_TYPE), nt);
            // keep the option in the range the type uses
            int o = iround(pp->get(modParamB(B, s, MS_OPT)));
            int n = nt == 1 ? o % 6 : (nt == 2 ? std::min(2, o % 4) + 4 * ((o / 4) % 3) : std::min(129, o));
            if (n != o) pp->setOnce(modParamB(B, s, MS_OPT), n);
        });
        if (ty == 0) {
            ui.text(R(x + 136, y, 200, h), "off", th::faint(), F_REGULAR, 11.5f);
            return;
        }
        int tb = std::max(0, std::min(L.nTargets - 1, iround(P.get(modParamB(B, s, MS_TARGET))))), tp = iround(P.get(modParamB(B, s, MS_PARAM)));
        if (tb == 0 && tp > L.nGlobals - 1) { P.setOnce(modParamB(B, s, MS_PARAM), L.nGlobals - 1); tp = L.nGlobals - 1; }
        pick(ui, P, R(x + 132, y, 112, h), tb, L.targets, L.nTargets, [pp, B, s](int k) { pp->setOnce(modParamB(B, s, MS_TARGET), k); });
        pick(ui, P, R(x + 250, y, 128, h), tp, tb == 0 ? L.globals : L.params, tb == 0 ? L.nGlobals : L.nParams,
             [pp, B, s](int k) { pp->setOnce(modParamB(B, s, MS_PARAM), k); });
        ui.bar(R(x + 384, y, 96, h), modParamB(B, s, MS_AMT), SC_LIN, true, KD_OTHER, true);
        ui.valueBox(R(x + 484, y, 62, h), modParamB(B, s, MS_AMT), true, KD_OTHER, 2.9, 0.29);
        int o = iround(P.get(modParamB(B, s, MS_OPT)));
        float sx = x + 556;
        char b[32];
        if (ty == 1) {
            pick(ui, P, R(sx, y, 112, h), o % 6, L_SHAPE, 6, [pp, B, s](int k) { pp->setOnce(modParamB(B, s, MS_OPT), k); });
            pick(ui, P, R(sx + 118, y, 90, h), iround(P.get(modParamB(B, s, MS_SYNC))), L_SYNC, 13, [pp, B, s](int k) { pp->setOnce(modParamB(B, s, MS_SYNC), k); });
            bool free = P.get(modParamB(B, s, MS_SYNC)) < 0.5;
            snprintf(b, sizeof b, "%.2f Hz", P.get(modParamB(B, s, MS_RATE)));
            ui.valueBox(R(sx + 214, y, std::min(116.f, nowX - 10 - (sx + 214)), h), modParamB(B, s, MS_RATE), free, KD_LOG, std::log(50000.0) / 138,
                        std::log(50000.0) / 1380, 2, free ? b : "at the tempo");
        } else if (ty == 2) {
            // the source and the range are packed in one setting: source + 4 * range
            int src = std::min(2, o % 4), rng = (o / 4) % 3;
            pick(ui, P, R(sx, y, 96, h), src, L.sources, 3, [pp, B, s, rng](int k) { pp->setOnce(modParamB(B, s, MS_OPT), k + 4 * rng); });
            pick(ui, P, R(sx + 100, y, 72, h), rng, L_RANGE, 3, [pp, B, s, src](int k) { pp->setOnce(modParamB(B, s, MS_OPT), src + 4 * k); });
            snprintf(b, sizeof b, "A %.1f ms", P.get(modParamB(B, s, MS_RATE)));
            ui.valueBox(R(sx + 176, y, 78, h), modParamB(B, s, MS_RATE), true, KD_LOG, std::log(50000.0) / 138, std::log(50000.0) / 1380, 2, b);
            char b2[32];
            snprintf(b2, sizeof b2, "R %.0f ms", P.get(modParamB(B, s, MS_RATE2)));
            ui.valueBox(R(sx + 258, y, 78, h), modParamB(B, s, MS_RATE2), true, KD_LOG, std::log(2000.0) / 138, std::log(2000.0) / 1380, 2, b2);
        } else {
            int kind = o < 128 ? 0 : o - 127;
            pick(ui, P, R(sx, y, 116, h), kind, L_MIDI, 3, [pp, B, s, o](int k) {
                pp->setOnce(modParamB(B, s, MS_OPT), k == 0 ? (o < 128 ? o : 1) : 127 + k);
            });
            snprintf(b, sizeof b, "CC %d", o);
            ui.valueBox(R(sx + 122, y, 84, h), modParamB(B, s, MS_OPT), o < 128, KD_OTHER, 0.3, 0.05, 2, o < 128 ? b : "");
        }
        ui.fill(R(nowX, y + 9, nowW, 8), Paint(th::well()), 3);
        ui.stroke(R(nowX, y + 9, nowW, 8), Paint(th::border()), 3);
    }
    if (ui.drawD() && ty > 0 && live && mv) {
        double v = mv[s].load(std::memory_order_relaxed);
        R m(nowX, y + 10, nowW, 6);
        if (ty == 1) {
            float c = m.x + m.w / 2, ww = (float)(m.w / 2 * std::max(-1.0, std::min(1.0, v)));
            ui.fill(ww >= 0 ? R(c, m.y, ww, m.h) : R(c + ww, m.y, -ww, m.h), Paint(th::purple()), 2);
        } else {
            ui.fill(R(m.x, m.y, (float)(m.w * std::max(0.0, std::min(1.0, v))), m.h), Paint(th::purple()), 2);
        }
    }
}

void modSlots(Ui &ui, Plugin &P, float x, float y, float w, float dy, const ModSlotLabels &L, const std::atomic<float> *mv, bool live)
{
    if (ui.drawS()) {
        auto hd = [&](float dx, const char *t) { ui.text(R(x + dx, y, 200, 14), t, th::faint(), F_SEMIBOLD, 10); };
        hd(24, "TYPE"); hd(136, "TARGET"); hd(254, "SETTING"); hd(388, "AMOUNT"); hd(560, "SHAPE / SOURCE / CC");
        ui.text(R(x + w - 60, y, 60, 14), "NOW", th::faint(), F_SEMIBOLD, 10, 1);
    }
    for (int s = 0; s < MOD_N; ++s) row(ui, P, x, y + 20 + s * dy, w, s, L, mv, live);
}

} // namespace gui
} // namespace ts
