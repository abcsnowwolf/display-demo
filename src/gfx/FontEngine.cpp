#include "FontEngine.h"

#include <cmath>
#include <cstring>

#include "GfxAlloc.h"

namespace {

const char* const FACE_NAMES[FACE_COUNT] = {"sans", "sans-bold", "kai", "display", "icons"};

uint32_t readU32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
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
    FontFace order[5] = {want, want == FACE_SANS_BOLD ? FACE_SANS_BOLD : FACE_SANS, FACE_SANS_BOLD, FACE_KAI,
                         FACE_DISPLAY};
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
    CacheEntry entry{key, {int16_t(x1 - x0), int16_t(y1 - y0), int16_t(x0), int16_t(y0), nullptr}, 64};
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
