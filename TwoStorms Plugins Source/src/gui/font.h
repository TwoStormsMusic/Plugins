// Fonts: Inter, embedded, rasterized with stb_truetype and cached per size and quarter-pixel offset.
#pragma once
#include "canvas.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ts {
namespace gui {

class Font {
public:
    Font();
    ~Font();
    bool load(const unsigned char *data, size_t size);
    float ascent(float size) const;    // above the baseline (positive)
    float descent(float size) const;   // below the baseline (positive)
    float capHeight(float size) const;
    float measure(float size, const char *s, bool tabular = false);
    float draw(Canvas &c, float size, float x, float y, const char *s, const Col &col, bool tabular, int cx0, int cy0,
               int cx1, int cy1);

private:
    struct Glyph {
        int w = 0, h = 0, x0 = 0, y0 = 0;
        std::vector<uint8_t> bmp;
    };
    struct Impl;
    std::unique_ptr<Impl> I;
    std::unordered_map<uint64_t, Glyph> cache_;
    size_t bytes_ = 0;
    const Glyph &glyph(int gi, float size, int sub);
    float advance(int gi, float size);
    int glyphIndex(uint32_t cp);
    float tabWidth(float size);
};

// the four weights, created on first use
enum FontWeight { F_REGULAR, F_MEDIUM, F_SEMIBOLD, F_BOLD };
Font &font(FontWeight w);

// UTF-8 decoding (returns the code point, advances s)
uint32_t utf8next(const char *&s);

} // namespace gui
} // namespace ts
