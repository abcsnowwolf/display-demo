#pragma once

#include <cstdint>
#include <vector>

#include "FontEngine.h"
#include "Style.h"

enum class Align : uint8_t { Left, Center, Right };
enum class VAlign : uint8_t { Top, Middle, Bottom };

struct TextStyle {
    FontFamily family = FAMILY_SANS;
    bool bold = false;
    bool underline = false;
    float scale = 1;
    RGBA color;
    RGBA highlight;  // fill behind the glyphs, transparent = none
};

struct RichText {
    std::vector<uint32_t> cps;
    std::vector<uint16_t> style;  // per codepoint, index into styles
    std::vector<TextStyle> styles;

    bool empty() const { return cps.empty(); }
};

/* Parses the inline markup understood by every text field:
 *   **bold**   [b]bold[/b]   [u]underline[/u]
 *   [color=red]..[/color]  (alias [c=..])   [bg=#fde68a]highlight[/bg]
 *   [size=1.5]..[/size] (relative)          [font=kai]..[/font]
 *   [/] closes the innermost tag; \[ \* \\ escape.
 * Unknown or malformed tags are kept as literal text. With markup == false
 * the string is taken verbatim. */
void parseRichText(const char* utf8, const TextStyle& base, const Theme& theme, bool markup, RichText& out);

struct LayoutGlyph {
    float x;         // pen x relative to the block's left edge (alignment applied)
    float baseline;  // relative to the block top
    float advance;
    int glyph;
    uint16_t px;
    uint16_t style;
    FontFace face;
};

struct LayoutLine {
    float top;
    float height;
    float width;  // without trailing spaces
    int first;    // index into glyphs
    int count;
    bool paragraphStart;  // first line of the text or after a '\n'
    int words;            // visible glyphs, punctuation excluded
    float leading;        // height minus ascent+descent, split above/below
};

struct TextLayout {
    std::vector<LayoutGlyph> glyphs;
    std::vector<LayoutLine> lines;
    float width = 0;   // widest line
    float height = 0;  // sum of line heights
    bool truncated = false;
    bool brokeWord = false;  // a word had to be split mid-way

    // Height without the half-leading above the first and below the last line.
    float tightHeight() const {
        return lines.empty() ? 0 : height - lines.front().leading / 2 - lines.back().leading / 2;
    }
};

struct LayoutOptions {
    float maxWidth = 1e9f;   // alignment box
    float wrapWidth = 0;     // line-breaking width, 0 = maxWidth (balanced centring)
    float size = 20;
    float lineHeight = 1.3f;  // multiple of the font size
    int maxLines = 0;         // 0 = unlimited
    bool ellipsis = true;     // end the last allowed line with "…" when cut
    Align align = Align::Left;
};

void layoutRichText(FontEngine& fonts, const RichText& text, const LayoutOptions& opt, TextLayout& out);

struct FitOptions {
    float width = 0;
    float height = 0;
    float size = 0;  // > 0: fixed size; 0: pick the largest that fits
    float minSize = 14;
    float maxSize = 72;
    float lineHeight = 1.3f;
    int maxLines = 0;
    Align align = Align::Left;
    bool paginate = false;  // overflow at minSize -> pages instead of ellipsis
};

struct FitResult {
    float size = 0;
    std::vector<int> pageFirstLine;  // one entry per page
    bool truncated = false;

    int pages() const { return pageFirstLine.empty() ? 1 : int(pageFirstLine.size()); }
};

void fitText(FontEngine& fonts, const RichText& text, const FitOptions& opt, TextLayout& out, FitResult& res);

// Line range [first, last) of a page produced by fitText.
void pageLines(const TextLayout& layout, const FitResult& res, int page, int& first, int& last);
