// Shim letting the firmware renderer run on a desktop compiler:
// bot_render.cpp allocates its polygon scratch buffers via esp_heap_caps.
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static inline void *heap_caps_malloc(size_t n, unsigned)
{
    return malloc(n);
}
