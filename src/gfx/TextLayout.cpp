#include "TextLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Utf8.h"

namespace {

bool isCjk(uint32_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFFEF) || c >= 0x20000;
}

bool isEmojiBlock(uint32_t c) {
    return (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF);
}

/* Zero-width joiners of emoji sequences. Without shaping they can't combine,
 * so they are dropped: a ZWJ family shows its members side by side, a
 * skin-toned hand shows the default tone. */
bool isInvisible(uint32_t c) {
    return c == 0x200D || c == 0xFE0E || c == 0xFE0F || c == 0x20E3 || (c >= 0x1F3FB && c <= 0x1F3FF) ||
           (c >= 0xE0020 && c <= 0xE007F);
}

bool isSpace(uint32_t c) {
    return c == ' ' || c == '\t' || c == 0x3000;
}

// Kinsoku: characters that must not start a line...
bool noLineStart(uint32_t c) {
    static const char16_t S[] = u"，。、；：！？）」』】》〉〕］｝”’…—～·・%‰℃°,.;:!?)]}>";
    for (const char16_t* p = S; *p; ++p)
        if (c == *p) return true;
    return false;
}

// ...and characters that must not end one.
bool noLineEnd(uint32_t c) {
    static const char16_t S[] = u"（「『【《〈〔［｛“‘([{<$¥￥#@";
    for (const char16_t* p = S; *p; ++p)
        if (c == *p) return true;
    return false;
}

bool canBreakBefore(const std::vector<uint32_t>& cps, size_t i) {
    if (i == 0) return false;
    uint32_t prev = cps[i - 1], cur = cps[i];
    if (isInvisible(cur)) return false;  // stays with the emoji it modifies
    if (noLineStart(cur) || noLineEnd(prev)) return false;
    if (isSpace(prev) && !isSpace(cur)) return true;
    if (isCjk(prev) || isCjk(cur) || isEmojiBlock(prev) || isEmojiBlock(cur) || isInvisible(prev)) return !isSpace(cur);
    return prev == '-' || prev == '/';
}

struct Measured {
    FontFace face;
    int glyph;
    uint16_t px;
    float advance;
    float asc, desc;
};

void measure(FontEngine& fonts, const RichText& text, float size, std::vector<Measured>& out) {
    out.resize(text.cps.size());
    for (size_t i = 0; i < text.cps.size(); ++i) {
        const TextStyle& st = text.styles[text.style[i]];
        Measured& m = out[i];
        uint32_t cp = text.cps[i];
        m.px = static_cast<uint16_t>(std::max(6.0f, std::round(size * st.scale)));
        FontFace want = FontEngine::faceFor(st.family, st.bold);
        if (cp == '\t') cp = ' ';
        if (isInvisible(cp)) {
            m.face = want;
            m.glyph = -1;  // not drawn
            m.advance = 0;
            m.asc = m.desc = 0;
            continue;
        }
        // Emoji font first for emoji-default codepoints or an explicit VS16;
        // VS15 (U+FE0E) asks for the text glyph.
        uint32_t next = i + 1 < text.cps.size() ? text.cps[i + 1] : 0;
        if (next != 0xFE0E && (next == 0xFE0F || FontEngine::isEmojiPresentation(cp)) && fonts.hasFace(FACE_EMOJI))
            want = FACE_EMOJI;
        fonts.resolve(want, cp, m.face, m.glyph);
        m.advance = cp == '\n' ? 0 : fonts.advance(m.face, m.glyph, m.px);
        // Faux bold widens strokes by ~1px; keep the spacing honest.
        if (st.bold && m.face != FACE_SANS_BOLD && m.face != FACE_EMOJI && cp != ' ')
            m.advance += std::max(1.0f, m.px / 24.0f);
        if (i > 0 && cp < 0x2000 && text.cps[i - 1] < 0x2000 && out[i - 1].face == m.face &&
            out[i - 1].px == m.px) {
            out[i - 1].advance += fonts.kern(m.face, out[i - 1].glyph, m.glyph, m.px);
        }
        // Symbol fonts would otherwise change the line height.
        FontFace metrics = m.face == FACE_ICONS || m.face == FACE_EMOJI ? FACE_SANS : m.face;
        m.asc = fonts.ascent(metrics, m.px);
        m.desc = fonts.descent(metrics, m.px);
    }
}

}  // namespace

