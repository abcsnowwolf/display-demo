#pragma once

#include <cstdint>
#include <vector>

#include "Canvas.h"
#include "FontEngine.h"
#include "TextLayout.h"

/* A recorded scene: shapes and positioned glyphs in paint order. Recording
 * once and replaying per band is what lets a frame be rendered strip by strip
 * without re-running layout. */
class DisplayList {
public:
    void clear();
    void clip(int x, int y, int w, int h);
    void resetClip();
    void append(const DisplayList& other);

    void gradient(int x, int y, int w, int h, RGBA top, RGBA bottom);
    void rect(int x, int y, int w, int h, RGBA c);
    void roundRect(float x, float y, float w, float h, float r, RGBA fill, RGBA border = RGBA(), float bw = 0);
    void shadow(float x, float y, float w, float h, float r, float blur, RGBA c);
    void circle(float cx, float cy, float r, RGBA fill, RGBA border = RGBA(), float bw = 0);
    void arc(float cx, float cy, float r, float thickness, float startDeg, float endDeg, RGBA c);
    void line(float x1, float y1, float x2, float y2, float width, RGBA c);

    // Lines [firstLine, lastLine) of a layout; (x, y) is where firstLine's top lands.
    void text(const TextLayout& layout, const RichText& rich, float x, float y, int firstLine = 0,
              int lastLine = -1);
    // Icon whose em box has its top-left corner at (x, y).
    void icon(uint32_t codepoint, float x, float y, float size, RGBA c, FontEngine& fonts);

    void render(Canvas& canvas, FontEngine& fonts) const;

private:
    enum class Kind : uint8_t { Gradient, RoundRect, Shadow, Arc, Line, Glyphs, Clip, ResetClip };
    struct Op {
        Kind kind;
        float f[6];
        RGBA c1, c2;
        int16_t top, bottom;  // vertical extent, for band culling
        uint32_t first, count;
    };
    struct Glyph {
        float x;
        int16_t baseline;
        uint16_t px;
        int glyph;
        FontFace face;
        bool fauxBold;
        RGBA color;
    };

    void push(Kind kind, std::initializer_list<float> f, RGBA c1, RGBA c2, float top, float bottom);

    std::vector<Op> ops_;
    std::vector<Glyph> glyphs_;
};
