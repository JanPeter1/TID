#pragma once

#include "esp_err.h"
#include "lvgl.h"

esp_err_t display_hw_init(lv_display_t *display);
void display_hw_flush(lv_display_t *display, const lv_area_t *area,
                      const uint8_t *pixels);
