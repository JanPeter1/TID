#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define EVENT_THUMBNAIL_WIDTH 120
#define EVENT_THUMBNAIL_HEIGHT 80
#define EVENT_THUMBNAIL_BYTES (EVENT_THUMBNAIL_WIDTH * EVENT_THUMBNAIL_HEIGHT * sizeof(uint16_t))

esp_err_t image_cache_init(void);
bool image_cache_has(const char *url);
esp_err_t image_cache_read(const char *url, uint16_t **pixels, size_t *size);
esp_err_t image_cache_write(const char *url, const uint16_t *pixels, size_t size);
void image_cache_evict_if_needed(void);
void image_cache_clear(void);