#include "Canvas.h"

#include <algorithm>
#include <cmath>

namespace {

inline uint16_t pack565(int r, int g, int b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

inline float clamp01(float v) {
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

// 4x4 Bayer matrix, values 0..15.
const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

float roundRectSdf(float px, float py, float cx, float cy, float hx, float hy, float r) {
    float qx = std::fabs(px - cx) - (hx - r);
    float qy = std::fabs(py - cy) - (hy - r);
    float ox = std::max(qx, 0.0f);
    float oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

}  // namespace

Canvas::Canvas(uint16_t* buffer, int width, int bandY, int bandHeight)
    : buf_(buffer), width_(width), bandY_(bandY), bandH_(bandHeight) {}

bool Canvas::clipRows(int y0, int y1, int& from, int& to) const {
    from = std::max(y0, bandY_);
    to = std::min(y1, bandY_ + bandH_);
    return from < to;
}

void Canvas::blend(int x, int y, RGBA c, int coverage) {
    if (x < 0 || x >= width_ || y < bandY_ || y >= bandY_ + bandH_) return;
    int a = (c.a * coverage + 127) / 255;
    if (a <= 0) return;
    uint16_t& px = buf_[(y - bandY_) * width_ + x];
    if (a >= 255) {
        px = pack565(c.r, c.g, c.b);
        return;
    }
    int dr = (px >> 8) & 0xF8, dg = (px >> 3) & 0xFC, db = (px << 3) & 0xF8;
    dr |= dr >> 5;
    dg |= dg >> 6;
    db |= db >> 5;
    px = pack565(dr + ((c.r - dr) * a + 127) / 255, dg + ((c.g - dg) * a + 127) / 255,
                 db + ((c.b - db) * a + 127) / 255);
}

void Canvas::fillRect(int x, int y, int w, int h, RGBA c) {
    if (!c.visible()) return;
    int x0 = std::max(x, 0), x1 = std::min(x + w, width_);
    int y0, y1;
    if (x0 >= x1 || !clipRows(y, y + h, y0, y1)) return;
    if (c.a == 255) {
        uint16_t v = pack565(c.r, c.g, c.b);
        for (int yy = y0; yy < y1; ++yy) std::fill_n(buf_ + (yy - bandY_) * width_ + x0, x1 - x0, v);
        return;
    }
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) blend(xx, yy, c, 255);
}

void Canvas::gradientRect(int x, int y, int w, int h, RGBA top, RGBA bottom) {
    int x0 = std::max(x, 0), x1 = std::min(x + w, width_);
    int y0, y1;
    if (x0 >= x1 || h <= 0 || !clipRows(y, y + h, y0, y1)) return;
    for (int yy = y0; yy < y1; ++yy) {
        float t = h > 1 ? float(yy - y) / float(h - 1) : 0;
        // Channel values scaled by 16 so the Bayer threshold picks between the
        // two nearest 565 levels.
        int r = int((top.r + (bottom.r - top.r) * t) * 31 * 16 / 255);
        int g = int((top.g + (bottom.g - top.g) * t) * 63 * 16 / 255);
        int b = int((top.b + (bottom.b - top.b) * t) * 31 * 16 / 255);
        uint16_t* row = buf_ + (yy - bandY_) * width_;
        for (int xx = x0; xx < x1; ++xx) {
            int d = BAYER[yy & 3][xx & 3];
            int r5 = std::min((r + d) >> 4, 31), g6 = std::min((g + d) >> 4, 63), b5 = std::min((b + d) >> 4, 31);
            row[xx] = static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
        }
    }
}

void Canvas::roundRect(float x, float y, float w, float h, float r, RGBA fill, RGBA border, float bw) {
    if (w <= 0 || h <= 0) return;
    if (!border.visible()) bw = 0;
    float hx = w / 2, hy = h / 2, cx = x + hx, cy = y + hy;
    r = std::max(0.0f, std::min(r, std::min(hx, hy)));
    int x0 = std::max(0, int(std::floor(x))), x1 = std::min(width_, int(std::ceil(x + w)));
    int y0, y1;
    if (x0 >= x1 || !clipRows(int(std::floor(y)), int(std::ceil(y + h)), y0, y1)) return;

    for (int yy = y0; yy < y1; ++yy) {
        float py = yy + 0.5f;
        for (int xx = x0; xx < x1; ++xx) {
            float d = roundRectSdf(xx + 0.5f, py, cx, cy, hx, hy, r);
            float outer = clamp01(0.5f - d);
            if (outer <= 0) continue;
            float inner = bw > 0 ? clamp01(0.5f - (d + bw)) : outer;
            if (fill.visible() && inner > 0) blend(xx, yy, fill, int(inner * 255));
            if (bw > 0 && outer > inner) blend(xx, yy, border, int((outer - inner) * 255));
        }
    }
}

void Canvas::shadow(float x, float y, float w, float h, float r, float blur, RGBA color) {
    float hx = w / 2, hy = h / 2, cx = x + hx, cy = y + hy;
    r = std::max(0.0f, std::min(r, std::min(hx, hy)));
    int x0 = std::max(0, int(x - blur)), x1 = std::min(width_, int(std::ceil(x + w + blur)));
    int y0, y1;
    if (x0 >= x1 || !clipRows(int(y - blur), int(std::ceil(y + h + blur)), y0, y1)) return;
    for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) {
            float d = roundRectSdf(xx + 0.5f, yy + 0.5f, cx, cy, hx, hy, r);
            float t = clamp01((blur - d) / (2 * blur));
            t = t * t * (3 - 2 * t);
            if (t > 0) blend(xx, yy, color, int(t * 255));
        }
    }
}

