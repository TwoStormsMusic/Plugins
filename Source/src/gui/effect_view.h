// What every TwoStorms effect window shares besides the widgets: the top bar, the subtitle (track, latency,
// the set playing), the window's heartbeat to the audio (UiLink: hold buttons, level matching, displays), the
// settings popover (peak hold, meter range, zoom, finale, help, Plugin ID), the Act card, and the status hash
// that tells the still layer to redraw. A plug-in's view derives from it and draws its own panels.
#pragma once
#include "../core/abmatch.h"
#include "../core/act_view.h"
#include "../core/clock.h"
#include "../core/plugin.h"
#include "common.h"
#include "editor.h"
#include <functional>
#include <string>
#include <vector>

namespace ts {
namespace gui {

class EffectView : public View {
public:
    EffectView(Plugin &p, UiLink &u, float dw, float dh, const char *title) : P(p), av(p), U(u), DW(dw), DH(dh), title_(title) {}
    void designSize(float &w, float &h) override { w = DW; h = DH; }
    bool wantsFull() override { return st.settings || extraFull(); }
    uint64_t statusHash() override;

protected:
    Plugin &P;
    ActView av;
    CommonState st;
    UiLink &U;
    float DW, DH;
    const float M = 14;
    bool live = false;
    float SW = 620, SH = 196;   // the settings popover
    std::vector<std::string> help;   // lines at the bottom of the settings popover

    // the frame's start: heartbeat, background, subtitle (latency in ms, 0 = none; side = the set playing), top bar;
    // returns the gear (for settingsEnd... see finish)
    R begin(Ui &ui, double latms, bool side, const std::string &extraSub = std::string());
    // the frame's end: the settings popover (with `extra` drawn inside it, from y, returning the next y)
    void finish(Ui &ui, const R &gear, std::function<float(Ui &, const R &, float)> extra = nullptr);
    // what else makes the still layer redraw
    virtual void hashMore(std::function<void(uint64_t)> mix) { (void)mix; }
    virtual bool extraFull() { return false; }

    void enumDD(Ui &ui, const R &r, int id, const char *const *labels, int n, const char *prefix = nullptr, bool en = true);
    void label(Ui &ui, const R &r, const char *t, bool en = true) { ui.text(r, t, en ? th::dim() : th::faint(), F_REGULAR, 12); }
    // an on/off button for parameter id; Shift + click holds it while the mouse is down (momentary)
    void toggle(Ui &ui, const R &r, const char *label, int id, int style = BS_NORMAL, bool momentary = false);
    void card(Ui &ui, const R &r);
    void actCardAt(Ui &ui, float y) { actCard(ui, P, av, R(M, y, DW - 2 * M, 74)); }
    // the Input row (label, bar, box): Ctrl + a change moves the output the other way (the input / output link),
    // Shift adjusts finely
    void inputRow(Ui &ui, const R &r, int id, float labelW, float boxW, const char *label = "Input");

private:
    const char *title_;
    double inLast_ = 1e9;
    bool iol_ = false;
};

} // namespace gui
} // namespace ts
