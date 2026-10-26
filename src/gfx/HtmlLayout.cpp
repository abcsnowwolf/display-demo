#include "HtmlLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "Utf8.h"

namespace HtmlLayout {
namespace {
constexpr float LIMIT = 1024;
enum class Unit { Auto, Px, Percent };
struct Length {
    float n = 0;
    Unit unit = Unit::Auto;
    float get(float ref, float fallback) const {
        return unit == Unit::Auto ? fallback : (unit == Unit::Percent ? ref * n / 100 : n);
    }
};
enum class Cross { Auto, Start, Center, End, Stretch };
enum class Justify { Start, Center, End, Between, Around, Evenly };
struct Style {
    Length w, h, minW, minH, maxW, maxH, basis;
    float pad[4] = {};  // top right bottom left; all sizes are border-box
    float gap = 0, grow = 0, shrink = 1, radius = 0, border = 0;
    bool flex = false, row = true, wrap = false, hidden = false;
    Cross align = Cross::Stretch, self = Cross::Auto;
    Justify justify = Justify::Start;
    TextStyle text;
    float size = 16, leading = 1.3f;
    Align textAlign = Align::Left;
    RGBA bg, borderColor;
    float px() const { return pad[1] + pad[3] + 2 * border; }
    float py() const { return pad[0] + pad[2] + 2 * border; }
};
// Text and child elements in source order, so mixed content can be turned
// into inline rich text after parsing.
struct Seg { int child; std::string text; };
struct Node {
    Style s;
    std::string tag, id, text, bullet;
    std::vector<int> children;
    std::vector<Seg> seq;
    bool inlineText = false;  // text holds rich-text markup built from inline children
    bool flattened = false;   // merged into an ancestor's inline text
    int listCount = 0;        // ol numbering
    RichText rich;
    Box box;
    float preferred = -1;
    float cacheW = -1, cacheRef = -2, cacheHeight = 0;
};
struct Item { int node; float main, cross, min, max, weight; };
struct Plan { std::vector<Box> boxes; float height = 0; };

bool space(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && space(s[a])) ++a;
    while (b > a && space(s[b - 1])) --b;
    return s.substr(a, b - a);
}
std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
bool number(const std::string& v, float& n, float max = LIMIT) {
    if (v.empty() || v.find_first_not_of("0123456789.") != std::string::npos) return false;
    char* end;
    n = std::strtof(v.c_str(), &end);
    return end == v.c_str() + v.size() && std::isfinite(n) && n >= 0 && n <= max;
}
bool ends(const std::string& v, const char* suffix) {
    size_t n = strlen(suffix);
    return v.size() > n && v.compare(v.size() - n, n, suffix) == 0;
}
// px (or unitless), %, em (relative to `em`), rem (16px) and pt.
bool length(std::string v, Length& out, bool percent = true, bool autoValue = true, float em = 16) {
    if (v == "auto" && autoValue) { out = {}; return true; }
    out.unit = Unit::Px;
    float scale = 1;
    if (ends(v, "px")) v.resize(v.size() - 2);
    else if (ends(v, "rem")) { v.resize(v.size() - 3); scale = 16; }
    else if (ends(v, "em")) { v.resize(v.size() - 2); scale = em; }
    else if (ends(v, "pt")) { v.resize(v.size() - 2); scale = 4.0f / 3; }
    else if (!v.empty() && v.back() == '%' && percent) { out.unit = Unit::Percent; v.pop_back(); }
    if (!number(v, out.n, out.unit == Unit::Percent ? 100 : LIMIT)) return false;
    out.n *= scale;
    return out.n <= LIMIT;
}
bool pixels(const std::string& v, float& out, float max = LIMIT, float em = 16) {
    Length l;
    if (!length(v, l, false, false, em) || l.n > max) return false;
    out = l.n;
    return true;
}
// rgb()/rgba() with 0-255 channels and 0-1 (or %) alpha.
bool rgbFunction(const std::string& v, RGBA& c) {
    bool alpha = v.compare(0, 5, "rgba(") == 0;
    if (!alpha && v.compare(0, 4, "rgb(") != 0) return false;
    if (v.back() != ')') return false;
    std::string body = v.substr(alpha ? 5 : 4, v.size() - (alpha ? 6 : 5));
    for (char& ch : body) if (ch == ',' || ch == '/') ch = ' ';
    std::istringstream in(body); std::string t; float ch[4] = {0, 0, 0, 1}; int n = 0;
    while (in >> t && n < 4) {
        float f; bool pct = !t.empty() && t.back() == '%'; if (pct) t.pop_back();
        if (!number(t, f, pct ? 100 : 255)) return false;
        ch[n] = n < 3 ? (pct ? f * 2.55f : f) : (pct ? f / 100 : std::min(1.0f, f));
        ++n;
    }
    if (n < 3 || in >> t) return false;
    c = RGBA(uint8_t(ch[0] + .5f), uint8_t(ch[1] + .5f), uint8_t(ch[2] + .5f), uint8_t(ch[3] * 255 + .5f));
    return true;
}
// Tags understood as boxes, as inline formatting, or dropped/refused.
bool boxTag(const std::string& t) {
    static const char* const T[] = {"div", "p", "span", "section", "header", "footer", "article", "main", "nav",
                                    "aside", "blockquote", "figure", "ul", "ol", "li", "h1", "h2", "h3", "h4", "h5", "h6"};
    for (const char* x : T) if (t == x) return true;
    return false;
}
bool inlineTag(const std::string& t) {
    static const char* const T[] = {"b", "strong", "em", "i", "u", "small", "mark", "code", "a", "s", "sub", "sup"};
    for (const char* x : T) if (t == x) return true;
    return false;
}
// Active or embedded content is never rendered; refuse instead of guessing.
bool refusedTag(const std::string& t) {
    static const char* const T[] = {"script", "style", "iframe", "object", "embed", "svg", "canvas", "video", "audio",
                                    "form", "input", "button", "textarea", "select", "link", "meta", "template"};
    for (const char* x : T) if (t == x) return true;
    return false;
}
bool cross(const std::string& v, Cross& a, bool allowAuto) {
    if (v == "start" || v == "flex-start") a = Cross::Start;
    else if (v == "end" || v == "flex-end") a = Cross::End;
    else if (v == "center") a = Cross::Center;
    else if (v == "stretch") a = Cross::Stretch;
    else if (v == "auto" && allowAuto) a = Cross::Auto;
    else return false;
    return true;
}

class Engine {
public:
    Engine(FontEngine& f, const Theme& t, Report& r) : fonts(f), theme(t), rep(r) { nodes.reserve(MAX_NODES); }
    FontEngine& fonts;
    const Theme& theme;
    Report& rep;
    // The bounded node table is the largest persistent parser allocation;
    // prefer PSRAM on the board, preserving internal RAM for WiFi/BLE.
    std::vector<Node, GfxAllocator<Node>> nodes;

