// The immediate-mode widget kit every TwoStorms window is drawn with (the JSFX suite's @gfx helpers,
// redone): each frame the layout code calls the widgets, which draw themselves and handle the mouse
// and keyboard. Coordinates are design units (the window at 100%); the kit scales them.
#pragma once
#include "canvas.h"
#include "font.h"
#include "theme.h"
#include <functional>
#include <string>
#include <vector>

namespace ts {
namespace gui {

struct R {
    float x = 0, y = 0, w = 0, h = 0;
    R() {}
    R(float x_, float y_, float w_, float h_) : x(x_), y(y_), w(w_), h(h_) {}
    R inset(float d) const { return R(x + d, y + d, w - 2 * d, h - 2 * d); }
    float r() const { return x + w; }
    float b() const { return y + h; }
};

enum Key { K_NONE = 0, K_ENTER, K_ESC, K_BACK, K_DEL, K_LEFT, K_RIGHT, K_HOME, K_END, K_TAB };
enum Mod { M_SHIFT = 1, M_CTRL = 2, M_ALT = 4 };
enum Cursor { CUR_ARROW = 0, CUR_HAND, CUR_HRESIZE, CUR_IBEAM, CUR_MOVE, CUR_VRESIZE };

struct Input {
    float mx = -1000, my = -1000;   // pixels
    int buttons = 0;                // 1 left, 2 right, 4 middle
    int mods = 0;                   // Mod
    float wheel = 0, hwheel = 0;    // notches
    std::vector<uint32_t> chars;    // typed text
    std::vector<int> keys;          // Key
    // presses that came and went between two frames still count
    int pressed = 0, released = 0;
};

struct MenuItem {
    std::string label;
    int id = 0;          // what the callback gets (0: nothing)
    bool checked = false, enabled = true, separator = false;
    // a check box: a click flips it and calls back, and the menu stays open (a list to pick several from)
    bool toggle = false;
    // stays open when clicked; a button row (drawn as one, centered); clears: unchecks every toggle in the menu
    bool keepOpen = false, asButton = false, clears = false;
    std::vector<MenuItem> sub;
    static MenuItem toggleItem(const std::string &l, int id, bool checked)
    {
        MenuItem m;
        m.label = l; m.id = id; m.checked = checked; m.toggle = true; m.keepOpen = true;
        return m;
    }
    static MenuItem buttonItem(const std::string &l, int id, bool clears)
    {
        MenuItem m;
        m.label = l; m.id = id; m.asButton = true; m.keepOpen = true; m.clears = clears;
        return m;
    }
    static MenuItem sep() { MenuItem m; m.separator = true; return m; }
    static MenuItem item(const std::string &l, int id, bool checked = false, bool enabled = true)
    {
        MenuItem m;
        m.label = l; m.id = id; m.checked = checked; m.enabled = enabled;
        return m;
    }
    static MenuItem submenu(const std::string &l, std::vector<MenuItem> s)
    {
        MenuItem m;
        m.label = l; m.sub = std::move(s);
        return m;
    }
};

// what parameter controls need; keys are parameter IDs (or, for a window that shows other
// instances, (slot + 2) * 256 + ID, like the JSFX)
class ParamAccess {
public:
    virtual ~ParamAccess() {}
    virtual double get(int key) = 0;
    virtual void begin(int key) = 0;
    virtual void set(int key, double v) = 0;   // inside begin / end
    virtual void end(int key) = 0;
    virtual void setOnce(int key, double v) { begin(key); set(key, v); end(key); }
    virtual double def(int key) = 0;
    virtual double lo(int key) = 0;
    virtual double hi(int key) = 0;
    virtual void format(int key, double v, char *b, int n) = 0;
    virtual bool parse(int key, const char *s, double *v) = 0;
};

// bar scales (the JSFX's bar_frac / bar_val): 1 gain (square root about 0 dB), 2 linear, 3 log, 4 square root
enum Scale { SC_GAIN = 1, SC_LIN = 2, SC_LOG = 3, SC_SQRT = 4 };
double barFrac(int m, double v, double lo, double hi);
double barVal(int m, double f, double lo, double hi);

// kinds of value (for the right-click menu and the wheel): the JSFX's knd
enum Kind { KD_GAIN = 0, KD_TIME = 2, KD_TARGET = 3, KD_LOG = 5, KD_OTHER = 6 };

enum Icon { IC_NONE = 0, IC_CHEVRON, IC_GEAR, IC_DOTS, IC_PLUS, IC_X, IC_CHECK, IC_LINK, IC_BYPASS, IC_SWAP, IC_PAR, IC_SER, IC_CHAIN };
// IC_PAR "//" (side by side), IC_SER "--" (one into the other), IC_CHAIN a chain on its side (linked)
enum BStyle { BS_NORMAL = 0, BS_PURPLE = 1, BS_GHOST = 2, BS_ORANGE_OUTLINE = 3 };

class Ui {
public:
    // ---------------- passes ----------------
    // A frame is drawn in one pass (PASS_ALL), or in two: the still parts into a cached layer
    // (PASS_STATIC, only when something changed: input, parameters, status) and, every frame, the
    // moving parts over a copy of it (PASS_DYN: meters, curves; no input). Widgets draw in the still
    // pass; a view guards its own drawing with drawS() / drawD().
    enum { PASS_ALL = 0, PASS_STATIC = 1, PASS_DYN = 2 };
    int pass = PASS_ALL;
    bool drawS() const { return pass != PASS_DYN; }
    bool drawD() const { return pass != PASS_STATIC; }

