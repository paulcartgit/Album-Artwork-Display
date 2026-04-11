#pragma once

// Mock esp_heap_caps for native builds — routes to stdlib malloc/free

#include <cstdlib>
#include <cstddef>

#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_INTERNAL 0

inline void* heap_caps_malloc(size_t size, uint32_t) {
    return malloc(size);
}

inline void* heap_caps_calloc(size_t n, size_t size, uint32_t) {
    return calloc(n, size);
}

inline void heap_caps_free(void* ptr) {
    free(ptr);
}
