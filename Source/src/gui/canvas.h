// A small software 2D renderer: anti-aliased rounded rectangles (with gradients, borders and soft
// shadows), paths and strokes (an exact-area coverage rasterizer), and text. It draws into a 32-bit
// buffer (0xAARRGGBB, alpha ignored on screen) that the platform window copies to the screen, so
// every TwoStorms window looks the same on Linux and Windows and needs no graphics library.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace gui {

struct Col {
    float r = 0, g = 0, b = 0, a = 1;
    Col() {}
    Col(float r_, float g_, float b_, float a_ = 1) : r(r_), g(g_), b(b_), a(a_) {}
    static Col hex(uint32_t rgb, float a = 1)
    {
        return Col(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
    }
    Col alpha(float x) const { return Col(r, g, b, a * x); }
    Col mix(const Col &o, float t) const { return Col(r + (o.r - r) * t, g + (o.g - g) * t, b + (o.b - b) * t, a + (o.a - a) * t); }
    Col lighter(float t) const { return mix(Col(1, 1, 1, a), t); }
    Col darker(float t) const { return mix(Col(0, 0, 0, a), t); }
};

// a fill: one color, or a gradient between two points
struct Paint {
    Col c0, c1;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool grad = false;
    Paint() {}
    Paint(const Col &c) : c0(c), c1(c) {}
    static Paint linear(float x0, float y0, const Col &a, float x1, float y1, const Col &b)
    {
        Paint p;
        p.c0 = a; p.c1 = b; p.x0 = x0; p.y0 = y0; p.x1 = x1; p.y1 = y1; p.grad = true;
        return p;
    }
    static Paint vertical(float y0, const Col &a, float y1, const Col &b) { return linear(0, y0, a, 0, y1, b); }
};

struct Pt { float x, y; };

class Font;

class Canvas {
public:
    void resize(int w, int h);
    int width() const { return w_; }
    int height() const { return h_; }
    uint32_t *pixels() { return px_.data(); }
    const uint32_t *pixels() const { return px_.data(); }

    // clipping (pixels, integer edges), nested
    void pushClip(float x, float y, float w, float h);
    void popClip();
    void resetClip();
    bool clipped(float x, float y, float w, float h) const;   // entirely outside the clip?

    void clear(const Col &c);
    // all coordinates in pixels
    void fillRect(float x, float y, float w, float h, const Paint &p);
    void fillRoundRect(float x, float y, float w, float h, float r, const Paint &p);
    void strokeRoundRect(float x, float y, float w, float h, float r, float lw, const Paint &p);
    // a soft shadow (or glow) under a rounded rectangle; blur in pixels. Only the band around the edge is drawn
    // (a panel covers the rest) unless solid (a glow behind something see-through, like text)
    void shadow(float x, float y, float w, float h, float r, float blur, const Col &c, bool solid = false);

    // paths: contours of points, filled with the nonzero rule
    void beginPath();
    void moveTo(float x, float y);
    void lineTo(float x, float y);
    void closePath();
    void arc(float cx, float cy, float r, float a0, float a1);   // radians, adds to the current contour
    void fillPath(const Paint &p);
    // a stroke along points (round joins and caps)
    void strokePolyline(const Pt *pts, int n, float lw, const Paint &p, bool closed = false);
    void line(float x0, float y0, float x1, float y1, float lw, const Col &c);
    void fillCircle(float cx, float cy, float r, const Paint &p);
    void strokeCircle(float cx, float cy, float r, float lw, const Paint &p);

    // text, baseline at y; returns the advance
    float text(Font &f, float size, float x, float y, const char *s, const Col &c, bool tabular = false);

    void blendSpan(int y, int x0, int x1, const float *cov, const Paint &p);   // cov: x0..x1 coverage
    // graphs: one-pixel columns from tops[i] down to bottom (x = x0 + i), colored by a vertical paint
    void columns(int x0, const float *tops, int n, float bottom, const Paint &p);
    // the same, from the top edge down to bottoms[i]
    void columnsDown(int x0, const float *bottoms, int n, float top, const Paint &p);
    // a thin line (up to ~3 px) through points: each segment as a capsule, no big scratch buffer
    void strokeThin(const Pt *pts, int n, float lw, const Col &c);
    // a picture drawn once and kept (premultiplied 0xAARRGGBB, w x h), over what's there, at (x, y) in pixels
    void blit(int x, int y, int w, int h, const uint32_t *img);

private:
    int w_ = 0, h_ = 0;
    std::vector<uint32_t> px_;
    struct Clip { int x0, y0, x1, y1; };
    Clip clip_{0, 0, 0, 0};
    std::vector<Clip> clipStack_;
    // path
    std::vector<std::vector<Pt>> contours_;
    // rasterizer scratch
    std::vector<float> acc_, cov_;
    std::vector<Col> hcache_;
    void rasterize(const std::vector<std::vector<Pt>> &cs, const Paint &p);
    inline void blendPx(uint32_t &d, const Col &c, float a);
    Col paintAt(const Paint &p, float x, float y) const;
};

} // namespace gui
} // namespace ts
