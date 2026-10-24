#include "DisplayTools.h"

#include <ArduinoJson.h>

#include <functional>
#include <initializer_list>
#include <memory>

#include "DisplayService.h"
#include "gfx/Icons.h"
#include "gfx/Style.h"

namespace {

class FnHandler : public ToolHandler {
public:
    using Fn = std::function<JsonDocument(JsonVariantConst, bool&)>;
    explicit FnHandler(Fn fn) : fn_(std::move(fn)) {}
    JsonDocument call(JsonVariantConst params) override {
        bool ignored = false;
        return call(params, ignored);
    }
    JsonDocument call(JsonVariantConst params, bool& isError) override { return fn_(params, isError); }

private:
    Fn fn_;
};

// ------------------------------------------------------------ schema helpers

Properties prop(const char* type, const String& description) {
    Properties p;
    p.type = type;
    p.description = description;
    return p;
}

Properties anyValue(const String& description) {
    Properties p;
    p.description = description;
    return p;
}

Properties enumProp(const String& description, std::initializer_list<const char*> values) {
    Properties p = prop("string", description);
    for (const char* v : values) p.enumValues.push_back(v);
    return p;
}

Properties objectSchema() {
    Properties p;
    p.type = "object";
    return p;
}

Properties arrayOf(Properties item, const String& description) {
    Properties p = prop("array", description);
    p.items.reset(new Properties(std::move(item)));
    return p;
}

Properties themeProp() {
    Properties p = prop("string", "Color theme: midnight (default), light, paper, aurora, sunset, forest, ocean, mono");
    for (int i = 0; i < themeCount(); ++i) p.enumValues.push_back(themeAt(i).name);
    return p;
}

const char* const COLOR_HELP =
    "Color: #RGB, #RRGGBB, #RRGGBBAA, a palette name (red, orange, amber, yellow, lime, green, emerald, teal, "
    "cyan, sky, blue, indigo, violet, purple, fuchsia, pink, rose, slate, gray, white, black) or a theme role "
    "(text, muted, accent, surface, border, success, warning, danger)";

const char* const MARKUP_HELP =
    " Inline markup: **bold**, [color=red]..[/color], [bg=amber]highlight[/bg], [size=1.5]..[/size] "
    "(relative), [font=kai]..[/font], [u]underline[/u]; \\n for line breaks.";

// Keys shared by every full-screen tool.
void addScreenProps(Properties& s) {
    s.properties["theme"] = themeProp();
    s.properties["background"] =
        prop("string", "Override the theme background: one color, or two comma-separated colors for a vertical "
                       "gradient, e.g. \"#0f172a,#312e81\"");
    s.properties["accent"] = prop("string", "Accent color for icons and highlights. " + String(COLOR_HELP));
    s.properties["status_bar"] = prop("boolean", "Show a slim clock + WiFi bar at the top (default false)");
    s.properties["duration_s"] =
        prop("number", "Seconds before returning to the previous screen; 0 or omitted keeps it up");
    s.properties["persist"] = prop("boolean", "Restore this screen after a reboot (default true)");
}

void addTextProps(Properties& s) {
    s.properties["font"] = enumProp(
        "Typeface: sans (Noto Sans SC, default, clean), kai (LXGW WenKai, calligraphic, for quotes/poems), "
        "display (Smiley Sans, condensed italic, for headlines/numbers)",
        {"sans", "kai", "display"});
    s.properties["bold"] = prop("boolean", "Bold body text");
    s.properties["color"] = prop("string", "Body text color. " + String(COLOR_HELP));
    s.properties["size"] = prop("number", "Fixed font size in px; omit to auto-fit the largest size that fits");
    s.properties["min_size"] = prop("number", "Smallest auto-fit size before paginating (default 14)");
    s.properties["max_size"] = prop("number", "Largest auto-fit size");
    s.properties["align"] = enumProp("Horizontal alignment; auto centers short text", {"auto", "left", "center", "right"});
    s.properties["line_height"] = prop("number", "Line height as a multiple of the font size (default 1.35)");
    s.properties["markup"] = prop("boolean", "Interpret inline markup (default true)");
}

Properties resultSchema() {
    Properties s = objectSchema();
    s.properties["ok"] = prop("boolean", "True when the screen was accepted and drawn");
    s.properties["pages"] = prop("integer", "Pages the content was split into (they rotate automatically)");
    s.properties["slides"] = prop("integer", "Number of slides");
    s.properties["text_size"] = prop("number", "Font size chosen for the main text, px");
    s.properties["render_ms"] = prop("integer", "Time spent laying out and drawing");
    s.properties["warnings"] = arrayOf(prop("string", "warning"), "Layout problems: unknown icons/colors, text cut off");
    s.properties["error"] = prop("string", "Why the request was rejected");
    s.required = {"ok"};
    return s;
}

JsonDocument toJson(const DisplayService::Result& r, bool& isError) {
    JsonDocument out;
    isError = !r.ok;
    out["ok"] = r.ok;
    if (!r.ok) {
        out["error"] = r.error;
        return out;
    }
    out["pages"] = r.report.pages;
    out["slides"] = r.report.slides;
    if (r.report.textSize > 0) out["text_size"] = r.report.textSize;
    out["render_ms"] = r.renderMs;
    if (!r.report.warnings.empty()) {
        JsonArray w = out["warnings"].to<JsonArray>();
        for (const auto& s : r.report.warnings) w.add(s);
    }
    return out;
}

// Registers a tool that forwards its arguments as a screen of `type`.
void registerScreenTool(MCPServerBase& server, const char* name, const char* type, const String& description,
                        Properties input) {
    Tool tool;
    tool.name = name;
    tool.description = description;
    tool.inputSchema = std::move(input);
    tool.outputSchema = resultSchema();
    std::string screenType = type;
    tool.handler = std::make_shared<FnHandler>([screenType](JsonVariantConst params, bool& isError) {
        JsonDocument spec;
        spec.set(params);
        spec["type"] = screenType;
        return toJson(display.show(std::move(spec)), isError);
    });
    server.RegisterTool(std::move(tool));
}

void registerShowText(MCPServerBase& server) {
    Properties s = objectSchema();
    s.properties["text"] = prop("string", String("Main text. Auto-fitted as large as possible; long text is "
                                                 "paginated.") + MARKUP_HELP);
    s.properties["title"] = prop("string", "Optional title row");
    s.properties["icon"] = prop("string", "Icon name (see get_display_info): beside the title, or large above "
                                          "the text when there is no title");
    s.properties["footer"] = prop("string", "Small muted line at the bottom, e.g. a source or timestamp");
    s.properties["valign"] = enumProp("Vertical alignment of the text block", {"top", "middle", "bottom"});
    s.properties["interval_s"] = prop("number", "Seconds per page when paginated (default 6)");
    addTextProps(s);
    addScreenProps(s);
    s.required = {"text"};
    registerScreenTool(server, "show_text", "text",
                       "Show a message on the 320x240 screen. Simplest way to display text (Chinese supported); "
                       "picks the largest font size that fits and centers short text.",
                       std::move(s));
}

void registerShowNotice(MCPServerBase& server) {
    Properties s = objectSchema();
    s.properties["message"] = prop("string", String("Body text.") + MARKUP_HELP);
    s.properties["title"] = prop("string", "Headline");
    s.properties["level"] = enumProp("Sets icon and color", {"info", "success", "warning", "error", "tip", "announcement"});
    s.properties["icon"] = prop("string", "Override the level icon");
    s.properties["color"] = prop("string", "Override the level color. " + String(COLOR_HELP));
    s.properties["font"] = enumProp("Typeface for the message", {"sans", "kai", "display"});
    s.properties["markup"] = prop("boolean", "Interpret inline markup (default true)");
    addScreenProps(s);
    registerScreenTool(server, "show_notice", "notice",
                       "Show a status card: big colored icon badge, headline and message. Good for alerts, "
                       "reminders, build/deploy results and announcements.",
                       std::move(s));
}

void registerShowList(MCPServerBase& server) {
    Properties item = objectSchema();
    item.properties["text"] = prop("string", String("Item text, wraps to two lines.") + MARKUP_HELP);
    item.properties["done"] = prop("boolean", "Checked state for the check style");
    item.properties["value"] = anyValue("Right-aligned value, e.g. a time, price or count");
    item.properties["icon"] = prop("string", "Per-item icon for the icon style");
    item.properties["color"] = prop("string", "Item text color");
    item.required = {"text"};

    Properties s = objectSchema();
    s.properties["title"] = prop("string", "List heading");
    s.properties["icon"] = prop("string", "Heading icon");
    s.properties["items"] = arrayOf(std::move(item), "Rows in order. Plain strings are also accepted.");
    s.properties["style"] = enumProp("Row marker; defaults to check when any item has done, else bullet",
                                     {"bullet", "number", "check", "icon", "none"});
    addTextProps(s);
    addScreenProps(s);
    s.required = {"items"};
    registerScreenTool(server, "show_list", "list",
                       "Show a list: to-dos with check marks, numbered steps, agenda with times, rankings. "
                       "Rows auto-size to fill the screen; long lists paginate.",
                       std::move(s));
}

void registerShowMetrics(MCPServerBase& server) {
    Properties m = objectSchema();
    m.properties["label"] = prop("string", "Caption above the value");
    m.properties["value"] = anyValue("Number or short string shown big");
    m.properties["unit"] = prop("string", "Unit after the value, e.g. ℃, %, 元");
    m.properties["icon"] = prop("string", "Icon before the label");
    m.properties["color"] = prop("string", "Accent for icon and progress bar. " + String(COLOR_HELP));
    m.properties["value_color"] = prop("string", "Color of the value itself");
    m.properties["progress"] = prop("number", "0-100, draws a progress bar");
    m.properties["note"] = prop("string", "Small line under the value, e.g. +3.2% or 较昨日");
    m.properties["trend"] = enumProp("Arrow + color for the note", {"up", "down", "flat"});
    m.required = {"value"};

    Properties s = objectSchema();
    s.properties["title"] = prop("string", "Heading");
    s.properties["icon"] = prop("string", "Heading icon");
    s.properties["metrics"] = arrayOf(std::move(m), "1-6 tiles; 1 is a hero number, 2-3 one row, 4 a 2x2 grid, 5-6 3x2");
    s.properties["font"] = enumProp("Typeface for the values (default sans bold)", {"sans", "kai", "display"});
    addScreenProps(s);
    s.required = {"metrics"};
    registerScreenTool(server, "show_metrics", "metrics",
                       "Show 1-6 key numbers as dashboard tiles with labels, units, icons, trends and progress "
                       "bars. Good for weather, sensors, stocks, KPIs, countdowns.",
                       std::move(s));
}

const char* const LAYOUT_HELP =
    "Free-form canvas, 320x240 px, origin top-left. Elements paint in order. Types and fields:\n"
    "- rect: x,y,w,h, fill (color or \"c1,c2\" gradient when radius is 0; default surface), radius, border, "
    "border_width, shadow\n"
    "- text: x,y,w,h box, text (markup), font, bold, size (omit = auto-fit to box), min_size, max_size, color, "
    "align, valign (top/middle/bottom), line_height, max_lines, bg, radius, padding\n"
    "- icon: name, x,y (top-left) or x,y,w,h (centered in box), size, color\n"
    "- circle: cx, cy, r, fill, border, border_width\n"
    "- line: x1,y1,x2,y2, width, color\n"
    "- progress: x,y,w,h, value 0-100, color, track, radius\n"
    "- ring: cx, cy, r, thickness, value 0-100 (clockwise from 12 o'clock), color, track\n"
    "- arc: cx, cy, r, thickness, start, end (degrees, 0 = 12 o'clock, clockwise), color";

void registerShowLayout(MCPServerBase& server) {
    Properties el = objectSchema();
    el.description = LAYOUT_HELP;
    el.properties["type"] = enumProp("Element kind", {"rect", "text", "icon", "circle", "line", "progress", "ring", "arc"});
    el.properties["x"] = prop("number", "Left edge, px");
    el.properties["y"] = prop("number", "Top edge, px");
    el.properties["w"] = prop("number", "Width, px");
    el.properties["h"] = prop("number", "Height, px");
    el.required = {"type"};

    Properties s = objectSchema();
    s.properties["elements"] = arrayOf(std::move(el), LAYOUT_HELP);
    addScreenProps(s);
    s.required = {"elements"};
    registerScreenTool(server, "show_layout", "layout",
                       "Compose a custom screen from positioned shapes, text boxes, icons, progress bars and "
                       "rings, for designs the templates don't cover. Text boxes auto-fit when size is omitted.",
                       std::move(s));
}

void registerShowSlides(MCPServerBase& server) {
    Properties slide = objectSchema();
    slide.description =
        "One screen: type (text | notice | list | metrics | layout) plus the same fields as the matching "
        "show_* tool. Slides inherit theme, background, status_bar, font and accent from the deck.";
    slide.properties["type"] = enumProp("Template", {"text", "notice", "list", "metrics", "layout"});
    slide.properties["interval_s"] = prop("number", "Override how long this slide stays");
    slide.required = {"type"};

    Properties s = objectSchema();
    s.properties["slides"] = arrayOf(std::move(slide), "Screens shown in rotation");
    s.properties["interval_s"] = prop("number", "Seconds per slide (default 8)");
    addScreenProps(s);
    s.required = {"slides"};
    registerScreenTool(server, "show_slides", "slides",
                       "Rotate through several screens automatically, e.g. weather, then agenda, then a quote.",
                       std::move(s));
}

void registerNotify(MCPServerBase& server) {
    Tool tool;
    tool.name = "notify";
    tool.description =
        "Pop a short toast over the current screen for a few seconds, then it disappears without replacing "
        "what is shown.";
    Properties s = objectSchema();
    s.properties["text"] = prop("string", String("Message, up to two lines.") + MARKUP_HELP);
    s.properties["level"] = enumProp("Icon and color preset", {"info", "success", "warning", "error"});
    s.properties["icon"] = prop("string", "Override the icon");
    s.properties["color"] = prop("string", "Override the icon color. " + String(COLOR_HELP));
    s.properties["position"] = enumProp("Where the toast appears (default top)", {"top", "center", "bottom"});
    s.properties["duration_s"] = prop("number", "Seconds to stay visible (default 5)");
    s.required = {"text"};
    tool.inputSchema = std::move(s);
    tool.outputSchema = resultSchema();
    tool.handler = std::make_shared<FnHandler>([](JsonVariantConst params, bool& isError) {
        JsonDocument spec;
        spec.set(params);
        return toJson(display.toast(std::move(spec)), isError);
    });
    server.RegisterTool(std::move(tool));
}

void registerClear(MCPServerBase& server) {
    Tool tool;
    tool.name = "clear_screen";
    tool.description = "Remove the current content and return to the home screen (clock + MCP address).";
    tool.inputSchema = objectSchema();
    tool.outputSchema = resultSchema();
    tool.handler = std::make_shared<FnHandler>([](JsonVariantConst, bool& isError) {
        return toJson(display.clear(), isError);
    });
    server.RegisterTool(std::move(tool));
}

void registerBacklight(MCPServerBase& server) {
    Tool tool;
    tool.name = "set_backlight";
    tool.description = "Turn the screen backlight on or off (content is kept).";
    Properties s = objectSchema();
    s.properties["on"] = prop("boolean", "true = backlight on");
    s.required = {"on"};
    tool.inputSchema = std::move(s);
    tool.outputSchema = resultSchema();
    tool.handler = std::make_shared<FnHandler>([](JsonVariantConst params, bool& isError) {
        return toJson(display.setBacklight(params["on"] | true), isError);
    });
    server.RegisterTool(std::move(tool));
}

void registerDisplayInfo(MCPServerBase& server) {
    Tool tool;
    tool.name = "get_display_info";
    tool.description =
        "Screen size, available themes, fonts, icon names, colors and markup, plus what is shown right now. "
        "Call it before designing a custom layout.";
    tool.inputSchema = objectSchema();
    Properties out = objectSchema();
    out.properties["width"] = prop("integer", "Pixels");
    out.properties["height"] = prop("integer", "Pixels");
    out.properties["themes"] = arrayOf(objectSchema(), "Theme names and descriptions");
    out.properties["fonts"] = arrayOf(objectSchema(), "Font families");
    out.properties["icons"] = prop("string", "Comma-separated icon names");
    out.properties["colors"] = prop("string", "Color syntax");
    out.properties["markup"] = prop("string", "Inline text markup");
    out.properties["current"] = objectSchema();
    out.properties["current"].description = "What is on screen now";
    out.required = {"width", "height", "themes", "fonts", "icons", "current"};
    tool.outputSchema = std::move(out);
    tool.handler = std::make_shared<FnHandler>([](JsonVariantConst, bool& isError) {
        isError = false;
        JsonDocument r;
        r["width"] = SceneBuilder::WIDTH;
        r["height"] = SceneBuilder::HEIGHT;
        JsonArray themes = r["themes"].to<JsonArray>();
        for (int i = 0; i < themeCount(); ++i) {
            JsonObject t = themes.add<JsonObject>();
            t["name"] = themeAt(i).name;
            t["description"] = themeAt(i).description;
        }
        JsonArray fonts = r["fonts"].to<JsonArray>();
        const char* const FONTS[][2] = {
            {"sans", "Noto Sans SC regular + bold; default, best for UI and body text"},
            {"kai", "LXGW WenKai; calligraphic, for quotes, poetry, warm notes"},
            {"display", "Smiley Sans oblique; condensed headlines and big numbers"},
        };
        for (const auto& f : FONTS) {
            JsonObject o = fonts.add<JsonObject>();
            o["name"] = f[0];
            o["description"] = f[1];
        }
        r["icons"] = iconNameList();
        r["colors"] = COLOR_HELP;
        r["markup"] = MARKUP_HELP + 1;
        DisplayService::Status st = display.status();
        JsonObject cur = r["current"].to<JsonObject>();
        cur["screen"] = st.screen;
        if (!st.title.empty()) cur["title"] = st.title;
        cur["page"] = st.page + 1;
        cur["pages"] = st.pages;
        if (st.slides > 1) {
            cur["slide"] = st.slide + 1;
            cur["slides"] = st.slides;
        }
        if (st.remainingS) cur["remaining_s"] = st.remainingS;
        cur["backlight"] = st.backlight;
        cur["fonts_installed"] = st.fontsReady;
        return r;
    });
    server.RegisterTool(std::move(tool));
}

}  // namespace

