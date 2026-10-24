#include "canvas.h"
#include "font.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ts {
namespace gui {

static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline uint32_t pack(const Col &c)
{
    auto u = [](float v) { int x = (int)(clamp01(v) * 255.f + 0.5f); return (uint32_t)x; };
    return 0xff000000u | (u(c.r) << 16) | (u(c.g) << 8) | u(c.b);
}

void Canvas::resize(int w, int h)
{
    w_ = std::max(1, w);
    h_ = std::max(1, h);
    px_.assign((size_t)w_ * h_, 0xff000000u);
    clipStack_.clear();
    resetClip();
}

void Canvas::resetClip() { clip_ = {0, 0, w_, h_}; }

void Canvas::pushClip(float x, float y, float w, float h)
{
    clipStack_.push_back(clip_);
    Clip c;
    c.x0 = std::max(clip_.x0, (int)std::floor(x));
    c.y0 = std::max(clip_.y0, (int)std::floor(y));
    c.x1 = std::min(clip_.x1, (int)std::ceil(x + w));
    c.y1 = std::min(clip_.y1, (int)std::ceil(y + h));
    if (c.x1 < c.x0) c.x1 = c.x0;
    if (c.y1 < c.y0) c.y1 = c.y0;
    clip_ = c;
}

void Canvas::popClip()
{
    if (!clipStack_.empty()) { clip_ = clipStack_.back(); clipStack_.pop_back(); }
}

bool Canvas::clipped(float x, float y, float w, float h) const
{
    return x + w < clip_.x0 || y + h < clip_.y0 || x >= clip_.x1 || y >= clip_.y1;
}

void Canvas::clear(const Col &c)
{
    uint32_t v = pack(c);
    for (int y = clip_.y0; y < clip_.y1; ++y) {
        uint32_t *row = &px_[(size_t)y * w_];
        std::fill(row + clip_.x0, row + clip_.x1, v);
    }
}

inline void Canvas::blendPx(uint32_t &d, const Col &c, float a)
{
    a *= c.a;
    if (a <= 0.002f) return;
    if (a >= 0.998f) { d = pack(c); return; }
    int ia = (int)(a * 256.f);
    int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    int sr = (int)(clamp01(c.r) * 255.f + 0.5f), sg = (int)(clamp01(c.g) * 255.f + 0.5f), sb = (int)(clamp01(c.b) * 255.f + 0.5f);
    dr += ((sr - dr) * ia) >> 8;
    dg += ((sg - dg) * ia) >> 8;
    db += ((sb - db) * ia) >> 8;
    d = 0xff000000u | ((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db;
}

Col Canvas::paintAt(const Paint &p, float x, float y) const
{
    if (!p.grad) return p.c0;
    float dx = p.x1 - p.x0, dy = p.y1 - p.y0;
    float L = dx * dx + dy * dy;
    float t = L > 0 ? ((x - p.x0) * dx + (y - p.y0) * dy) / L : 0;
    return p.c0.mix(p.c1, clamp01(t));
}

void Canvas::blendSpan(int y, int x0, int x1, const float *cov, const Paint &p)
{
    if (y < clip_.y0 || y >= clip_.y1) return;
    int a = std::max(x0, clip_.x0), b = std::min(x1, clip_.x1);
    uint32_t *row = &px_[(size_t)y * w_];
    bool vert = p.grad && p.x0 == p.x1;
    Col rc = vert || !p.grad ? paintAt(p, 0, y + 0.5f) : p.c0;
    for (int x = a; x < b; ++x) {
        float c = cov[x - x0];
        if (c <= 0) continue;
        if (p.grad && !vert) rc = paintAt(p, x + 0.5f, y + 0.5f);
        blendPx(row[x], rc, c);
    }
}

// ------------------------------------------------------------------ rectangles
void Canvas::fillRect(float x, float y, float w, float h, const Paint &p)
{
    if (w <= 0 || h <= 0) return;
    float x1 = x + w, y1 = y + h;
    int ix0 = std::max(clip_.x0, (int)std::floor(x)), ix1 = std::min(clip_.x1, (int)std::ceil(x1));
    int iy0 = std::max(clip_.y0, (int)std::floor(y)), iy1 = std::min(clip_.y1, (int)std::ceil(y1));
    if (ix0 >= ix1 || iy0 >= iy1) return;
    bool vert = p.grad && p.x0 == p.x1;
    bool horiz = p.grad && !vert && p.y0 == p.y1;
    // fully covered columns
    int fx0 = std::max(ix0, (int)std::ceil(x)), fx1 = std::min(ix1, (int)std::floor(x1));
    // a horizontal gradient is the same on every row: its colors once
    std::vector<Col> &hc = hcache_;
    if (horiz) {
        hc.resize(ix1 - ix0);
        for (int px = ix0; px < ix1; ++px) hc[px - ix0] = paintAt(p, px + 0.5f, 0);
    }
    for (int py = iy0; py < iy1; ++py) {
        float rc = std::min((float)py + 1, y1) - std::max((float)py, y);
        if (rc <= 0) continue;
        Col c = (!p.grad || vert) ? paintAt(p, 0, py + 0.5f) : p.c0;
        uint32_t *row = &px_[(size_t)py * w_];
        auto edge = [&](int px) {
            float cc = std::min((float)px + 1, x1) - std::max((float)px, x);
            if (cc <= 0) return;
            Col cx = horiz ? hc[px - ix0] : (p.grad && !vert ? paintAt(p, px + 0.5f, py + 0.5f) : c);
            blendPx(row[px], cx, rc * cc);
        };
        for (int px = ix0; px < std::min(fx0, ix1); ++px) edge(px);
        if (fx1 > fx0) {
            if (horiz) {
                for (int px = fx0; px < fx1; ++px) blendPx(row[px], hc[px - ix0], rc);
            } else if (p.grad && !vert) {
                for (int px = fx0; px < fx1; ++px) blendPx(row[px], paintAt(p, px + 0.5f, py + 0.5f), rc);
            } else if (rc >= 0.999f && c.a >= 0.998f) {
                std::fill(row + fx0, row + fx1, pack(c));
            } else {
                // the same blend across the row: integer math once
                float a = rc * c.a;
                int ia = (int)(a * 256.f);
                int sr = (int)(clamp01(c.r) * 255.f + 0.5f), sg = (int)(clamp01(c.g) * 255.f + 0.5f), sb = (int)(clamp01(c.b) * 255.f + 0.5f);
                if (a > 0.002f)
                    for (int px = fx0; px < fx1; ++px) {
                        uint32_t d = row[px];
                        int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
                        dr += ((sr - dr) * ia) >> 8;
                        dg += ((sg - dg) * ia) >> 8;
                        db += ((sb - db) * ia) >> 8;
                        row[px] = 0xff000000u | ((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db;
                    }
            }
        }
        for (int px = std::max(fx1, fx0); px < ix1; ++px) if (px >= std::min(fx0, ix1)) edge(px);
    }
}

// signed distance to a rounded box (negative inside)
static inline float sdBox(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    float qx = std::fabs(px - cx) - (hw - r), qy = std::fabs(py - cy) - (hh - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float outside = std::sqrt(ox * ox + oy * oy);
    float inside = std::min(std::max(qx, qy), 0.f);
    return outside + inside - r;
}

void Canvas::fillRoundRect(float x, float y, float w, float h, float r, const Paint &p)
{
    if (w <= 0 || h <= 0) return;
    r = std::min(r, std::min(w, h) * 0.5f);
    if (r < 0.6f) { fillRect(x, y, w, h, p); return; }
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    int ix0 = std::max(clip_.x0, (int)std::floor(x)), ix1 = std::min(clip_.x1, (int)std::ceil(x + w));
    int iy0 = std::max(clip_.y0, (int)std::floor(y)), iy1 = std::min(clip_.y1, (int)std::ceil(y + h));
    if (ix0 >= ix1 || iy0 >= iy1) return;
    bool vert = p.grad && p.x0 == p.x1;
    for (int py = iy0; py < iy1; ++py) {
        float fy = py + 0.5f;
        Col c = (!p.grad || vert) ? paintAt(p, 0, fy) : p.c0;
        uint32_t pc = pack(c);
        bool opaque = c.a >= 0.998f && (!p.grad || vert);
        uint32_t *row = &px_[(size_t)py * w_];
        int a = ix0;
        for (; a < ix1; ++a) {
            float cov = clamp01(0.5f - sdBox(a + 0.5f, fy, cx, cy, hw, hh, r));
            if (cov >= 0.999f) break;
            if (p.grad && !vert) c = paintAt(p, a + 0.5f, fy);
            blendPx(row[a], c, cov);
        }
        int b = ix1 - 1;
        for (; b >= a; --b) {
            float cov = clamp01(0.5f - sdBox(b + 0.5f, fy, cx, cy, hw, hh, r));
            if (cov >= 0.999f) break;
            if (p.grad && !vert) c = paintAt(p, b + 0.5f, fy);
            blendPx(row[b], c, cov);
        }
        for (int px = a; px <= b; ++px) {
            if (opaque) row[px] = pc;
            else {
                if (p.grad && !vert) c = paintAt(p, px + 0.5f, fy);
                blendPx(row[px], c, 1);
            }
        }
    }
}

void Canvas::strokeRoundRect(float x, float y, float w, float h, float r, float lw, const Paint &p)
{
    if (w <= 0 || h <= 0 || lw <= 0) return;
    r = std::min(r, std::min(w, h) * 0.5f);
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    int ix0 = std::max(clip_.x0, (int)std::floor(x)), ix1 = std::min(clip_.x1, (int)std::ceil(x + w));
    int iy0 = std::max(clip_.y0, (int)std::floor(y)), iy1 = std::min(clip_.y1, (int)std::ceil(y + h));
    float band = lw + std::max(r, 1.f) + 1;
    for (int py = iy0; py < iy1; ++py) {
        float fy = py + 0.5f;
        uint32_t *row = &px_[(size_t)py * w_];
        bool edgeRow = fy < y + band || fy > y + h - band;
        auto doPx = [&](int px) {
            float sd = sdBox(px + 0.5f, fy, cx, cy, hw, hh, std::max(r, 0.01f));
            float ring = std::fabs(sd + lw * 0.5f) - lw * 0.5f;
            float cov = clamp01(0.5f - ring);
            if (cov > 0) blendPx(row[px], paintAt(p, px + 0.5f, fy), cov);
        };
        if (edgeRow) {
            for (int px = ix0; px < ix1; ++px) doPx(px);
        } else {
            int l1 = std::min(ix1, (int)std::ceil(x + band)), r0 = std::max(l1, (int)std::floor(x + w - band));
            for (int px = ix0; px < l1; ++px) doPx(px);
            for (int px = r0; px < ix1; ++px) doPx(px);
        }
    }
}

void Canvas::shadow(float x, float y, float w, float h, float r, float blur, const Col &c, bool solid)
{
    if (blur < 0.5f) { fillRoundRect(x, y, w, h, r, Paint(c)); return; }
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    r = std::min(r, std::min(w, h) * 0.5f);
    int ix0 = std::max(clip_.x0, (int)std::floor(x - blur)), ix1 = std::min(clip_.x1, (int)std::ceil(x + w + blur));
    int iy0 = std::max(clip_.y0, (int)std::floor(y - blur)), iy1 = std::min(clip_.y1, (int)std::ceil(y + h + blur));
    // A shadow sits under its panel: inside the panel (deeper than the blur) nothing shows, so only
    // the band around the edge is drawn.
    float band = blur + r + 1;
    auto px1 = [&](uint32_t *row, int px, float fy) {
        float sd = sdBox(px + 0.5f, fy, cx, cy, hw, hh, r);
        if (sd >= blur) return;
        if (sd < -blur) {
            if (solid) blendPx(row[px], c, 1);
            return;
        }
        float t = clamp01((sd + blur) / (2 * blur));
        float a = 1 - t * t * (3 - 2 * t);
        blendPx(row[px], c, a);
    };
    for (int py = iy0; py < iy1; ++py) {
        float fy = py + 0.5f;
        uint32_t *row = &px_[(size_t)py * w_];
        if (solid || fy < y + band || fy > y + h - band) {
            for (int px = ix0; px < ix1; ++px) px1(row, px, fy);
        } else {
            int l1 = std::min(ix1, (int)std::ceil(x + band)), r0 = std::max(l1, (int)std::floor(x + w - band));
            for (int px = ix0; px < l1; ++px) px1(row, px, fy);
            for (int px = r0; px < ix1; ++px) px1(row, px, fy);
        }
    }
}

// ------------------------------------------------------------------ paths
void Canvas::beginPath() { contours_.clear(); }
void Canvas::moveTo(float x, float y) { contours_.push_back({{x, y}}); }
void Canvas::lineTo(float x, float y)
{
    if (contours_.empty()) contours_.push_back({});
    contours_.back().push_back({x, y});
}
void Canvas::closePath() {}
void Canvas::arc(float cx, float cy, float r, float a0, float a1)
{
    int n = std::max(4, (int)(std::fabs(a1 - a0) * std::max(r, 1.f) * 0.5f) + 2);
    n = std::min(n, 256);
    for (int i = 0; i <= n; ++i) {
        float a = a0 + (a1 - a0) * i / n;
        lineTo(cx + std::cos(a) * r, cy + std::sin(a) * r);
    }
}

void Canvas::fillPath(const Paint &p) { rasterize(contours_, p); }

// Exact-area coverage accumulation (the font-rs approach): each edge adds its signed area to the
// cells it crosses; a running sum along each row gives the coverage.
void Canvas::rasterize(const std::vector<std::vector<Pt>> &cs, const Paint &p)
{
    float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
    for (auto &c : cs)
        for (auto &q : c) { bx0 = std::min(bx0, q.x); by0 = std::min(by0, q.y); bx1 = std::max(bx1, q.x); by1 = std::max(by1, q.y); }
    if (bx0 > bx1) return;
    int ix0 = std::max(clip_.x0, (int)std::floor(bx0)) - 2, ix1 = std::min(clip_.x1, (int)std::ceil(bx1));
    int iy0 = std::max(clip_.y0, (int)std::floor(by0)), iy1 = std::min(clip_.y1, (int)std::ceil(by1));
    if (ix1 <= ix0 + 2 || iy1 <= iy0) return;
    int W = ix1 - ix0 + 3, H = iy1 - iy0;
    acc_.assign((size_t)W * H, 0.f);
    const float xmin = 0, xmax = (float)(W - 2) - 0.001f;
    auto addLine = [&](float x0, float y0, float x1, float y1) {
        x0 -= ix0; x1 -= ix0; y0 -= iy0; y1 -= iy0;
        if (y0 == y1) return;
        float dir = 1;
        if (y0 > y1) { dir = -1; std::swap(x0, x1); std::swap(y0, y1); }
        float dxdy = (x1 - x0) / (y1 - y0);
        float x = x0;
        if (y0 < 0) { x -= y0 * dxdy; y0 = 0; }
        float ye = std::min(y1, (float)H);
        if (ye <= y0) return;
        int ys = (int)y0, yend = (int)std::ceil(ye);
        for (int yy = ys; yy < yend; ++yy) {
            float *row = &acc_[(size_t)yy * W];
            float dy = std::min((float)(yy + 1), y1) - std::max((float)yy, y0);
            float xnext = x + dxdy * dy;
            float d = dy * dir;
            float xa = std::min(x, xnext), xb = std::max(x, xnext);
            xa = std::min(std::max(xa, xmin), xmax);
            xb = std::min(std::max(xb, xmin), xmax);
            float x0f = std::floor(xa);
            int x0i = (int)x0f;
            float x1c = std::ceil(xb);
            int x1i = (int)x1c;
            if (x1i <= x0i + 1) {
                float xmf = 0.5f * (xa + xb) - x0f;
                row[x0i] += d - d * xmf;
                row[x0i + 1] += d * xmf;
            } else {
                float s = 1.f / (xb - xa);
                float fr = xa - x0f;
                float a0 = 0.5f * s * (1 - fr) * (1 - fr);
                float x1f = xb - x1c + 1;
                float am = 0.5f * s * x1f * x1f;
                row[x0i] += d * a0;
                if (x1i == x0i + 2) row[x0i + 1] += d * (1 - a0 - am);
                else {
                    float a1 = s * (1.5f - fr);
                    row[x0i + 1] += d * (a1 - a0);
                    for (int xi = x0i + 2; xi < x1i - 1; ++xi) row[xi] += d * s;
                    float a2 = a1 + (x1i - x0i - 3) * s;
                    row[x1i - 1] += d * (1 - a2 - am);
                }
                row[x1i] += d * am;
            }
            x = xnext;
        }
    };
    for (auto &c : cs) {
        if (c.size() < 2) continue;
        for (size_t i = 0; i < c.size(); ++i) {
            const Pt &a = c[i], &b = c[(i + 1) % c.size()];
            addLine(a.x, a.y, b.x, b.y);
        }
    }
    cov_.resize(W);
    for (int yy = 0; yy < H; ++yy) {
        float *row = &acc_[(size_t)yy * W];
        float sum = 0;
        bool any = false;
        for (int xx = 0; xx < W; ++xx) {
            sum += row[xx];
            float c = std::fabs(sum);
            cov_[xx] = c > 1 ? 1 : c;
            if (cov_[xx] > 0.002f) any = true;
        }
        if (any) blendSpan(iy0 + yy, ix0, ix0 + W, cov_.data(), p);
    }
}

static void addContour(std::vector<std::vector<Pt>> &cs, std::vector<Pt> c)
{
    // every contour the same way round, so overlaps add up instead of cancelling
    float a = 0;
    for (size_t i = 0; i < c.size(); ++i) {
        const Pt &p = c[i], &q = c[(i + 1) % c.size()];
        a += p.x * q.y - q.x * p.y;
    }
    if (a < 0) std::reverse(c.begin(), c.end());
    cs.push_back(std::move(c));
}

static std::vector<Pt> circlePts(float cx, float cy, float r)
{
    int n = std::max(8, std::min(64, (int)(r * 2.5f)));
    std::vector<Pt> c(n);
    for (int i = 0; i < n; ++i) {
        float a = 6.2831853f * i / n;
        c[i] = {cx + std::cos(a) * r, cy + std::sin(a) * r};
    }
    return c;
}

void Canvas::strokePolyline(const Pt *pts, int n, float lw, const Paint &p, bool closed)
{
    if (n < 2) return;
    std::vector<std::vector<Pt>> cs;
    float hw = lw * 0.5f;
    int segs = closed ? n : n - 1;
    for (int i = 0; i < segs; ++i) {
        const Pt &a = pts[i], &b = pts[(i + 1) % n];
        float dx = b.x - a.x, dy = b.y - a.y, L = std::sqrt(dx * dx + dy * dy);
        if (L < 1e-4f) continue;
        float nx = -dy / L * hw, ny = dx / L * hw;
        addContour(cs, {{a.x - nx, a.y - ny}, {b.x - nx, b.y - ny}, {b.x + nx, b.y + ny}, {a.x + nx, a.y + ny}});
    }
    if (lw > 2.4f) {
        for (int i = 0; i < n; ++i) {
            bool end = !closed && (i == 0 || i == n - 1);
            if (!end) {
                const Pt &a = pts[(i + n - 1) % n], &b = pts[i], &cc = pts[(i + 1) % n];
                float ux = b.x - a.x, uy = b.y - a.y, vx = cc.x - b.x, vy = cc.y - b.y;
                float lu = std::sqrt(ux * ux + uy * uy), lv = std::sqrt(vx * vx + vy * vy);
                if (lu > 1e-4f && lv > 1e-4f && (ux * vx + uy * vy) / (lu * lv) > 0.97f) continue;   // almost straight
            }
            addContour(cs, circlePts(pts[i].x, pts[i].y, hw));
        }
    }
    rasterize(cs, p);
}

void Canvas::line(float x0, float y0, float x1, float y1, float lw, const Col &c)
{
    Pt p[2] = {{x0, y0}, {x1, y1}};
    strokePolyline(p, 2, lw, Paint(c));
}

void Canvas::fillCircle(float cx, float cy, float r, const Paint &p)
{
    std::vector<std::vector<Pt>> cs;
    addContour(cs, circlePts(cx, cy, r));
    rasterize(cs, p);
}

void Canvas::strokeCircle(float cx, float cy, float r, float lw, const Paint &p)
{
    std::vector<Pt> c = circlePts(cx, cy, r);
    strokePolyline(c.data(), (int)c.size(), lw, p, true);
}


// ------------------------------------------------------------------ pictures
void Canvas::blit(int x, int y, int w, int h, const uint32_t *img)
{
    int ya = std::max(clip_.y0, y), yb = std::min(clip_.y1, y + h), xa = std::max(clip_.x0, x), xb = std::min(clip_.x1, x + w);
    for (int yy = ya; yy < yb; ++yy) {
        const uint32_t *sp = img + (size_t)(yy - y) * w;
        uint32_t *dp = px_.data() + (size_t)yy * w_;
        for (int xx = xa; xx < xb; ++xx) {
            uint32_t sv = sp[xx - x];
            int sa = (int)(sv >> 24);
            if (sa == 0) continue;
            uint32_t &d = dp[xx];
            int ia = 255 - sa;
            int r = (int)((sv >> 16) & 255) + (((int)((d >> 16) & 255) * ia + 127) / 255);
            int g = (int)((sv >> 8) & 255) + (((int)((d >> 8) & 255) * ia + 127) / 255);
            int b = (int)(sv & 255) + (((int)(d & 255) * ia + 127) / 255);
            d = 0xff000000u | (uint32_t)std::min(255, r) << 16 | (uint32_t)std::min(255, g) << 8 | (uint32_t)std::min(255, b);
        }
    }
}

// ------------------------------------------------------------------ graphs
void Canvas::columns(int x0, const float *tops, int n, float bottom, const Paint &p)
{
    int yb = std::min(clip_.y1, (int)std::ceil(bottom));
    int ya0 = clip_.y0;
    if (yb <= ya0) return;
    // the paint's color per row, once
    std::vector<Col> &lut = hcache_;
    lut.resize(yb - ya0);
    for (int y = ya0; y < yb; ++y) lut[y - ya0] = paintAt(p, 0, y + 0.5f);
    float bf = bottom;
    for (int i = 0; i < n; ++i) {
        int x = x0 + i;
        if (x < clip_.x0 || x >= clip_.x1) continue;
        float t = tops[i];
        if (t >= bf) continue;
        int yt = std::max(ya0, (int)std::floor(t));
        for (int y = yt; y < yb; ++y) {
            float cov = std::min((float)y + 1, bf) - std::max((float)y, t);
            if (cov <= 0) continue;
            blendPx(px_[(size_t)y * w_ + x], lut[y - ya0], cov > 1 ? 1 : cov);
        }
    }
}

void Canvas::columnsDown(int x0, const float *bottoms, int n, float top, const Paint &p)
{
    int yt = std::max(clip_.y0, (int)std::floor(top));
    int ye = clip_.y1;
    if (ye <= yt) return;
    std::vector<Col> &lut = hcache_;
    lut.resize(ye - yt);
    for (int y = yt; y < ye; ++y) lut[y - yt] = paintAt(p, 0, y + 0.5f);
    for (int i = 0; i < n; ++i) {
        int x = x0 + i;
        if (x < clip_.x0 || x >= clip_.x1) continue;
        float b = bottoms[i];
        if (b <= top) continue;
        int yb = std::min(ye, (int)std::ceil(b));
        for (int y = yt; y < yb; ++y) {
            float cov = std::min((float)y + 1, b) - std::max((float)y, top);
            if (cov <= 0) continue;
            blendPx(px_[(size_t)y * w_ + x], lut[y - yt], cov > 1 ? 1 : cov);
        }
    }
}

void Canvas::strokeThin(const Pt *pts, int n, float lw, const Col &c)
{
    float hw = lw * 0.5f;
    for (int i = 0; i + 1 < n; ++i) {
        const Pt &a = pts[i], &b = pts[i + 1];
        float dx = b.x - a.x, dy = b.y - a.y, L2 = dx * dx + dy * dy;
        int x0 = std::max(clip_.x0, (int)std::floor(std::min(a.x, b.x) - hw - 1));
        int x1 = std::min(clip_.x1, (int)std::ceil(std::max(a.x, b.x) + hw + 1));
        int y0 = std::max(clip_.y0, (int)std::floor(std::min(a.y, b.y) - hw - 1));
        int y1 = std::min(clip_.y1, (int)std::ceil(std::max(a.y, b.y) + hw + 1));
        for (int y = y0; y < y1; ++y) {
            uint32_t *row = &px_[(size_t)y * w_];
            float py = y + 0.5f;
            for (int x = x0; x < x1; ++x) {
                float px = x + 0.5f;
                float t = L2 > 0 ? ((px - a.x) * dx + (py - a.y) * dy) / L2 : 0;
                // the start cap belongs to the previous segment (no double cover at the joints)
                if (i > 0 && t < 0) continue;
                t = t < 0 ? 0 : (t > 1 ? 1 : t);
                float qx = a.x + dx * t - px, qy = a.y + dy * t - py;
                float d = std::sqrt(qx * qx + qy * qy);
                float cov = hw + 0.5f - d;
                if (cov <= 0) continue;
                blendPx(row[x], c, cov > 1 ? 1 : cov);
            }
        }
    }
}

// ------------------------------------------------------------------ text
float Canvas::text(Font &f, float size, float x, float y, const char *s, const Col &c, bool tabular)
{
    return f.draw(*this, size, x, y, s, c, tabular, clip_.x0, clip_.y0, clip_.x1, clip_.y1);
}

} // namespace gui
} // namespace ts
