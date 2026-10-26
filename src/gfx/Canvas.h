#pragma once

#include <cstdint>

#include "Style.h"

/* A horizontal band of the screen backed by an RGB565 buffer. All drawing
 * calls take full-screen coordinates and are clipped to the band, so a scene
 * can be rendered strip by strip when there is no RAM for a whole frame.
 *
 * Shapes are anti-aliased with signed distance functions: each pixel's
 * coverage is how far its centre lies inside the outline, clamped to one
 * pixel of ramp. */
class Canvas {
public:
    Canvas(uint16_t* buffer, int width, int bandY, int bandHeight);

    int width() const { return width_; }
    int bandY() const { return bandY_; }
    int bandHeight() const { return bandH_; }
    const uint16_t* buffer() const { return buf_; }
    void setClip(int x, int y, int w, int h);
    void resetClip();

    void fillRect(int x, int y, int w, int h, RGBA c);
    // Vertical gradient with ordered dithering to hide RGB565 banding.
    void gradientRect(int x, int y, int w, int h, RGBA top, RGBA bottom);
    // Filled rounded rectangle with an optional inside border.
    void roundRect(float x, float y, float w, float h, float r, RGBA fill, RGBA border = RGBA(),
                   float borderWidth = 0);
    // Soft drop shadow for a rounded rectangle.
    void shadow(float x, float y, float w, float h, float r, float blur, RGBA color);
    void circle(float cx, float cy, float r, RGBA fill, RGBA border = RGBA(), float borderWidth = 0);
    // Arc with round caps. Angles in degrees, 0 = 12 o'clock, clockwise.
    void arc(float cx, float cy, float r, float thickness, float startDeg, float endDeg, RGBA c);
    // Thick line with round caps.
    void line(float x1, float y1, float x2, float y2, float width, RGBA c);
    // Paints `color` through an 8-bit coverage mask.
    void mask(int x, int y, int w, int h, const uint8_t* alpha, RGBA color);
    // Ellipse with an optional inside border (distance approximated by scaling).
    void ellipse(float cx, float cy, float rx, float ry, RGBA fill, RGBA border = RGBA(), float borderWidth = 0);
    // Simple (even-odd) polygon from n x/y pairs, with an optional inside border.
    void polygon(const float* xy, int n, RGBA fill, RGBA border = RGBA(), float borderWidth = 0);
    // Indexed pixel art: w*h palette indices (0xFF = transparent), each drawn as a scale x scale block.
    void bitmap(int x, int y, int w, int h, int scale, const uint8_t* pixels, const RGBA* palette);

private:
    void blend(int x, int y, RGBA c, int coverage);  // coverage 0..255, x/y screen coords
    bool clipRows(int y0, int y1, int& from, int& to) const;

    uint16_t* buf_;
    int width_;
    int bandY_;
    int bandH_;
    int clipX0_, clipY0_, clipX1_, clipY1_;
};
