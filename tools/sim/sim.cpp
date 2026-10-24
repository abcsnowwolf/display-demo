// Desktop preview of the display scenes: renders a screen description to PNG
// with exactly the layout and rasterization code that runs on the device.
//
//   tools/sim/build.sh
//   tools/sim/sim fonts.bin examples/list.json out.png [slide] [page] [toast.json]
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include "../../src/gfx/Scenes.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include "stb_image_write.h"

static std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void printReport(const char* what, const SceneReport& rep) {
    printf("%s: pages=%d slides=%d textSize=%.0f%s%s\n", what, rep.pages, rep.slides, rep.textSize,
           rep.error.empty() ? "" : " error=", rep.error.c_str());
    for (const auto& w : rep.warnings) printf("  warning: %s\n", w.c_str());
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s fonts.bin spec.json out.png [slide] [page] [toast.json]\n", argv[0]);
        return 2;
    }
    std::string pack = readFile(argv[1]);
    FontEngine fonts;
    if (!fonts.begin(reinterpret_cast<const uint8_t*>(pack.data()), pack.size())) {
        fprintf(stderr, "bad font pack\n");
        return 1;
    }
    fonts.setCacheBudget(4 << 20);

    JsonDocument spec;
    if (DeserializationError err = deserializeJson(spec, readFile(argv[2]))) {
        fprintf(stderr, "spec: %s\n", err.c_str());
        return 1;
    }
    SystemInfo sys;
    sys.bleName = "MCP-Display-3A7F";
    sys.wifi = SystemInfo::Wifi::Connected;
    sys.ssid = "Home-WiFi";
    sys.mcpUrl = "http://192.168.1.42:9000/mcp";
    sys.timeValid = true;
    sys.year = 2026, sys.month = 10, sys.day = 24, sys.weekday = 6, sys.hour = 9, sys.minute = 41;
    if (spec["sys_wifi"].is<int>()) sys.wifi = SystemInfo::Wifi(spec["sys_wifi"].as<int>());
    if (spec["sys_time"].is<bool>()) sys.timeValid = spec["sys_time"].as<bool>();

    SceneBuilder builder(fonts, sys);
    DisplayList list;
    SceneReport rep;
    int slide = argc > 4 ? atoi(argv[4]) : 0, page = argc > 5 ? atoi(argv[5]) : 0;
    auto t0 = std::chrono::steady_clock::now();
    builder.build(spec.as<JsonVariantConst>(), slide, page, list, rep);
    printReport("scene", rep);
    if (argc > 6) {
        JsonDocument toast;
        deserializeJson(toast, readFile(argv[6]));
        SceneReport trep;
        builder.buildToast(toast.as<JsonVariantConst>(), list, trep);
        printReport("toast", trep);
    }

    const int w = SceneBuilder::WIDTH, h = SceneBuilder::HEIGHT, band = 40;
    std::vector<uint16_t> frame(w * h);
    for (int y = 0; y < h; y += band) {  // exercise the strip path like a PSRAM-less board
        Canvas canvas(frame.data() + y * w, w, y, band);
        list.render(canvas, fonts);
    }
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("render: %.1f ms (host), glyph cache %zu KiB\n", ms, fonts.cacheBytes() / 1024);

    std::vector<uint8_t> rgb(w * h * 3);
    for (int i = 0; i < w * h; ++i) {
        uint16_t p = frame[i];
        int r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
        rgb[i * 3] = uint8_t(r << 3 | r >> 2);
        rgb[i * 3 + 1] = uint8_t(g << 2 | g >> 4);
        rgb[i * 3 + 2] = uint8_t(b << 3 | b >> 2);
    }
    return stbi_write_png(argv[3], w, h, 3, rgb.data(), w * 3) ? 0 : 1;
}
