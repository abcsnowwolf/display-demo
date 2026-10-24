#include "Style.h"

#include <cctype>
#include <cstring>
#include <string>

RGBA mix(RGBA a, RGBA b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    auto lerp = [t](uint8_t x, uint8_t y) { return static_cast<uint8_t>(x + (y - x) * t + 0.5f); };
    return RGBA(lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), lerp(a.a, b.a));
}

namespace {

const Theme THEMES[] = {
    {"midnight", "deep navy gradient, sky-blue accent (default)",
     RGBA::hex(0x0B1220), RGBA::hex(0x1A2440), RGBA::hex(0xFFFFFF, 14), RGBA::hex(0xFFFFFF, 26),
     RGBA::hex(0xF1F5F9), RGBA::hex(0x94A3B8), RGBA::hex(0x38BDF8), false},
    {"light", "clean white, blue accent",
     RGBA::hex(0xF8FAFC), RGBA::hex(0xE9EEF5), RGBA::hex(0xFFFFFF), RGBA::hex(0xD5DDE8),
     RGBA::hex(0x0F172A), RGBA::hex(0x64748B), RGBA::hex(0x2563EB), true},
    {"paper", "warm ivory paper, ink text, rust accent; pairs well with the kai font",
     RGBA::hex(0xF7F1E3), RGBA::hex(0xEDE3CC), RGBA::hex(0xFFFDF7, 200), RGBA::hex(0xD9CBAE),
     RGBA::hex(0x3A3226), RGBA::hex(0x8A7D68), RGBA::hex(0xB4532A), true},
    {"aurora", "indigo-to-violet gradient, pink accent",
     RGBA::hex(0x1E1B4B), RGBA::hex(0x581C87), RGBA::hex(0xFFFFFF, 20), RGBA::hex(0xFFFFFF, 34),
     RGBA::hex(0xFFFFFF), RGBA::hex(0xC4B5FD), RGBA::hex(0xF0ABFC), false},
    {"sunset", "ember-to-rose gradient, gold accent",
     RGBA::hex(0x431407), RGBA::hex(0x9F1239), RGBA::hex(0xFFFFFF, 20), RGBA::hex(0xFFFFFF, 34),
     RGBA::hex(0xFFF7ED), RGBA::hex(0xFDBA74), RGBA::hex(0xFCD34D), false},
    {"forest", "deep green gradient, mint accent",
     RGBA::hex(0x03190D), RGBA::hex(0x14532D), RGBA::hex(0xFFFFFF, 16), RGBA::hex(0xFFFFFF, 28),
     RGBA::hex(0xECFDF5), RGBA::hex(0x86EFAC), RGBA::hex(0x4ADE80), false},
    {"ocean", "dark teal-blue gradient, cyan accent",
     RGBA::hex(0x061A2E), RGBA::hex(0x0E5A73), RGBA::hex(0xFFFFFF, 16), RGBA::hex(0xFFFFFF, 28),
     RGBA::hex(0xF0F9FF), RGBA::hex(0x7DD3FC), RGBA::hex(0x67E8F9), false},
    {"mono", "pure black, white text, maximum contrast",
     RGBA::hex(0x000000), RGBA::hex(0x000000), RGBA::hex(0xFFFFFF, 18), RGBA::hex(0xFFFFFF, 40),
     RGBA::hex(0xFFFFFF), RGBA::hex(0xA3A3A3), RGBA::hex(0xFFFFFF), false},
};

struct PaletteColor {
    const char* name;
    uint32_t onDark;   // Tailwind 400
    uint32_t onLight;  // Tailwind 600
};

const PaletteColor PALETTE[] = {
    {"red", 0xF87171, 0xDC2626},     {"orange", 0xFB923C, 0xEA580C}, {"amber", 0xFBBF24, 0xD97706},
    {"yellow", 0xFACC15, 0xCA8A04},  {"lime", 0xA3E635, 0x65A30D},   {"green", 0x4ADE80, 0x16A34A},
    {"emerald", 0x34D399, 0x059669}, {"teal", 0x2DD4BF, 0x0D9488},   {"cyan", 0x22D3EE, 0x0891B2},
    {"sky", 0x38BDF8, 0x0284C7},     {"blue", 0x60A5FA, 0x2563EB},   {"indigo", 0x818CF8, 0x4F46E5},
    {"violet", 0xA78BFA, 0x7C3AED},  {"purple", 0xC084FC, 0x9333EA}, {"fuchsia", 0xE879F9, 0xC026D3},
    {"pink", 0xF472B6, 0xDB2777},    {"rose", 0xFB7185, 0xE11D48},   {"slate", 0x94A3B8, 0x475569},
    {"gray", 0x9CA3AF, 0x4B5563},    {"white", 0xFFFFFF, 0xFFFFFF},  {"black", 0x000000, 0x000000},
};

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool parseHex(const char* s, size_t len, RGBA& out) {
    uint32_t v = 0;
    for (size_t i = 0; i < len; ++i) {
        int d = hexDigit(s[i]);
        if (d < 0) return false;
        v = (v << 4) | d;
    }
    switch (len) {
        case 3:
            out = RGBA(((v >> 8) & 0xF) * 17, ((v >> 4) & 0xF) * 17, (v & 0xF) * 17);
            return true;
        case 6:
            out = RGBA::hex(v);
            return true;
        case 8:
            out = RGBA::hex(v >> 8, v & 0xFF);
            return true;
    }
    return false;
}

bool equalsIgnoreCase(const char* a, const char* b, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (tolower(static_cast<unsigned char>(a[i])) != b[i]) return false;
    }
    return b[len] == '\0';
}

