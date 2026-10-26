#include "Scenes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Icons.h"
#include "HtmlLayout.h"
#include "Utf8.h"

namespace {

constexpr float W = SceneBuilder::WIDTH;
constexpr float H = SceneBuilder::HEIGHT;
constexpr float STATUS_BAR_H = 22;

struct Rect {
    float x, y, w, h;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
};

/* A slide reads its own keys first and falls back to the deck for the
 * presentation-wide ones, so a deck can set the theme once. */
struct Spec {
    JsonVariantConst v;
    JsonVariantConst parent;

    JsonVariantConst operator[](const char* key) const {
        JsonVariantConst x = v[key];
        if (!x.isNull() || parent.isNull()) return x;
        static const char* const INHERITED[] = {"theme", "background", "status_bar", "font", "accent"};
        for (const char* k : INHERITED)
            if (!strcmp(k, key)) return parent[key];
        return x;
    }
};

struct Block {
    RichText rich;
    TextLayout layout;
    FitResult fit;

    float height(int first, int last) const {
        if (first >= last) return 0;
        return layout.lines[last - 1].top + layout.lines[last - 1].height - layout.lines[first].top;
    }
};

std::string formatNumber(JsonVariantConst v) {
    if (v.is<const char*>()) return v.as<const char*>();
    if (v.isNull()) return "";
    if (v.is<bool>()) return v.as<bool>() ? "true" : "false";
    double d = v.as<double>();
    char buf[32];
    if (std::fabs(d - std::round(d)) < 1e-9 && std::fabs(d) < 1e15) {
        snprintf(buf, sizeof(buf), "%.0f", d);
    } else {
        snprintf(buf, sizeof(buf), "%.2f", d);
        char* end = buf + strlen(buf) - 1;
        while (*end == '0') *end-- = '\0';
        if (*end == '.') *end = '\0';
    }
    return buf;
}

void appendText(RichText& rt, const char* s, const TextStyle& st) {
    if (rt.styles.empty()) rt.styles.push_back(st);
    uint16_t idx = uint16_t(rt.styles.size());
    rt.styles.push_back(st);
    while (s && *s) {
        uint32_t cp = utf8Next(s);
        if (cp == '\r') continue;
        rt.cps.push_back(cp);
        rt.style.push_back(idx);
    }
}

class Ctx {
public:
    Ctx(FontEngine& fonts, const SystemInfo& sys, DisplayList& out, SceneReport& rep, Spec spec)
        : fonts(fonts), sys(sys), out(out), rep(rep), spec(spec) {}

    FontEngine& fonts;
    const SystemInfo& sys;
    DisplayList& out;
    SceneReport& rep;
    Spec spec;
    const Theme* theme = &defaultTheme();
    RGBA accent;

    void warn(const std::string& w) { rep.warnings.push_back(w); }

    RGBA color(JsonVariantConst v, RGBA fallback, const char* what) {
        const char* s = v.as<const char*>();
        if (!s || !*s) return fallback;
        RGBA c;
        if (parseColor(s, *theme, c)) return c;
        warn(std::string("unknown color '") + s + "' for " + what);
        return fallback;
    }

    uint32_t icon(JsonVariantConst v, const char* what) {
        const char* s = v.as<const char*>();
        if (!s || !*s) return 0;
        uint32_t cp = iconCodepoint(s);
        if (!cp) warn(std::string("unknown icon '") + s + "' for " + what + " (see get_display_info)");
        return cp;
    }

    TextStyle baseStyle(JsonVariantConst font, JsonVariantConst bold, RGBA color) {
        TextStyle st;
        const char* f = font.as<const char*>();
        if (f && !parseFamily(f, st.family)) warn(std::string("unknown font '") + f + "', using sans");
        st.bold = bold | false;
        st.color = color;
        return st;
    }

    bool markup(JsonVariantConst v) { return v | true; }

    // Lays out `text` to fit `opt`. Returns false when there is nothing to show.
    bool fit(Block& b, const char* text, const TextStyle& st, bool markup, const FitOptions& opt) {
        parseRichText(text ? text : "", st, *theme, markup, b.rich);
        if (b.rich.empty()) return false;
        fitText(fonts, b.rich, opt, b.layout, b.fit);
        return true;
    }

    // Draws one page of a fitted block inside `box`; returns the drawn height.
    float draw(const Block& b, Rect box, VAlign va, int page = 0) {
        int first, last;
        pageLines(b.layout, b.fit, page, first, last);
        if (first >= last) return 0;
        float h = b.height(first, last);
        // Centre and bottom-align by the ink box: the half-leadings outside
        // the first and last lines may hang over the box edges.
        float above = b.layout.lines[first].leading / 2, below = b.layout.lines[last - 1].leading / 2;
        float ink = h - above - below;
        float y = box.y;
        if (va == VAlign::Middle) y = box.y - above + (box.h - ink) / 2;
        if (va == VAlign::Bottom) y = box.y - above + box.h - ink;
        out.text(b.layout, b.rich, box.x, y, first, last);
        return h;
    }

    // Single line of plain text, shrunk then ellipsized to fit.
    float label(const char* text, TextStyle st, Rect box, float size, Align align, VAlign va = VAlign::Middle,
                float minSize = 0, bool markup = false) {
        Block b;
        FitOptions o;
        o.width = box.w;
        o.height = box.h;
        o.minSize = minSize > 0 ? minSize : size;
        o.maxSize = size;
        o.size = minSize > 0 ? 0 : size;
        o.maxLines = 1;
        o.lineHeight = 1.2f;
        o.align = align;
        if (!fit(b, text, st, markup, o)) return 0;
        draw(b, box, va);
        return b.layout.width;
    }

    float measure(const char* text, const TextStyle& st, float size) {
        RichText rt;
        parseRichText(text, st, *theme, false, rt);
        TextLayout l;
        LayoutOptions o;
        o.size = size;
        o.maxLines = 1;
        layoutRichText(fonts, rt, o, l);
        return l.width;
    }

