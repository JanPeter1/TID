#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#define INFO_PAGE_MAX_LINES 10

typedef struct {
    const char *title;
    const char *url;
    const lv_image_dsc_t *image;
    const char *lines[INFO_PAGE_MAX_LINES];
} info_page_t;

extern const info_page_t info_pages[];
extern const size_t info_page_count;
extern const uint8_t splash_image_data[];
extern const size_t splash_image_size;