void parseRichText(const char* s, const TextStyle& base, const Theme& theme, bool markup, RichText& out) {
    out = RichText();
    out.styles.push_back(base);
    if (!s) return;

    struct Frame {
        std::string tag;
        uint16_t style;
    };
    std::vector<Frame> stack{{"", 0}};
    auto push = [&](const std::string& tag, const TextStyle& st) {
        out.styles.push_back(st);
        stack.push_back({tag, static_cast<uint16_t>(out.styles.size() - 1)});
    };
    auto emit = [&](uint32_t cp) {
        out.cps.push_back(cp);
        out.style.push_back(stack.back().style);
    };

    while (*s) {
        if (markup && s[0] == '\\' && (s[1] == '[' || s[1] == '*' || s[1] == '\\')) {
            emit(static_cast<unsigned char>(s[1]));
            s += 2;
            continue;
        }
        if (markup && s[0] == '*' && s[1] == '*') {
            if (stack.size() > 1 && stack.back().tag == "**") {
                stack.pop_back();
            } else {
                TextStyle st = out.styles[stack.back().style];
                st.bold = true;
                push("**", st);
            }
            s += 2;
            continue;
        }
        if (markup && s[0] == '[') {
            const char* end = strchr(s, ']');
            if (end && end - s <= 48) {
                std::string body(s + 1, end);
                bool closing = !body.empty() && body[0] == '/';
                if (closing) {
                    std::string name = body.substr(1);
                    if (name == "c") name = "color";
                    // "[/]" closes the innermost tag; otherwise unwind to the match.
                    int match = -1;
                    for (int i = int(stack.size()) - 1; i > 0; --i) {
                        if (name.empty() || stack[i].tag == name) {
                            match = i;
                            break;
                        }
                    }
                    if (match > 0) {
                        stack.resize(match);
                        s = end + 1;
                        continue;
                    }
                } else {
                    std::string name = body, value;
                    size_t eq = body.find('=');
                    if (eq != std::string::npos) {
                        name = body.substr(0, eq);
                        value = body.substr(eq + 1);
                    }
                    if (name == "c") name = "color";
                    TextStyle st = out.styles[stack.back().style];
                    bool ok = true;
                    if (name == "b" && value.empty()) {
                        st.bold = true;
                    } else if (name == "u" && value.empty()) {
                        st.underline = true;
                    } else if (name == "color") {
                        ok = parseColor(value.c_str(), theme, st.color);
                    } else if (name == "bg") {
                        ok = parseColor(value.c_str(), theme, st.highlight);
                    } else if (name == "size") {
                        float v = strtof(value.c_str(), nullptr);
                        ok = v >= 0.3f && v <= 4.0f;
                        st.scale *= v;
                    } else if (name == "font") {
                        ok = parseFamily(value.c_str(), st.family);
                    } else {
                        ok = false;
                    }
                    if (ok) {
                        push(name, st);
                        s = end + 1;
                        continue;
                    }
                }
            }
        }
        uint32_t cp = utf8Next(s);
        if (cp == '\r') continue;
        emit(cp);
    }
}

