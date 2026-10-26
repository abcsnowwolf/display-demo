#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>

#include "../third_party/stb_truetype.h"
#include "GfxAlloc.h"
#include "Style.h"

enum FontFace : uint8_t {
    FACE_SANS,
    FACE_SANS_BOLD,
    FACE_KAI,
    FACE_DISPLAY,
    FACE_ICONS,
    FACE_EMOJI,  // Twemoji, COLR/CPAL color layers
    FACE_COUNT,
};

// Typeface families exposed through the MCP API.
enum FontFamily : uint8_t {
    FAMILY_SANS,     // Noto Sans SC — body text, UI
    FAMILY_KAI,      // LXGW WenKai — calligraphic, quotes and poetry
    FAMILY_DISPLAY,  // Smiley Sans — condensed oblique, headlines and numbers
};

bool parseFamily(const char* name, FontFamily& out);
const char* familyName(FontFamily f);

struct GlyphBitmap {
    int16_t w, h;        // bitmap size
    int16_t xoff, yoff;  // offset from pen position / baseline
    uint8_t* alpha;      // w*h coverage, nullptr for blank glyphs
};

/* TrueType/OpenType rasterizer over the memory-mapped font pack (see
 * tools/build_fonts.py). Glyphs are rendered on demand at any pixel size and
 * kept in an LRU cache bounded by a byte budget.
 *
 * Not thread-safe: owned by the display task. */
class FontEngine {
public:
    FontEngine() = default;
    ~FontEngine();
    FontEngine(const FontEngine&) = delete;
    FontEngine& operator=(const FontEngine&) = delete;

    // Parses an "FPK1" pack. The data must stay valid for the engine's lifetime.
    bool begin(const uint8_t* pack, size_t size);
    bool hasFace(FontFace f) const { return faces_[f].loaded; }
    bool ready() const { return hasFace(FACE_SANS); }

    static FontFace faceFor(FontFamily family, bool bold);

    /* Finds a face that has `cp`, starting with `want` and falling back to the
     * sans faces. Returns false (with glyph 0 = .notdef of `want`) if none do. */
    bool resolve(FontFace want, uint32_t cp, FontFace& face, int& glyph) const;

    float advance(FontFace f, int glyph, float px) const;
    float kern(FontFace f, int g1, int g2, float px) const;
    // Typographic ascent/descent in pixels, both positive.
    float ascent(FontFace f, float px) const;
    float descent(FontFace f, float px) const;

    /* COLR v0 color glyphs. colorLayers returns how many layers `glyph` is
     * painted with (0 = an ordinary glyph) and the index of the first;
     * colorLayer gives a layer's outline glyph and palette color, or
     * foreground = true when it takes the text color. */
    int colorLayers(FontFace f, int glyph, int& first) const;
    void colorLayer(FontFace f, int index, int& glyph, RGBA& color, bool& foreground) const;
    // Codepoints shown as emoji by default (Unicode Emoji_Presentation, approximated).
    static bool isEmojiPresentation(uint32_t cp);

    // Rendered glyph, cached. The pointer stays valid until the next call.
    const GlyphBitmap* glyph(FontFace f, int glyph, int px);

    void setCacheBudget(size_t bytes);
    size_t cacheBytes() const { return cacheBytes_; }

private:
    struct Face {
        stbtt_fontinfo info;
        float scalePerPx = 0;  // font units -> pixels at 1px em
        float ascent = 0.88f;  // em fractions
        float descent = 0.12f;
        bool loaded = false;
        // COLR v0 / CPAL palette 0, when present.
        const uint8_t* baseGlyphs = nullptr;  // {glyph, firstLayer, numLayers} x numBase
        const uint8_t* layers = nullptr;      // {glyph, paletteIndex} x numLayers
        const uint8_t* palette = nullptr;     // BGRA x paletteSize
        uint16_t numBase = 0, numLayers = 0, paletteSize = 0;
    };
    static void loadColorTables(Face& face, const uint8_t* font, size_t size);
    struct CacheEntry {
        uint64_t key;
        GlyphBitmap bmp;
        size_t bytes;
    };

    void evict();

    Face faces_[FACE_COUNT];
    using Lru = std::list<CacheEntry, GfxAllocator<CacheEntry>>;
    using Index = std::unordered_map<uint64_t, Lru::iterator, std::hash<uint64_t>, std::equal_to<uint64_t>,
                                     GfxAllocator<std::pair<const uint64_t, Lru::iterator>>>;

    Lru lru_;  // front = most recently used
    Index index_;
    size_t cacheBytes_ = 0;
    size_t cacheBudget_ = 64 * 1024;
};
