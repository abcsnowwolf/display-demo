#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <vector>

#include "../../src/gfx/HtmlLayout.h"
#include "../../src/gfx/Scenes.h"

using namespace HtmlLayout;
static FontEngine fonts;
static int cases = 0;
static std::string read(const char* path) {
    std::ifstream f(path, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str();
}
static std::vector<uint16_t> raster(const DisplayList& list, int band = 240) {
    std::vector<uint16_t> frame(320 * 240, 0);
    for (int y = 0; y < 240; y += band) {
        Canvas canvas(frame.data() + y * 320, 320, y, std::min(band, 240 - y));
        list.render(canvas, fonts);
    }
    return frame;
}
static std::vector<DebugBox> layout(const std::string& html, DisplayList* result = nullptr, Report* stats = nullptr) {
    Report rep; DisplayList out; std::vector<DebugBox> boxes;
    bool ok = build(html.data(), html.size(), fonts, defaultTheme(), {0, 0, 320, 240}, out, rep, &boxes);
    if (!ok) { std::cerr << html << "\n" << rep.error << "\n"; std::abort(); }
    assert(rep.nodes <= MAX_NODES && rep.steps <= MAX_STEPS && rep.textWork <= MAX_TEXT_WORK);
    if (result) *result = std::move(out);
    if (stats) *stats = rep;
    ++cases; return boxes;
}
static Box box(const std::vector<DebugBox>& b, const char* id) {
    for (const auto& n : b) if (n.id == id) return n.box;
    std::abort();
}
static void near(float a, float b) { if (std::fabs(a - b) > .1f) { std::cerr << a << " != " << b << "\n"; std::abort(); } }
static void reject(const std::string& html, const char* reason) {
    Report r; DisplayList out; out.rect(0, 0, 320, 240, RGBA::hex(0x123456));
    auto before = raster(out);
    assert(!build(html.data(), html.size(), fonts, defaultTheme(), {0, 0, 320, 240}, out, r));
    if (r.error.find(reason) == std::string::npos) { std::cerr << r.error << " expected " << reason << "\n"; std::abort(); }
    assert(before == raster(out));  // rejected snippets never publish partial drawing
    ++cases;
}
// Accepted, but must report a warning containing `reason`.
static Report accept(const std::string& html, const char* reason) {
    Report r; DisplayList out;
    if (!build(html.data(), html.size(), fonts, defaultTheme(), {0, 0, 320, 240}, out, r)) {
        std::cerr << html << "\n" << r.error << "\n"; std::abort();
    }
    bool found = false;
    for (const auto& w : r.warnings) found = found || w.find(reason) != std::string::npos;
    if (!found) { std::cerr << html << ": missing warning '" << reason << "'\n"; for (auto& w : r.warnings) std::cerr << "  " << w << "\n"; std::abort(); }
    raster(out);
    ++cases; return r;
}
int main(int argc, char** argv) {
    assert(argc == 2);
    std::string pack = read(argv[1]);
    assert(fonts.begin(reinterpret_cast<const uint8_t*>(pack.data()), pack.size()));
    fonts.setCacheBudget(1536 * 1024);
    auto b = layout("<div style='display:flex;height:100%;gap:8px;padding:12px'><div id='a' style='flex:2'></div><div id='b' style='flex:1'></div></div>");
    near(box(b, "a").w, 192); near(box(b, "b").w, 96);
    near(box(b, "a").x, 12); near(box(b, "b").x, 212); near(box(b, "a").h, 216);

    b = layout("<div style='display:flex;flex-direction:column;height:100%;gap:10px'><div id='head' style='height:30px'/><div id='body' style='flex:1;display:flex;gap:8px'><div id='side' style='width:72px;flex-shrink:0'/><div id='main' style='flex:1'/></div><div id='foot' style='height:20px'/></div>");
    near(box(b, "body").y, 40); near(box(b, "body").h, 170); near(box(b, "foot").y, 220);
    near(box(b, "main").x, 80); near(box(b, "main").w, 240);

    b = layout("<div style='display:flex;height:100%'><div id='a' style='flex:1;max-width:60px'/><div id='b' style='flex:1'/></div>");
    near(box(b, "a").w, 60); near(box(b, "b").w, 260);
    b = layout("<div style='display:flex'><div id='a' style='width:240px;min-width:200px'/><div id='b' style='width:240px'/></div>");
    near(box(b, "a").w, 200); near(box(b, "b").w, 120);
    b = layout("<div style='display:flex;height:100%;align-items:center;justify-content:space-between'><div id='a' style='width:40px;height:20px'/><div id='b' style='width:60px;height:40px;align-self:end'/></div>");
    near(box(b, "a").y, 110); near(box(b, "b").x, 260); near(box(b, "b").y, 200);
    b = layout("<div style='display:flex;flex-wrap:wrap;gap:10px'><div id='a' style='width:100px;height:20px'/><div id='b' style='width:100px;height:30px'/><div id='c' style='width:100px;height:20px'/><div id='d' style='width:100px;height:40px'/></div>");
    near(box(b, "c").x, 220); near(box(b, "d").x, 0); near(box(b, "d").y, 40);
    b = layout("<div style='height:100%;padding:10px'><div id='a' style='width:50%;height:50%'/></div>");
    near(box(b, "a").w, 150); near(box(b, "a").h, 110);
    b = layout("<div style='height:100%;padding:2px 4px 6px 8px'><div id='a' style='height:20px'/></div>");
    near(box(b, "a").x, 8); near(box(b, "a").y, 2); near(box(b, "a").w, 308);

    // Chinese wraps by available width, and explicit br survives text parsing.
    const std::string prose = "窗外的树影轻轻摇动，今天也要给自己一点安静的时间。";
    b = layout("<div style='display:flex;align-items:start;gap:10px'><p id='narrow' style='width:80px;flex-shrink:0;font-size:16px'>" + prose + "</p><p id='wide' style='width:230px;font-size:16px'>" + prose + "</p></div>");
    assert(box(b, "narrow").h > box(b, "wide").h);
    b = layout("<p id='lines' style='font-size:16px'>第一行<br>第二行</p>");
    assert(box(b, "lines").h > 32);
    b = layout("<p id='lines' style='font-size:16px'>第一行<br></p>");
    assert(box(b, "lines").h > 32);
    layout("<p>&lt;div&gt; &amp; &#20013; &#x6587; &quot; &apos;</p>");
    layout("<p>**literal** [size=999]literal[/size]</p>");

    // Overflow does not paint into the adjacent sibling; strip replay must
    // restore clip state before a later overlay.
    DisplayList clipped; Report rep;
    layout("<div style='display:flex;height:100%'><div style='width:160px;flex-shrink:0'><div style='width:300px;height:300px;background:#ff0000'/></div><div style='flex:1;background:#0000ff'/></div>", &clipped, &rep);
    assert(rep.clipped);
    clipped.rect(0, 220, 320, 20, RGBA::hex(0x00ff00));
    auto pixels = raster(clipped);
    assert(pixels[100 * 320 + 150] == 0xf800 && pixels[100 * 320 + 170] == 0x001f);
    assert(pixels[230 * 320] == 0x07e0 && pixels[230 * 320 + 319] == 0x07e0);
    assert(pixels == raster(clipped, 40));
    assert(pixels == raster(clipped, 17));
    layout("<p style='height:28px;font-size:16px'>" + prose + prose + prose + "</p>", nullptr, &rep);
    assert(rep.clipped);

    // Refused outright: active/embedded content, broken encoding, limits.
    reject("<script>alert(1)</script>", "unsupported tag");
    reject("<iframe></iframe>", "unsupported tag");
    reject("<p><input/></p>", "unsupported tag");
    reject("<p style='color:red>x</p>", "unclosed attribute");
    reject("<p>&#0; ok</p><!-- x", "unclosed comment");
    reject(std::string("<p>\xF0\x80\x80\x80</p>"), "UTF-8");
    reject(std::string("<p>\xE4\xB8</p>"), "UTF-8");
    reject(std::string("<p>x\0y</p>", 10), "control");
    reject(std::string(MAX_BYTES + 1, 'a'), "12288");
    reject("<p>" + std::string(MAX_TEXT + 1, 'a') + "</p>", "text limit");

    // Lenient: shown, with the problem reported as a warning.
    accept("<img src='https://example.com/a.png'><p>x</p>", "img");
    accept("<div onclick='x()'>hello</div>", "ignored attribute onclick");
    accept("<div class=card>hello</div>", "ignored attribute class");
    accept("<div STYLE='background:url(https://x)'>x</div>", "background");
    accept("<div style='position:absolute'>x</div>", "unsupported CSS property: position");
    accept("<p style='margin:4px'>x</p>", "margin");
    accept("<div style='width:calc(100% - 2px)'>x</div>", "width");
    accept("<div style='font-size:NaN'>x</div>", "font-size");
    accept("<div style='font-size:999px'>x</div>", "clamped");
    accept("<div style='width:-1px'>x</div>", "width");
    accept("<div style='min-width:100px;max-width:40px'>x</div>", "minimum");
    accept("<div><p style='height:50%'>x</p></div>", "definite parent");
    accept("<div style='display:flex;flex-direction:column'><p style='flex-basis:50%'>x</p></div>", "definite parent");
    accept("<div style='display:flex;flex-direction:column;flex-wrap:wrap'>x</div>", "flex-wrap");
    accept("<div><p>x</div>", "auto-closed");
    accept("<p>x</p></div>", "stray");
    accept("<div><p>x</p>", "auto-closed");
    accept("<p style='color:red' style='color:blue'>x</p>", "duplicate style");
    accept("<p>&nope; &#0; A & B</p>", "unknown entity");
    accept("<table>x</table>", "unknown tag");
    accept("<p style='font-family:Arial, sans-serif'>x</p>", "font-family");
    layout("<p style=color:red>unquoted ok</p>");
    layout("<p>a < b, 1 &lt; 2</p>");

    // Inline formatting: mixed text and inline tags become one rich-text leaf.
    b = layout("<p id='mix' style='font-size:16px'>温度 <span style='color:red;font-weight:bold'>23</span> ℃，"
               "<b>重要</b><strong>提醒</strong> <u>下划线</u> <small>小字</small> <mark>高亮</mark></p>");
    assert(box(b, "mix").h > 16 && box(b, "mix").h < 40);
    b = layout("<div><p id='a'>上</p><br><p id='c'>下</p></div>");
    assert(box(b, "c").y - box(b, "a").y > 2 * box(b, "a").h - 0.5f);  // br adds a blank line
    b = layout("<div style='display:flex'><p id='a'>左</p><br><p id='c'>右</p></div>");
    near(box(b, "c").x, box(b, "a").x + box(b, "a").w);  // ignored in a row
    b = layout("<h1 id='h'>标题</h1><ul><li id='li'>一</li><li>二</li></ul><hr id='hr'/><ol><li>甲</li></ol>");
    assert(box(b, "h").h > box(b, "li").h); near(box(b, "hr").h, 1);
    b = layout("<div style='display:flex;width:300px'><p id='a' style='flex:1 1 0'>A</p><p id='n' style='flex:none;width:100px'>N</p>"
               "<p id='c' style='flex:2 1 0px'>C</p></div>");
    near(box(b, "n").w, 100); near(box(b, "c").w, 2 * box(b, "a").w);
    b = layout("<p id='e' style='font-size:1.5em;padding:0.5em;color:rgb(255,0,0);border:1px solid #ccc;font-weight:600'>大</p>"
               "<p id='hid' style='display:none'>隐藏</p><p>A<span style='display:none'>X</span>B</p>");
    for (const auto& n : b) assert(n.id != "e" || n.box.h > 24 + 24);
    layout("<!DOCTYPE html><section><header><p>页眉</p></header><blockquote>引用</blockquote></section>");

    // Color emoji: COLR layers paint palette colors, not the white text color;
    // VS16/ZWJ/skin-tone codepoints take no space.
    if (fonts.hasFace(FACE_EMOJI)) {
        DisplayList emoji;
        layout("<p style='font-size:40px;color:#ffffff'>\xF0\x9F\x98\x80</p>", &emoji);  // U+1F600
        auto px = raster(emoji);
        bool colored = false;
        for (uint16_t v : px) colored = colored || (v != 0 && v != 0xFFFF && (v >> 11) > 20 && ((v >> 5) & 63) > 40 && (v & 31) < 10);
        assert(colored);  // yellow face
        auto plain = layout("<p id='a' style='font-size:20px'>\xF0\x9F\x91\x8D</p>");                          // 👍
        auto toned = layout("<p id='a' style='font-size:20px'>\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD\xEF\xB8\x8F</p>");  // 👍🏽 + VS16
        near(box(plain, "a").h, box(toned, "a").h);
    }

    std::string deep;
    for (size_t i = 0; i < MAX_DEPTH; ++i) deep += "<div>";
    deep += "hello";
    for (size_t i = 0; i < MAX_DEPTH; ++i) deep += "</div>";
    layout(deep); reject("<div>" + deep + "</div>", "nesting");
    std::string many;
    for (size_t i = 1; i < MAX_NODES; ++i) many += "<div/>";
    layout(many); reject(many + "<div/>", "node limit");

    // Scene/MCP shared entry point: rejected source must not be drawn. The
    // device similarly calls lcd.startWrite only after builder.build succeeds.
    SystemInfo sys; SceneBuilder builder(fonts, sys); SceneReport scene;
    JsonDocument spec; spec["type"] = "html"; spec["html"] = "<p>你好</p>";
    DisplayList list;
    assert(builder.build(spec.as<JsonVariantConst>(), 0, 0, list, scene));
    auto screen = raster(list);
    spec["html"] = "<script>x</script>";
    if (builder.build(spec.as<JsonVariantConst>(), 0, 0, list, scene)) screen = raster(list);
    assert(!scene.error.empty());
    spec["html"] = "<p>你好</p>";
    assert(builder.build(spec.as<JsonVariantConst>(), 0, 0, list, scene));
    assert(screen == raster(list));
    spec.clear(); spec["type"] = "slides";
    spec["slides"][0]["type"] = "text"; spec["slides"][0]["text"] = "valid";
    spec["slides"][1]["type"] = "html"; spec["slides"][1]["html"] = "<script>x</script>";
    assert(!builder.build(spec.as<JsonVariantConst>(), 0, 0, list, scene));

    // Seeded malformed-input exercise, suitable for ASan/UBSan. Check every
    // error leaves an existing list intact; valid mutations remain bounded.
    std::mt19937 rng(734);
    const std::string base = "<div style='display:flex;height:100%;gap:8px'><p style='flex:2'>hello 中文</p><p style='flex:1'>world</p></div>";
    for (int i = 0; i < 600; ++i) {
        std::string sample = base;
        for (unsigned j = 0, count = 1 + rng() % 8; j < count; ++j) sample[rng() % sample.size()] = char(rng() % 256);
        Report r; DisplayList out; out.rect(0, 0, 320, 240, RGBA::hex(0x102030));
        bool ok = build(sample.data(), sample.size(), fonts, defaultTheme(), {0, 0, 320, 240}, out, r);
        if (!ok) { auto f = raster(out); assert(f.front() == 0x1106 && f.back() == 0x1106); }
        else { assert(r.steps <= MAX_STEPS && r.textWork <= MAX_TEXT_WORK); raster(out); }
    }
    std::cout << "PASS: " << cases << " layout/parser cases, scene rollback, 600 seeded mutations, full/strip clipping\n";
}