    // ---------------- frame ----------------
    void beginFrame(Canvas &c, const Input &in, double now, float scale, float ox = 0, float oy = 0);
    void endFrame();
    Canvas &cv() { return *cv_; }
    float s = 1, ox = 0, oy = 0;
    double now = 0, dt = 0;
    ParamAccess *pa = nullptr;

    // ---------------- input state ----------------
    const Input &in() const { return in_; }
    bool lclick = false, rclick = false, lrel = false, ldown = false, fine = false;
    int mods = 0;
    float wheel = 0;
    bool used = false;      // the click (or wheel) was taken this frame
    Cursor cursor = CUR_ARROW;
    bool wantKeyboard() const { return editKey != 0 || promptOpen_; }
    bool busy() const { return menuOpen() || promptOpen_; }   // an overlay takes the mouse
    // a popover drawn later in the frame: widgets under it don't get the mouse
    void block(const R &r) { blockR = r; blockOn = true; }
    // a region outside which nothing is drawn and nothing takes the mouse (a scrolled list under a toolbar)
    bool areaOn = false;
    R area;
    void beginArea(const R &r)
    {
        area = r;
        areaOn = true;
        cv_->pushClip(X(r.x), Y(r.y), r.w * s, r.h * s);
    }
    void endArea()
    {
        if (!areaOn) return;
        areaOn = false;
        cv_->popClip();
    }
    R blockR;
    bool blockOn = false;
    bool dragging() const { return dragKey != 0; }

    // ---------------- coordinates (design units -> pixels) ----------------
    float X(float v) const { return ox + v * s; }
    float Y(float v) const { return oy + v * s; }
    float W(float v) const { return v * s; }
    float mx() const { return (in_.mx - ox) / s; }   // the mouse in design units
    float my() const { return (in_.my - oy) / s; }
    bool hit(const R &r) const;           // the mouse is over r (and nothing on top)
    bool hitRaw(const R &r) const;        // the mouse is over r

    // ---------------- drawing, design units ----------------
    void fill(const R &r, const Paint &p, float rad = 0);
    void stroke(const R &r, const Paint &p, float rad = 0, float lw = 1);
    void shadow(const R &r, float rad, float blur, const Col &c);
    void line(float x0, float y0, float x1, float y1, const Col &c, float lw = 1);
    float textW(FontWeight f, float size, const char *t, bool tab = false);
    // text in r: align 0 left, 1 center, 2 right; vertically centered on cap height
    void text(const R &r, const char *t, const Col &c, FontWeight f = F_REGULAR, float size = 12, int align = 0, bool tab = false);
    void textAt(float x, float baseline, const char *t, const Col &c, FontWeight f = F_REGULAR, float size = 12, bool tab = false);
    // text that doesn't fit gets "..."
    void textFit(const R &r, const char *t, const Col &c, FontWeight f = F_REGULAR, float size = 12, int align = 0);
    void icon(Icon ic, float cx, float cy, float size, const Col &c);

