#pragma once
#include <ArduinoJson.h>
#include "esp_heap_caps.h"

/* ArduinoJson's pool and strings in PSRAM. A catalog parse makes hundreds of
 * small strings, which the default allocator would put in internal RAM
 * (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) at the moment WiFi needs it most. */
class PsramJsonAllocator : public ArduinoJson::Allocator {
public:
    void* allocate(size_t size) override { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
    void deallocate(void* ptr) override { heap_caps_free(ptr); }
    void* reallocate(void* ptr, size_t size) override { return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM); }
    static PsramJsonAllocator* instance() {
        static PsramJsonAllocator allocator;
        return &allocator;
    }
};