void layoutRichText(FontEngine& fonts, const RichText& text, const LayoutOptions& opt, TextLayout& out) {
    out = TextLayout();
    const size_t n = text.cps.size();
    std::vector<Measured> m;
    measure(fonts, text, opt.size, m);

    // Greedy line breaking over break opportunities.
    struct Range {
        size_t begin, end;
    };
    std::vector<Range> lines;
    const float wrap = opt.wrapWidth > 0 ? std::min(opt.wrapWidth, opt.maxWidth) : opt.maxWidth;
    size_t i = 0;
    while (i < n || lines.empty()) {
        size_t start = i;
        float width = 0;
        size_t lastBreak = 0;  // 0 = none yet
        while (i < n && text.cps[i] != '\n') {
            if (i > start && canBreakBefore(text.cps, i)) lastBreak = i;
            float w = width + m[i].advance;
            if (w > wrap && i > start && !isSpace(text.cps[i])) {
                if (lastBreak > start) {
                    i = lastBreak;
                } else {
                    out.brokeWord = out.brokeWord || !isCjk(text.cps[i]);
                }
                break;
            }
            width = w;
            ++i;
        }
        lines.push_back({start, i});
        if (i < n && text.cps[i] == '\n') {
            ++i;
            if (i == n) lines.push_back({n, n});  // trailing newline -> empty line
        } else {
            while (i < n && text.cps[i] == ' ') ++i;  // spaces at a soft break vanish
        }
        if (i >= n) break;
    }

    if (opt.maxLines > 0 && int(lines.size()) > opt.maxLines) {
        lines.resize(opt.maxLines);
        out.truncated = true;
    }

    float top = 0;
    for (size_t li = 0; li < lines.size(); ++li) {
        Range r = lines[li];
        bool ellipsize = out.truncated && li + 1 == lines.size() && opt.ellipsis;
        float lineW = 0;
        for (size_t k = r.begin; k < r.end; ++k) lineW += m[k].advance;

        Measured dots{};
        if (ellipsize) {
            size_t styleSrc = r.end > r.begin ? r.end - 1 : std::min(r.begin, n - 1);
            RichText one;
            one.cps = {0x2026};
            one.style = {0};
            one.styles = {text.styles[text.style[styleSrc]]};
            std::vector<Measured> md;
            measure(fonts, one, opt.size, md);
            dots = md[0];
            while (r.end > r.begin && lineW + dots.advance > opt.maxWidth) lineW -= m[--r.end].advance;
        }
        // Trailing spaces neither count toward width nor alignment.
        size_t visibleEnd = r.end;
        while (visibleEnd > r.begin && isSpace(text.cps[visibleEnd - 1])) lineW -= m[--visibleEnd].advance;

        float asc = 0, desc = 0, maxPx = 0;
        for (size_t k = r.begin; k < r.end; ++k) {
            asc = std::max(asc, m[k].asc);
            desc = std::max(desc, m[k].desc);
            maxPx = std::max(maxPx, float(m[k].px));
        }
        if (maxPx == 0) {
            maxPx = opt.size;
            asc = fonts.ascent(FACE_SANS, opt.size);
            desc = fonts.descent(FACE_SANS, opt.size);
        }
        if (ellipsize) {
            asc = std::max(asc, dots.asc);
            desc = std::max(desc, dots.desc);
            lineW += dots.advance;
        }
        float height = std::max(maxPx * opt.lineHeight, asc + desc);
        float baseline = top + (height - asc - desc) / 2 + asc;

        float x = 0;
        if (opt.align == Align::Center) x = (opt.maxWidth - lineW) / 2;
        if (opt.align == Align::Right) x = opt.maxWidth - lineW;
        x = std::max(0.0f, x);

        bool paraStart = r.begin == 0 || text.cps[r.begin - 1] == '\n';
        LayoutLine line{top, height, lineW, int(out.glyphs.size()), 0, paraStart, 0, height - asc - desc};
        for (size_t k = r.begin; k < visibleEnd; ++k) {
            if (m[k].glyph < 0) continue;
            out.glyphs.push_back({x, baseline, m[k].advance, m[k].glyph, m[k].px, text.style[k], m[k].face});
            x += m[k].advance;
            // Each CJK character is a word; a Latin word counts once.
            uint32_t cp = text.cps[k];
            if (noLineStart(cp) || isSpace(cp)) continue;
            if (isCjk(cp) || k == r.begin || isSpace(text.cps[k - 1]) || isCjk(text.cps[k - 1])) ++line.words;
        }
        if (ellipsize) {
            uint16_t st = text.style[r.end > r.begin ? r.end - 1 : std::min(r.begin, n - 1)];
            out.glyphs.push_back({x, baseline, dots.advance, dots.glyph, dots.px, st, dots.face});
        }
        line.count = int(out.glyphs.size()) - line.first;
        out.lines.push_back(line);
        out.width = std::max(out.width, lineW);
        top += height;
    }
    out.height = top;
}

