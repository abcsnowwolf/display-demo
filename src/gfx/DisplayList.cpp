#include "DisplayList.h"

#include <algorithm>
#include <cmath>

void DisplayList::clear() {
    ops_.clear();
    glyphs_.clear();
    points_.clear();
    pixels_.clear();
    palette_.clear();
}

void DisplayList::clip(int x, int y, int w, int h) {
    push(Kind::Clip, {float(x), float(y), float(w), float(h)}, {}, {}, 0, 0);
}

void DisplayList::resetClip() { push(Kind::ResetClip, {}, {}, {}, 0, 0); }

void DisplayList::append(const DisplayList& other) {
    const uint32_t offset = uint32_t(glyphs_.size());
    glyphs_.insert(glyphs_.end(), other.glyphs_.begin(), other.glyphs_.end());
    for (Op op : other.ops_) {
        if (op.kind == Kind::Glyphs) op.first += offset;
        ops_.push_back(op);
    }
}

void DisplayList::push(Kind kind, std::initializer_list<float> f, RGBA c1, RGBA c2, float top, float bottom) {
    Op op{};
    op.kind = kind;
    std::copy(f.begin(), f.end(), op.f);
    op.c1 = c1;
    op.c2 = c2;
    op.top = int16_t(std::floor(top));
    op.bottom = int16_t(std::ceil(bottom));
    ops_.push_back(op);
}

void DisplayList::gradient(int x, int y, int w, int h, RGBA top, RGBA bottom) {
    push(Kind::Gradient, {float(x), float(y), float(w), float(h)}, top, bottom, y, y + h);
}

void DisplayList::rect(int x, int y, int w, int h, RGBA c) {
    if (c.visible()) roundRect(x, y, w, h, 0, c);
}

void DisplayList::roundRect(float x, float y, float w, float h, float r, RGBA fill, RGBA border, float bw) {
    if (!fill.visible() && (!border.visible() || bw <= 0)) return;
    push(Kind::RoundRect, {x, y, w, h, r, bw}, fill, border, y, y + h);
}

void DisplayList::shadow(float x, float y, float w, float h, float r, float blur, RGBA c) {
    push(Kind::Shadow, {x, y, w, h, r, blur}, c, RGBA(), y - blur, y + h + blur);
}

void DisplayList::circle(float cx, float cy, float r, RGBA fill, RGBA border, float bw) {
    roundRect(cx - r, cy - r, 2 * r, 2 * r, r, fill, border, bw);
}

void DisplayList::arc(float cx, float cy, float r, float thickness, float startDeg, float endDeg, RGBA c) {
    if (!c.visible() || endDeg <= startDeg) return;
    float ext = r + thickness / 2 + 1;
    push(Kind::Arc, {cx, cy, r, thickness, startDeg, endDeg}, c, RGBA(), cy - ext, cy + ext);
}

void DisplayList::line(float x1, float y1, float x2, float y2, float width, RGBA c) {
    if (!c.visible()) return;
    push(Kind::Line, {x1, y1, x2, y2, width}, c, RGBA(), std::min(y1, y2) - width, std::max(y1, y2) + width);
}

void DisplayList::ellipse(float cx, float cy, float rx, float ry, RGBA fill, RGBA border, float bw) {
    if (!fill.visible() && (!border.visible() || bw <= 0)) return;
    push(Kind::Ellipse, {cx, cy, rx, ry, bw}, fill, border, cy - ry, cy + ry);
}

void DisplayList::polygon(const float* xy, int n, RGBA fill, RGBA border, float bw) {
    if (n < 3 || (!fill.visible() && (!border.visible() || bw <= 0))) return;
    float top = xy[1], bottom = xy[1];
    for (int i = 1; i < n; ++i) {
        top = std::min(top, xy[2 * i + 1]);
        bottom = std::max(bottom, xy[2 * i + 1]);
    }
    push(Kind::Polygon, {bw}, fill, border, top - 1, bottom + 1);
    ops_.back().first = uint32_t(points_.size());
    ops_.back().count = uint32_t(n);
    points_.insert(points_.end(), xy, xy + 2 * n);
}

