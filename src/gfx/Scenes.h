#pragma once

#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "DisplayList.h"
#include "FontEngine.h"

// Device state shown by the system screens and the optional status bar.
struct SystemInfo {
    enum class Wifi : uint8_t { Unconfigured, Connecting, Connected, Failed };

    std::string deviceName = "MCP Display";
    std::string bleName;
    Wifi wifi = Wifi::Unconfigured;
    std::string ssid;
    std::string mcpUrl;
    bool timeValid = false;
    int year = 0, month = 0, day = 0, weekday = 0, hour = 0, minute = 0;

    bool operator==(const SystemInfo& o) const {
        return deviceName == o.deviceName && bleName == o.bleName && wifi == o.wifi && ssid == o.ssid &&
               mcpUrl == o.mcpUrl && timeValid == o.timeValid && year == o.year && month == o.month &&
               day == o.day && hour == o.hour && minute == o.minute;
    }
    bool operator!=(const SystemInfo& o) const { return !(*this == o); }
};

struct SceneReport {
    int pages = 1;         // pages of the current screen (or current slide)
    int slides = 1;        // number of slides when type == "slides"
    float textSize = 0;    // fitted size of the primary text, px
    bool usesClock = false;  // re-render once a minute
    std::vector<std::string> warnings;
    std::string error;     // non-empty: the spec was rejected, nothing to show
};

/* Turns a screen description (the JSON accepted by the MCP display tools)
 * into a DisplayList. Pure layout code: no hardware, so the same builder runs
 * in the desktop preview tool (tools/sim). */
class SceneBuilder {
public:
    static constexpr int WIDTH = 320;
    static constexpr int HEIGHT = 240;

    SceneBuilder(FontEngine& fonts, const SystemInfo& sys) : fonts_(fonts), sys_(sys) {}

    // `slide` and `page` select what to show for multi-slide / paginated specs.
    bool build(JsonVariantConst spec, int slide, int page, DisplayList& out, SceneReport& rep);
    // Overlay drawn on top of whatever is on screen.
    bool buildToast(JsonVariantConst spec, DisplayList& out, SceneReport& rep);

private:
    FontEngine& fonts_;
    const SystemInfo& sys_;
};
