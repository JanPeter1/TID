#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 240

esp_err_t display_init(void);
void display_process(void);
bool display_touch_read(int *x, int *y);
void display_clear(uint16_t color);
void display_fill_rect(int x, int y, int width, int height, uint16_t color);
void display_fill_round_rect(int x, int y, int width, int height, uint16_t color);
void display_fill_round_rect_radius(int x, int y, int width, int height,
                                    int radius, uint16_t color);
void display_line(int x0, int y0, int x1, int y1, uint16_t color);
void display_circle(int center_x, int center_y, int radius, uint16_t color);
void display_fill_circle(int center_x, int center_y, int radius, uint16_t color);
void display_set_page_timer(uint8_t elapsed_seconds);
esp_err_t display_draw_tide_chart(const int32_t *values, size_t count,
                                  int32_t minimum, int32_t maximum);
esp_err_t display_draw_qr_code(int x, int y, int size, const char *data);
esp_err_t display_draw_image(int x, int y, const lv_image_dsc_t *image);
esp_err_t display_draw_jpeg(int x, int y, const uint8_t *data, size_t size);
void display_text(int x, int y, const char *text, uint16_t color, uint8_t scale);
void display_text_box(int x, int y, int width, int height, const char *text,
                      uint16_t color, uint8_t scale);
void display_text_bg(int x, int y, const char *text, uint16_t foreground,
                     uint16_t background, uint8_t scale);
void display_draw_frame(void);
void display_draw_message(const char *title, const char *body);