void Canvas::circle(float cx, float cy, float r, RGBA fill, RGBA border, float bw) {
    roundRect(cx - r, cy - r, 2 * r, 2 * r, r, fill, border, bw);
}

void Canvas::arc(float cx, float cy, float r, float thickness, float startDeg, float endDeg, RGBA c) {
    float sweep = endDeg - startDeg;
    if (sweep <= 0 || !c.visible()) return;
    const float rad = 3.14159265f / 180.0f;
    float ht = thickness / 2;
    float ext = r + ht + 1;
    int x0 = std::max(0, int(cx - ext)), x1 = std::min(width_, int(std::ceil(cx + ext)));
    int y0, y1;
    if (x0 >= x1 || !clipRows(int(cy - ext), int(std::ceil(cy + ext)), y0, y1)) return;

    bool full = sweep >= 360;
    float s = std::fmod(startDeg, 360.0f);
    if (s < 0) s += 360;
    float ax = cx + r * std::sin(s * rad), ay = cy - r * std::cos(s * rad);
    float bx = cx + r * std::sin((s + sweep) * rad), by = cy - r * std::cos((s + sweep) * rad);

    for (int yy = y0; yy < y1; ++yy) {
        float py = yy + 0.5f - cy;
        for (int xx = x0; xx < x1; ++xx) {
            float px = xx + 0.5f - cx;
            float dist = std::sqrt(px * px + py * py);
            float ring = std::fabs(dist - r) - ht;
            if (ring > 1) continue;
            float d;
            float ang = std::atan2(px, -py) / rad;
            if (ang < 0) ang += 360;
            float rel = ang - s;
            if (rel < 0) rel += 360;
            if (full || rel <= sweep) {
                d = ring;
            } else {
                float da = std::hypot(xx + 0.5f - ax, yy + 0.5f - ay);
                float db = std::hypot(xx + 0.5f - bx, yy + 0.5f - by);
                d = std::min(da, db) - ht;
            }
            float cov = clamp01(0.5f - d);
            if (cov > 0) blend(xx, yy, c, int(cov * 255));
        }
    }
}

void Canvas::line(float x1, float y1, float x2, float y2, float width, RGBA c) {
    if (!c.visible()) return;
    float hw = width / 2;
    int bx0 = std::max(0, int(std::min(x1, x2) - hw - 1));
    int bx1 = std::min(width_, int(std::ceil(std::max(x1, x2) + hw + 1)));
    int y0, yEnd;
    if (bx0 >= bx1 ||
        !clipRows(int(std::min(y1, y2) - hw - 1), int(std::ceil(std::max(y1, y2) + hw + 1)), y0, yEnd))
        return;
    float dx = x2 - x1, dy = y2 - y1;
    float len2 = dx * dx + dy * dy;
    for (int yy = y0; yy < yEnd; ++yy) {
        for (int xx = bx0; xx < bx1; ++xx) {
            float px = xx + 0.5f - x1, py = yy + 0.5f - y1;
            float t = len2 > 0 ? clamp01((px * dx + py * dy) / len2) : 0;
            float d = std::hypot(px - t * dx, py - t * dy) - hw;
            float cov = clamp01(0.5f - d);
            if (cov > 0) blend(xx, yy, c, int(cov * 255));
        }
    }
}

void Canvas::mask(int x, int y, int w, int h, const uint8_t* alpha, RGBA color) {
    int y0, y1;
    if (!clipRows(y, y + h, y0, y1)) return;
    int x0 = std::max(x, 0), x1 = std::min(x + w, width_);
    for (int yy = y0; yy < y1; ++yy) {
        const uint8_t* src = alpha + (yy - y) * w;
        for (int xx = x0; xx < x1; ++xx) {
            uint8_t a = src[xx - x];
            if (a) blend(xx, yy, color, a);
        }
    }
}
