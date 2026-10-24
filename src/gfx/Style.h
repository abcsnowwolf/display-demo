#pragma once

#include <cstdint>

struct RGBA {
    uint8_t r, g, b, a;

    constexpr RGBA() : r(0), g(0), b(0), a(0) {}
    constexpr RGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) : r(r), g(g), b(b), a(a) {}

    static constexpr RGBA hex(uint32_t rgb, uint8_t a = 255) {
        return RGBA((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
    }
    RGBA withAlpha(uint8_t alpha) const { return RGBA(r, g, b, alpha); }
    bool visible() const { return a != 0; }
    bool operator==(const RGBA& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};

// Linear interpolation in sRGB space; good enough for UI tints and gradients.
RGBA mix(RGBA a, RGBA b, float t);

struct Theme {
    const char* name;
    const char* description;
    RGBA bgTop;
    RGBA bgBottom;
    RGBA surface;   // cards and tiles, usually translucent over the background
    RGBA border;
    RGBA text;
    RGBA muted;
    RGBA accent;
    bool light;
};

const Theme& defaultTheme();
// Returns nullptr for unknown names.
const Theme* findTheme(const char* name);
int themeCount();
const Theme& themeAt(int i);

/* Accepts "#RGB", "#RRGGBB", "#RRGGBBAA", a palette name ("red", "sky", ...),
 * or a theme role ("text", "muted", "accent", "surface", "border", "bg",
 * "success", "warning", "danger", "info"). Palette names resolve to a lighter
 * shade on dark themes and a deeper one on light themes so they stay readable. */
bool parseColor(const char* s, const Theme& theme, RGBA& out);

const char* paletteNames();  // comma separated, for get_display_info

struct Background {
    RGBA top;
    RGBA bottom;
};

/* "#111827" (solid), "#0f172a,#312e81" (vertical gradient), any color accepted
 * by parseColor, or null/"" for the theme's own background. */
bool parseBackground(const char* s, const Theme& theme, Background& out);
