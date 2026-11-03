#include "display.h"

#include <ctype.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TFT_HOST SPI2_HOST
#define TFT_PIN_MOSI 13
#define TFT_PIN_MISO 12
#define TFT_PIN_CLK 14
#define TFT_PIN_CS 15
#define TFT_PIN_DC 2
#define TFT_PIN_BL 21

static const char *TAG = "display";
static spi_device_handle_t tft;

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
        .max_transfer_sz = 4096,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(TFT_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "initialize SPI bus");
    spi_device_interface_config_t device = {
        .clock_speed_hz = 40000000,
        .mode = 0,
        .spics_io_num = TFT_PIN_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(TFT_HOST, &device, &tft), TAG, "add TFT device");

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
    display_clear(0x0841);
    ESP_LOGI(TAG, "ILI9341 ready in landscape mode");
    return ESP_OK;
}

void display_fill_rect(int x, int y, int width, int height, uint16_t color)
{
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > DISPLAY_WIDTH) width = DISPLAY_WIDTH - x;
    if (y + height > DISPLAY_HEIGHT) height = DISPLAY_HEIGHT - y;
    if (width <= 0 || height <= 0) return;

    set_window(x, y, width, height);
    uint8_t pixels[1024];
    for (size_t i = 0; i < sizeof(pixels); i += 2) {
        pixels[i] = color >> 8;
        pixels[i + 1] = color;
    }
    size_t remaining = (size_t)width * height * 2;
    while (remaining) {
        size_t chunk = remaining < sizeof(pixels) ? remaining : sizeof(pixels);
        if (send_bytes(true, pixels, chunk) != ESP_OK) return;
        remaining -= chunk;
    }
}

void display_clear(uint16_t color)
{
    display_fill_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, color);
}

