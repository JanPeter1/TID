#include "memory.h"

#include "boards/board_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdint.h>

static const char *TAG = "memory";

void *memory_alloc(size_t size, memory_allocation_t allocation)
{
    if (size == 0) return NULL;

    uint32_t capabilities = MALLOC_CAP_8BIT;
    if (allocation == MEMORY_ALLOC_DMA) {
        capabilities |= MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    } else if (BOARD_HAS_PSRAM) {
        capabilities |= MALLOC_CAP_SPIRAM;
    }

    void *memory = heap_caps_malloc(size, capabilities);
    if (!memory && allocation == MEMORY_ALLOC_LARGE && BOARD_HAS_PSRAM) {
        ESP_LOGW(TAG, "PSRAM allocation of %u bytes failed; trying internal RAM",
                 (unsigned)size);
        memory = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!memory) {
        ESP_LOGE(TAG, "Could not allocate %u bytes (class %u)",
                 (unsigned)size, (unsigned)allocation);
    }
    return memory;
}

void memory_free(void *memory)
{
    heap_caps_free(memory);
}

static SemaphoreHandle_t s_tls_lock;

void memory_tls_init(void)
{
    if (!s_tls_lock) {
        s_tls_lock = xSemaphoreCreateMutex();
        if (!s_tls_lock) {
            ESP_LOGE(TAG, "Could not create TLS lock");
        }
    }
}

void memory_tls_lock(void)
{
    if (s_tls_lock) {
        xSemaphoreTake(s_tls_lock, portMAX_DELAY);
    }
}

void memory_tls_unlock(void)
{
    if (s_tls_lock) {
        xSemaphoreGive(s_tls_lock);
    }
}

void memory_log_heap(const char *tag, const char *context)
{
    uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGW(tag, "Internal heap %s: %u bytes free, %u-byte largest block",
             context, (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps));
}