    void iconAt(uint32_t cp, float x, float y, float size, RGBA c) {
        out.icon(cp, x, y, std::max(4.0f, std::min(size, 200.0f)), c, fonts);
    }

    // Theme, background and optional status bar. Returns the free area.
    Rect begin(const char* forcedTheme = nullptr) {
        const char* t = forcedTheme ? forcedTheme : spec["theme"].as<const char*>();
        if (t && *t) {
            if (const Theme* found = findTheme(t))
                theme = found;
            else
                warn(std::string("unknown theme '") + t + "', using " + theme->name);
        }
        accent = color(spec["accent"], theme->accent, "accent");
        Background bg;
        const char* bgs = spec["background"].as<const char*>();
        if (!parseBackground(bgs, *theme, bg)) {
            warn(std::string("unknown background '") + bgs + "'");
            parseBackground(nullptr, *theme, bg);
        }
        out.gradient(0, 0, int(W), int(H), bg.top, bg.bottom);
        Rect area{0, 0, W, H};
        if (spec["status_bar"] | false) {
            statusBar();
            area.y = STATUS_BAR_H;
            area.h -= STATUS_BAR_H;
        }
        return area;
    }

    void statusBar() {
        rep.usesClock = true;
        TextStyle st;
        st.bold = true;
        st.color = theme->muted;
        char clock[32];
        if (sys.timeValid)
            snprintf(clock, sizeof(clock), "%02d:%02d", sys.hour, sys.minute);
        else
            snprintf(clock, sizeof(clock), "%s", sys.deviceName.c_str());
        label(clock, st, {12, 3, 200, 17}, 13, Align::Left);
        bool online = sys.wifi == SystemInfo::Wifi::Connected;
        iconAt(iconCodepoint(online ? "wifi" : "wifi_off"), W - 12 - 15, 4, 15, theme->muted);
        out.rect(0, int(STATUS_BAR_H) - 1, int(W), 1, theme->border.withAlpha(theme->border.a / 2));
    }

    // Title row with optional leading icon and a hairline underneath.
    void header(Rect& area, const char* title, uint32_t icon, RGBA iconColor) {
        float x = 16, y = area.y + 10;
        if (icon) {
            iconAt(icon, x, y + 2, 24, iconColor);
            x += 32;
        }
        TextStyle st;
        const char* f = spec["font"];
        if (f) parseFamily(f, st.family);
        st.bold = st.family == FAMILY_SANS;
        st.color = theme->text;
        label(title, st, {x, y, W - 16 - x, 28}, 21, Align::Left, VAlign::Middle, 15, true);
        out.rect(16, int(y + 36), int(W - 32), 1, theme->border);
        float consumed = y + 37 - area.y;
        area.y += consumed;
        area.h -= consumed;
    }

    void pageDots(int pages, int page, float cy) {
        if (pages <= 1) return;
        if (pages > 10) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d / %d", page + 1, pages);
            TextStyle st;
            st.bold = true;
            st.color = theme->muted;
            label(buf, st, {0, cy - 8, W, 16}, 12, Align::Center);
            return;
        }
        float total = (pages - 1) * 10 + 14;
        float x = (W - total) / 2;
        for (int i = 0; i < pages; ++i) {
            if (i == page) {
                out.roundRect(x, cy - 3, 14, 6, 3, accent);
                x += 20;
            } else {
                out.circle(x + 3, cy, 3, theme->muted.withAlpha(110));
                x += 10;
            }
        }
    }

    Align parseAlign(JsonVariantConst v, Align fallback, bool* isAuto = nullptr) {
        const char* s = v.as<const char*>();
        if (isAuto) *isAuto = !s || !strcmp(s, "auto");
        if (!s || !strcmp(s, "auto")) return fallback;
        if (!strcmp(s, "left")) return Align::Left;
        if (!strcmp(s, "center")) return Align::Center;
        if (!strcmp(s, "right")) return Align::Right;
        warn(std::string("unknown align '") + s + "'");
        return fallback;
    }

    VAlign parseVAlign(JsonVariantConst v, VAlign fallback) {
        const char* s = v.as<const char*>();
        if (!s) return fallback;
        if (!strcmp(s, "top")) return VAlign::Top;
        if (!strcmp(s, "middle") || !strcmp(s, "center")) return VAlign::Middle;
        if (!strcmp(s, "bottom")) return VAlign::Bottom;
        warn(std::string("unknown valign '") + s + "'");
        return fallback;
    }

    void noteTruncated(const Block& b, const char* what) {
        if (b.fit.truncated) warn(std::string(what) + " did not fit and was cut with an ellipsis");
    }
};

/* Fits body text: largest size first, centring short text. With `paginate`
 * the block may span several pages; `reserveForDots` shrinks the box when it
 * does so the page indicator has room. */
void fitBody(Ctx& c, Block& b, const char* text, const TextStyle& st, bool markup, FitOptions o, bool autoAlign,
             Rect& box) {
    o.align = autoAlign ? Align::Left : o.align;
    if (!c.fit(b, text, st, markup, o)) return;
    if (b.fit.pages() > 1) {
        box.h -= 14;
        o.height = box.h;
        c.fit(b, text, st, markup, o);
    }
    if (autoAlign && b.fit.pages() == 1 && b.layout.lines.size() <= 3) {
        o.align = Align::Center;
        o.size = b.fit.size;
        c.fit(b, text, st, markup, o);
    }
}

// ---------------------------------------------------------------- templates