    bool fail(const std::string& s) { if (rep.error.empty()) rep.error = "html: " + s; return false; }
    void warn(const std::string& s) {
        std::string w = "html: " + s;
        if (rep.warnings.size() >= 12 || std::find(rep.warnings.begin(), rep.warnings.end(), w) != rep.warnings.end()) return;
        rep.warnings.push_back(w);
    }
    bool step() { return ++rep.steps <= MAX_STEPS || fail("layout work limit exceeded"); }

    /* Each declaration is applied to a copy and kept only if valid, so an
     * unsupported property or value is skipped with a warning instead of
     * failing the whole fragment. `parentSize` resolves em units. */
    void css(Style& s, const std::string& input, float parentSize) {
        size_t p = 0;
        while (p < input.size()) {
            size_t end = input.find(';', p);
            if (end == std::string::npos) end = input.size();
            std::string d = trim(input.substr(p, end - p));
            p = end + 1;
            if (d.empty()) continue;
            size_t colon = d.find(':');
            if (colon == std::string::npos) { warn("ignored CSS without ':' (" + d.substr(0, 32) + ")"); continue; }
            std::string k = lower(trim(d.substr(0, colon))), v = lower(trim(d.substr(colon + 1)));
            if (ends(v, "!important")) v = trim(v.substr(0, v.size() - 10));
            Style t = s;
            int ok = declaration(t, k, v, parentSize);
            if (ok > 0) s = t;
            else if (ok == 0) warn("ignored CSS value " + k.substr(0, 32) + ":" + v.substr(0, 32));
            else warn("ignored unsupported CSS property: " + k.substr(0, 32));
        }
        if (s.wrap && (!s.flex || !s.row)) { warn("flex-wrap needs display:flex with flex-direction:row; ignored"); s.wrap = false; }
    }
    // 1 applied, 0 bad value, -1 unknown property.
    int declaration(Style& s, const std::string& k, const std::string& v, float em) {
        bool ok = true;
        if (k == "width") ok = length(v, s.w, true, true, em);
        else if (k == "height") ok = length(v, s.h, true, true, em);
        else if (k == "min-width") ok = length(v, s.minW, true, false, em);
        else if (k == "max-width") ok = v == "none" ? (s.maxW = {}, true) : length(v, s.maxW, true, false, em);
        else if (k == "min-height") ok = length(v, s.minH, true, false, em);
        else if (k == "max-height") ok = v == "none" ? (s.maxH = {}, true) : length(v, s.maxH, true, false, em);
        else if (k == "flex-basis") ok = length(v, s.basis, true, true, em);
        else if (k == "flex-grow") ok = number(v, s.grow, 100);
        else if (k == "flex-shrink") ok = number(v, s.shrink, 100);
        else if (k == "flex") {
            if (v == "none") { s.grow = 0; s.shrink = 0; s.basis = {}; }
            else if (v == "auto") { s.grow = 1; s.shrink = 1; s.basis = {}; }
            else if (v == "initial") { s.grow = 0; s.shrink = 1; s.basis = {}; }
            else {
                // flex: <grow> [<shrink>] [<basis>]; a lone number has zero basis.
                std::istringstream in(v); std::string tok; std::vector<std::string> parts;
                while (in >> tok) parts.push_back(tok);
                ok = !parts.empty() && parts.size() <= 3 && number(parts[0], s.grow, 100);
                s.shrink = 1; s.basis = {0, Unit::Px};
                if (ok && parts.size() >= 2) {
                    float n;
                    if (number(parts[1], n, 100)) s.shrink = n;
                    else ok = parts.size() == 2 && length(parts[1], s.basis, true, true, em);
                }
                if (ok && parts.size() == 3) ok = length(parts[2], s.basis, true, true, em);
            }
        } else if (k == "display") {
            if (v == "none") s.hidden = true;
            else if (v == "flex" || v == "inline-flex") s.flex = true;
            else if (v == "block" || v == "inline-block" || v == "inline" || v == "list-item") s.flex = false;
            else ok = false;
        }
        else if (k == "flex-direction") { ok = v == "row" || v == "column"; s.row = v == "row"; }
        else if (k == "flex-wrap") { ok = v == "wrap" || v == "nowrap"; s.wrap = v == "wrap"; }
        else if (k == "gap" || k == "row-gap" || k == "column-gap") ok = pixels(v.substr(0, v.find(' ')), s.gap, 128, em);
        else if (k == "align-items") ok = cross(v, s.align, false);
        else if (k == "align-self") ok = cross(v, s.self, true);
        else if (k == "justify-content") {
            if (v == "start" || v == "flex-start" || v == "left" || v == "normal") s.justify = Justify::Start;
            else if (v == "end" || v == "flex-end" || v == "right") s.justify = Justify::End;
            else if (v == "center") s.justify = Justify::Center;
            else if (v == "space-between") s.justify = Justify::Between;
            else if (v == "space-around") s.justify = Justify::Around;
            else if (v == "space-evenly") s.justify = Justify::Evenly;
            else ok = false;
        } else if (k == "padding") {
            std::istringstream in(v); std::string token; std::vector<float> p;
            while (in >> token) {
                float n;
                if (p.size() == 4 || !pixels(token, n, 128, em)) { ok = false; break; }
                p.push_back(n);
            }
            if (p.empty()) ok = false;
            if (ok) { s.pad[0] = p[0]; s.pad[1] = p.size() > 1 ? p[1] : p[0];
                s.pad[2] = p.size() > 2 ? p[2] : p[0]; s.pad[3] = p.size() > 3 ? p[3] : s.pad[1]; }
        } else if (k == "padding-top") ok = pixels(v, s.pad[0], 128, em);
        else if (k == "padding-right") ok = pixels(v, s.pad[1], 128, em);
        else if (k == "padding-bottom") ok = pixels(v, s.pad[2], 128, em);
        else if (k == "padding-left") ok = pixels(v, s.pad[3], 128, em);
        else if (k == "background" || k == "background-color") ok = color(v, s.bg);
        else if (k == "color") ok = color(v, s.text.color);
        else if (k == "border-color") ok = color(v, s.borderColor);
        else if (k == "border-width") ok = pixels(v, s.border, 16, em);
        else if (k == "border") {
            // border: <width> [style] [color] in any order; "none"/"0" clears it.
            if (v == "none" || v == "0") { s.border = 0; return 1; }
            std::istringstream in(v); std::string tok; bool any = false;
            while (in >> tok) {
                float w; RGBA c;
                if (pixels(tok, w, 16, em)) { s.border = w; any = true; }
                else if (tok == "solid" || tok == "dashed" || tok == "dotted" || tok == "double") any = true;
                else if (color(tok, c)) { s.borderColor = c; any = true; }
                else ok = false;
            }
            if (ok && any && s.border == 0) s.border = 1;
            ok = ok && any;
        }
        else if (k == "border-radius") ok = pixels(v.substr(0, v.find(' ')), s.radius, 128, em);
        else if (k == "font-size") {
            Length l;
            ok = length(v, l, true, false, em);
            if (ok) {
                s.size = l.unit == Unit::Percent ? em * l.n / 100 : l.n;
                if (s.size < 8 || s.size > 64) { warn("font-size clamped to 8..64px"); s.size = std::max(8.0f, std::min(64.0f, s.size)); }
            }
        }
        else if (k == "font-family") {
            // First recognised family in a fallback list; anything else is sans.
            std::string list = v; for (char& c : list) if (c == '"' || c == '\'') c = ' ';
            FontFamily f = FAMILY_SANS; bool found = false; size_t a = 0;
            while (a <= list.size() && !found) {
                size_t b = list.find(',', a); if (b == std::string::npos) b = list.size();
                found = parseFamily(trim(list.substr(a, b - a)).c_str(), f) && trim(list.substr(a, b - a)) != "";
                a = b + 1;
            }
            s.text.family = f;
            if (!found) warn("unknown font-family " + v.substr(0, 32) + ", using sans");
        }
        else if (k == "font-weight") {
            float n;
            if (v == "bold" || v == "bolder") s.text.bold = true;
            else if (v == "normal" || v == "lighter") s.text.bold = false;
            else if (number(v, n, 1000) && n >= 1) s.text.bold = n >= 600;
            else ok = false;
        }
        else if (k == "line-height") {
            float n; Length l;
            if (v == "normal") s.leading = 1.3f;
            else if (number(v, n, 2.5f)) s.leading = n;
            else if (length(v, l, true, false, em)) s.leading = l.unit == Unit::Percent ? l.n / 100 : l.n / s.size;
            else ok = false;
            if (ok) s.leading = std::max(1.0f, std::min(2.5f, s.leading));
        }
        else if (k == "text-align") {
            ok = v == "left" || v == "center" || v == "right" || v == "start" || v == "end" || v == "justify";
            s.textAlign = v == "center" ? Align::Center : (v == "right" || v == "end" ? Align::Right : Align::Left);
        }
        else if (k == "text-decoration" || k == "text-decoration-line") {
            ok = v == "none" || v.find("underline") != std::string::npos;
            s.text.underline = v.find("underline") != std::string::npos;
        }
        else if (k == "box-sizing" || k == "overflow" || k == "overflow-x" || k == "overflow-y") return 1;  // always border-box, clipped
        else return -1;
        return ok ? 1 : 0;
    }
    bool color(const std::string& v, RGBA& c) {
        if (v == "transparent") { c = {}; return true; }
        if (rgbFunction(v, c)) return true;
        // CSS names the project palette lacks.
        static const struct { const char* name; uint32_t rgb; } EXTRA[] = {
            {"grey", 0x808080}, {"silver", 0xC0C0C0}, {"gold", 0xFFD700}, {"navy", 0x000080}, {"maroon", 0x800000},
            {"olive", 0x808000}, {"aqua", 0x00FFFF}, {"lightgray", 0xD3D3D3}, {"darkgray", 0xA9A9A9}};
        for (const auto& e : EXTRA) if (v == e.name) { c = RGBA::hex(e.rgb); return true; }
        return parseColor(v.c_str(), theme, c);
    }

