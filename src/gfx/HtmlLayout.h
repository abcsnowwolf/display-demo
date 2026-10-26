#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "DisplayList.h"

// Versioned, deliberately small layout language. This is not a browser.
namespace HtmlLayout {
constexpr size_t MAX_BYTES = 12 * 1024;
constexpr size_t MAX_NODES = 96;  // includes the implicit viewport
constexpr size_t MAX_DEPTH = 12;
constexpr size_t MAX_TEXT = 2048;  // Unicode codepoints
constexpr size_t MAX_STEPS = 4096;
constexpr size_t MAX_TEXT_WORK = 65536;

struct Box { float x = 0, y = 0, w = 0, h = 0; };
struct DebugBox { std::string id; Box box; };
struct Report {
    std::string error;
    std::vector<std::string> warnings;  // ignored CSS/attributes, auto-closed tags, ...
    bool clipped = false;
    size_t nodes = 0, steps = 0, textWork = 0;
};

// Appends only on success. All parsing/layout completes before emitting paint.
// debug is optional and used by host geometry tests, never by the device.
bool build(const char* html, size_t bytes, FontEngine& fonts, const Theme& theme,
           Box viewport, DisplayList& out, Report& report, std::vector<DebugBox>* debug = nullptr);
const char* help();
}  // namespace HtmlLayout
