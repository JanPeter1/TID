#include "display.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "german_font.h"
#include "lvgl.h"
#include "touch.h"

#define TFT_HOST SPI2_HOST
#define TFT_PIN_MOSI 13
#define TFT_PIN_MISO 12
#define TFT_PIN_CLK 14
#define TFT_PIN_CS 15
#define TFT_PIN_DC 2
#define TFT_PIN_BL 21
#define LVGL_BUFFER_ROWS 20
#define LVGL_LINE_CAPACITY 256

static const char *TAG = "display";
static spi_device_handle_t tft;
static lv_display_t *lv_display;
static lv_obj_t *screen;
static lv_indev_t *touch_indev;
static int touch_x;
static int touch_y;
static bool touch_pressed;
static size_t line_count;
static lv_point_precise_t line_points[LVGL_LINE_CAPACITY][2];
DMA_ATTR static uint8_t display_buffer[DISPLAY_WIDTH * LVGL_BUFFER_ROWS * 2]
    __attribute__((aligned(4)));

static lv_color_t color_from_rgb565(uint16_t color)
{
    uint8_t red = ((color >> 11) & 0x1F) * 255 / 31;
    uint8_t green = ((color >> 5) & 0x3F) * 255 / 63;
    uint8_t blue = (color & 0x1F) * 255 / 31;
    return lv_color_make(red, green, blue);
}

static esp_err_t send_bytes(bool data, const void *bytes, size_t length)
{
    gpio_set_level(TFT_PIN_DC, data);
    spi_transaction_t transaction = {
        .length = length * 8,
        .tx_buffer = bytes,
    };
    return spi_device_transmit(tft, &transaction);
}

static void command(uint8_t value)
{
    send_bytes(false, &value, 1);
}

static void data_bytes(const void *values, size_t length)
{
    send_bytes(true, values, length);
}

static void set_window(int x, int y, int width, int height)
{
    uint8_t range[4];
    command(0x2A);
    range[0] = x >> 8;
    range[1] = x;
    range[2] = (x + width - 1) >> 8;
    range[3] = x + width - 1;
    data_bytes(range, sizeof(range));
    command(0x2B);
    range[0] = y >> 8;
    range[1] = y;
    range[2] = (y + height - 1) >> 8;
    range[3] = y + height - 1;
    data_bytes(range, sizeof(range));
    command(0x2C);
}

static void flush_display(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    int width = area->x2 - area->x1 + 1;
    int height = area->y2 - area->y1 + 1;
    set_window(area->x1, area->y1, width, height);
    esp_err_t err = send_bytes(true, pixels, (size_t)width * height * 2);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LVGL display flush failed: %s", esp_err_to_name(err));
    }
    lv_display_flush_ready(display);
}

static uint32_t lvgl_tick(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int x = 0;
    int y = 0;
    touch_pressed = touch_read(&x, &y);
    if (touch_pressed) {
        touch_x = x;
        touch_y = y;
    }
    data->state = touch_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = touch_x;
    data->point.y = touch_y;
}