bool parseColorN(const char* s, size_t len, const Theme& theme, RGBA& out) {
    while (len && isspace(static_cast<unsigned char>(*s))) ++s, --len;
    while (len && isspace(static_cast<unsigned char>(s[len - 1]))) --len;
    if (!len) return false;
    if (s[0] == '#') return parseHex(s + 1, len - 1, out);

    for (const auto& c : PALETTE) {
        if (equalsIgnoreCase(s, c.name, len)) {
            out = RGBA::hex(theme.light ? c.onLight : c.onDark);
            return true;
        }
    }
    struct Role {
        const char* name;
        RGBA color;
    } roles[] = {
        {"text", theme.text},
        {"muted", theme.muted},
        {"accent", theme.accent},
        {"surface", theme.surface},
        {"border", theme.border},
        {"bg", theme.bgTop},
        {"success", RGBA::hex(theme.light ? 0x16A34A : 0x4ADE80)},
        {"warning", RGBA::hex(theme.light ? 0xD97706 : 0xFBBF24)},
        {"danger", RGBA::hex(theme.light ? 0xDC2626 : 0xF87171)},
        {"error", RGBA::hex(theme.light ? 0xDC2626 : 0xF87171)},
        {"info", theme.accent},
        {"transparent", RGBA()},
    };
    for (const auto& r : roles) {
        if (equalsIgnoreCase(s, r.name, len)) {
            out = r.color;
            return true;
        }
    }
    return false;
}

}  // namespace

const Theme& defaultTheme() {
    return THEMES[0];
}

const Theme* findTheme(const char* name) {
    if (!name || !*name) return nullptr;
    for (const auto& t : THEMES) {
        if (equalsIgnoreCase(name, t.name, strlen(name))) return &t;
    }
    return nullptr;
}

int themeCount() {
    return sizeof(THEMES) / sizeof(THEMES[0]);
}

const Theme& themeAt(int i) {
    return THEMES[i];
}

bool parseColor(const char* s, const Theme& theme, RGBA& out) {
    return s && parseColorN(s, strlen(s), theme, out);
}

const char* paletteNames() {
    return "red, orange, amber, yellow, lime, green, emerald, teal, cyan, sky, blue, indigo, violet, "
           "purple, fuchsia, pink, rose, slate, gray, white, black; roles: text, muted, accent, surface, "
           "border, bg, success, warning, danger, info, transparent";
}

bool parseBackground(const char* s, const Theme& theme, Background& out) {
    out.top = theme.bgTop;
    out.bottom = theme.bgBottom;
    if (!s || !*s) return true;
    const char* comma = strchr(s, ',');
    if (!comma) {
        if (!parseColor(s, theme, out.top)) return false;
        out.top.a = 255;
        out.bottom = out.top;
        return true;
    }
    if (!parseColorN(s, comma - s, theme, out.top) || !parseColor(comma + 1, theme, out.bottom)) return false;
    out.top.a = out.bottom.a = 255;
    return true;
}