void buildText(Ctx& c, int page) {
    const Spec& s = c.spec;
    const char* text = s["text"];
    if (!text || !*text) {
        c.rep.error = "text is required";
        return;
    }
    Rect area = c.begin();
    const char* title = s["title"];
    uint32_t icon = c.icon(s["icon"], "icon");

    if (title && *title) c.header(area, title, icon, c.accent);
    Rect body{18, area.y + (title && *title ? 10 : 16), W - 36, 0};
    float bottom = H - 14;

    const char* footer = s["footer"];
    if (footer && *footer) {
        TextStyle fs;
        fs.color = c.theme->muted;
        c.label(footer, fs, {18, H - 30, W - 36, 18}, 13, Align::Center, VAlign::Middle, 11, true);
        bottom = H - 36;
    }
    if (icon && !(title && *title)) {
        c.iconAt(icon, (W - 44) / 2, body.y, 44, c.accent);
        body.y += 54;
    }
    body.h = bottom - body.y;

    TextStyle st = c.baseStyle(s["font"], s["bold"], c.color(s["color"], c.theme->text, "color"));
    FitOptions o;
    o.width = body.w;
    o.height = body.h;
    o.size = s["size"] | 0.0f;
    o.minSize = s["min_size"] | 14.0f;
    o.maxSize = s["max_size"] | ((title && *title) || icon ? 56.0f : 80.0f);
    o.lineHeight = s["line_height"] | (st.family == FAMILY_KAI ? 1.45f : 1.35f);
    o.paginate = true;
    bool autoAlign;
    o.align = c.parseAlign(s["align"], Align::Left, &autoAlign);

    Block b;
    fitBody(c, b, s["text"], st, c.markup(s["markup"]), o, autoAlign, body);
    if (b.rich.empty()) return;
    int pages = b.fit.pages();
    page = std::min(page, pages - 1);
    c.draw(b, body, c.parseVAlign(s["valign"], VAlign::Middle), page);
    c.pageDots(pages, page, H - 9);
    c.rep.pages = pages;
    c.rep.textSize = b.fit.size;
    c.noteTruncated(b, "text");
}

void buildNotice(Ctx& c, int page) {
    const Spec& s = c.spec;
    Rect area = c.begin();
    struct Level {
        const char* name;
        const char* icon;
        const char* color;
    } levels[] = {{"info", "info", "accent"},          {"success", "check_circle", "success"},
                  {"warning", "warning", "warning"},   {"error", "error", "danger"},
                  {"tip", "lightbulb", "amber"},       {"announcement", "campaign", "accent"}};
    const Level* level = &levels[0];
    const char* ln = s["level"];
    if (ln) {
        bool found = false;
        for (const auto& l : levels) {
            if (!strcmp(l.name, ln)) {
                level = &l;
                found = true;
            }
        }
        if (!found) c.warn(std::string("unknown level '") + ln + "'");
    }
    RGBA levelColor;
    parseColor(level->color, *c.theme, levelColor);
    RGBA color = c.color(s["color"], levelColor, "color");
    uint32_t icon = s["icon"].isNull() ? iconCodepoint(level->icon) : c.icon(s["icon"], "icon");
    if (!icon) icon = iconCodepoint(level->icon);

    const char* title = s["title"];
    bool hasTitle = title && *title;
    const char* message = s["message"] | s["text"].as<const char*>();
    if (!hasTitle && !(message && *message)) {
        c.rep.error = "title or message is required";
        return;
    }
    bool markup = c.markup(s["markup"]);
    TextStyle ms = c.baseStyle(s["font"], s["bold"], mix(c.theme->text, c.theme->bgBottom, 0.18f));
    FitOptions mo;
    mo.minSize = s["min_size"] | 14.0f;
    mo.maxSize = s["max_size"] | 21.0f;
    mo.size = s["size"] | 0.0f;
    mo.lineHeight = s["line_height"] | 1.4f;
    mo.paginate = true;
    bool autoAlign;
    mo.align = c.parseAlign(s["align"], Align::Center, &autoAlign);

    c.out.rect(0, int(area.y), int(W), 4, color);

    // Hero layout: big badge, centred title, message underneath.
    Block title_, msg;
    Rect body{18, 0, W - 36, 0};
    float badgeY = area.y + 16;
    float titleY = badgeY + 64 + 12;
    float msgY = titleY;
    if (hasTitle) {
        TextStyle ts;
        ts.bold = true;
        ts.color = c.theme->text;
        FitOptions to;
        to.width = W - 32;
        to.height = 64;
        to.minSize = 16;
        to.maxSize = 24;
        to.maxLines = 2;
        to.lineHeight = 1.25f;
        to.align = Align::Center;
        c.fit(title_, title, ts, markup, to);
        msgY = titleY + title_.layout.height + 6;
    }
    body.y = msgY;
    body.h = H - 12 - msgY;
    mo.width = body.w;
    mo.height = body.h;
    bool hero = true;
    if (message && *message) {
        fitBody(c, msg, message, ms, markup, mo, autoAlign, body);
        hero = msg.fit.pages() == 1 && msg.fit.size >= 15;
    }

    if (hero) {
        float cx = W / 2, cy = badgeY + 32;
        c.out.circle(cx, cy, 32, color.withAlpha(46), color.withAlpha(110), 1.5f);
        c.iconAt(icon, cx - 19, cy - 19, 38, color);
        if (hasTitle) c.draw(title_, {16, titleY, W - 32, title_.layout.height}, VAlign::Top);
    } else {
        // Compact layout for long messages: header row, then the message.
        if (hasTitle) {
            c.header(area, title, icon, color);
        } else {
            c.iconAt(icon, 18, area.y + 14, 28, color);
            area.y += 46;
            area.h -= 46;
        }
        body = {18, area.y + 8, W - 36, H - 12 - (area.y + 8)};
        mo.width = body.w;
        mo.height = body.h;
        mo.maxSize = std::min(mo.maxSize, 20.0f);
        if (autoAlign) mo.align = Align::Left;
        fitBody(c, msg, message, ms, markup, mo, false, body);
    }
    if (!msg.rich.empty()) {
        int pages = msg.fit.pages();
        page = std::min(page, pages - 1);
        c.draw(msg, body, hero ? VAlign::Top : VAlign::Top, page);
        c.pageDots(pages, page, H - 9);
        c.rep.pages = pages;
        c.rep.textSize = msg.fit.size;
        c.noteTruncated(msg, "message");
    }
}

