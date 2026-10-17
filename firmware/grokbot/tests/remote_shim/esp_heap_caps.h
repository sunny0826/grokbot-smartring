#pragma once
#include <cstdlib>
constexpr int MALLOC_CAP_SPIRAM = 1;
inline void *heap_caps_calloc(size_t n, size_t size, int) { return calloc(n, size); }
inline void *heap_caps_malloc(size_t size, int) { return malloc(size); }
