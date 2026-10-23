#include "font.h"
#include <cmath>
#include <cstring>
#include <mutex>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

namespace ts {
namespace gui {

extern const unsigned char font_inter_regular[], font_inter_medium[], font_inter_semibold[], font_inter_bold[];
extern const size_t font_inter_regular_size, font_inter_medium_size, font_inter_semibold_size, font_inter_bold_size;

struct Font::Impl {
    stbtt_fontinfo info;
    bool ok = false;
    int asc = 0, desc = 0, gap = 0, unitsCap = 0;
    float unitsPerEm = 1000;
    std::unordered_map<uint32_t, int> gidx;
};

Font::Font() : I(new Impl()) {}
Font::~Font() {}

bool Font::load(const unsigned char *data, size_t)
{
    if (!stbtt_InitFont(&I->info, data, stbtt_GetFontOffsetForIndex(data, 0))) return false;
    stbtt_GetFontVMetrics(&I->info, &I->asc, &I->desc, &I->gap);
    I->unitsPerEm = 1.0f / stbtt_ScaleForMappingEmToPixels(&I->info, 1.0f);
    int x0, y0, x1, y1;
    int g = stbtt_FindGlyphIndex(&I->info, 'H');
    if (stbtt_GetGlyphBox(&I->info, g, &x0, &y0, &x1, &y1)) I->unitsCap = y1;
    else I->unitsCap = (int)(I->asc * 0.7f);
    I->ok = true;
    return true;
}

// sizes are in pixels per em
static inline float sc(const stbtt_fontinfo *f, float size) { return stbtt_ScaleForMappingEmToPixels(f, size); }
float Font::ascent(float size) const { return I->asc * sc(&I->info, size); }
float Font::descent(float size) const { return -I->desc * sc(&I->info, size); }
float Font::capHeight(float size) const { return I->unitsCap * sc(&I->info, size); }

uint32_t utf8next(const char *&s)
{
    unsigned char c = (unsigned char)*s++;
    if (c < 0x80) return c;
    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    uint32_t cp = c & (0x3F >> n);
    for (int i = 0; i < n && *s; ++i) cp = (cp << 6) | ((unsigned char)*s++ & 0x3F);
    return cp;
}

int Font::glyphIndex(uint32_t cp)
{
    auto it = I->gidx.find(cp);
    if (it != I->gidx.end()) return it->second;
    int g = stbtt_FindGlyphIndex(&I->info, (int)cp);
    I->gidx[cp] = g;
    return g;
}

float Font::advance(int gi, float size)
{
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&I->info, gi, &adv, &lsb);
    return adv * sc(&I->info, size);
}

float Font::tabWidth(float size) { return advance(glyphIndex('0'), size); }

const Font::Glyph &Font::glyph(int gi, float size, int sub)
{
    uint64_t key = ((uint64_t)(uint32_t)(size * 8.f + 0.5f) << 32) | ((uint64_t)gi << 4) | (uint64_t)sub;
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    // a window resized through many sizes (the Tuner's huge note) would keep every one: past 24 MB, start over
    if (bytes_ > (24u << 20)) { cache_.clear(); bytes_ = 0; }
    Glyph g;
    float s = sc(&I->info, size);
    float shift = sub * 0.25f;
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBoxSubpixel(&I->info, gi, s, s, shift, 0, &x0, &y0, &x1, &y1);
    g.w = x1 - x0;
    g.h = y1 - y0;
    g.x0 = x0;
    g.y0 = y0;
    if (g.w > 0 && g.h > 0) {
        g.bmp.assign((size_t)g.w * g.h, 0);
        stbtt_MakeGlyphBitmapSubpixel(&I->info, g.bmp.data(), g.w, g.h, g.w, s, s, shift, 0, gi);
        // a touch heavier: light text on a dark ground otherwise reads thin
        for (auto &v : g.bmp) v = (uint8_t)(255.f * std::pow(v / 255.f, 0.82f) + 0.5f);
    }
    bytes_ += g.bmp.size() + 64;
    return cache_.emplace(key, std::move(g)).first->second;
}

float Font::measure(float size, const char *s, bool tabular)
{
    float x = 0;
    int prev = -1;
    float tw = tabular ? tabWidth(size) : 0;
    while (*s) {
        uint32_t cp = utf8next(s);
        int gi = glyphIndex(cp);
        if (prev >= 0) x += stbtt_GetGlyphKernAdvance(&I->info, prev, gi) * sc(&I->info, size);
        x += (tabular && cp >= '0' && cp <= '9') ? tw : advance(gi, size);
        prev = gi;
    }
    return x;
}

float Font::draw(Canvas &c, float size, float x, float y, const char *s, const Col &col, bool tabular, int cx0, int cy0,
                 int cx1, int cy1)
{
    float pen = x;
    int prev = -1;
    float tw = tabular ? tabWidth(size) : 0;
    int by = (int)std::floor(y + 0.5f);
    uint32_t *px = c.pixels();
    int W = c.width();
    int ia, sr, sg, sb;
    sr = (int)(std::min(1.f, std::max(0.f, col.r)) * 255.f + 0.5f);
    sg = (int)(std::min(1.f, std::max(0.f, col.g)) * 255.f + 0.5f);
    sb = (int)(std::min(1.f, std::max(0.f, col.b)) * 255.f + 0.5f);
    float ca = col.a;
    while (*s) {
        uint32_t cp = utf8next(s);
        int gi = glyphIndex(cp);
        if (prev >= 0) pen += stbtt_GetGlyphKernAdvance(&I->info, prev, gi) * sc(&I->info, size);
        float adv = advance(gi, size);
        float gx = pen;
        if (tabular && cp >= '0' && cp <= '9') { gx += (tw - adv) * 0.5f; adv = tw; }
        float fl = std::floor(gx);
        int sub = (int)((gx - fl) * 4.f);
        if (sub > 3) sub = 3;
        const Glyph &g = glyph(gi, size, sub);
        int ox = (int)fl + g.x0, oy = by + g.y0;
        for (int j = 0; j < g.h; ++j) {
            int yy = oy + j;
            if (yy < cy0 || yy >= cy1) continue;
            uint32_t *row = px + (size_t)yy * W;
            const uint8_t *src = &g.bmp[(size_t)j * g.w];
            for (int i = 0; i < g.w; ++i) {
                int xx = ox + i;
                if (xx < cx0 || xx >= cx1) continue;
                int a = src[i];
                if (!a) continue;
                ia = (int)(a * ca + 0.5f);
                uint32_t d = row[xx];
                int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
                dr += ((sr - dr) * ia) / 255;
                dg += ((sg - dg) * ia) / 255;
                db += ((sb - db) * ia) / 255;
                row[xx] = 0xff000000u | ((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db;
            }
        }
        pen += adv;
        prev = gi;
    }
    return pen - x;
}

Font &font(FontWeight w)
{
    static Font f[4];
    static std::once_flag once;
    std::call_once(once, [] {
        f[0].load(font_inter_regular, font_inter_regular_size);
        f[1].load(font_inter_medium, font_inter_medium_size);
        f[2].load(font_inter_semibold, font_inter_semibold_size);
        f[3].load(font_inter_bold, font_inter_bold_size);
    });
    return f[w];
}

} // namespace gui
} // namespace ts