void DisplayList::bitmap(int x, int y, int w, int h, int scale, const uint8_t* pixels,
                         const std::vector<RGBA>& palette) {
    if (w <= 0 || h <= 0 || scale < 1) return;
    // f[4] = palette offset; first/count index the pixels.
    push(Kind::Bitmap, {float(x), float(y), float(w), float(h), float(scale), float(palette_.size())}, RGBA(), RGBA(),
         y, y + h * scale);
    ops_.back().first = uint32_t(pixels_.size());
    ops_.back().count = uint32_t(w * h);
    pixels_.insert(pixels_.end(), pixels, pixels + w * h);
    palette_.insert(palette_.end(), palette.begin(), palette.end());
}

void DisplayList::text(const TextLayout& layout, const RichText& rich, float x, float y, int firstLine,
                       int lastLine) {
    if (lastLine < 0 || lastLine > int(layout.lines.size())) lastLine = int(layout.lines.size());
    if (firstLine >= lastLine) return;
    float dy = y - layout.lines[firstLine].top;

    // Highlights and underlines go below the glyph run.
    for (int li = firstLine; li < lastLine; ++li) {
        const LayoutLine& line = layout.lines[li];
        for (int i = line.first; i < line.first + line.count; ++i) {
            const LayoutGlyph& g = layout.glyphs[i];
            const TextStyle& st = rich.styles[g.style];
            float gx = x + g.x;
            if (st.highlight.visible()) {
                float pad = g.px * 0.12f;
                roundRect(gx - (i == line.first ? pad : 0), dy + g.baseline - g.px * 0.92f,
                          g.advance + (i == line.first ? pad : 0) + (i + 1 == line.first + line.count ? pad : 0),
                          g.px * 1.18f, 0, st.highlight);
            }
            if (st.underline) {
                float t = std::max(1.0f, g.px / 16.0f);
                roundRect(gx, dy + g.baseline + g.px * 0.12f, g.advance, t, 0, st.color);
            }
        }
    }

    Op op{};
    op.kind = Kind::Glyphs;
    op.first = uint32_t(glyphs_.size());
    float top = 1e9f, bottom = -1e9f;
    for (int li = firstLine; li < lastLine; ++li) {
        const LayoutLine& line = layout.lines[li];
        for (int i = line.first; i < line.first + line.count; ++i) {
            const LayoutGlyph& g = layout.glyphs[i];
            const TextStyle& st = rich.styles[g.style];
            float base = dy + g.baseline;
            glyphs_.push_back({x + g.x, int16_t(std::lround(base)), g.px, g.glyph, g.face,
                               st.bold && g.face != FACE_SANS_BOLD && g.face != FACE_ICONS && g.face != FACE_EMOJI,
                               st.color});
            top = std::min(top, base - g.px * 1.1f);
            bottom = std::max(bottom, base + g.px * 0.4f);
        }
    }
    op.count = uint32_t(glyphs_.size()) - op.first;
    if (!op.count) return;
    op.top = int16_t(std::floor(top));
    op.bottom = int16_t(std::ceil(bottom));
    ops_.push_back(op);
}

void DisplayList::icon(uint32_t codepoint, float x, float y, float size, RGBA c, FontEngine& fonts) {
    FontFace face;
    int glyph;
    if (!codepoint || !fonts.hasFace(FACE_ICONS) || !fonts.resolve(FACE_ICONS, codepoint, face, glyph) ||
        face != FACE_ICONS)
        return;
    Op op{};
    op.kind = Kind::Glyphs;
    op.first = uint32_t(glyphs_.size());
    op.count = 1;
    op.top = int16_t(std::floor(y));
    op.bottom = int16_t(std::ceil(y + size));
    uint16_t px = uint16_t(std::lround(size));
    glyphs_.push_back({x, int16_t(std::lround(y + size)), px, glyph, FACE_ICONS, false, c});
    ops_.push_back(op);
}