    // Validate the byte stream before handing strings to the existing UTF-8 reader.
    bool utf8(const std::string& s) {
        for (size_t i = 0; i < s.size();) {
            unsigned c = uint8_t(s[i++]), n = 0, cp = c, min = 0;
            if (c < 128) { if (c < 32 && !space(char(c))) return fail("control character"); continue; }
            if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 31; min = 0x80; }
            else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 15; min = 0x800; }
            else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 7; min = 0x10000; }
            else return fail("invalid UTF-8");
            if (i + n > s.size()) return fail("incomplete UTF-8");
            while (n--) { unsigned b = uint8_t(s[i++]); if ((b & 192) != 128) return fail("invalid UTF-8"); cp = cp * 64 + (b & 63); }
            if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return fail("invalid UTF-8 codepoint");
        }
        return true;
    }
    // Unknown or malformed entities stay as literal text.
    void decode(const std::string& raw, std::string& result) {
        for (size_t p = 0; p < raw.size();) {
            if (raw[p] != '&') { result += raw[p++]; continue; }
            size_t end = raw.find(';', p);
            if (end == std::string::npos || end - p > 12) { result += raw[p++]; continue; }
            std::string e = raw.substr(p + 1, end - p - 1);
            unsigned long cp = 0;
            if (e == "amp") result += '&';
            else if (e == "lt") result += '<';
            else if (e == "gt") result += '>';
            else if (e == "quot") result += '"';
            else if (e == "apos") result += '\'';
            else if (e == "nbsp") result += "\xC2\xA0";
            else if (e == "middot") result += "\xC2\xB7";
            else if (e == "mdash") result += "\xE2\x80\x94";
            else if (e == "hellip") result += "\xE2\x80\xA6";
            else if (e == "copy") result += "\xC2\xA9";
            else if (e == "deg") result += "\xC2\xB0";
            else if (!e.empty() && e[0] == '#' && numericEntity(e, cp)) {
                if (cp < 128) result += char(cp);
                else if (cp < 2048) { result += char(0xC0 | (cp >> 6)); result += char(0x80 | (cp & 63)); }
                else if (cp < 65536) { result += char(0xE0 | (cp >> 12)); result += char(0x80 | ((cp >> 6) & 63)); result += char(0x80 | (cp & 63)); }
                else { result += char(0xF0 | (cp >> 18)); result += char(0x80 | ((cp >> 12) & 63)); result += char(0x80 | ((cp >> 6) & 63)); result += char(0x80 | (cp & 63)); }
            } else {
                warn("kept unknown entity &" + e.substr(0, 16) + "; as text");
                result += raw[p++];
                continue;
            }
            p = end + 1;
        }
    }
    bool numericEntity(const std::string& e, unsigned long& cp) {
        bool hex = e.size() > 1 && (e[1] == 'x' || e[1] == 'X');
        std::string digits = e.substr(hex ? 2 : 1);
        if (digits.empty() || digits.size() > 8 ||
            digits.find_first_not_of(hex ? "0123456789abcdefABCDEF" : "0123456789") != std::string::npos)
            return false;
        cp = std::strtoul(digits.c_str(), nullptr, hex ? 16 : 10);
        return cp >= 32 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
    }

    // ---- source order bookkeeping for mixed content
    void addChar(int node, char c) {
        auto& seq = nodes[node].seq;
        if (seq.empty() || seq.back().child >= 0) seq.push_back({-1, ""});
        seq.back().text += c;
    }
    void addSpace(int node) {
        auto& seq = nodes[node].seq;
        if (seq.empty()) return;
        if (seq.back().child >= 0) { seq.push_back({-1, " "}); return; }
        std::string& t = seq.back().text;
        if (!t.empty() && t.back() != ' ' && t.back() != '\n') t += ' ';
    }
    // Creates an element node under `parent` with inherited text styles.
    int addNode(int parent, const std::string& tag, const std::string& id, const std::string& style) {
        Node n; n.tag = tag; n.id = id;
        const Style& ps = nodes[parent].s;
        n.s.text = ps.text; n.s.size = ps.size; n.s.leading = ps.leading; n.s.textAlign = ps.textAlign;
        // Tag defaults, overridable by style.
        static const float HEADINGS[] = {26, 22, 19, 17, 16, 15};
        if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') { n.s.size = HEADINGS[tag[1] - '1']; n.s.text.bold = true; }
        if (tag == "b" || tag == "strong") n.s.text.bold = true;
        if (tag == "u") n.s.text.underline = true;
        if (tag == "small") n.s.size = ps.size * 0.85f;
        if (tag == "mark") n.s.bg = RGBA::hex(0xFDE68A, 200);
        if (tag == "hr") { n.s.h = {1, Unit::Px}; n.s.bg = theme.border; }
        if (tag == "blockquote") { n.s.pad[3] = 10; n.s.text.color = theme.muted; }
        if (tag == "li") {
            Node& list = nodes[parent];
            n.bullet = list.tag == "ol" ? std::to_string(++list.listCount) + ". " : "\xE2\x80\xA2 ";
        }
        css(n.s, style, ps.size);
        int index = int(nodes.size());
        nodes.push_back(std::move(n));
        nodes[parent].children.push_back(index);
        nodes[parent].seq.push_back({index, ""});
        return index;
    }

    bool parse(const std::string& input) {
        if (!utf8(input)) return false;
        nodes.emplace_back(); nodes[0].tag = "viewport"; nodes[0].s.text.color = theme.text;
        std::vector<int> stack{0};
        size_t p = 0;
        auto skip = [&] { while (p < input.size() && space(input[p])) ++p; };
        auto name = [&] {
            size_t start = p;
            while (p < input.size() && ((input[p] >= 'a' && input[p] <= 'z') || (input[p] >= 'A' && input[p] <= 'Z') ||
                   (input[p] >= '0' && input[p] <= '9') || input[p] == '-' || input[p] == '_' || input[p] == ':')) ++p;
            return lower(input.substr(start, p - start));
        };
        while (p < input.size()) {
            int parent = stack.back();
            if (input[p] != '<') {
                size_t end = input.find('<', p); if (end == std::string::npos) end = input.size();
                std::string raw;
                decode(input.substr(p, end - p), raw);
                for (char c : raw) { if (space(c)) addSpace(parent); else addChar(parent, c); }
                p = end; continue;
            }
            if (input.compare(p, 4, "<!--") == 0) {
                size_t end = input.find("-->", p + 4);
                if (end == std::string::npos) return fail("unclosed comment");
                p = end + 3; continue;
            }
            if (input.compare(p, 2, "<!") == 0) {  // <!DOCTYPE html> and the like
                size_t end = input.find('>', p);
                if (end == std::string::npos) return fail("unclosed declaration");
                p = end + 1; continue;
            }
            ++p;
            bool closing = p < input.size() && input[p] == '/'; if (closing) ++p;
            std::string tag = name();
            if (tag.empty()) { addChar(parent, '<'); if (closing) addChar(parent, '/'); continue; }  // a literal "<"
            if (closing) {
                skip();
                if (p >= input.size() || input[p++] != '>') return fail("unclosed end tag </" + tag.substr(0, 32));
                // Close up to the matching element (implicitly closing what is
                // still open, like a browser); an unmatched end tag is ignored.
                int match = -1;
                for (int i = int(stack.size()) - 1; i > 0; --i) if (nodes[stack[i]].tag == tag) { match = i; break; }
                if (match < 0) { if (tag != "br" && tag != "hr" && tag != "img") warn("ignored stray </" + tag.substr(0, 32) + ">"); continue; }
                if (match != int(stack.size()) - 1) warn("auto-closed elements before </" + tag.substr(0, 32) + ">");
                stack.resize(match);
                continue;
            }
            if (refusedTag(tag)) return fail("unsupported tag: " + tag.substr(0, 32) + " (no scripts, forms or embedded media)");
            // Attributes: style and id are used; anything else is ignored.
            std::string style, id; bool hasStyle = false, hasId = false;
            skip();
            while (p < input.size() && input[p] != '>' && !(input[p] == '/' && p + 1 < input.size() && input[p + 1] == '>')) {
                std::string attr = name(); skip();
                if (attr.empty()) { ++p; skip(); continue; }
                std::string value;
                if (p < input.size() && input[p] == '=') {
                    ++p; skip();
                    if (p < input.size() && (input[p] == '\'' || input[p] == '"')) {
                        char quote = input[p++]; size_t end = input.find(quote, p);
                        if (end == std::string::npos) return fail("unclosed attribute value");
                        decode(input.substr(p, end - p), value); p = end + 1;
                    } else {
                        size_t start = p;
                        while (p < input.size() && !space(input[p]) && input[p] != '>') ++p;
                        decode(input.substr(start, p - start), value);
                    }
                }
                if (attr == "style") { if (hasStyle) warn("ignored duplicate style attribute"); else { style = value; hasStyle = true; } }
                else if (attr == "id") { if (!hasId) id = value.substr(0, 48); hasId = true; }
                else warn("ignored attribute " + attr.substr(0, 32));
                skip();
            }
            bool self = p < input.size() && input[p] == '/'; if (self) ++p;
            if (p >= input.size() || input[p++] != '>') return fail("unclosed start tag <" + tag.substr(0, 32));
            if (tag == "br") { addChar(parent, '\n'); continue; }
            if (tag == "img") { warn("images are not supported; <img> dropped"); continue; }
            if (!boxTag(tag) && !inlineTag(tag) && tag != "hr") warn("unknown tag <" + tag.substr(0, 32) + "> treated as div");
            if (nodes.size() >= MAX_NODES) return fail("node limit exceeded (96 including viewport)");
            if (stack.size() > MAX_DEPTH) return fail("nesting limit exceeded (12)");
            int index = addNode(parent, boxTag(tag) || inlineTag(tag) || tag == "hr" ? tag : "div", id, style);
            if (!self && tag != "hr") stack.push_back(index);
        }
        if (stack.size() != 1) warn("auto-closed unclosed elements at the end");
        if (!finishTree()) return false;
        size_t cps = 0;
        for (auto& n : nodes) {
            if (n.flattened) continue;
            parseRichText(n.text.c_str(), n.s.text, theme, n.inlineText, n.rich);
            cps += n.rich.cps.size();
        }
        if (cps > MAX_TEXT) return fail("text limit exceeded (2048 codepoints)");
        rep.nodes = nodes.size();
        return true;
    }

    /* Resolves each element's content after parsing:
     *  - text only            -> plain text leaf
     *  - text mixed with elements, or inline tags (b, strong, ...) -> one text
     *    leaf whose inline children become rich-text markup
     *  - elements only        -> box children; <br> between boxes in a vertical
     *    flow becomes an empty line, elsewhere whitespace is dropped */
    bool finishTree() {
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].flattened) continue;
            // display:none removes the element and its subtree.
            auto& kids = nodes[i].children;
            kids.erase(std::remove_if(kids.begin(), kids.end(), [&](int c) { return nodes[c].s.hidden; }), kids.end());
            auto& seq = nodes[i].seq;
            seq.erase(std::remove_if(seq.begin(), seq.end(), [&](const Seg& g) { return g.child >= 0 && nodes[g.child].s.hidden; }), seq.end());

            bool text = false, inlineChild = false;
            for (const Seg& g : nodes[i].seq) {
                if (g.child < 0) text = text || g.text.find_first_not_of(" \n") != std::string::npos;
                else inlineChild = inlineChild || inlineTag(nodes[g.child].tag);
            }
            if (nodes[i].children.empty() || text || inlineChild) {
                std::string out;
                bool markup = !nodes[i].children.empty();
                if (markup) { inlineMarkup(int(i), out); nodes[i].children.clear(); }
                else for (const Seg& g : nodes[i].seq) out += g.text;
                // Trim spaces only: an explicit <br> at either end stays visible.
                size_t a = out.find_first_not_of(' '), b = out.find_last_not_of(' ');
                out = a == std::string::npos ? "" : out.substr(a, b - a + 1);
                if (!nodes[i].bullet.empty() && !out.empty()) out = nodes[i].bullet + out;
                nodes[i].text = out;
                nodes[i].inlineText = markup;
                continue;
            }
            // Boxes only. Rebuild the child list so <br> can add blank lines.
            bool vertical = !(nodes[i].s.flex && nodes[i].s.row);
            std::vector<int> rebuilt;
            for (const Seg& g : std::vector<Seg>(nodes[i].seq)) {
                if (g.child >= 0) { rebuilt.push_back(g.child); continue; }
                size_t breaks = std::count(g.text.begin(), g.text.end(), '\n');
                for (size_t k = 0; vertical && k < breaks && nodes.size() < MAX_NODES; ++k) {
                    Node gapNode; gapNode.tag = "br";
                    gapNode.s.h = {nodes[i].s.size * nodes[i].s.leading, Unit::Px};
                    gapNode.flattened = false;
                    rebuilt.push_back(int(nodes.size()));
                    nodes.push_back(std::move(gapNode));
                }
            }
            nodes[i].children = rebuilt;
            if (!nodes[i].bullet.empty()) warn("list item with block children shows no bullet");
        }
        return true;
    }

    static void escapeMarkup(const std::string& in, std::string& out) {
        for (char c : in) { if (c == '[' || c == '*' || c == '\\') out += '\\'; out += c; }
    }
    static std::string hexColor(RGBA c) {
        char buf[12]; snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a); return buf;
    }
    // Serialises node `i`'s mixed content as rich-text markup. Each inline
    // child's style is expressed relative to its parent.
    void inlineMarkup(int i, std::string& out) {
        for (const Seg& g : nodes[i].seq) {
            if (g.child < 0) { escapeMarkup(g.text, out); continue; }
            Node& c = nodes[g.child];
            c.flattened = true;
            if (c.s.hidden) continue;
            const Style& ps = nodes[i].s; const Style& cs = c.s;
            if (boxTag(c.tag) && c.tag != "span") warn("<" + c.tag + "> inside text is shown inline");
            if (cs.flex || cs.w.unit != Unit::Auto || cs.h.unit != Unit::Auto || cs.pad[0] + cs.pad[1] + cs.pad[2] + cs.pad[3] > 0)
                warn("box styles on inline <" + c.tag + "> are ignored");
            std::vector<const char*> close;
            if (cs.text.bold && !ps.text.bold) { out += "[b]"; close.push_back("[/b]"); }
            if (cs.text.underline && !ps.text.underline) { out += "[u]"; close.push_back("[/u]"); }
            if (!(cs.text.color == ps.text.color)) { out += "[color=" + hexColor(cs.text.color) + "]"; close.push_back("[/color]"); }
            if (cs.bg.visible() && !(cs.bg == ps.bg)) { out += "[bg=" + hexColor(cs.bg) + "]"; close.push_back("[/bg]"); }
            if (std::fabs(cs.size - ps.size) > .01f) {
                char buf[24]; snprintf(buf, sizeof(buf), "[size=%.3f]", std::max(0.3f, std::min(4.0f, cs.size / ps.size)));
                out += buf; close.push_back("[/size]");
            }
            if (cs.text.family != ps.text.family) { out += std::string("[font=") + familyName(cs.text.family) + "]"; close.push_back("[/font]"); }
            if (!c.bullet.empty()) escapeMarkup(c.bullet, out);
            inlineMarkup(g.child, out);
            for (auto it = close.rbegin(); it != close.rend(); ++it) out += *it;
        }
    }

    bool textLayout(Node& n, float w, TextLayout& l, int maxLines = 0) {
        rep.textWork += n.rich.cps.size();
        if (rep.textWork > MAX_TEXT_WORK) return fail("text measurement work limit exceeded");
        LayoutOptions opt; opt.maxWidth = std::max(1.0f, w); opt.size = n.s.size;
        opt.lineHeight = n.s.leading; opt.align = n.s.textAlign; opt.maxLines = maxLines;
        layoutRichText(fonts, n.rich, opt, l);
        return true;
    }
    float resolved(const Length& l, float ref, float fallback) {
        if (l.unit == Unit::Percent && ref < 0) { warn("percentage height/basis needs a definite parent height; treated as auto"); return fallback; }
        return l.get(ref, fallback);
    }
    float bound(float value, const Length& lo, const Length& hi, float ref, float padding) {
        float a = std::max(padding, resolved(lo, ref, 0)), b = std::max(padding, resolved(hi, ref, LIMIT));
        if (a > b) { warn("minimum size exceeds maximum size; minimum wins"); return a; }
        return std::max(a, std::min(b, value));
    }
    float width(int i, float ref, float fallback) {
        Style& s = nodes[i].s;
        return bound(resolved(s.w, ref, fallback), s.minW, s.maxW, ref, s.px());
    }
    float preferred(int i) {
        Node& n = nodes[i]; if (n.preferred >= 0) return n.preferred;
        if (!step()) return 0;
        float w = 0;
        if (!n.rich.empty()) { TextLayout t; textLayout(n, LIMIT, t); w = t.width; }
        else for (int c : n.children) {
            float cw = width(c, LIMIT, preferred(c));
            if (n.s.flex && n.s.row) w += cw; else w = std::max(w, cw);
        }
        if (n.s.flex && n.s.row && !n.children.empty()) w += n.s.gap * (n.children.size() - 1);
        return n.preferred = std::min(LIMIT, w + n.s.px());
    }
    float height(int i, float w, float ref) {
        Node& n = nodes[i];
        if (n.cacheW == w && n.cacheRef == ref) return n.cacheHeight;
        if (!step() || !rep.error.empty()) return 0;
        float h;
        bool percentWithoutRef = n.s.h.unit == Unit::Percent && ref < 0;
        if (percentWithoutRef) warn("percentage height needs a definite parent height; treated as auto");
        if (n.s.h.unit != Unit::Auto && !percentWithoutRef) h = resolved(n.s.h, ref, 0);
        else if (!n.rich.empty()) { TextLayout t; textLayout(n, w - n.s.px(), t); h = t.height + n.s.py(); }
        else h = plan(i, w, -1).height;
        h = bound(h, n.s.minH, n.s.maxH, ref, n.s.py());
        n.cacheW = w; n.cacheRef = ref; n.cacheHeight = h;
        return h;
    }

    // Freeze constrained items and redistribute the remaining free space. Widths
    // are border-box, minimum defaults to padding (not browser min-content).
    void distribute(std::vector<Item>& items, size_t a, size_t b, float available, float gap, bool flex) {
        if (!flex || a == b || available < 0) return;
        for (size_t pass = a; pass <= b; ++pass) {
            if (!step()) return;
            float used = gap * (b - a - 1), total = 0;
            for (size_t j = a; j < b; ++j) used += items[j].main;
            float free = available - used;
            if (std::fabs(free) < .01f) return;
            for (size_t j = a; j < b; ++j) {
                auto& it = items[j]; Style& s = nodes[it.node].s;
                it.weight = free > 0 ? (it.main < it.max - .01f ? s.grow : 0) : (it.main > it.min + .01f ? s.shrink * it.main : 0);
                total += it.weight;
            }
            if (total <= 0) return;
            for (size_t j = a; j < b; ++j) { auto& it = items[j]; it.main = std::max(it.min, std::min(it.max, it.main + free * it.weight / total)); }
        }
    }
    Plan plan(int index, float w, float h) {
        Plan result;
        if (!step() || !rep.error.empty()) return result;
        Node& n = nodes[index]; Style& s = n.s;
        const float cw = std::max(0.0f, w - s.px()), ch = h < 0 ? -1 : std::max(0.0f, h - s.py());
        const bool row = s.flex && s.row;
        const float main = row ? cw : ch;
        std::vector<Item> items; items.reserve(n.children.size());
        for (int child : n.children) {
            Style& c = nodes[child].s;
            float naturalW = width(child, cw, row ? preferred(child) : cw);
            if (!row && (c.self == Cross::Auto ? s.align : c.self) != Cross::Stretch)
                naturalW = width(child, cw, std::min(cw, preferred(child)));
            float basis;
            if (s.flex && c.basis.unit == Unit::Percent && main < 0)
                warn("percentage height/basis needs a definite parent height; treated as auto");
            if (s.flex && c.basis.unit != Unit::Auto && main >= 0) basis = resolved(c.basis, main, 0);
            else basis = row ? naturalW : height(child, naturalW, ch);
            float pad = row ? c.px() : c.py();
            float low = std::max(pad, resolved(row ? c.minW : c.minH, main, 0));
            float high = std::max(pad, resolved(row ? c.maxW : c.maxH, main, LIMIT));
            if (low > high) { warn("minimum size exceeds maximum size; minimum wins"); high = low; }
            items.push_back({child, std::max(low, std::min(high, basis)), row ? 0 : naturalW, low, high, 0});
        }
        result.boxes.resize(items.size());
        float crossPos = 0, totalMain = 0;
        for (size_t a = 0; a < items.size();) {
            size_t b = a + 1;
            float occupied = items[a].main;
            while (b < items.size() && (!s.wrap || occupied + s.gap + items[b].main <= main + .01f)) {
                occupied += s.gap + items[b].main; ++b;
            }
            distribute(items, a, b, main, s.gap, s.flex);
            float lineCross = row && !s.wrap && ch >= 0 ? ch : (row ? 0 : cw);
            occupied = s.gap * (b - a - 1);
            for (size_t j = a; j < b; ++j) {
                auto& it = items[j];
                if (row) it.cross = height(it.node, it.main, ch);
                if (row && (s.wrap || ch < 0)) lineCross = std::max(lineCross, it.cross);
                occupied += it.main;
            }
            float extra = main < 0 ? 0 : std::max(0.0f, main - occupied), pos = 0, gap = s.gap;
            if (s.flex) {
                if (s.justify == Justify::Center) pos = extra / 2;
                else if (s.justify == Justify::End) pos = extra;
                else if (s.justify == Justify::Between && b - a > 1) gap += extra / (b - a - 1);
                else if (s.justify == Justify::Around) { gap += extra / (b - a); pos = extra / (b - a) / 2; }
                else if (s.justify == Justify::Evenly) { gap += extra / (b - a + 1); pos = extra / (b - a + 1); }
            }
            for (size_t j = a; j < b; ++j) {
                auto& it = items[j]; Style& c = nodes[it.node].s;
                Cross align = c.self == Cross::Auto ? s.align : c.self;
                bool autoCross = (row ? c.h : c.w).unit == Unit::Auto;
                if (align == Cross::Stretch && autoCross)
                    it.cross = bound(lineCross, row ? c.minH : c.minW, row ? c.maxH : c.maxW, row ? ch : cw, row ? c.py() : c.px());
                float offset = align == Cross::Center ? (lineCross - it.cross) / 2 : (align == Cross::End ? lineCross - it.cross : 0);
                offset = std::max(0.0f, offset);
                result.boxes[j] = row ? Box{pos, crossPos + offset, it.main, it.cross} : Box{offset, pos, it.cross, it.main};
                pos += it.main + gap;
            }
            totalMain = occupied;
            crossPos += lineCross + (b < items.size() ? s.gap : 0);
            a = b;
        }
        result.height = (row ? crossPos : totalMain) + s.py();
        return result;
    }
    bool arrange(int i, Box box) {
        if (!step() || !rep.error.empty()) return false;
        Node& n = nodes[i]; n.box = box;
        if (n.children.empty()) return true;
        Plan p = plan(i, box.w, box.h);
        if (!rep.error.empty()) return false;
        for (size_t j = 0; j < n.children.size(); ++j) {
            Box b = p.boxes[j]; b.x += box.x + n.s.pad[3] + n.s.border; b.y += box.y + n.s.pad[0] + n.s.border;
            if (!arrange(n.children[j], b)) return false;
        }
        return true;
    }
    Box intersect(Box a, Box b) {
        float x = std::max(a.x, b.x), y = std::max(a.y, b.y);
        return {x, y, std::max(0.0f, std::min(a.x + a.w, b.x + b.w) - x), std::max(0.0f, std::min(a.y + a.h, b.y + b.h) - y)};
    }
    void clip(DisplayList& out, Box b) {
        int x = int(std::ceil(b.x)), y = int(std::ceil(b.y));
        out.clip(x, y, std::max(0, int(std::floor(b.x + b.w)) - x), std::max(0, int(std::floor(b.y + b.h)) - y));
    }
    bool paint(int i, DisplayList& out, Box parentClip) {
        Node& n = nodes[i]; Style& s = n.s; Box b = n.box, visible = intersect(b, parentClip);
        if (visible.w + .1f < b.w || visible.h + .1f < b.h) rep.clipped = true;
        if (visible.w <= 0 || visible.h <= 0) return true;
        clip(out, visible);
        out.roundRect(b.x, b.y, b.w, b.h, s.radius, s.bg, s.borderColor, s.border);
        Box content{b.x + s.pad[3] + s.border, b.y + s.pad[0] + s.border,
                    std::max(0.0f, b.w - s.px()), std::max(0.0f, b.h - s.py())};
        Box childClip = intersect(visible, content);
        if (!n.rich.empty()) {
            TextLayout t;
            if (!textLayout(n, content.w, t)) return false;
            int lines = 0;
            while (lines < int(t.lines.size()) && t.lines[lines].top + t.lines[lines].height <= content.h + .1f) ++lines;
            if (lines < int(t.lines.size())) {
                rep.clipped = true;
                if (lines > 0 && !textLayout(n, content.w, t, lines)) return false;
            }
            if (t.width > content.w + .1f) rep.clipped = true;
            if (lines > 0 && content.w > 0) { clip(out, childClip); out.text(t, n.rich, content.x, content.y, 0, lines); }
        }
        for (int c : n.children) if (!paint(c, out, childClip)) return false;
        return true;
    }
};
}  // namespace