void buildList(Ctx& c, int page) {
    const Spec& s = c.spec;
    JsonArrayConst items = s["items"];
    if (items.isNull() || items.size() == 0) {
        c.rep.error = "items must be a non-empty array";
        return;
    }
    Rect area = c.begin();
    const char* title = s["title"];
    bool hasTitle = title && *title;
    if (hasTitle) c.header(area, title, c.icon(s["icon"], "icon"), c.accent);

    const char* style = s["style"];
    bool anyDone = false;
    for (JsonVariantConst it : items) anyDone = anyDone || !it["done"].isNull();
    if (!style) style = anyDone ? "check" : "bullet";
    if (strcmp(style, "bullet") && strcmp(style, "number") && strcmp(style, "check") && strcmp(style, "icon") &&
        strcmp(style, "none")) {
        c.warn(std::string("unknown list style '") + style + "', using bullet");
        style = "bullet";
    }
    bool markup = c.markup(s["markup"]);
    TextStyle base = c.baseStyle(s["font"], s["bold"], c.color(s["color"], c.theme->text, "color"));
    float lineHeight = s["line_height"] | 1.3f;

    const float left = 16, right = W - 16;
    const float top = area.y + (hasTitle ? 8 : 14);
    float avail = H - 12 - top;

    struct Row {
        Block text;
        std::string value;
        float valueW = 0;
        float h = 0;
        RGBA color;
        bool done = false;
        uint32_t icon = 0;
    };
    std::vector<Row> rows(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        JsonVariantConst it = items[i];
        Row& r = rows[i];
        r.color = it.is<JsonObjectConst>() ? c.color(it["color"], base.color, "item color") : base.color;
        r.done = it["done"] | false;
        r.value = formatNumber(it["value"]);
        if (!strcmp(style, "icon")) {
            r.icon = c.icon(it["icon"], "item icon");
            if (!r.icon) r.icon = c.icon(s["icon"], "icon");
        }
    }

    auto layoutAt = [&](float size) {
        float markerW = !strcmp(style, "none") ? 0 : size * 1.55f;
        float total = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            JsonVariantConst it = items[i];
            Row& r = rows[i];
            std::string text = it.is<JsonObjectConst>() ? formatNumber(it["text"]) : formatNumber(it);
            if (text.empty()) text = " ";  // keep the row so numbering stays right
            TextStyle st = base;
            st.color = r.done ? c.theme->muted : r.color;
            TextStyle vs;
            vs.color = c.theme->muted;
            r.valueW = r.value.empty() ? 0 : c.measure(r.value.c_str(), vs, size * 0.9f) + 10;
            FitOptions o;
            o.width = right - left - markerW - r.valueW;
            o.height = 1e6f;
            o.size = size;
            o.maxLines = 2;
            o.lineHeight = lineHeight;
            c.fit(r.text, text.c_str(), st, markup, o);
            r.h = std::max(r.text.layout.height, size * lineHeight) + size * 0.5f;
            total += r.h;
        }
        return total - size * 0.5f;  // no gap needed after the last row
    };

    float maxSize = s["max_size"] | 26.0f, minSize = s["min_size"] | 14.0f;
    float size = s["size"] | 0.0f;
    if (size <= 0) {
        for (size = maxSize; size > minSize; size -= 1)
            if (layoutAt(size) <= avail) break;
        // Too long for one page anyway: paginate at a comfortable size rather
        // than cramming small rows onto every page.
        if (layoutAt(size) > avail) size = std::max(minSize, std::min(maxSize, 19.0f));
    }
    float total = layoutAt(size);

    // Paginate by whole rows.
    std::vector<size_t> pageStart{0};
    if (total > avail) {
        avail -= 14;
        float acc = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (acc + rows[i].h - size * 0.5f > avail && i > pageStart.back()) {
                pageStart.push_back(i);
                acc = 0;
            }
            acc += rows[i].h;
        }
    }
    int pages = int(pageStart.size());
    page = std::min(page, pages - 1);
    size_t from = pageStart[page], to = page + 1 < pages ? pageStart[page + 1] : rows.size();

    float pageH = 0;
    for (size_t i = from; i < to; ++i) pageH += rows[i].h;
    float y = top;
    if (!hasTitle && pages == 1) y += (avail - pageH) / 2;

    float markerW = !strcmp(style, "none") ? 0 : size * 1.55f;
    for (size_t i = from; i < to; ++i) {
        Row& r = rows[i];
        const LayoutLine& first = r.text.layout.lines[0];
        float rowTop = y + size * 0.25f;
        float midY = rowTop + first.top + first.height / 2;
        float mx = left + size * 0.5f;
        if (!strcmp(style, "bullet")) {
            c.out.circle(mx, midY, std::max(2.5f, size * 0.16f), c.accent);
        } else if (!strcmp(style, "number")) {
            float rad = size * 0.62f;
            c.out.circle(mx + 1, midY, rad, c.accent.withAlpha(52));
            TextStyle ns;
            ns.bold = true;
            ns.color = c.accent;
            char num[8];
            snprintf(num, sizeof(num), "%d", int(i + 1));
            c.label(num, ns, {mx + 1 - rad, midY - rad, 2 * rad, 2 * rad}, size * 0.72f, Align::Center);
        } else if (!strcmp(style, "check")) {
            if (r.done) {
                RGBA ok;
                parseColor("success", *c.theme, ok);
                c.iconAt(iconCodepoint("check_circle"), mx - size * 0.6f, midY - size * 0.6f, size * 1.2f, ok);
            } else {
                c.out.circle(mx, midY, size * 0.46f, RGBA(), c.theme->muted, 1.6f);
            }
        } else if (!strcmp(style, "icon") && r.icon) {
            c.iconAt(r.icon, mx - size * 0.55f, midY - size * 0.55f, size * 1.1f, r.color == base.color ? c.accent : r.color);
        }
        c.draw(r.text, {left + markerW, rowTop, right - left - markerW - r.valueW, r.text.layout.height}, VAlign::Top);
        if (!r.value.empty()) {
            TextStyle vs;
            vs.color = c.theme->muted;
            vs.bold = true;
            c.label(r.value.c_str(), vs, {right - r.valueW, rowTop + first.top, r.valueW, first.height}, size * 0.9f,
                    Align::Right);
        }
        if (r.text.layout.truncated) c.warn("item " + std::to_string(i + 1) + " was cut to two lines");
        y += r.h;
        if (i + 1 < to) c.out.rect(int(left + markerW), int(y), int(right - left - markerW), 1,
                                   c.theme->border.withAlpha(std::max(1, c.theme->border.a * 2 / 3)));
    }
    c.pageDots(pages, page, H - 9);
    c.rep.pages = pages;
    c.rep.textSize = size;
}