void DisplayList::render(Canvas& canvas, FontEngine& fonts) const {
    const int bandTop = canvas.bandY(), bandBottom = canvas.bandY() + canvas.bandHeight();
    for (const Op& op : ops_) {
        // Clip state must be replayed even when its own rectangle misses a band.
        if (op.kind == Kind::Clip) {
            canvas.setClip(int(op.f[0]), int(op.f[1]), int(op.f[2]), int(op.f[3]));
            continue;
        }
        if (op.kind == Kind::ResetClip) { canvas.resetClip(); continue; }
        if (op.bottom < bandTop || op.top >= bandBottom) continue;
        const float* f = op.f;
        switch (op.kind) {
            case Kind::Clip:
            case Kind::ResetClip:
                break;
            case Kind::Gradient:
                canvas.gradientRect(int(f[0]), int(f[1]), int(f[2]), int(f[3]), op.c1, op.c2);
                break;
            case Kind::RoundRect:
                canvas.roundRect(f[0], f[1], f[2], f[3], f[4], op.c1, op.c2, f[5]);
                break;
            case Kind::Shadow:
                canvas.shadow(f[0], f[1], f[2], f[3], f[4], f[5], op.c1);
                break;
            case Kind::Arc:
                canvas.arc(f[0], f[1], f[2], f[3], f[4], f[5], op.c1);
                break;
            case Kind::Line:
                canvas.line(f[0], f[1], f[2], f[3], f[4], op.c1);
                break;
            case Kind::Ellipse:
                canvas.ellipse(f[0], f[1], f[2], f[3], op.c1, op.c2, f[4]);
                break;
            case Kind::Polygon:
                canvas.polygon(points_.data() + op.first, int(op.count), op.c1, op.c2, f[0]);
                break;
            case Kind::Bitmap:
                canvas.bitmap(int(f[0]), int(f[1]), int(f[2]), int(f[3]), int(f[4]), pixels_.data() + op.first,
                              palette_.data() + int(f[5]));
                break;
            case Kind::Glyphs:
                for (uint32_t i = op.first; i < op.first + op.count; ++i) {
                    const Glyph& g = glyphs_[i];
                    // Cheap cull before touching the rasterizer.
                    if (g.baseline + g.px * 0.5f < bandTop || g.baseline - g.px * 1.2f >= bandBottom) continue;
                    int firstLayer = 0;
                    int layers = g.face == FACE_EMOJI ? fonts.colorLayers(g.face, g.glyph, firstLayer) : 0;
                    if (layers > 0) {
                        // Color glyph: paint each outline layer in its palette color.
                        for (int l = 0; l < layers; ++l) {
                            int lg;
                            RGBA lc;
                            bool fg;
                            fonts.colorLayer(g.face, firstLayer + l, lg, lc, fg);
                            const GlyphBitmap* lb = fonts.glyph(g.face, lg, g.px);
                            if (!lb->alpha) continue;
                            canvas.mask(int(std::lround(g.x)) + lb->xoff, g.baseline + lb->yoff, lb->w, lb->h, lb->alpha,
                                        fg ? g.color : lc);
                        }
                        continue;
                    }
                    const GlyphBitmap* bmp = fonts.glyph(g.face, g.glyph, g.px);
                    if (!bmp->alpha) continue;
                    int gx = int(std::lround(g.x)) + bmp->xoff, gy = g.baseline + bmp->yoff;
                    canvas.mask(gx, gy, bmp->w, bmp->h, bmp->alpha, g.color);
                    if (g.fauxBold) {
                        int extra = std::max(1, int(g.px / 24));
                        for (int k = 1; k <= extra; ++k) canvas.mask(gx + k, gy, bmp->w, bmp->h, bmp->alpha, g.color);
                    }
                }
                break;
        }
    }
}