bool build(const char* html, size_t bytes, FontEngine& fonts, const Theme& theme, Box viewport,
           DisplayList& out, Report& report, std::vector<DebugBox>* debug) {
    report = {};
    if (!html || !bytes || bytes > MAX_BYTES) { report.error = "html: expected 1..12288 UTF-8 bytes"; return false; }
    Engine e(fonts, theme, report);
    if (!e.parse(std::string(html, bytes)) || !e.arrange(0, viewport)) return false;
    // Rendering can also reach the text work budget. Use a temporary list so
    // even that failure never publishes a partially constructed scene.
    DisplayList next;
    if (!e.paint(0, next, viewport)) return false;
    next.resetClip();
    if (debug) { debug->clear(); for (const auto& n : e.nodes) debug->push_back({n.id, n.box}); }
    out.append(next);
    return true;
}

const char* help() {
    return "html-layout-v1 (lenient): div/p/span/section/header/footer/article/main/nav/aside/blockquote/ul/ol/li/h1-h6 "
           "are boxes; b/strong/u/small/mark/em/i/code/a and spans mixed with text are inline formatting "
           "(bold, underline, color, background, font-size, font-family). br breaks a line (between boxes in a "
           "vertical flow it adds a blank line); hr is a 1px rule; li gets a bullet or ol number. "
           "display:block|flex|none; flex-direction:row|column; flex:N | N M | N M basis | none | auto; flex-grow, "
           "flex-shrink, flex-basis, flex-wrap:wrap (row only), gap, justify-content:start|center|end|"
           "space-between|space-around|space-evenly, align-items/align-self:start|center|end|stretch. "
           "width/height/min-/max-: px, %, em, rem, pt (percent heights need a definite parent height). "
           "padding (1-4 values); all sizes border-box, overflow clipped; no margins (use gap/padding). "
           "color/background/border-color: #hex, rgb()/rgba(), CSS or project color names; border shorthand, "
           "border-width, border-radius. font-size 8..64px (em/rem/%/pt ok), font-family:sans|kai|display, "
           "font-weight:bold|normal|100-900, line-height, text-align, text-decoration:underline. Color emoji in "
           "text are supported (single codepoints; ZWJ sequences split). "
           "Unsupported CSS, values and attributes are ignored and reported in warnings; img is dropped; "
           "unclosed tags auto-close. Refused: script/style/iframe/svg/forms/media. No JS, URLs or stylesheets. "
           "Limits:12288 bytes,96 nodes including viewport,12 element levels,2048 text codepoints,4096 layout "
           "steps,65536 measured codepoints. MCP request max 16384 bytes including JSON. Screens over 6144 "
           "serialized bytes are shown but not restored after reboot. Standalone show_html only (no HTML slides). "
           "Viewport is a vertical block; use height:100% on a root container to fill it.";
}
}  // namespace HtmlLayout
