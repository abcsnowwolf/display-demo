#pragma once

#include <ArduinoJson.h>

#include "gfx/GfxAlloc.h"

/* ArduinoJson allocator backed by PSRAM. Screen descriptions — a canvas can
 * hold hundreds of elements — would otherwise live in the internal heap that
 * WiFi, BLE and AsyncTCP depend on. */
class PsramJsonAllocator : public ArduinoJson::Allocator {
public:
    void* allocate(size_t size) override { return gfxAlloc(size); }
    void deallocate(void* p) override { gfxFree(p); }
    void* reallocate(void* p, size_t size) override { return gfxRealloc(p, size); }

    static ArduinoJson::Allocator* instance() {
        static PsramJsonAllocator allocator;
        return &allocator;
    }
};

inline JsonDocument psramDoc() {
    return JsonDocument(PsramJsonAllocator::instance());
}