namespace {

bool hasWidow(const TextLayout& l, float maxWidth) {
    for (size_t i = 0; i < l.lines.size(); ++i) {
        const LayoutLine& line = l.lines[i];
        bool paragraphEnd = i + 1 == l.lines.size() || l.lines[i + 1].paragraphStart;
        if (paragraphEnd && !line.paragraphStart && (line.words <= 2 || line.width < maxWidth * 0.2f)) return true;
    }
    return false;
}

/* Centred multi-line text looks best with even line lengths: find the
 * narrowest wrap width that keeps the same number of lines. */
void balance(FontEngine& fonts, const RichText& text, LayoutOptions lo, TextLayout& out) {
    size_t lines = out.lines.size();
    if (lines < 2 || lines > 6 || out.truncated) return;
    float lo_ = lo.maxWidth * 0.4f, hi = lo.maxWidth;
    TextLayout probe;
    for (int iter = 0; iter < 8; ++iter) {
        float mid = (lo_ + hi) / 2;
        lo.wrapWidth = mid;
        layoutRichText(fonts, text, lo, probe);
        if (probe.lines.size() == lines && !probe.brokeWord)
            hi = mid;
        else
            lo_ = mid;
    }
    lo.wrapWidth = hi;
    layoutRichText(fonts, text, lo, probe);
    if (probe.lines.size() == lines && !probe.brokeWord) out = std::move(probe);
}

}  // namespace

void fitText(FontEngine& fonts, const RichText& text, const FitOptions& opt, TextLayout& out, FitResult& res) {
    res = FitResult();
    LayoutOptions lo;
    lo.maxWidth = opt.width;
    lo.lineHeight = opt.lineHeight;
    lo.maxLines = opt.maxLines;
    lo.align = opt.align;

    auto fits = [&](float size) {
        lo.size = size;
        layoutRichText(fonts, text, lo, out);
        return out.tightHeight() <= opt.height + 0.5f && !out.truncated && !out.brokeWord;
    };

    // Bounded so one glyph bitmap can't exhaust the heap.
    const float MIN_PX = 6, MAX_PX = 200;
    float size = opt.size > 0 ? std::max(MIN_PX, std::min(opt.size, MAX_PX)) : 0;
    if (size <= 0) {
        float minSize = std::max(MIN_PX, std::min(opt.minSize, MAX_PX));
        int lo_ = int(minSize), hi = int(std::max(minSize, std::min(opt.maxSize, MAX_PX)));
        if (!fits(lo_)) {
            size = lo_;
        } else {
            while (lo_ < hi) {
                int mid = (lo_ + hi + 1) / 2;
                if (fits(mid))
                    lo_ = mid;
                else
                    hi = mid - 1;
            }
            size = lo_;
        }
    }
    // A paragraph ending in a lone character or two ("…地上\n霜。") reads
    // badly; give up to 20% of the size to avoid it.
    if (opt.size <= 0 && size > opt.minSize) {
        for (float s = size; s >= std::max(std::max(MIN_PX, opt.minSize), size * 0.8f); s -= 1) {
            if (fits(s) && !hasWidow(out, lo.maxWidth)) {
                size = s;
                break;
            }
        }
    }
    lo.size = size;
    res.size = size;
    layoutRichText(fonts, text, lo, out);
    if (out.tightHeight() <= opt.height + 0.5f) {
        res.truncated = out.truncated;
        if (opt.align == Align::Center) balance(fonts, text, lo, out);
        return;
    }

    if (opt.paginate) {
        float pageTop = 0;
        res.pageFirstLine.push_back(0);
        for (size_t i = 0; i < out.lines.size(); ++i) {
            const LayoutLine& l = out.lines[i];
            if (l.top + l.height - pageTop > opt.height + 0.5f && int(i) > res.pageFirstLine.back()) {
                res.pageFirstLine.push_back(int(i));
                pageTop = l.top;
            }
        }
        return;
    }

    // Keep as many lines as fit and end with an ellipsis.
    int keep = 0;
    for (const LayoutLine& l : out.lines) {
        if (l.top + l.height - (l.leading + out.lines[0].leading) / 2 > opt.height + 0.5f) break;
        ++keep;
    }
    lo.maxLines = std::max(1, keep);
    layoutRichText(fonts, text, lo, out);
    res.truncated = true;
}

void pageLines(const TextLayout& layout, const FitResult& res, int page, int& first, int& last) {
    if (res.pageFirstLine.empty()) {
        first = 0;
        last = int(layout.lines.size());
        return;
    }
    page = std::max(0, std::min(page, res.pages() - 1));
    first = res.pageFirstLine[page];
    last = page + 1 < res.pages() ? res.pageFirstLine[page + 1] : int(layout.lines.size());
}
