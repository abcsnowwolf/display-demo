#include "FontEngine.h"

#include <cmath>
#include <cstring>

#include "GfxAlloc.h"

namespace {

const char* const FACE_NAMES[FACE_COUNT] = {"sans", "sans-bold", "kai", "display", "icons", "emoji"};

uint32_t readU32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

// OpenType tables are big-endian.
uint16_t be16(const uint8_t* p) {
    return uint16_t(p[0] << 8 | p[1]);
}
uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Locates a table in the font's directory; returns nullptr if absent or out of bounds.
const uint8_t* findTable(const uint8_t* font, size_t size, const char* tag, uint32_t& length) {
    if (size < 12) return nullptr;
    uint16_t n = be16(font + 4);
    for (uint16_t i = 0; i < n && 12 + 16u * (i + 1) <= size; ++i) {
        const uint8_t* rec = font + 12 + 16 * i;
        if (memcmp(rec, tag, 4) != 0) continue;
        uint32_t off = be32(rec + 8);
        length = be32(rec + 12);
        return off + length <= size ? font + off : nullptr;
    }
    return nullptr;
}

/* stb_truetype produces linear coverage, which makes light-on-dark CJK strokes
 * look thin and grey on this panel. A mild gamma lift (≈1/1.3) restores the
 * stroke weight without visibly bolding the glyph. */
struct CoverageLut {
    uint8_t v[256];
    CoverageLut() {
        for (int i = 0; i < 256; ++i) v[i] = static_cast<uint8_t>(std::pow(i / 255.0f, 0.77f) * 255 + 0.5f);
    }
};
const CoverageLut LUT;

}  // namespace

bool parseFamily(const char* name, FontFamily& out) {
    if (!name || !*name || !strcmp(name, "sans")) {
        out = FAMILY_SANS;
    } else if (!strcmp(name, "kai")) {
        out = FAMILY_KAI;
    } else if (!strcmp(name, "display")) {
        out = FAMILY_DISPLAY;
    } else {
        return false;
    }
    return true;
}

const char* familyName(FontFamily f) {
    switch (f) {
        case FAMILY_KAI:
            return "kai";
        case FAMILY_DISPLAY:
            return "display";
        default:
            return "sans";
    }
}

FontEngine::~FontEngine() {
    setCacheBudget(0);
}

bool FontEngine::begin(const uint8_t* pack, size_t size) {
    if (!pack || size < 8 || memcmp(pack, "FPK1", 4) != 0) return false;
    uint32_t count = readU32(pack + 4);
    if (8 + count * 32 > size) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = pack + 8 + i * 32;
        char name[25] = {0};
        memcpy(name, e, 24);
        uint32_t off = readU32(e + 24), len = readU32(e + 28);
        if (off + len > size) continue;
        for (int f = 0; f < FACE_COUNT; ++f) {
            if (strcmp(name, FACE_NAMES[f]) != 0) continue;
            Face& face = faces_[f];
            if (!stbtt_InitFont(&face.info, pack + off, 0)) break;
            face.scalePerPx = stbtt_ScaleForMappingEmToPixels(&face.info, 1.0f);
            int asc, desc, gap;
            if (!stbtt_GetFontVMetricsOS2(&face.info, &asc, &desc, &gap)) stbtt_GetFontVMetrics(&face.info, &asc, &desc, &gap);
            face.ascent = asc * face.scalePerPx;
            face.descent = -desc * face.scalePerPx;
            loadColorTables(face, pack + off, len);
            face.loaded = true;
        }
    }
    return ready();
}

FontFace FontEngine::faceFor(FontFamily family, bool bold) {
    switch (family) {
        case FAMILY_KAI:
            return FACE_KAI;
        case FAMILY_DISPLAY:
            return FACE_DISPLAY;
        default:
            return bold ? FACE_SANS_BOLD : FACE_SANS;
    }
}

bool FontEngine::resolve(FontFace want, uint32_t cp, FontFace& face, int& glyph) const {
    // Then the other text faces, which between them cover more symbols.
    FontFace order[6] = {want, want == FACE_SANS_BOLD ? FACE_SANS_BOLD : FACE_SANS, FACE_SANS_BOLD, FACE_KAI,
                         FACE_DISPLAY, FACE_EMOJI};
    for (FontFace f : order) {
        if (!faces_[f].loaded) continue;
        int g = stbtt_FindGlyphIndex(&faces_[f].info, int(cp));
        if (g) {
            face = f;
            glyph = g;
            return true;
        }
    }
    face = faces_[want].loaded ? want : FACE_SANS;
    glyph = 0;
    return false;
}