    // ---------------- widgets ----------------
    // a push button; style: BS_*; active: drawn lit (orange, or purple for BS_PURPLE)
    bool button(const R &r, const char *label, bool active = false, bool enabled = true, int style = BS_NORMAL, Icon ic = IC_NONE);
    // a dropdown face (the caller opens the menu when it returns true)
    bool dropdown(const R &r, const char *label, bool enabled = true, const char *prefix = nullptr);
    bool checkbox(const R &r, bool on, bool enabled = true);
    void badge(float x, float y, const char *t, const Col &fg, const Col &bg);
    float badgeW(const char *t) { return textW(F_SEMIBOLD, 10, t) + 12; }
    void badgeDyn(float x, float y, const char *t, const Col &fg, const Col &bg);   // a badge that changes (drawn every frame)
    // a bar for parameter `key` on scale `sc`; double-click resets, right-click menu, wheel, Shift/Ctrl fine
    void bar(const R &r, int key, int sc, bool enabled = true, int kind = KD_OTHER, bool centered = false);
    // a value box: click and type; wheel steps; right-click menu
    void valueBox(const R &r, int key, bool enabled = true, int kind = KD_OTHER, double step = 0, double fineStep = 0,
                  int align = 2, const char *override = nullptr, const char *prefix = nullptr);
    // label, bar and value box in one row (the JSFX's cctl)
    void paramRow(const R &r, const char *label, int key, int sc, float labelW, float boxW, bool enabled = true, int kind = KD_OTHER);
    // relative dragging on an area (the JSFX's curve: drag across it to move the threshold)
    void dragArea(const R &r, int key, double perUnit, double finePerUnit);
    // a value box that is also a small slider: a see-through fill shows the value (from the middle when centered);
    // drag left or right to change it, click without dragging to type it; wheel steps, right-click menu
    // (pxPerUnit: for a count, the pixels dragged per step; otherwise the box's width spans the range)
    void slideBox(const R &r, int key, int sc, const Col &col, bool centered = false, bool enabled = true, double step = 0,
                  double fineStep = 0, float pxPerUnit = 0);

    // ---------------- menus ----------------
    void menu(std::vector<MenuItem> items, float x, float y, std::function<void(int)> cb, float minW = 0);
    bool menuOpen() const { return !levels_.empty(); }
    void closeMenu() { levels_.clear(); }
    // the right-click menu of a value
    void contextMenu(int key, int kind);

    // ---------------- typing ----------------
    void beginEdit(int key);
    void commitEdit();
    int editKey = 0;
    std::string editBuf;
    bool editFresh = false;
    R editRect;
    // a small dialog that asks for a line of text
    void prompt(const std::string &title, const std::string &initial, std::function<void(const std::string &)> cb,
                const std::string &okLabel = "Save");
    // the same dialog without the text field: a question and OK / Cancel
    void confirm(const std::string &title, const std::string &okLabel, std::function<void()> cb);

    // ---------------- dragging ----------------
    int dragKey = 0;
    // momentary toggles: Shift + click flips a switch only while the mouse is held
    int momKey = 0;
    double momPrev = 0;

    // repaint hints for the window
    bool changed = true;

private:
    Canvas *cv_ = nullptr;
    Input in_;
    int prevButtons_ = 0;
    // drag
    int dragMode_ = 0;   // 1 bar (absolute along its scale), 2 relative
    float dragX0_ = 0, dragW_ = 1;
    double dragF0_ = 0, dragLo_ = 0, dragHi_ = 1, dragV_ = 0, dragSens_ = 0, dragFineSens_ = 0;
    int dragSc_ = 2;
    bool dragFine_ = false;
    bool dragLog_ = false;
    float dragLastX_ = 0;
    // double-click
    double lastClickT_ = 0;
    int lastClickKey_ = 0;
    // menus
    struct Level {
        std::vector<MenuItem> items;
        float x = 0, y = 0, w = 0, h = 0;   // pixels
        int hover = -1;
        int subOf = -1;   // the parent item that opened it
        double hoverT = 0;
    };
    std::vector<Level> levels_;
    R lastClick_;   // the widget that opened the menu (a menu that can't hang below it goes above it)
    std::function<void(int)> menuCb_;
    void layoutLevel(Level &L, float x, float y, float flipY = -1);
    void menuInput();
    void menuDraw();
    // prompt
    bool promptOpen_ = false, promptConfirm_ = false;
    std::string promptTitle_, promptBuf_, promptOk_;
    std::function<void(const std::string &)> promptCb_;
    void promptInput();
    void promptDraw();
    void editInput();
};

} // namespace gui
} // namespace ts