void buildMetrics(Ctx& c) {
    const Spec& s = c.spec;
    JsonArrayConst metrics = s["metrics"];
    if (metrics.isNull() || metrics.size() == 0) {
        c.rep.error = "metrics must be a non-empty array";
        return;
    }
    Rect area = c.begin();
    const char* title = s["title"];
    bool hasTitle = title && *title;
    if (hasTitle) c.header(area, title, c.icon(s["icon"], "icon"), c.accent);

    int n = int(metrics.size());
    if (n > 6) {
        c.warn("only the first 6 metrics are shown");
        n = 6;
    }
    int cols = n <= 3 ? n : (n == 4 ? 2 : 3);
    int rowsN = (n + cols - 1) / cols;
    const float gap = 8;
    Rect grid{12, area.y + (hasTitle ? 6 : 12), W - 24, 0};
    grid.h = H - 12 - grid.y;
    float tw = (grid.w - (cols - 1) * gap) / cols;
    float th = (grid.h - (rowsN - 1) * gap) / rowsN;
    TextStyle valueBase = c.baseStyle(s["font"], s["bold"].isNull() ? JsonVariantConst() : s["bold"], c.theme->text);
    if (s["bold"].isNull()) valueBase.bold = true;

    for (int i = 0; i < n; ++i) {
        JsonVariantConst m = metrics[i];
        int row = i / cols, col = i % cols;
        int inRow = std::min(cols, n - row * cols);
        float x = grid.x + col * (tw + gap) + (cols - inRow) * (tw + gap) / 2;
        float y = grid.y + row * (th + gap);
        bool big = n == 1;
        float pad = big ? 16 : 10;

        if (c.theme->light) c.out.shadow(x, y + 2, tw, th, 14, 6, RGBA(15, 23, 42, 28));
        c.out.roundRect(x, y, tw, th, 14, c.theme->surface, c.theme->border, 1);

        RGBA color = c.color(m["color"], c.accent, "metric color");
        float ls = std::max(12.0f, std::min(big ? 18.0f : 15.0f, th * 0.13f));
        float lx = x + pad;
        uint32_t icon = c.icon(m["icon"], "metric icon");
        if (icon) {
            c.iconAt(icon, lx, y + pad - 1, ls + 3, color);
            lx += ls + 8;
        }
        TextStyle lst;
        lst.color = c.theme->muted;
        const char* label = m["label"];
        if (label) c.label(label, lst, {lx, y + pad - 2, x + tw - pad - lx, ls * 1.3f}, ls, big ? Align::Left : Align::Left,
                           VAlign::Middle, 11, true);
        float valueTop = y + pad + ls * 1.3f + 2;
        float valueBottom = y + th - pad;

        // Footer: note/trend and/or progress bar.
        JsonVariantConst progress = m["progress"];
        if (!progress.isNull()) {
            float p = std::max(0.0f, std::min(100.0f, progress.as<float>())) / 100;
            float bh = big ? 8 : 6;
            float by = valueBottom - bh;
            c.out.roundRect(x + pad, by, tw - 2 * pad, bh, bh / 2, c.theme->border);
            if (p > 0) c.out.roundRect(x + pad, by, std::max(bh, (tw - 2 * pad) * p), bh, bh / 2, color);
            valueBottom = by - 6;
        }
        const char* note = m["note"];
        const char* trend = m["trend"];
        if ((note && *note) || (trend && *trend)) {
            RGBA tc = c.theme->muted;
            uint32_t ticon = 0;
            if (trend && !strcmp(trend, "up")) {
                parseColor("success", *c.theme, tc);
                ticon = iconCodepoint("trending_up");
            } else if (trend && !strcmp(trend, "down")) {
                parseColor("danger", *c.theme, tc);
                ticon = iconCodepoint("trending_down");
            } else if (trend && !strcmp(trend, "flat")) {
                ticon = iconCodepoint("trending_flat");
            } else if (trend && *trend) {
                c.warn(std::string("unknown trend '") + trend + "'");
            }
            float ny = valueBottom - ls * 1.3f;
            float nx = x + pad;
            if (ticon) {
                c.iconAt(ticon, nx, ny + ls * 0.15f - 1, ls + 2, tc);
                nx += ls + 6;
            }
            TextStyle ns;
            ns.color = tc;
            ns.bold = true;
            if (note) c.label(note, ns, {nx, ny, x + tw - pad - nx, ls * 1.3f}, ls, Align::Left, VAlign::Middle, 11, true);
            valueBottom = ny - 2;
        }

        // Value + unit, fitted on one line.
        std::string value = formatNumber(m["value"]);
        TextStyle vs = valueBase;
        vs.color = c.color(m["value_color"], c.theme->text, "value_color");
        RichText rt;
        parseRichText(value.c_str(), vs, *c.theme, false, rt);
        const char* unit = m["unit"];
        if (unit && *unit) {
            TextStyle us = vs;
            us.bold = false;
            us.family = FAMILY_SANS;
            us.scale = 0.42f;
            us.color = c.theme->muted;
            appendText(rt, " ", us);
            appendText(rt, unit, us);
        }
        Block b;
        b.rich = rt;
        FitOptions o;
        o.width = tw - 2 * pad;
        o.height = std::max(16.0f, valueBottom - valueTop);
        o.minSize = 14;
        o.maxSize = std::min(big ? 96.0f : 64.0f, o.height * 0.95f);
        o.maxLines = 1;
        o.lineHeight = 1.05f;
        o.align = big ? Align::Center : Align::Left;
        fitText(c.fonts, b.rich, o, b.layout, b.fit);
        c.draw(b, {x + pad, valueTop, o.width, o.height}, VAlign::Middle);
        if (b.fit.truncated) c.warn("metric " + std::to_string(i + 1) + " value was cut");
    }
}