void registerDisplayTools(MCPServerBase& server) {
    registerShowText(server);
    registerShowNotice(server);
    registerShowList(server);
    registerShowMetrics(server);
    registerShowLayout(server);
    registerShowSlides(server);
    registerNotify(server);
    registerClear(server);
    registerBacklight(server);
    registerDisplayInfo(server);
    registerDescriptionTool(server);
}

void registerDescriptionTool(MCPServerBase& server) {
    Tool tool;
    tool.name = "get_description";
    tool.description = "Get display device identity metadata";
    tool.inputSchema = objectSchema();
    tool.outputSchema = objectSchema();
    tool.outputSchema.properties["device_type"] = prop("string", "Device category");
    tool.outputSchema.properties["brand"] = prop("string", "Manufacturer brand");
    tool.outputSchema.properties["model"] = prop("string", "Model identifier");
    tool.outputSchema.properties["description"] = prop("string", "Device description");
    tool.outputSchema.properties["alias"] = prop("string", "User-defined alias");
    tool.outputSchema.required = {"device_type", "brand", "model", "description", "alias"};
    tool.handler = std::make_shared<FnHandler>([](JsonVariantConst, bool& isError) {
        isError = false;
        JsonDocument r;
        r["device_type"] = "信息屏";
        r["brand"] = "DIY";
        r["model"] = "ESP32-S3 2.4\" LCD 320x240";
        r["description"] = "通过 MCP 展示文字、通知、列表和数据指标的彩色信息屏，支持中文";
        r["alias"] = "桌面信息屏";
        return r;
    });
    server.RegisterTool(std::move(tool));
}
