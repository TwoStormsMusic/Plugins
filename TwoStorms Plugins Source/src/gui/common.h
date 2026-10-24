// Window parts every TwoStorms plug-in shares, so they look and work the same across the suite:
// the top bar (title, finale, Presets, A / B / Copy, Swap and Bypass at matched loudness, Settings),
// the built-in Act's card (drive link, Auto, meters, output gain), and meters.
#pragma once
#include "../core/act_view.h"
#include "ui.h"
#include <atomic>
#include <string>

namespace ts {
class Plugin;
namespace gui {

struct CommonState {
    bool settings = false;   // the Settings popover is open
    int hl = 0;              // latched hold: 1 Bypass, 2 Swap
    int holdm = 0;           // held with Shift + click
    bool abm = false;        // A/B flipped with Shift + click: flip back on release
    int holdNow() const { return holdm ? holdm : hl; }
    std::string subtitle;    // set by the plug-in (track, latency, playing set)
};

// the top bar; returns the Settings button's rect (the popover hangs from it)
enum { TB_PRESETS = 1, TB_BYPASS = 2 };   // what the top bar has besides A/B (the Act and the Gain Stage: neither)
R topBar(Ui &ui, Plugin &p, ActView &av, CommonState &st, float W, const char *title1, const char *title2, bool hasAB,
         int extra = TB_PRESETS | TB_BYPASS);
void presetsMenu(Ui &ui, Plugin &p, float x, float y);
// a TwoStorms Tuner listening to this plug-in has its Mute on (the subtitle says so instead)
bool tunerMuted(const ActView &av);

// the Settings popover: where it goes (block it before drawing the rest: ui.block(...)), and its frame,
// which returns the inner rect and closes on a click outside (or the gear)
R gearRect(float W);
R settingsRect(const R &gear, float w, float h);
bool settingsBegin(Ui &ui, CommonState &st, const R &gear, float w, float h, R &inner);
void settingsEnd(Ui &ui);

// the Act card: drive link, Auto, meters, output gain (a linked stereo pair)
void actCard(Ui &ui, Plugin &p, ActView &av, const R &r);
float actCardHeight(ActView &av);   // grows with the gain groups (74 for one)
void actMenu(Ui &ui, Plugin &p, ActView &av, float x, float y);
void driveMenu(Ui &ui, Plugin &p, ActView &av, float x, float y);

// meters (dB; range from P_RANGE): horizontal (the Act's), vertical, gain reduction (0..24 dB from the top)
float meterFrac(double db, double range);
void hMeter(Ui &ui, const R &r, int slot, int ch, double tgt, bool active, double range);
void vMeter(Ui &ui, const R &r, double db, bool active, double range, double hold = -200);
void grMeter(Ui &ui, const R &r, double gr);

// a small bolt: the TwoStorms mark
void logo(Ui &ui, float x, float y, float size);

} // namespace gui
} // namespace ts
