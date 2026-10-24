#pragma once

#include <cstdint>
#include <string>

// Codepoint of a Material Icons (Round) glyph by name, 0 if unknown.
uint32_t iconCodepoint(const char* name);
// Comma separated list of every icon name, for get_display_info.
std::string iconNameList();