void FontEngine::loadColorTables(Face& face, const uint8_t* font, size_t size) {
    uint32_t colrLen = 0, cpalLen = 0;
    const uint8_t* colr = findTable(font, size, "COLR", colrLen);
    const uint8_t* cpal = findTable(font, size, "CPAL", cpalLen);
    if (!colr || !cpal || colrLen < 14 || cpalLen < 12 || be16(colr) != 0) return;
    uint16_t numBase = be16(colr + 2), numLayers = be16(colr + 12);
    uint32_t baseOff = be32(colr + 4), layerOff = be32(colr + 8);
    uint16_t entries = be16(cpal + 2), records = be16(cpal + 6);
    uint32_t recordsOff = be32(cpal + 8);
    uint16_t first = be16(cpal + 12);  // palette 0's first color record
    if (baseOff + 6u * numBase > colrLen || layerOff + 4u * numLayers > colrLen ||
        recordsOff + 4u * records > cpalLen || first + entries > records)
        return;
    face.baseGlyphs = colr + baseOff;
    face.layers = colr + layerOff;
    face.numBase = numBase;
    face.numLayers = numLayers;
    face.palette = cpal + recordsOff + 4 * first;
    face.paletteSize = entries;
}

int FontEngine::colorLayers(FontFace f, int glyph, int& first) const {
    const Face& face = faces_[f];
    int lo = 0, hi = int(face.numBase) - 1;
    while (lo <= hi) {  // base glyph records are sorted by glyph id
        int mid = (lo + hi) / 2;
        const uint8_t* rec = face.baseGlyphs + 6 * mid;
        int g = be16(rec);
        if (g == glyph) {
            first = be16(rec + 2);
            int n = be16(rec + 4);
            return first + n <= face.numLayers ? n : 0;
        }
        if (g < glyph)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

void FontEngine::colorLayer(FontFace f, int index, int& glyph, RGBA& color, bool& foreground) const {
    const Face& face = faces_[f];
    const uint8_t* rec = face.layers + 4 * index;
    glyph = be16(rec);
    uint16_t pal = be16(rec + 2);
    foreground = pal == 0xFFFF || pal >= face.paletteSize;
    if (!foreground) {
        const uint8_t* c = face.palette + 4 * pal;
        color = RGBA(c[2], c[1], c[0], c[3]);
    }
}

bool FontEngine::isEmojiPresentation(uint32_t cp) {
    if (cp >= 0x1F000) return !(cp >= 0x1F100 && cp <= 0x1F1E5);  // enclosed alphanumerics are text
    static const uint16_t RANGES[][2] = {
        {0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE},
        {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
        {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4},
        {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD},
        {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E},
        {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
        {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}};
    for (const auto& r : RANGES)
        if (cp >= r[0] && cp <= r[1]) return true;
    return false;
}

float FontEngine::advance(FontFace f, int glyph, float px) const {
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&faces_[f].info, glyph, &adv, &lsb);
    return adv * faces_[f].scalePerPx * px;
}

float FontEngine::kern(FontFace f, int g1, int g2, float px) const {
    return stbtt_GetGlyphKernAdvance(&faces_[f].info, g1, g2) * faces_[f].scalePerPx * px;
}

float FontEngine::ascent(FontFace f, float px) const {
    return faces_[f].ascent * px;
}

float FontEngine::descent(FontFace f, float px) const {
    return faces_[f].descent * px;
}

const GlyphBitmap* FontEngine::glyph(FontFace f, int glyph, int px) {
    uint64_t key = (uint64_t(f) << 48) | (uint64_t(px & 0xFFFF) << 32) | uint32_t(glyph);
    auto it = index_.find(key);
    if (it != index_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second);
        return &it->second->bmp;
    }

    const Face& face = faces_[f];
    float scale = face.scalePerPx * px;
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&face.info, glyph, scale, scale, &x0, &y0, &x1, &y1);
    // Base cost approximates the list and hash nodes, so the budget is honest.
    CacheEntry entry{key, {int16_t(x1 - x0), int16_t(y1 - y0), int16_t(x0), int16_t(y0), nullptr}, 96};
    if (entry.bmp.w > 0 && entry.bmp.h > 0) {
        size_t n = size_t(entry.bmp.w) * entry.bmp.h;
        entry.bmp.alpha = static_cast<uint8_t*>(gfxAlloc(n));
        if (entry.bmp.alpha) {
            stbtt_MakeGlyphBitmap(&face.info, entry.bmp.alpha, entry.bmp.w, entry.bmp.h, entry.bmp.w, scale, scale,
                                  glyph);
            for (size_t i = 0; i < n; ++i) entry.bmp.alpha[i] = LUT.v[entry.bmp.alpha[i]];
            entry.bytes += n;
        } else {
            entry.bmp.w = entry.bmp.h = 0;
        }
    }
    lru_.push_front(entry);
    index_[key] = lru_.begin();
    cacheBytes_ += entry.bytes;
    evict();
    return &lru_.front().bmp;
}

void FontEngine::setCacheBudget(size_t bytes) {
    cacheBudget_ = bytes;
    evict();
}

void FontEngine::evict() {
    // Never evict the entry just inserted at the front: its pointer is returned.
    while (cacheBytes_ > cacheBudget_ && lru_.size() > (cacheBudget_ ? 1u : 0u)) {
        CacheEntry& e = lru_.back();
        cacheBytes_ -= e.bytes;
        gfxFree(e.bmp.alpha);
        index_.erase(e.key);
        lru_.pop_back();
    }
}