void buildLayout(Ctx& c) {
    const Spec& s = c.spec;
    c.begin();
    JsonArrayConst elements = s["elements"];
    if (elements.isNull()) {
        c.rep.error = "elements must be an array";
        return;
    }
    int idx = 0;
    for (JsonVariantConst e : elements) {
        ++idx;
        std::string where = "element " + std::to_string(idx);
        const char* type = e["type"] | "";
        float x = e["x"] | 0.0f, y = e["y"] | 0.0f, w = e["w"] | 0.0f, h = e["h"] | 0.0f;
        if (!strcmp(type, "rect")) {
            const char* fill = e["fill"];
            RGBA border = c.color(e["border"], RGBA(), where.c_str());
            float radius = e["radius"] | 0.0f;
            if (e["shadow"] | false) c.out.shadow(x, y + 3, w, h, radius, 8, RGBA(0, 0, 0, c.theme->light ? 40 : 90));
            if (fill && strchr(fill, ',') && radius <= 0) {
                Background bg;
                if (parseBackground(fill, *c.theme, bg))
                    c.out.gradient(int(x), int(y), int(w), int(h), bg.top, bg.bottom);
                else
                    c.warn("bad gradient for " + where);
                c.out.roundRect(x, y, w, h, 0, RGBA(), border, e["border_width"] | 1.0f);
            } else {
                RGBA f = c.color(e["fill"], fill ? RGBA() : c.theme->surface, where.c_str());
                c.out.roundRect(x, y, w, h, radius, f, border, border.visible() ? (e["border_width"] | 1.0f) : 0);
            }
        } else if (!strcmp(type, "circle")) {
            float r = e["r"] | 10.0f;
            RGBA f = c.color(e["fill"], c.accent, where.c_str());
            RGBA border = c.color(e["border"], RGBA(), where.c_str());
            c.out.circle(e["cx"] | x, e["cy"] | y, r, f, border, border.visible() ? (e["border_width"] | 1.0f) : 0);
        } else if (!strcmp(type, "line")) {
            c.out.line(e["x1"] | x, e["y1"] | y, e["x2"] | (x + w), e["y2"] | (y + h), e["width"] | 1.0f,
                       c.color(e["color"], c.theme->border, where.c_str()));
        } else if (!strcmp(type, "icon")) {
            float size = e["size"] | 24.0f;
            uint32_t cp = c.icon(e["name"], where.c_str());
            // With a box, centre the icon in it; otherwise (x, y) is its top-left.
            float ix = w > 0 ? x + (w - size) / 2 : x, iy = h > 0 ? y + (h - size) / 2 : y;
            c.iconAt(cp, ix, iy, size, c.color(e["color"], c.accent, where.c_str()));
        } else if (!strcmp(type, "progress")) {
            if (h <= 0) h = 8;
            float p = std::max(0.0f, std::min(100.0f, e["value"] | 0.0f)) / 100;
            float r = e["radius"] | (h / 2);
            c.out.roundRect(x, y, w, h, r, c.color(e["track"], c.theme->border, where.c_str()));
            if (p > 0) c.out.roundRect(x, y, std::max(2 * r, w * p), h, r, c.color(e["color"], c.accent, where.c_str()));
        } else if (!strcmp(type, "ring")) {
            float r = e["r"] | 30.0f, t = e["thickness"] | 8.0f;
            float p = std::max(0.0f, std::min(100.0f, e["value"] | 0.0f));
            float cx = e["cx"] | x, cy = e["cy"] | y;
            c.out.arc(cx, cy, r, t, 0, 360, c.color(e["track"], c.theme->border, where.c_str()));
            if (p > 0) c.out.arc(cx, cy, r, t, 0, std::max(1.0f, p * 3.6f), c.color(e["color"], c.accent, where.c_str()));
        } else if (!strcmp(type, "arc")) {
            c.out.arc(e["cx"] | x, e["cy"] | y, e["r"] | 30.0f, e["thickness"] | 6.0f, e["start"] | 0.0f,
                      e["end"] | 90.0f, c.color(e["color"], c.accent, where.c_str()));
        } else if (!strcmp(type, "text")) {
            if (w <= 0) w = W - x;
            if (h <= 0) h = H - y;
            float pad = e["padding"] | 0.0f;
            RGBA bg = c.color(e["bg"], RGBA(), where.c_str());
            if (bg.visible()) c.out.roundRect(x, y, w, h, e["radius"] | 0.0f, bg);
            TextStyle st = c.baseStyle(e["font"], e["bold"], c.color(e["color"], c.theme->text, where.c_str()));
            FitOptions o;
            o.width = w - 2 * pad;
            o.height = h - 2 * pad;
            o.size = e["size"] | 0.0f;
            o.minSize = e["min_size"] | 10.0f;
            o.maxSize = e["max_size"] | 96.0f;
            o.lineHeight = e["line_height"] | 1.3f;
            o.maxLines = e["max_lines"] | 0;
            o.align = c.parseAlign(e["align"], Align::Left);
            Block b;
            if (c.fit(b, e["text"], st, c.markup(e["markup"]), o)) {
                c.draw(b, {x + pad, y + pad, o.width, o.height}, c.parseVAlign(e["valign"], VAlign::Top));
                c.noteTruncated(b, where.c_str());
                if (c.rep.textSize == 0) c.rep.textSize = b.fit.size;
            }
        } else {
            c.warn(where + ": unknown type '" + type + "'");
        }
    }
}