void display_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        display_fill_rect(x0, y0, 1, 1, color);
        if (x0 == x1 && y0 == y1) break;
        int doubled_error = 2 * error;
        if (doubled_error >= dy) {
            error += dy;
            x0 += sx;
        }
        if (doubled_error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void display_circle(int center_x, int center_y, int radius, uint16_t color)
{
    int x = radius;
    int y = 0;
    int error = 1 - radius;
    while (x >= y) {
        display_fill_rect(center_x + x, center_y + y, 1, 1, color);
        display_fill_rect(center_x + y, center_y + x, 1, 1, color);
        display_fill_rect(center_x - y, center_y + x, 1, 1, color);
        display_fill_rect(center_x - x, center_y + y, 1, 1, color);
        display_fill_rect(center_x - x, center_y - y, 1, 1, color);
        display_fill_rect(center_x - y, center_y - x, 1, 1, color);
        display_fill_rect(center_x + y, center_y - x, 1, 1, color);
        display_fill_rect(center_x + x, center_y - y, 1, 1, color);
        y++;
        if (error < 0) {
            error += 2 * y + 1;
        } else {
            x--;
            error += 2 * (y - x + 1);
        }
    }
}

void display_fill_circle(int center_x, int center_y, int radius, uint16_t color)
{
    for (int y = -radius; y <= radius; y++) {
        int width = 0;
        while (width * width + y * y <= radius * radius) width++;
        display_fill_rect(center_x - width + 1, center_y + y, width * 2 - 1, 1, color);
    }
}

static void glyph(char character, uint8_t columns[5])
{
    memset(columns, 0, 5);
    char c = (char)toupper((unsigned char)character);
    switch (c) {
    case 'A': { uint8_t g[]={0x7E,0x11,0x11,0x11,0x7E}; memcpy(columns,g,5); break; }
    case 'B': { uint8_t g[]={0x7F,0x49,0x49,0x49,0x36}; memcpy(columns,g,5); break; }
    case 'C': { uint8_t g[]={0x3E,0x41,0x41,0x41,0x22}; memcpy(columns,g,5); break; }
    case 'D': { uint8_t g[]={0x7F,0x41,0x41,0x22,0x1C}; memcpy(columns,g,5); break; }
    case 'E': { uint8_t g[]={0x7F,0x49,0x49,0x49,0x41}; memcpy(columns,g,5); break; }
    case 'F': { uint8_t g[]={0x7F,0x09,0x09,0x09,0x01}; memcpy(columns,g,5); break; }
    case 'G': { uint8_t g[]={0x3E,0x41,0x49,0x49,0x7A}; memcpy(columns,g,5); break; }
    case 'H': { uint8_t g[]={0x7F,0x08,0x08,0x08,0x7F}; memcpy(columns,g,5); break; }
    case 'I': { uint8_t g[]={0x00,0x41,0x7F,0x41,0x00}; memcpy(columns,g,5); break; }
    case 'J': { uint8_t g[]={0x20,0x40,0x41,0x3F,0x01}; memcpy(columns,g,5); break; }
    case 'K': { uint8_t g[]={0x7F,0x08,0x14,0x22,0x41}; memcpy(columns,g,5); break; }
    case 'L': { uint8_t g[]={0x7F,0x40,0x40,0x40,0x40}; memcpy(columns,g,5); break; }
    case 'M': { uint8_t g[]={0x7F,0x02,0x0C,0x02,0x7F}; memcpy(columns,g,5); break; }
    case 'N': { uint8_t g[]={0x7F,0x04,0x08,0x10,0x7F}; memcpy(columns,g,5); break; }
    case 'O': { uint8_t g[]={0x3E,0x41,0x41,0x41,0x3E}; memcpy(columns,g,5); break; }
    case 'P': { uint8_t g[]={0x7F,0x09,0x09,0x09,0x06}; memcpy(columns,g,5); break; }
    case 'Q': { uint8_t g[]={0x3E,0x41,0x51,0x21,0x5E}; memcpy(columns,g,5); break; }
    case 'R': { uint8_t g[]={0x7F,0x09,0x19,0x29,0x46}; memcpy(columns,g,5); break; }
    case 'S': { uint8_t g[]={0x46,0x49,0x49,0x49,0x31}; memcpy(columns,g,5); break; }
    case 'T': { uint8_t g[]={0x01,0x01,0x7F,0x01,0x01}; memcpy(columns,g,5); break; }
    case 'U': { uint8_t g[]={0x3F,0x40,0x40,0x40,0x3F}; memcpy(columns,g,5); break; }
    case 'V': { uint8_t g[]={0x1F,0x20,0x40,0x20,0x1F}; memcpy(columns,g,5); break; }
    case 'W': { uint8_t g[]={0x3F,0x40,0x38,0x40,0x3F}; memcpy(columns,g,5); break; }
    case 'X': { uint8_t g[]={0x63,0x14,0x08,0x14,0x63}; memcpy(columns,g,5); break; }
    case 'Y': { uint8_t g[]={0x07,0x08,0x70,0x08,0x07}; memcpy(columns,g,5); break; }
    case 'Z': { uint8_t g[]={0x61,0x51,0x49,0x45,0x43}; memcpy(columns,g,5); break; }
    case '0': { uint8_t g[]={0x3E,0x51,0x49,0x45,0x3E}; memcpy(columns,g,5); break; }
    case '1': { uint8_t g[]={0x00,0x42,0x7F,0x40,0x00}; memcpy(columns,g,5); break; }
    case '2': { uint8_t g[]={0x42,0x61,0x51,0x49,0x46}; memcpy(columns,g,5); break; }
    case '3': { uint8_t g[]={0x21,0x41,0x45,0x4B,0x31}; memcpy(columns,g,5); break; }
    case '4': { uint8_t g[]={0x18,0x14,0x12,0x7F,0x10}; memcpy(columns,g,5); break; }
    case '5': { uint8_t g[]={0x27,0x45,0x45,0x45,0x39}; memcpy(columns,g,5); break; }
    case '6': { uint8_t g[]={0x3C,0x4A,0x49,0x49,0x30}; memcpy(columns,g,5); break; }
    case '7': { uint8_t g[]={0x01,0x71,0x09,0x05,0x03}; memcpy(columns,g,5); break; }
    case '8': { uint8_t g[]={0x36,0x49,0x49,0x49,0x36}; memcpy(columns,g,5); break; }
    case '9': { uint8_t g[]={0x06,0x49,0x49,0x29,0x1E}; memcpy(columns,g,5); break; }
    case ':': { uint8_t g[]={0x00,0x36,0x36,0x00,0x00}; memcpy(columns,g,5); break; }
    case '.': { uint8_t g[]={0x00,0x60,0x60,0x00,0x00}; memcpy(columns,g,5); break; }
    case '-': { uint8_t g[]={0x08,0x08,0x08,0x08,0x08}; memcpy(columns,g,5); break; }
    case '/': { uint8_t g[]={0x20,0x10,0x08,0x04,0x02}; memcpy(columns,g,5); break; }
    case '?': { uint8_t g[]={0x02,0x01,0x51,0x09,0x06}; memcpy(columns,g,5); break; }
    case '%': { uint8_t g[]={0x63,0x13,0x08,0x64,0x63}; memcpy(columns,g,5); break; }
    case '+': { uint8_t g[]={0x08,0x08,0x3E,0x08,0x08}; memcpy(columns,g,5); break; }
    case '<': { uint8_t g[]={0x08,0x14,0x22,0x41,0x00}; memcpy(columns,g,5); break; }
    case '>': { uint8_t g[]={0x41,0x22,0x14,0x08,0x00}; memcpy(columns,g,5); break; }
    case ' ': break;
    default: { uint8_t g[]={0x02,0x01,0x51,0x09,0x06}; memcpy(columns,g,5); break; }
    }
}

void display_text_bg(int x, int y, const char *text, uint16_t foreground,
                     uint16_t background, uint8_t scale)
{
    if (!text || scale == 0 || scale > 2) return;
    uint8_t columns[5];
    while (*text && x < DISPLAY_WIDTH - (int)(5 * scale)) {
        glyph(*text++, columns);
        uint8_t pixels[6 * 8 * 2 * 4];
        size_t cursor = 0;
        for (int row = 0; row < 8; row++) {
            for (int sy = 0; sy < scale; sy++) {
                for (int column = 0; column < 6; column++) {
                    bool set = column < 5 && row < 7 && (columns[column] & (1U << row));
                    uint16_t color = set ? foreground : background;
                    for (int sx = 0; sx < scale; sx++) {
                        pixels[cursor++] = color >> 8;
                        pixels[cursor++] = color;
                    }
                }
            }
        }
        set_window(x, y, 6 * scale, 8 * scale);
        send_bytes(true, pixels, cursor);
        x += 6 * scale;
    }
}

void display_text(int x, int y, const char *text, uint16_t color, uint8_t scale)
{
    display_text_bg(x, y, text, color, 0x0841, scale);
}

void display_draw_frame(void)
{
    display_clear(0x0841);
    display_fill_rect(0, 0, DISPLAY_WIDTH, 32, 0x10A3);
}

void display_draw_message(const char *title, const char *body)
{
    display_draw_frame();
    display_text_bg(12, 10, title, 0xFFFF, 0x10A3, 2);
    display_text(12, 52, body, 0xFD20, 1);
}
