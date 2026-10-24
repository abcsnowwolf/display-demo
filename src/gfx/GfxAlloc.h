#pragma once

#include <cstddef>
#include <cstdlib>

#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

/* Large, long-lived graphics buffers (glyph cache, frame strip) prefer PSRAM so
 * the internal heap stays available to WiFi, BLE and AsyncTCP. Plain malloc()
 * would keep anything below CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (4 KiB) — i.e.
 * every glyph bitmap — in internal RAM. */
inline void* gfxAlloc(size_t size) {
#ifdef ARDUINO
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
#else
    return malloc(size);
#endif
}

inline void gfxFree(void* p) {
    free(p);
}

inline bool gfxHasPsram() {
#ifdef ARDUINO
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
#else
    return true;
#endif
}
