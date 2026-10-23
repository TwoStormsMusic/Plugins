#include "ui.h"
#include "../core/params.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ts {
namespace gui {

// ---------------------------------------------------------------- scales
double barFrac(int m, double v, double lo, double hi)
{
    if (m == SC_GAIN) return v >= 0 ? 0.5 + 0.5 * std::sqrt(std::min(1.0, v / 60)) : 0.5 - 0.5 * std::sqrt(std::min(1.0, -v / 100));
    if (m == SC_LOG) return std::log(std::max(lo, v) / lo) / std::log(hi / lo);
    if (m == SC_SQRT) return std::sqrt(std::max(0.0, (v - lo) / (hi - lo)));
    return (v - lo) / (hi - lo);
}

double barVal(int m, double f, double lo, double hi)
{
    f = std::max(0.0, std::min(1.0, f));
    if (m == SC_GAIN) {
        double t = std::fabs(f - 0.5) * 2;
        return f >= 0.5 ? 60 * t * t : -100 * t * t;
    }
    if (m == SC_LOG) return lo * std::exp(f * std::log(hi / lo));
    if (m == SC_SQRT) return lo + f * f * (hi - lo);
    return lo + f * (hi - lo);
}

// ---------------------------------------------------------------- frame
void Ui::beginFrame(Canvas &c, const Input &in, double t, float scale, float ox_, float oy_)
{
    cv_ = &c;
    dt = now > 0 ? std::min(0.5, std::max(0.0, t - now)) : 0;
    now = t;
    s = scale;
    ox = ox_;
    oy = oy_;
    in_ = in;
    if (pass == PASS_DYN) {
        // the moving parts only: no input
        in_.pressed = in_.released = 0;
        in_.wheel = in_.hwheel = 0;
        in_.keys.clear();
        in_.chars.clear();
        lclick = rclick = lrel = false;
        ldown = (in.buttons & 1) != 0;
        mods = in.mods;
        fine = false;
        wheel = 0;
        used = true;
        blockOn = false;
        return;
    }
    mods = in.mods;
    int b = in.buttons;
    lclick = ((in.pressed & 1) != 0) || ((b & 1) && !(prevButtons_ & 1));
    rclick = ((in.pressed & 2) != 0) || ((b & 2) && !(prevButtons_ & 2));
    lrel = ((in.released & 1) != 0) || (!(b & 1) && (prevButtons_ & 1));
    ldown = (b & 1) != 0;
    fine = (mods & (M_SHIFT | M_CTRL)) != 0;
    wheel = in.wheel;
    used = false;
    blockOn = false;
    cursor = CUR_ARROW;
    if (promptOpen_) promptInput();
    else if (!levels_.empty()) menuInput();
    if (editKey) editInput();
}

void Ui::endFrame()
{
    if (pass == PASS_DYN) {
        if (promptOpen_) promptDraw();
        if (!levels_.empty()) menuDraw();
        return;
    }
    // a Shift + click toggle puts itself back when the mouse is let go
    if (momKey && !ldown) {
        if (pa) pa->setOnce(momKey, momPrev);
        momKey = 0;
    }
    if (dragKey && pa && dragMode_ == 3) {
        // a slide box pressed: moved far enough, it slides; let go where it was, it's typed into
        if (ldown) {
            if (std::fabs(mx() - dragX0_) > 3) {
                pa->begin(dragKey);
                dragMode_ = 2;
                dragLastX_ = dragX0_;
            }
        } else {
            int k = dragKey;
            dragKey = 0;
            dragMode_ = 0;
            beginEdit(k);
        }
    }
    if (dragKey && pa) {
        if (ldown) {
            double lo = pa->lo(dragKey), hi = pa->hi(dragKey);
            if (dragMode_ == 1) {
                // bars: the position moves exactly as far as the pointer does (a tenth while fine)
                double f1 = dragF0_ + (mx() - dragX0_) / dragW_ * (dragFine_ ? 0.1 : 1);
                if (fine != dragFine_) {
                    dragF0_ = std::max(0.0, std::min(1.0, f1));
                    dragX0_ = mx();
                    dragFine_ = fine;
                    f1 = dragF0_;
                }
                double v = barVal(dragSc_, f1, dragLo_, dragHi_);
                v = std::max(lo, std::min(hi, v));
                if (v != pa->get(dragKey)) { pa->set(dragKey, v); changed = true; }
                dragV_ = v;
            } else {
                float dx = mx() - dragLastX_;
                dragLastX_ = mx();
                if (dx != 0) {
                    dragV_ = dragLog_ ? std::exp(std::log(std::max(0.000001, dragV_)) + dx * (fine ? dragFineSens_ : dragSens_))
                                      : dragV_ + dx * (fine ? dragFineSens_ : dragSens_);
                    dragV_ = std::max(lo, std::min(hi, dragV_));
                    pa->set(dragKey, dragV_);
                    changed = true;
                }
            }
            cursor = dragMode_ == 1 ? CUR_HRESIZE : cursor;
        } else {
            pa->end(dragKey);
            dragKey = 0;
            changed = true;
        }
    }
    if (pass == PASS_ALL) {
        if (promptOpen_) promptDraw();
        if (!levels_.empty()) menuDraw();
    }
    prevButtons_ = in_.buttons;
}

bool Ui::hitRaw(const R &r) const
{
    float x = mx(), y = my();
    if (areaOn && !(x >= area.x && x < area.x + area.w && y >= area.y && y < area.y + area.h)) return false;
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

bool Ui::hit(const R &r) const
{
    if (busy()) return false;
    if (blockOn && hitRaw(blockR)) return false;
    return hitRaw(r);
}

// ---------------------------------------------------------------- drawing
void Ui::fill(const R &r, const Paint &p, float rad)
{
    Paint q = p;
    if (q.grad) { q.x0 = X(q.x0); q.x1 = X(q.x1); q.y0 = Y(q.y0); q.y1 = Y(q.y1); }
    if (rad > 0) cv_->fillRoundRect(X(r.x), Y(r.y), W(r.w), W(r.h), W(rad), q);
    else cv_->fillRect(X(r.x), Y(r.y), W(r.w), W(r.h), q);
}

void Ui::stroke(const R &r, const Paint &p, float rad, float lw)
{
    Paint q = p;
    if (q.grad) { q.x0 = X(q.x0); q.x1 = X(q.x1); q.y0 = Y(q.y0); q.y1 = Y(q.y1); }
    cv_->strokeRoundRect(X(r.x), Y(r.y), W(r.w), W(r.h), W(rad), std::max(1.f, W(lw)), q);
}

void Ui::shadow(const R &r, float rad, float blur, const Col &c) { cv_->shadow(X(r.x), Y(r.y), W(r.w), W(r.h), W(rad), W(blur), c); }

void Ui::line(float x0, float y0, float x1, float y1, const Col &c, float lw)
{
    cv_->line(X(x0), Y(y0), X(x1), Y(y1), std::max(1.f, W(lw)), c);
}

float Ui::textW(FontWeight f, float size, const char *t, bool tab) { return font(f).measure(size * s, t, tab) / s; }

void Ui::textAt(float x, float baseline, const char *t, const Col &c, FontWeight f, float size, bool tab)
{
    cv_->text(font(f), size * s, X(x), Y(baseline), t, c, tab);
}

void Ui::text(const R &r, const char *t, const Col &c, FontWeight f, float size, int align, bool tab)
{
    Font &F = font(f);
    float px = size * s;
    float tw = F.measure(px, t, tab) / s;
    float x = align == 0 ? r.x : (align == 1 ? r.x + (r.w - tw) * 0.5f : r.x + r.w - tw);
    float cap = F.capHeight(px) / s;
    float base = r.y + (r.h + cap) * 0.5f;
    cv_->text(F, px, X(x), Y(base), t, c, tab);
}

void Ui::textFit(const R &r, const char *t, const Col &c, FontWeight f, float size, int align)
{
    if (textW(f, size, t) <= r.w) { text(r, t, c, f, size, align); return; }
    std::string sx = t;
    while (!sx.empty()) {
        // drop whole UTF-8 characters
        do sx.pop_back(); while (!sx.empty() && ((unsigned char)sx.back() & 0xC0) == 0x80);
        std::string tt = sx + "\xE2\x80\xA6";
        if (textW(f, size, tt.c_str()) <= r.w) { text(r, tt.c_str(), c, f, size, align); return; }
    }
}

void Ui::icon(Icon ic, float cx, float cy, float sz, const Col &c)
{
    float lw = std::max(1.2f, sz * 0.13f);
    switch (ic) {
    case IC_CHEVRON: {
        Pt p[3] = {{X(cx - sz * 0.32f), Y(cy - sz * 0.14f)}, {X(cx), Y(cy + sz * 0.16f)}, {X(cx + sz * 0.32f), Y(cy - sz * 0.14f)}};
        cv_->strokePolyline(p, 3, W(lw), Paint(c));
        break;
    }
    case IC_CHECK: {
        Pt p[3] = {{X(cx - sz * 0.32f), Y(cy + sz * 0.02f)}, {X(cx - sz * 0.08f), Y(cy + sz * 0.26f)}, {X(cx + sz * 0.34f), Y(cy - sz * 0.24f)}};
        cv_->strokePolyline(p, 3, W(lw * 1.2f), Paint(c));
        break;
    }
    case IC_PLUS:
        line(cx - sz * 0.32f, cy, cx + sz * 0.32f, cy, c, lw);
        line(cx, cy - sz * 0.32f, cx, cy + sz * 0.32f, c, lw);
        break;
    case IC_X:
        line(cx - sz * 0.26f, cy - sz * 0.26f, cx + sz * 0.26f, cy + sz * 0.26f, c, lw);
        line(cx - sz * 0.26f, cy + sz * 0.26f, cx + sz * 0.26f, cy - sz * 0.26f, c, lw);
        break;
    case IC_DOTS:
        for (int i = -1; i <= 1; ++i) cv_->fillCircle(X(cx + i * sz * 0.3f), Y(cy), W(sz * 0.09f + 0.4f), Paint(c));
        break;
    case IC_GEAR: {
        // eight teeth around a ring
        cv_->beginPath();
        int n = 8;
        float ro = sz * 0.46f, ri = sz * 0.34f;
        for (int i = 0; i < n * 4; ++i) {
            float a = (float)i / (n * 4) * 6.2831853f - 0.39f;
            float rr = (i % 4 == 0 || i % 4 == 1) ? ro : ri;
            float px = X(cx + std::cos(a) * rr), py = Y(cy + std::sin(a) * rr);
            if (i == 0) cv_->moveTo(px, py);
            else cv_->lineTo(px, py);
        }
        cv_->fillPath(Paint(c));
        cv_->fillCircle(X(cx), Y(cy), W(sz * 0.15f), Paint(th::raised()));
        break;
    }
    case IC_LINK: {
        cv_->strokeCircle(X(cx - sz * 0.15f), Y(cy), W(sz * 0.2f), W(lw), Paint(c));
        cv_->strokeCircle(X(cx + sz * 0.15f), Y(cy), W(sz * 0.2f), W(lw), Paint(c));
        break;
    }
    case IC_PAR:
        // two slashes
        for (int i = 0; i < 2; ++i) {
            float ox = (i ? 0.2f : -0.2f) * sz;
            line(cx + ox - sz * 0.16f, cy + sz * 0.36f, cx + ox + sz * 0.16f, cy - sz * 0.36f, c, lw * 1.15f);
        }
        break;
    case IC_SER:
        // two dashes
        for (int i = 0; i < 2; ++i) {
            float ox = (i ? 0.24f : -0.24f) * sz;
            cv_->fillRoundRect(X(cx + ox - sz * 0.18f), Y(cy - lw * 0.6f), W(sz * 0.36f), W(lw * 1.2f), W(lw * 0.5f), Paint(c));
        }
        break;
    case IC_CHAIN: {
        // two links of a chain, on its side, through each other
        float lh = sz * 0.42f, lwk = sz * 0.6f;
        cv_->strokeRoundRect(X(cx - sz * 0.5f), Y(cy - lh * 0.5f), W(lwk), W(lh), W(lh * 0.5f), W(lw), Paint(c));
        cv_->strokeRoundRect(X(cx + sz * 0.5f - lwk), Y(cy - lh * 0.5f), W(lwk), W(lh), W(lh * 0.5f), W(lw), Paint(c));
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------- widgets
bool Ui::button(const R &r, const char *label, bool active, bool enabled, int style, Icon ic)
{
    if (pass == PASS_DYN) return false;
    bool hv = enabled && hit(r);
    if (hv) cursor = CUR_HAND;
    float rad = 5;
    if (active && enabled) {
        Col a = style == BS_PURPLE ? th::purple() : th::orange();
        Col b = style == BS_PURPLE ? th::purpleLo() : th::orangeLo();
        shadow(r, rad, 7, a.alpha(0.28f));
        fill(r, Paint::vertical(r.y, hv ? a.lighter(0.12f) : a, r.b(), b), rad);
        stroke(r, Paint(a.lighter(0.25f).alpha(0.7f)), rad);
    } else if (style == BS_GHOST) {
        if (hv) fill(r, Paint(th::hover()), rad);
    } else {
        Col top = !enabled ? th::panel() : (hv ? th::hover() : th::raised());
        Col bot = !enabled ? th::panel2() : (hv ? th::raised() : th::raised2());
        fill(r, Paint::vertical(r.y, top, r.b(), bot), rad);
        stroke(r, Paint(hv ? th::borderHi() : th::border()), rad);
        if (enabled) line(r.x + rad, r.y + 1, r.r() - rad, r.y + 1, Col(1, 1, 1, 0.035f));
    }
    Col tc = !enabled ? th::faint() : (active ? (style == BS_PURPLE ? Col(1, 1, 1) : th::orangeInk()) : th::text());
    if (ic != IC_NONE && (!label || !*label)) icon(ic, r.x + r.w * 0.5f, r.y + r.h * 0.5f, std::min(r.h * 0.62f, 14.f), tc);
    else if (ic != IC_NONE) {
        float tw = textW(F_MEDIUM, 12, label);
        float total = tw + 18;
        float x0 = r.x + (r.w - total) * 0.5f;
        icon(ic, x0 + 6, r.y + r.h * 0.5f, 13, tc);
        text(R(x0 + 18, r.y, tw, r.h), label, tc, F_MEDIUM, 12, 0);
    } else
        text(r, label, tc, F_MEDIUM, 12, 1);
    bool clk = hv && lclick && !used;
    if (clk) { used = true; changed = true; lastClick_ = r; }
    return clk;
}

bool Ui::dropdown(const R &r, const char *label, bool enabled, const char *prefix)
{
    if (pass == PASS_DYN) return false;
    bool hv = enabled && hit(r);
    if (hv) cursor = CUR_HAND;
    float rad = 5;
    Col top = !enabled ? th::panel() : (hv ? th::hover() : th::raised());
    Col bot = !enabled ? th::panel2() : (hv ? th::raised() : th::raised2());
    fill(r, Paint::vertical(r.y, top, r.b(), bot), rad);
    stroke(r, Paint(hv ? th::borderHi() : th::border()), rad);
    float x = r.x + 9;
    if (prefix && *prefix) {
        text(R(x, r.y, r.w, r.h), prefix, enabled ? th::dim() : th::faint(), F_REGULAR, 12, 0);
        x += textW(F_REGULAR, 12, prefix) + 5;
    }
    textFit(R(x, r.y, r.r() - 20 - x, r.h), label, enabled ? th::text() : th::faint(), F_MEDIUM, 12, 0);
    icon(IC_CHEVRON, r.r() - 11, r.y + r.h * 0.5f + 0.5f, 10, enabled ? th::orange() : th::faint());
    bool clk = hv && lclick && !used;
    if (clk) { used = true; changed = true; lastClick_ = r; }
    return clk;
}

bool Ui::checkbox(const R &r, bool on, bool enabled)
{
    if (pass == PASS_DYN) return false;
    bool hv = enabled && hit(R(r.x - 3, r.y - 3, r.w + 6, r.h + 6));
    if (on) {
        Col a = enabled ? th::orange() : th::orange().darker(0.45f);
        fill(r, Paint::vertical(r.y, a, r.b(), enabled ? th::orangeLo() : a.darker(0.2f)), 4);
        icon(IC_CHECK, r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.w, th::orangeInk());
    } else {
        fill(r, Paint(th::well()), 4);
        stroke(r, Paint(hv ? th::borderHi() : (enabled ? th::border().lighter(0.08f) : th::border())), 4);
    }
    bool clk = hv && lclick && !used;
    if (clk) { used = true; changed = true; }
    return clk;
}

void Ui::badge(float x, float y, const char *t, const Col &fg, const Col &bg)
{
    if (pass == PASS_DYN) return;
    float w = textW(F_SEMIBOLD, 10, t) + 12;
    fill(R(x, y, w, 16), Paint(bg), 8);
    text(R(x, y, w, 16), t, fg, F_SEMIBOLD, 10, 1);
}

void Ui::badgeDyn(float x, float y, const char *t, const Col &fg, const Col &bg)
{
    if (pass == PASS_STATIC) return;
    float w = textW(F_SEMIBOLD, 10, t) + 12;
    fill(R(x, y, w, 16), Paint(bg), 8);
    text(R(x, y, w, 16), t, fg, F_SEMIBOLD, 10, 1);
}

void Ui::bar(const R &r, int key, int sc, bool enabled, int kind, bool centered)
{
    if (pass == PASS_DYN) return;
    if (!pa) return;
    double v = pa->get(key), lo = pa->lo(key), hi = pa->hi(key);
    double f = std::max(0.0, std::min(1.0, barFrac(sc, v, lo, hi)));
    bool hv = enabled && hit(r);
    bool act = dragKey == key;
    // the track
    fill(r, Paint(th::well()), 4);
    stroke(r, Paint(hv || act ? th::borderHi() : th::border()), 4);
    R in(r.x + 3, r.y + 3, r.w - 6, r.h - 6);
    float fc = centered ? 0.5f : 0.f;
    float xa = in.x + in.w * std::min((float)f, fc), xb = in.x + in.w * std::max((float)f, fc);
    if (enabled) {
        Col c0 = th::orangeLo(), c1 = th::orange();
        if (xb - xa > 0.5f) fill(R(xa, in.y, xb - xa, in.h), Paint::linear(in.x, 0, c0.alpha(0.55f), in.r(), 0, c1), 2.5f);
    } else if (xb - xa > 0.5f) {
        fill(R(xa, in.y, xb - xa, in.h), Paint(th::ghost()), 2.5f);
    }
    if (centered) fill(R(in.x + in.w * 0.5f - 0.5f, r.y + 2, 1, r.h - 4), Paint(th::borderHi()));
    // the handle
    float hx = in.x + in.w * (float)f;
    if (enabled) {
        if (hv || act) shadow(R(hx - 2, in.y - 1, 4, in.h + 2), 2, 6, th::orange().alpha(0.5f));
        fill(R(hx - 1.5f, r.y + 1.5f, 3, r.h - 3), Paint(Col::hex(0xfff3e4)), 1.5f);
    } else {
        fill(R(hx - 1, r.y + 2, 2, r.h - 4), Paint(th::faint()), 1);
    }
    if (!enabled) return;
    if (hv) cursor = CUR_HRESIZE;
    if (hv && lclick && !used) {
        used = true;
        changed = true;
        if (now - lastClickT_ < 0.35 && lastClickKey_ == key) {
            // double-click: back to the default
            pa->setOnce(key, kind == KD_GAIN ? 0 : (kind == KD_TARGET ? -12 : pa->def(key)));
            lastClickT_ = 0;
        } else {
            dragKey = key;
            dragMode_ = 1;
            dragSc_ = sc;
            dragLo_ = lo;
            dragHi_ = hi;
            dragF0_ = barFrac(sc, v, lo, hi);
            dragX0_ = mx();
            dragW_ = std::max(1.f, in.w);
            dragFine_ = fine;
            dragV_ = v;
            pa->begin(key);
            lastClickT_ = now;
            lastClickKey_ = key;
        }
    }
    if (hv && rclick && !used) { used = true; contextMenu(key, kind); }
    if (hv && wheel != 0 && !used) {
        used = true;
        double d = wheel > 0 ? 1 : -1;
        double nv;
        if (sc == SC_LOG) nv = v * std::exp(d * std::log(hi / lo) / (fine ? 1380 : 138) * 10);
        else if (sc == SC_GAIN) nv = v + d * (fine ? 0.01 : 0.1);
        else nv = v + d * (hi - lo) / (fine ? 1380 : 138);
        pa->setOnce(key, std::max(lo, std::min(hi, nv)));
        changed = true;
    }
}

void Ui::valueBox(const R &r, int key, bool enabled, int kind, double step, double fineStep, int align, const char *override,
                  const char *prefix)
{
    if (pass == PASS_DYN) return;
    if (!pa) return;
    bool hv = enabled && hit(r);
    bool editing = editKey == key;
    fill(r, Paint(th::well()), 4);
    if (editing) {
        editRect = r;
        stroke(r, Paint(th::orange()), 4, 1);
        if (editFresh) {
            float tw = textW(F_MEDIUM, 12, editBuf.c_str(), true);
            float x = r.r() - 8 - tw;
            fill(R(x - 2, r.y + 4, tw + 4, r.h - 8), Paint(th::orange().alpha(0.35f)), 2);
        }
        std::string sh = editBuf;
        text(R(r.x + 6, r.y, r.w - 12, r.h), sh.c_str(), th::text(), F_MEDIUM, 12, 2, true);
        // caret
        if (std::fmod(now, 1.0) < 0.6) fill(R(r.r() - 6, r.y + 5, 1.2f, r.h - 10), Paint(th::orange()));
    } else {
        stroke(r, Paint(hv ? th::borderHi() : th::border()), 4);
        char buf[64];
        if (override) snprintf(buf, sizeof buf, "%s", override);
        else pa->format(key, pa->get(key), buf, sizeof buf);
        if (prefix) text(R(r.x + 9, r.y, r.w - 18, r.h), prefix, enabled ? th::dim() : th::faint(), F_REGULAR, 12, 0);
        text(R(r.x + 7, r.y, r.w - 14, r.h), buf, enabled ? th::text() : th::faint(), F_MEDIUM, 12, align, true);
    }
    if (!enabled) return;
    if (hv) cursor = CUR_IBEAM;
    if (hv && lclick && !used) {
        used = true;
        changed = true;
        if (!editing) beginEdit(key);
    }
    if (hv && rclick && !used) { used = true; contextMenu(key, kind); }
    if (hv && wheel != 0 && !used) {
        used = true;
        double v = pa->get(key), lo = pa->lo(key), hi = pa->hi(key), d = wheel > 0 ? 1 : -1;
        double st = fine ? (fineStep > 0 ? fineStep : (hi - lo) / 1380) : (step > 0 ? step : (hi - lo) / 138);
        double nv = kind == KD_LOG ? v * std::exp(d * st * 10) : v + d * st;
        pa->setOnce(key, std::max(lo, std::min(hi, nv)));
        changed = true;
    }
}

void Ui::paramRow(const R &r, const char *label, int key, int sc, float labelW, float boxW, bool enabled, int kind)
{
    if (pass == PASS_DYN) return;
    text(R(r.x, r.y, labelW, r.h), label, enabled ? th::dim() : th::faint(), F_REGULAR, 12, 0);
    float bw = r.w - labelW - boxW - 6;
    bar(R(r.x + labelW, r.y, bw, r.h), key, sc, enabled, kind);
    double lo = pa->lo(key), hi = pa->hi(key);
    double st = sc == SC_LOG ? std::log(hi / lo) / 138 : (hi - lo) / 138;
    double fst = sc == SC_LOG ? std::log(hi / lo) / 1380 : (hi - lo) / 1380;
    valueBox(R(r.r() - boxW, r.y, boxW, r.h), key, enabled, sc == SC_LOG ? KD_LOG : kind, st, fst);
}

void Ui::dragArea(const R &r, int key, double perUnit, double finePerUnit)
{
    if (pass == PASS_DYN) return;
    if (!pa) return;
    bool hv = hit(r);
    if (hv) cursor = CUR_HRESIZE;
    if (hv && lclick && !used) {
        used = true;
        dragKey = key;
        dragMode_ = 2;
        dragV_ = pa->get(key);
        dragLastX_ = mx();
        dragSens_ = perUnit;
        dragFineSens_ = finePerUnit;
        dragLog_ = false;
        pa->begin(key);
        changed = true;
    }
    if (hv && rclick && !used) { used = true; contextMenu(key, KD_OTHER); }
    if (hv && wheel != 0 && !used) {
        used = true;
        double v = pa->get(key), d = wheel > 0 ? 1 : -1;
        pa->setOnce(key, std::max(pa->lo(key), std::min(pa->hi(key), v + d * (fine ? finePerUnit : perUnit) * 10)));
        changed = true;
    }
}

void Ui::slideBox(const R &r, int key, int sc, const Col &col, bool centered, bool enabled, double step, double fineStep, float pxPerUnit)
{
    if (pass == PASS_DYN) return;
    if (!pa) return;
    if (editKey == key) {
        valueBox(r, key, enabled, sc == SC_LOG ? KD_LOG : KD_OTHER, step, fineStep);
        return;
    }
    bool hv = enabled && hit(r);
    double lo = pa->lo(key), hi = pa->hi(key), v = pa->get(key);
    fill(r, Paint(th::well()), 4);
    // the fill: from the left, or from the middle (0, or the centre of the range) when centered
    double f = std::max(0.0, std::min(1.0, barFrac(sc, v, lo, hi)));
    double f0 = 0;
    if (centered) f0 = (lo < 0 && hi > 0) ? barFrac(sc, 0, lo, hi) : 0.5;
    float a = (float)std::min(f, f0), b = (float)std::max(f, f0);
    if (b - a > 0.001f) {
        cv_->pushClip(X(r.x), Y(r.y), W(r.w), W(r.h));
        fill(R(r.x + r.w * a, r.y, std::max(1.f, r.w * (b - a)), r.h), Paint((enabled ? col : th::faint()).alpha(hv || dragKey == key ? 0.42f : 0.3f)));
        cv_->popClip();
    }
    stroke(r, Paint(hv || dragKey == key ? th::borderHi() : th::border()), 4);
    char buf[64];
    pa->format(key, v, buf, sizeof buf);
    text(R(r.x + 6, r.y, r.w - 12, r.h), buf, enabled ? th::text() : th::faint(), F_MEDIUM, 12, 2, true);
    if (!enabled) return;
    if (hv) cursor = CUR_HRESIZE;
    if (hv && lclick && !used) {
        // not yet a drag: it becomes one when the mouse moves, typing when it is let go where it was
        used = true;
        dragKey = key;
        dragMode_ = 3;
        dragX0_ = mx();
        dragLastX_ = mx();
        dragV_ = v;
        dragLog_ = sc == SC_LOG && lo > 0;
        dragSens_ = dragLog_ ? std::log(hi / lo) / std::max(20.f, r.w) : (hi - lo) / std::max(20.f, r.w);
        if (pxPerUnit > 0) dragSens_ = 1.0 / pxPerUnit;
        dragFineSens_ = dragSens_ * (pxPerUnit > 0 ? 0.25 : 0.1);
        changed = true;
    }
    if (hv && rclick && !used) { used = true; contextMenu(key, sc == SC_LOG ? KD_LOG : KD_OTHER); }
    if (hv && wheel != 0 && !used) {
        used = true;
        double d = wheel > 0 ? 1 : -1;
        double st = fine ? (fineStep > 0 ? fineStep : (hi - lo) / 1380) : (step > 0 ? step : (hi - lo) / 138);
        double nv = sc == SC_LOG ? v * std::exp(d * st * 10) : v + d * st;
        pa->setOnce(key, std::max(lo, std::min(hi, nv)));
        changed = true;
    }
}

// ---------------------------------------------------------------- typing
void Ui::beginEdit(int key)
{
    if (editKey && editKey != key) commitEdit();
    editKey = key;
    editFresh = true;
    char buf[64];
    double v = pa->get(key);
    if (v <= -100 && pa->lo(key) <= -100) snprintf(buf, sizeof buf, "-inf");
    else snprintf(buf, sizeof buf, "%.2f", v);
    editBuf = buf;
    changed = true;
}

void Ui::commitEdit()
{
    if (editKey && pa) {
        double v;
        if (pa->parse(editKey, editBuf.c_str(), &v)) pa->setOnce(editKey, std::max(pa->lo(editKey), std::min(pa->hi(editKey), v)));
    }
    editKey = 0;
    changed = true;
}

void Ui::editInput()
{
    // a click anywhere else commits
    if (lclick && !hitRaw(editRect)) commitEdit();
    if (!editKey) return;
    for (int k : in_.keys) {
        if (k == K_ENTER) { commitEdit(); return; }
        if (k == K_ESC) { editKey = 0; changed = true; return; }
        if (k == K_BACK) {
            if (editFresh) editBuf.clear();
            else if (!editBuf.empty()) editBuf.pop_back();
            editFresh = false;
            changed = true;
        }
    }
    for (uint32_t ch : in_.chars) {
        if ((ch >= '0' && ch <= '9') || ch == '.' || ch == ',' || ch == '-' || ch == '+' || ch == 'i' || ch == 'n' || ch == 'f' ||
            ch == 'I' || ch == 'N' || ch == 'F' || ch == 'k' || ch == 'K') {
            if (editFresh) { editBuf.clear(); editFresh = false; }
            if (editBuf.size() < 12) editBuf += (char)ch;
            changed = true;
        }
    }
}

// ---------------------------------------------------------------- menus
void Ui::contextMenu(int key, int kind)
{
    std::vector<MenuItem> m;
    if (kind == KD_GAIN) {
        m = {MenuItem::item("Reset to 0 dB", 1), MenuItem::item("-inf (silent)", 2), MenuItem::item("Type a value\xE2\x80\xA6", 3)};
        menu(m, mx(), my(), [this, key](int r) {
            if (r == 1) pa->setOnce(key, 0);
            if (r == 2) pa->setOnce(key, -100);
            if (r == 3) beginEdit(key);
        });
    } else if (kind == KD_TIME) {
        m = {MenuItem::item("Reset to 20 ms", 1), MenuItem::item("Instant (0 ms)", 2), MenuItem::item("Type a value\xE2\x80\xA6", 3)};
        menu(m, mx(), my(), [this, key](int r) {
            if (r == 1) pa->setOnce(key, 20);
            if (r == 2) pa->setOnce(key, 0);
            if (r == 3) beginEdit(key);
        });
    } else if (kind == KD_TARGET) {
        m = {MenuItem::item("-12 dBFS", 1), MenuItem::item("-18 dBFS", 2), MenuItem::item("-6 dBFS", 3), MenuItem::sep(),
             MenuItem::item("Type a value\xE2\x80\xA6", 4)};
        menu(m, mx(), my(), [this, key](int r) {
            if (r == 1) pa->setOnce(key, -12);
            if (r == 2) pa->setOnce(key, -18);
            if (r == 3) pa->setOnce(key, -6);
            if (r == 4) beginEdit(key);
        });
    } else {
        m = {MenuItem::item("Reset to default", 1), MenuItem::item("Type a value\xE2\x80\xA6", 2)};
        menu(m, mx(), my(), [this, key](int r) {
            if (r == 1) pa->setOnce(key, pa->def(key));
            if (r == 2) beginEdit(key);
        });
    }
}

static const float MI_H = 26, MI_SEP = 9, MI_PADX = 12;

void Ui::layoutLevel(Level &L, float x, float y, float flipY)
{
    float w = 0, h = 8;
    bool anySub = false;
    for (auto &it : L.items) {
        if (it.separator) { h += MI_SEP; continue; }
        w = std::max(w, textW(F_REGULAR, 12.5f, it.label.c_str()));
        if (!it.sub.empty()) anySub = true;
        h += MI_H;
    }
    w += MI_PADX * 2 + 22 + (anySub ? 16 : 0);
    w = std::max(w, L.w / s);   // a minimum width (from the anchor)
    L.w = w * s;
    L.h = h * s;
    // inside the window
    float cw = (float)cv_->width(), ch = (float)cv_->height();
    if (x + L.w > cw - 2) x = std::max(2.f, cw - 2 - L.w);
    if (y + L.h > ch - 2) {
        // no room below: above the widget that opened it, if it fits there
        if (flipY >= 0 && flipY - L.h >= 2) y = flipY - L.h;
        else y = std::max(2.f, ch - 2 - L.h);
    }
    L.x = x;
    L.y = y;
}

void Ui::menu(std::vector<MenuItem> items, float x, float y, std::function<void(int)> cb, float minW)
{
    commitEdit();
    levels_.clear();
    Level L;
    L.items = std::move(items);
    L.w = minW * s;
    // opened from a widget just above (y): it can flip above that widget
    bool fromWidget = std::fabs(y - (lastClick_.b() + 4)) < 1 && x >= lastClick_.x - 1 && x <= lastClick_.r();
    layoutLevel(L, X(x), Y(y), fromWidget ? Y(lastClick_.y - 4) : -1);
    levels_.push_back(std::move(L));
    menuCb_ = std::move(cb);
    used = true;
    changed = true;
}

void Ui::menuInput()
{
    float pmx = in_.mx, pmy = in_.my;
    int over = -1;
    for (int l = (int)levels_.size() - 1; l >= 0; --l) {
        Level &L = levels_[l];
        if (pmx >= L.x && pmx < L.x + L.w && pmy >= L.y && pmy < L.y + L.h) { over = l; break; }
    }
    for (int k : in_.keys)
        if (k == K_ESC) { levels_.clear(); used = true; changed = true; return; }
    if (over < 0) {
        if (lclick || rclick) { levels_.clear(); used = true; changed = true; }
        // the hover stays on the open submenu's item
        return;
    }
    Level &L = levels_[over];
    float y = L.y + 4 * s;
    int hv = -1;
    float hy = 0;
    for (int i = 0; i < (int)L.items.size(); ++i) {
        const MenuItem &it = L.items[i];
        float ih = (it.separator ? MI_SEP : MI_H) * s;
        if (!it.separator && pmy >= y && pmy < y + ih) { hv = i; hy = y; }
        y += ih;
    }
    if (hv != L.hover) { L.hover = hv; L.hoverT = now; changed = true; }
    // a submenu opens on hover
    if (hv >= 0 && !L.items[hv].sub.empty() && L.items[hv].enabled) {
        if ((int)levels_.size() <= over + 1 || levels_[over + 1].subOf != hv) {
            levels_.resize(over + 1);
            Level S;
            S.subOf = hv;
            S.items = levels_[over].items[hv].sub;
            float sx = levels_[over].x + levels_[over].w - 2 * s;
            layoutLevel(S, sx, hy - 4 * s);
            if (S.x < levels_[over].x + levels_[over].w - 4 * s) {   // no room on the right: to the left
                float lx = levels_[over].x - S.w + 2 * s;
                S.x = std::max(2.f, lx);
            }
            levels_.push_back(std::move(S));
            changed = true;
        }
    } else if (hv >= 0 && (int)levels_.size() > over + 1) {
        levels_.resize(over + 1);
        changed = true;
    }
    if ((lclick || rclick) && hv >= 0) {
        MenuItem &it = levels_[over].items[hv];
        used = true;
        if (it.enabled && it.sub.empty() && it.id != 0 && it.keepOpen) {
            // a check box or a button that leaves the menu open
            int id = it.id;
            if (it.toggle) {
                it.checked = !it.checked;
                // and the copy the parent keeps (a submenu opened again shows it)
                if (over > 0 && levels_[over].subOf >= 0) {
                    MenuItem &pa = levels_[over - 1].items[levels_[over].subOf];
                    if (hv < (int)pa.sub.size()) pa.sub[hv].checked = it.checked;
                }
            }
            if (it.clears) {
                std::function<void(std::vector<MenuItem> &)> clr = [&](std::vector<MenuItem> &v) {
                    for (auto &m : v) {
                        if (m.toggle) m.checked = false;
                        clr(m.sub);
                    }
                };
                for (auto &L2 : levels_) clr(L2.items);
            }
            changed = true;
            if (menuCb_) menuCb_(id);
        } else if (it.enabled && it.sub.empty() && it.id != 0) {
            int id = it.id;
            auto cb = menuCb_;
            levels_.clear();
            changed = true;
            if (cb) cb(id);
        }
    } else if (lclick || rclick) {
        used = true;
    }
    if (in_.wheel != 0) used = true;
}

void Ui::menuDraw()
{
    for (size_t l = 0; l < levels_.size(); ++l) {
        Level &L = levels_[l];
        float x = L.x, y = L.y, w = L.w, h = L.h;
        cv_->shadow(x, y + 3 * s, w, h, 8 * s, 14 * s, Col(0, 0, 0, 0.6f));
        cv_->fillRoundRect(x, y, w, h, 7 * s, Paint::vertical(y, th::raised(), y + h, th::panel()));
        cv_->strokeRoundRect(x, y, w, h, 7 * s, std::max(1.f, s), Paint(th::borderHi()));
        float iy = y + 4 * s;
        for (int i = 0; i < (int)L.items.size(); ++i) {
            const MenuItem &it = L.items[i];
            if (it.separator) {
                cv_->fillRect(x + 8 * s, iy + MI_SEP * s * 0.5f, w - 16 * s, std::max(1.f, s * 0.8f), Paint(th::border()));
                iy += MI_SEP * s;
                continue;
            }
            float ih = MI_H * s;
            if (it.asButton) {
                // a button across the menu
                bool hvb = L.hover == i && it.enabled;
                float bx = x + 8 * s, bw = w - 16 * s, by = iy + 2 * s, bh = ih - 4 * s;
                cv_->fillRoundRect(bx, by, bw, bh, 5 * s, Paint::vertical(by, hvb ? th::hover() : th::raised2(), by + bh, hvb ? th::raised() : th::panel2()));
                cv_->strokeRoundRect(bx, by, bw, bh, 5 * s, std::max(1.f, s), Paint(hvb ? th::borderHi() : th::border()));
                Font &F = font(F_MEDIUM);
                float px = 12.5f * s;
                float tw = F.measure(px, it.label.c_str());
                cv_->text(F, px, bx + (bw - tw) * 0.5f, iy + (ih + F.capHeight(px)) * 0.5f, it.label.c_str(), it.enabled ? th::text() : th::faint());
                iy += ih;
                continue;
            }
            bool open = l + 1 < levels_.size() && !it.sub.empty() && L.hover == i;
            if ((L.hover == i || open) && it.enabled)
                cv_->fillRoundRect(x + 4 * s, iy + 1 * s, w - 8 * s, ih - 2 * s, 5 * s, Paint(th::orange().alpha(0.16f)));
            Col tc = it.enabled ? (L.hover == i ? th::text().lighter(0.2f) : th::text()) : th::faint();
            if (it.toggle) {
                // a pick-several list: just the tick, nothing when unticked
                if (it.checked) icon(IC_CHECK, (x + (MI_PADX + 4) * s - ox) / s, (iy + ih * 0.5f - oy) / s, 12, th::orange());
            } else if (it.checked) {
                float cx = (x + (MI_PADX + 4) * s - ox) / s, cy = (iy + ih * 0.5f - oy) / s;
                icon(IC_CHECK, cx, cy, 12, th::orange());
            }
            Font &F = font(F_REGULAR);
            float px = 12.5f * s;
            float base = iy + (ih + F.capHeight(px)) * 0.5f;
            cv_->text(F, px, x + (MI_PADX + 18) * s, base, it.label.c_str(), tc);
            if (!it.sub.empty()) {
                float cx = x + w - 12 * s, cy = iy + ih * 0.5f;
                Pt p[3] = {{cx - 2.5f * s, cy - 4 * s}, {cx + 1.5f * s, cy}, {cx - 2.5f * s, cy + 4 * s}};
                cv_->strokePolyline(p, 3, std::max(1.2f, 1.4f * s), Paint(it.enabled ? th::dim() : th::faint()));
            }
            iy += ih;
        }
    }
}

// ---------------------------------------------------------------- prompt
void Ui::prompt(const std::string &title, const std::string &initial, std::function<void(const std::string &)> cb, const std::string &ok)
{
    commitEdit();
    levels_.clear();
    promptOpen_ = true;
    promptConfirm_ = false;
    promptTitle_ = title;
    promptBuf_ = initial;
    promptOk_ = ok;
    promptCb_ = std::move(cb);
    used = true;
    changed = true;
}

void Ui::confirm(const std::string &title, const std::string &ok, std::function<void()> cb)
{
    prompt(title, "", [cb](const std::string &) { if (cb) cb(); }, ok);
    promptConfirm_ = true;
    promptBuf_ = " ";   // not empty: OK is lit
}

void Ui::promptInput()
{
    for (int k : in_.keys) {
        if (k == K_ESC) { promptOpen_ = false; used = true; changed = true; return; }
        if (k == K_ENTER) {
            promptOpen_ = false;
            used = true;
            changed = true;
            if (!promptBuf_.empty() && promptCb_) { auto cb = std::move(promptCb_); promptCb_ = nullptr; cb(promptBuf_); }
            return;
        }
        if (promptConfirm_) continue;
        if (k == K_BACK && !promptBuf_.empty()) {
            do promptBuf_.pop_back(); while (!promptBuf_.empty() && ((unsigned char)promptBuf_.back() & 0xC0) == 0x80);
            changed = true;
        }
    }
    for (uint32_t ch : in_.chars) {
        if (promptConfirm_ || ch < 32 || ch == '[' || ch == ']' || promptBuf_.size() > 60) continue;
        // UTF-8 encode
        if (ch < 0x80) promptBuf_ += (char)ch;
        else if (ch < 0x800) { promptBuf_ += (char)(0xC0 | (ch >> 6)); promptBuf_ += (char)(0x80 | (ch & 63)); }
        else { promptBuf_ += (char)(0xE0 | (ch >> 12)); promptBuf_ += (char)(0x80 | ((ch >> 6) & 63)); promptBuf_ += (char)(0x80 | (ch & 63)); }
        changed = true;
    }
    // buttons are handled in promptDraw (they need the layout); the click is taken either way
    if (lclick || rclick || in_.wheel != 0) used = true;
}

void Ui::promptDraw()
{
    float cw = (float)cv_->width() / s, ch = (float)cv_->height() / s;
    float ox0 = ox, oy0 = oy;
    ox = 0;
    oy = 0;   // the dialog sits in the window, not in the content
    fill(R(0, 0, cw, ch), Paint(Col(0, 0, 0, 0.55f)));
    float w = 380, h = promptConfirm_ ? 112 : 150;
    R d((cw - w) * 0.5f, (ch - h) * 0.5f, w, h);
    shadow(d, 10, 22, Col(0, 0, 0, 0.7f));
    fill(d, Paint::vertical(d.y, th::raised(), d.b(), th::panel()), 10);
    stroke(d, Paint(th::borderHi()), 10);
    text(R(d.x + 20, d.y + 14, w - 40, 22), promptTitle_.c_str(), th::text(), F_SEMIBOLD, 13.5f, 0);
    if (!promptConfirm_) {
        R f(d.x + 20, d.y + 48, w - 40, 32);
        fill(f, Paint(th::well()), 6);
        stroke(f, Paint(th::orange()), 6);
        float tw = textW(F_REGULAR, 13, promptBuf_.c_str());
        text(R(f.x + 10, f.y, f.w - 20, f.h), promptBuf_.c_str(), th::text(), F_REGULAR, 13, 0);
        if (std::fmod(now, 1.0) < 0.6) fill(R(f.x + 11 + std::min(tw, f.w - 22), f.y + 8, 1.3f, f.h - 16), Paint(th::orange()));
    }
    R ok(d.r() - 20 - 100, d.b() - 20 - 30, 100, 30), cancel(ok.x - 10 - 90, ok.y, 90, 30);
    bool saveLit = !promptBuf_.empty();
    // draw the buttons without the busy() guard (promptInput took the click for the dialog)
    bool was = promptOpen_;
    promptOpen_ = false;
    used = false;
    bool clickedOk = button(ok, promptOk_.c_str(), saveLit, saveLit);
    bool clickedCancel = button(cancel, "Cancel");
    used = true;
    promptOpen_ = was;
    ox = ox0;
    oy = oy0;
    if (clickedOk && saveLit) {
        promptOpen_ = false;
        if (promptCb_) { auto cb = std::move(promptCb_); promptCb_ = nullptr; cb(promptBuf_); }
    } else if (clickedCancel) {
        promptOpen_ = false;
    }
}

} // namespace gui
} // namespace ts
