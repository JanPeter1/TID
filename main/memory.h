#pragma once

#include <stddef.h>

typedef enum {
    MEMORY_ALLOC_LARGE,
    MEMORY_ALLOC_DMA,
} memory_allocation_t;

void *memory_alloc(size_t size, memory_allocation_t allocation);
void memory_free(void *memory);

/*
 * Only one TLS session at a time. On boards without PSRAM, a TLS handshake
 * (certificate parsing plus session buffers) needs a large share of the
 * internal heap, and two concurrent handshakes (content refresh and OTA check
 * right after boot) fail with PSA_ERROR_INSUFFICIENT_MEMORY (-0x008D).
 * memory_tls_init() must be called once before the tasks that use the lock start.
 */
void memory_tls_init(void);
void memory_tls_lock(void);
void memory_tls_unlock(void);
/* Logs free internal heap and largest free block with the given context. */
void memory_log_heap(const char *tag, const char *context);
