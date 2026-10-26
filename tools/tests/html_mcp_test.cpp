#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include "../../src/DisplayTools.h"
#include "../../src/DisplayService.h"
#include "../../src/gfx/HtmlLayout.h"

DisplayService display;
static FontEngine fonts;
static JsonDocument received;
static int submissions = 0;
static std::vector<uint16_t> pixels(320 * 240);

// Replace only the hardware/FreeRTOS service boundary. The actual registered
// tool handler, result conversion, SceneBuilder and renderer run unchanged.
DisplayService::Result DisplayService::show(JsonDocument spec, uint32_t) {
    ++submissions;
    received = spec;
    SystemInfo sys;
    SceneBuilder builder(fonts, sys);
    DisplayList next;
    Result result;
    result.ok = builder.build(spec.as<JsonVariantConst>(), 0, 0, next, result.report);
    result.error = result.report.error;
    if (result.ok) {
        Canvas canvas(pixels.data(), 320, 0, 240);
        next.render(canvas, fonts);
    }
    return result;
}
DisplayService::Result DisplayService::toast(JsonDocument, uint32_t) { return {}; }
DisplayService::Result DisplayService::clear(uint32_t) { return {}; }
DisplayService::Result DisplayService::setBacklight(bool, uint32_t) { return {}; }
DisplayService::Status DisplayService::status() { return {}; }
DisplayService::Result DisplayService::canvasDraw(JsonDocument, uint32_t) { return {}; }
DisplayService::Result DisplayService::canvasUpdate(JsonDocument, uint32_t) { return {}; }
DisplayService::Result DisplayService::canvasRemove(JsonDocument, uint32_t) { return {}; }
DisplayService::Result DisplayService::canvasGet(uint32_t) { return {}; }

int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream file(argv[1], std::ios::binary); std::stringstream data; data << file.rdbuf();
    auto pack = data.str();
    assert(fonts.begin(reinterpret_cast<const uint8_t*>(pack.data()), pack.size()));
    MCPServerBase registry;
    registerDisplayTools(registry);
    assert(registry.tools.size() == 16);  // includes canvas_draw/update/remove/get
    assert(registry.tools.count("show_html") && registry.tools.count("canvas_draw"));
    auto& tool = registry.tools.at("show_html");
    assert(tool.inputSchema.properties.at("html").type == "string");
    assert(tool.inputSchema.required == std::vector<String>{"html"});
    assert(tool.outputSchema.properties.count("html_layout_steps"));
    assert(!registry.tools.at("show_text").outputSchema.properties.count("html_layout_steps"));

    bool error = false;
    JsonDocument params;
    auto call = [&] { return tool.handler->call(params.as<JsonVariantConst>(), error); };
    auto response = call();
    assert(error && response["ok"] == false && submissions == 0);
    params["html"] = 42;
    response = call();
    assert(error && submissions == 0);
    params["html"] = std::string(HtmlLayout::MAX_BYTES + 1, 'a');
    response = call();
    assert(error && submissions == 0);

    params["html"] = "<div style='height:100%;display:flex;gap:8px;padding:12px'><p style='flex:2;background:accent'>中文</p><p style='flex:1'>MCP</p></div>";
    params["type"] = "text";  // caller cannot change the tool's scene type
    params["persist"] = false;
    params["theme"] = "paper";
    params["accent"] = "#00ff00";
    response = call();
    assert(!error && response["ok"] == true && submissions == 1);
    assert(received["type"] == "html" && received["persist"] == false && received["theme"] == "paper");
    assert(response["html_nodes"].as<int>() == 4 && response["html_layout_steps"].as<int>() > 0);
    assert(pixels[200 * 320 + 50] == 0x07e0);  // MCP accent reaches renderer
    auto previous = pixels;

    params["html"] = "<div><script>alert(1)</script></div>";
    response = call();
    assert(error && response["ok"] == false && submissions == 2);
    assert(std::string(response["error"].as<const char*>()).find("unsupported tag") != std::string::npos);
    assert(pixels == previous);

    // Lenient input is drawn and the ignored parts come back as warnings.
    params["html"] = "<div onclick='alert(1)' style='margin:4px'>温度 <b>23</b>℃</div>";
    response = call();
    assert(!error && response["ok"] == true && response["warnings"].size() >= 2);

    params.remove("theme");
    params["html"] = "<p style='height:25px'>长中文内容长中文内容长中文内容长中文内容长中文内容长中文内容长中文内容</p>";
    response = call();
    assert(!error && response["ok"] == true && response["warnings"].size() > 0);

    JsonDocument empty;
    response = registry.tools.at("get_display_info").handler->call(empty.as<JsonVariantConst>(), error);
    assert(!error && std::string(response["html_layout"].as<const char*>()).find("html-layout-v1") == 0);
    std::cout << "PASS: actual MCP registration/handler -> service boundary -> actual scene renderer; input rejection, errors, warnings, capabilities\n";
}