esp_err_t display_init(void)
{
    gpio_config_t dc_config = {
        .pin_bit_mask = 1ULL << TFT_PIN_DC,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&dc_config), TAG, "configure display DC");
    gpio_config_t backlight_config = {
        .pin_bit_mask = 1ULL << TFT_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&backlight_config), TAG, "configure backlight");
    gpio_set_level(TFT_PIN_BL, 0);

    spi_bus_config_t bus = {
        .mosi_io_num = TFT_PIN_MOSI,
        .miso_io_num = TFT_PIN_MISO,
        .sclk_io_num = TFT_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(display_buffer),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(TFT_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "initialize SPI bus");
    spi_device_interface_config_t device = {
        .clock_speed_hz = 40000000,
        .mode = 0,
        .spics_io_num = TFT_PIN_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(TFT_HOST, &device, &tft),
                        TAG, "add TFT device");

    gpio_set_level(TFT_PIN_BL, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    command(0x01);
    vTaskDelay(pdMS_TO_TICKS(120));
    command(0x28);
    command(0xCF); data_bytes((uint8_t[]){0x00, 0xC1, 0x30}, 3);
    command(0xED); data_bytes((uint8_t[]){0x64, 0x03, 0x12, 0x81}, 4);
    command(0xE8); data_bytes((uint8_t[]){0x85, 0x00, 0x78}, 3);
    command(0xCB); data_bytes((uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5);
    command(0xF7); data_bytes((uint8_t[]){0x20}, 1);
    command(0xEA); data_bytes((uint8_t[]){0x00, 0x00}, 2);
    command(0xC0); data_bytes((uint8_t[]){0x10}, 1);
    command(0xC1); data_bytes((uint8_t[]){0x00}, 1);
    command(0xC5); data_bytes((uint8_t[]){0x30, 0x30}, 2);
    command(0xC7); data_bytes((uint8_t[]){0xB7}, 1);
    command(0x3A); data_bytes((uint8_t[]){0x55}, 1);
    command(0x36); data_bytes((uint8_t[]){0x08}, 1);
    command(0xB1); data_bytes((uint8_t[]){0x00, 0x1A}, 2);
    command(0xB6); data_bytes((uint8_t[]){0x08, 0x82, 0x27}, 3);
    command(0xF2); data_bytes((uint8_t[]){0x00}, 1);
    command(0x26); data_bytes((uint8_t[]){0x01}, 1);
    command(0xE0); data_bytes((uint8_t[]){0x0F, 0x2A, 0x28, 0x08, 0x0E, 0x08, 0x54, 0xA9,
                                          0x43, 0x0A, 0x0F, 0x00, 0x00, 0x00, 0x00}, 15);
    command(0xE1); data_bytes((uint8_t[]){0x00, 0x15, 0x17, 0x07, 0x11, 0x06, 0x2B, 0x56,
                                          0x3C, 0x05, 0x10, 0x0F, 0x3F, 0x3F, 0x0F}, 15);
    command(0x2B);
    data_bytes((uint8_t[]){0x00, 0x00, 0x01, 0x3F}, 4);
    command(0x2A);
    data_bytes((uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4);
    command(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));
    command(0x36);
    data_bytes((uint8_t[]){0x28}, 1);
    command(0x21);
    command(0x29);
    vTaskDelay(pdMS_TO_TICKS(20));

    lv_init();
    lv_tick_set_cb(lvgl_tick);
    lv_display = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (!lv_display) return ESP_ERR_NO_MEM;
    lv_display_set_color_format(lv_display, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(lv_display, display_buffer, NULL, sizeof(display_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(lv_display, flush_display);
    screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, color_from_rgb565(0x0841), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    touch_indev = lv_indev_create();
    if (!touch_indev) return ESP_ERR_NO_MEM;
    lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(touch_indev, lv_display);
    lv_indev_set_read_cb(touch_indev, lvgl_touch_read);
    lv_refr_now(lv_display);
    ESP_LOGI(TAG, "ILI9341 ready with LVGL partial rendering");
    return ESP_OK;
}

void display_process(void)
{
    lv_timer_handler();
}

bool display_touch_read(int *x, int *y)
{
    if (x) *x = touch_x;
    if (y) *y = touch_y;
    return touch_pressed;
}

static lv_obj_t *new_object(void)
{
    if (!screen) {
        ESP_LOGE(TAG, "Cannot draw before LVGL display initialization");
        return NULL;
    }

    lv_obj_t *object = lv_obj_create(screen);
    if (!object) {
        ESP_LOGE(TAG, "LVGL could not allocate a display object");
        return NULL;
    }
    lv_obj_remove_style_all(object);
    lv_obj_set_clickable(object, false);
    lv_obj_set_scrollable(object, false);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    return object;
}

void display_fill_rect(int x, int y, int width, int height, uint16_t color)
{
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > DISPLAY_WIDTH) width = DISPLAY_WIDTH - x;
    if (y + height > DISPLAY_HEIGHT) height = DISPLAY_HEIGHT - y;
    if (width <= 0 || height <= 0) return;

    lv_obj_t *object = new_object();
    if (!object) return;
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, color_from_rgb565(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, 0, 0);
}

void display_fill_round_rect(int x, int y, int width, int height, uint16_t color)
{
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > DISPLAY_WIDTH) width = DISPLAY_WIDTH - x;
    if (y + height > DISPLAY_HEIGHT) height = DISPLAY_HEIGHT - y;
    if (width <= 0 || height <= 0) return;

    lv_obj_t *object = new_object();
    if (!object) return;
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, color_from_rgb565(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, height / 2, 0);
}

void display_clear(uint16_t color)
{
    if (!screen) return;
    lv_obj_clean(screen);
    line_count = 0;
    lv_obj_set_style_bg_color(screen, color_from_rgb565(color), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
}

void display_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    if (!screen) return;
    if (line_count >= LVGL_LINE_CAPACITY) {
        ESP_LOGE(TAG, "LVGL line capacity exceeded");
        return;
    }

    int left = x0 < x1 ? x0 : x1;
    int top = y0 < y1 ? y0 : y1;
    lv_point_precise_t *points = line_points[line_count++];
    points[0].x = x0 - left;
    points[0].y = y0 - top;
    points[1].x = x1 - left;
    points[1].y = y1 - top;

    lv_obj_t *object = lv_line_create(screen);
    if (!object) {
        line_count--;
        ESP_LOGE(TAG, "LVGL could not allocate a line object");
        return;
    }
    lv_obj_remove_style_all(object);
    lv_obj_set_clickable(object, false);
    lv_obj_set_scrollable(object, false);
    lv_obj_set_pos(object, left, top);
    lv_obj_set_size(object, (x0 > x1 ? x0 - x1 : x1 - x0) + 1,
                    (y0 > y1 ? y0 - y1 : y1 - y0) + 1);
    lv_obj_set_style_line_color(object, color_from_rgb565(color), 0);
    lv_obj_set_style_line_width(object, 1, 0);
    lv_obj_set_style_line_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_line_rounded(object, true, 0);
    lv_line_set_points(object, points, 2);
}

void display_circle(int center_x, int center_y, int radius, uint16_t color)
{
    if (radius <= 0) return;
    lv_obj_t *object = new_object();
    if (!object) return;
    int diameter = radius * 2 + 1;
    lv_obj_set_pos(object, center_x - radius, center_y - radius);
    lv_obj_set_size(object, diameter, diameter);
    lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(object, color_from_rgb565(color), 0);
    lv_obj_set_style_border_width(object, 1, 0);
}

void display_fill_circle(int center_x, int center_y, int radius, uint16_t color)
{
    if (radius <= 0) return;
    lv_obj_t *object = new_object();
    if (!object) return;
    int diameter = radius * 2 + 1;
    lv_obj_set_pos(object, center_x - radius, center_y - radius);
    lv_obj_set_size(object, diameter, diameter);
    lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(object, color_from_rgb565(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
}

esp_err_t display_draw_qr_code(int x, int y, int size, const char *data)
{
    if (!screen || !data || size <= 0 || x < 0 || y < 0 ||
        x + size > DISPLAY_WIDTH || y + size > DISPLAY_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    lv_obj_t *qr = lv_qrcode_create(screen);
    if (!qr) return ESP_ERR_NO_MEM;
    lv_qrcode_set_size(qr, size);
    lv_qrcode_set_quiet_zone(qr, true);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_obj_set_pos(qr, x, y);
    if (lv_qrcode_update(qr, data, (uint32_t)strlen(data)) != LV_RESULT_OK) {
        lv_obj_delete(qr);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static const lv_font_t *font_for_scale(uint8_t scale)
{
    return scale == 2 ? &lv_font_german_24 : &lv_font_german_12;
}

static void display_label(int x, int y, const char *text, uint16_t foreground,
                          uint16_t background, uint8_t scale, bool transparent)
{
    if (!screen || !text || !text[0] || !scale || scale > 2) return;
    lv_obj_t *label = lv_label_create(screen);
    if (!label) {
        ESP_LOGE(TAG, "LVGL could not allocate a text label");
        return;
    }
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, font_for_scale(scale), 0);
    lv_obj_set_style_text_color(label, color_from_rgb565(foreground), 0);
    lv_obj_set_style_bg_color(label, color_from_rgb565(background), 0);
    lv_obj_set_style_bg_opa(label, transparent ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    lv_obj_set_style_border_width(label, 0, 0);
}

void display_text_bg(int x, int y, const char *text, uint16_t foreground,
                     uint16_t background, uint8_t scale)
{
    display_label(x, y, text, foreground, background, scale, false);
}

void display_text(int x, int y, const char *text, uint16_t color, uint8_t scale)
{
    display_label(x, y, text, color, 0x0841, scale, true);
}

void display_draw_frame(void)
{
    display_clear(0x0841);
    display_fill_rect(0, 0, DISPLAY_WIDTH, 32, 0x10A3);
}

void display_draw_message(const char *title, const char *body)
{
    display_draw_frame();
    display_text_bg(12, 6, title, 0xFFFF, 0x10A3, 2);
    display_text(12, 52, body, 0xFD20, 1);
}
