#pragma once

#include <stddef.h>

typedef enum {
    MEMORY_ALLOC_LARGE,
    MEMORY_ALLOC_DMA,
} memory_allocation_t;

void *memory_alloc(size_t size, memory_allocation_t allocation);
void memory_free(void *memory);
