#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>

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

inline void* gfxRealloc(void* p, size_t size) {
#ifdef ARDUINO
    void* q = heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, size);
#else
    return realloc(p, size);
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

/* STL allocator over gfxAlloc. Node-based containers (list, unordered_map)
 * make many small allocations that malloc() would keep in internal RAM; a
 * cache of thousands of glyphs then starves WiFi and TCP of internal heap. */
template <typename T>
struct GfxAllocator {
    using value_type = T;

    GfxAllocator() = default;
    template <typename U>
    GfxAllocator(const GfxAllocator<U>&) {}

    T* allocate(size_t n) {
        void* p = gfxAlloc(n * sizeof(T));
        if (!p) abort();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, size_t) { gfxFree(p); }

    template <typename U>
    bool operator==(const GfxAllocator<U>&) const { return true; }
    template <typename U>
    bool operator!=(const GfxAllocator<U>&) const { return false; }
};