// ------------------------------------------------------------ system screens

const char* const WEEKDAYS[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};

void badge(Ctx& c, float cy, const char* iconName, RGBA color) {
    c.out.circle(W / 2, cy, 34, color.withAlpha(40), color.withAlpha(120), 1.5f);
    c.iconAt(iconCodepoint(iconName), W / 2 - 20, cy - 20, 40, color);
}

void buildHome(Ctx& c) {
    c.begin("midnight");
    const SystemInfo& sys = c.sys;
    c.rep.usesClock = true;
    TextStyle small;
    small.color = c.theme->muted;
    c.label(sys.deviceName.c_str(), small, {16, 12, 220, 20}, 14, Align::Left);
    bool online = sys.wifi == SystemInfo::Wifi::Connected;
    c.iconAt(iconCodepoint(online ? "wifi" : "wifi_off"), W - 16 - 18, 13, 18, online ? c.theme->muted : c.theme->muted.withAlpha(140));

    if (sys.timeValid) {
        char clock[8], date[64];
        snprintf(clock, sizeof(clock), "%02d:%02d", sys.hour, sys.minute);
        snprintf(date, sizeof(date), "%d月%d日  %s", sys.month, sys.day, WEEKDAYS[sys.weekday % 7]);
        TextStyle cs;
        cs.family = FAMILY_DISPLAY;
        cs.color = c.theme->text;
        c.label(clock, cs, {0, 40, W, 100}, 96, Align::Center);
        c.label(date, small, {0, 138, W, 24}, 17, Align::Center);
    } else {
        TextStyle ts;
        ts.bold = true;
        ts.color = c.theme->text;
        c.label("MCP Display", ts, {0, 62, W, 44}, 34, Align::Center);
        c.label("等待网络时间同步", small, {0, 112, W, 24}, 16, Align::Center);
    }

    Rect card{12, 172, W - 24, 56};
    c.out.roundRect(card.x, card.y, card.w, card.h, 14, c.theme->surface, c.theme->border, 1);
    const char* icon = "bluetooth";
    std::string line1, line2;
    RGBA iconColor = c.accent;
    switch (sys.wifi) {
        case SystemInfo::Wifi::Connected:
            icon = sys.mcpUrl.empty() ? "sync" : "link";
            line1 = sys.mcpUrl.empty() ? "正在启动 MCP 服务…" : "MCP 服务已就绪";
            line2 = sys.mcpUrl.empty() ? sys.ssid : sys.mcpUrl;
            parseColor(sys.mcpUrl.empty() ? "accent" : "success", *c.theme, iconColor);
            break;
        case SystemInfo::Wifi::Connecting:
            icon = "wifi";
            line1 = "正在连接 Wi-Fi…";
            line2 = sys.ssid;
            break;
        default:
            line1 = "等待蓝牙配网";
            line2 = sys.bleName;
            break;
    }
    c.out.circle(card.x + 28, card.y + card.h / 2, 17, iconColor.withAlpha(40));
    c.iconAt(iconCodepoint(icon), card.x + 28 - 11, card.y + card.h / 2 - 11, 22, iconColor);
    TextStyle l1;
    l1.bold = true;
    l1.color = c.theme->text;
    c.label(line1.c_str(), l1, {card.x + 54, card.y + 8, card.w - 66, 22}, 15, Align::Left);
    c.label(line2.c_str(), small, {card.x + 54, card.y + 29, card.w - 66, 20}, 13, Align::Left, VAlign::Middle, 10);
}

void buildSetup(Ctx& c) {
    c.begin("midnight");
    const SystemInfo& sys = c.sys;
    bool connecting = sys.wifi == SystemInfo::Wifi::Connecting;
    badge(c, 58, connecting ? "wifi" : "bluetooth", c.accent);

    TextStyle ts;
    ts.bold = true;
    ts.color = c.theme->text;
    c.label(connecting ? "正在连接 Wi-Fi" : "蓝牙配网", ts, {0, 100, W, 34}, 24, Align::Center);
    TextStyle ms;
    ms.color = c.theme->muted;

    if (connecting) {
        TextStyle ss = ms;
        ss.color = c.theme->text;
        c.label(sys.ssid.c_str(), ss, {16, 138, W - 32, 26}, 17, Align::Center, VAlign::Middle, 12);
        c.label(("也可通过蓝牙 " + sys.bleName + " 重新配网").c_str(), ms, {16, 196, W - 32, 22}, 13, Align::Center,
                VAlign::Middle, 10);
        return;
    }
    c.label("用 MCP 客户端连接蓝牙设备", ms, {0, 136, W, 22}, 14, Align::Center);
    TextStyle ns;
    ns.bold = true;
    ns.color = c.accent;
    float nw = std::min(W - 40, c.measure(sys.bleName.c_str(), ns, 16) + 28);
    c.out.roundRect((W - nw) / 2, 162, nw, 30, 15, c.accent.withAlpha(34), c.accent.withAlpha(90), 1);
    c.label(sys.bleName.c_str(), ns, {(W - nw) / 2 + 8, 162, nw - 16, 30}, 16, Align::Center, VAlign::Middle, 11);
    if (sys.wifi == SystemInfo::Wifi::Failed) {
        TextStyle ws;
        parseColor("warning", *c.theme, ws.color);
        c.label(("Wi-Fi「" + sys.ssid + "」连接失败，请重新配网").c_str(), ws, {12, 204, W - 24, 22}, 13,
                Align::Center, VAlign::Middle, 10);
    } else {
        c.label("调用 config_wifi 设置 Wi-Fi", ms, {0, 204, W, 22}, 13, Align::Center);
    }
}

}  // namespace

