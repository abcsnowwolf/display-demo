#pragma once

#include <cstdint>

// Decodes one UTF-8 sequence starting at *p and advances p. Malformed input
// yields U+FFFD and consumes one byte, so callers always make progress.
inline uint32_t utf8Next(const char*& p) {
    const uint8_t* s = reinterpret_cast<const uint8_t*>(p);
    uint32_t c = s[0];
    int len = 1;
    if (c < 0x80) {
    } else if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        c = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        len = 2;
    } else if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        c = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        len = 3;
    } else if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
               (s[3] & 0xC0) == 0x80) {
        c = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        len = 4;
    } else {
        c = 0xFFFD;
    }
    p += len;
    return c;
}