bool SceneBuilder::build(JsonVariantConst spec, int slide, int page, DisplayList& out, SceneReport& rep) {
    out.clear();
    rep = SceneReport();
    Spec s{spec, JsonVariantConst()};
    const char* type = spec["type"] | "text";
    if (!strcmp(type, "slides")) {
        JsonArrayConst slides = spec["slides"];
        if (slides.isNull() || slides.size() == 0) {
            rep.error = "slides must be a non-empty array";
            return false;
        }
        // V1 accepts HTML as a standalone screen. Reject it throughout a deck,
        // before accepting a first valid slide with a later invalid fragment.
        for (JsonVariantConst item : slides) {
            if (!strcmp(item["type"] | "", "html")) {
                rep.error = "HTML v1 requires show_html; HTML in slides is not supported";
                return false;
            }
        }
        rep.slides = int(slides.size());
        slide = std::max(0, std::min(slide, rep.slides - 1));
        s = Spec{slides[slide], spec};
        type = s.v["type"] | "text";
        if (!strcmp(type, "slides")) {
            rep.error = "slides cannot be nested";
            return false;
        }
    }

    Ctx c(fonts_, sys_, out, rep, s);
    page = std::max(0, page);
    if (!strcmp(type, "text"))
        buildText(c, page);
    else if (!strcmp(type, "notice"))
        buildNotice(c, page);
    else if (!strcmp(type, "list"))
        buildList(c, page);
    else if (!strcmp(type, "metrics"))
        buildMetrics(c);
    else if (!strcmp(type, "layout"))
        buildLayout(c);
    else if (!strcmp(type, "html")) {
        Rect area = c.begin();
        HtmlLayout::Report html;
        JsonString source = s["html"].as<JsonString>();
        Theme htmlTheme = *c.theme;
        htmlTheme.accent = c.accent;
        HtmlLayout::build(source.c_str(), source.size(), fonts_, htmlTheme,
                          {area.x, area.y, area.w, area.h}, out, html);
        rep.error = html.error;
        rep.htmlNodes = html.nodes;
        rep.htmlSteps = html.steps;
        rep.htmlTextWork = html.textWork;
        rep.warnings.insert(rep.warnings.end(), html.warnings.begin(), html.warnings.end());
        if (html.clipped) rep.warnings.push_back("HTML content overflow: clipped or ellipsized to its box");
    }
    else if (!strcmp(type, "home"))
        buildHome(c);
    else if (!strcmp(type, "setup"))
        buildSetup(c);
    else
        rep.error = std::string("unknown screen type '") + type + "'";

    if (!rep.error.empty()) {
        out.clear();
        return false;
    }
    return true;
}

bool SceneBuilder::buildToast(JsonVariantConst spec, DisplayList& out, SceneReport& rep) {
    Ctx c(fonts_, sys_, out, rep, Spec{spec, JsonVariantConst()});
    const char* text = spec["text"] | spec["message"].as<const char*>();
    if (!text || !*text) {
        rep.error = "text is required";
        return false;
    }
    const char* level = spec["level"] | "info";
    const char* iconName = "notifications";
    const char* colorName = "sky";
    if (!strcmp(level, "success")) {
        iconName = "check_circle";
        colorName = "green";
    } else if (!strcmp(level, "warning")) {
        iconName = "warning";
        colorName = "amber";
    } else if (!strcmp(level, "error")) {
        iconName = "error";
        colorName = "red";
    }
    // Toasts always use a dark glass pill so they read on any screen.
    RGBA color;
    parseColor(colorName, defaultTheme(), color);
    color = c.color(spec["color"], color, "color");
    uint32_t icon = spec["icon"].isNull() ? iconCodepoint(iconName) : c.icon(spec["icon"], "icon");

    float iconBlock = icon ? 40 : 0;
    float maxTextW = W - 24 - 28 - iconBlock;
    TextStyle st;
    st.color = RGBA::hex(0xF8FAFC);
    Block b;
    FitOptions o;
    o.width = maxTextW;
    o.height = 46;
    o.minSize = 13;
    o.maxSize = 16;
    o.maxLines = 2;
    o.lineHeight = 1.3f;
    if (!c.fit(b, text, st, c.markup(spec["markup"]), o)) return false;
    // Re-fit to the actual width so a short message gets a snug pill.
    float textW = std::ceil(b.layout.width) + 1;
    float w = std::min(W - 24, textW + 28 + iconBlock);
    float h = std::max(44.0f, b.layout.height + 18);
    float x = (W - w) / 2;
    const char* pos = spec["position"] | "top";
    float y = !strcmp(pos, "bottom") ? H - 12 - h : (!strcmp(pos, "center") ? (H - h) / 2 : 12);

    out.shadow(x, y + 3, w, h, std::min(h / 2, 22.0f), 10, RGBA(0, 0, 0, 120));
    out.roundRect(x, y, w, h, std::min(h / 2, 22.0f), RGBA::hex(0x111827, 240), RGBA::hex(0xFFFFFF, 36), 1);
    float tx = x + 14;
    if (icon) {
        out.circle(x + 14 + 15, y + h / 2, 15, color.withAlpha(56));
        c.iconAt(icon, x + 14 + 15 - 10, y + h / 2 - 10, 20, color);
        tx += iconBlock;
    }
    c.draw(b, {tx, y, textW, h}, VAlign::Middle);
    c.noteTruncated(b, "text");
    return true;
}
