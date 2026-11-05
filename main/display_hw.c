#include "display_hw.h"

#include "boards/board_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_TID_BOARD_JC3248W535C
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_axs15231b.h"
#endif

static const char *TAG = "display_hw";

#if CONFIG_TID_BOARD_CYD
static spi_device_handle_t tft;

static esp_err_t send_bytes(bool data, const void *bytes, size_t length)
{
    gpio_set_level(BOARD_TFT_PIN_DC, data);
    spi_transaction_t transaction = {
        .length = length * 8,
        .tx_buffer = bytes,
    };
    return spi_device_transmit(tft, &transaction);
}

static esp_err_t send_command(uint8_t command, const void *data, size_t length)
{
    ESP_RETURN_ON_ERROR(send_bytes(false, &command, 1), TAG, "send display command");
    if (length) {
        ESP_RETURN_ON_ERROR(send_bytes(true, data, length), TAG,
                            "send display command data");
    }
    return ESP_OK;
}

static esp_err_t initialize_ili9341(void)
{
    gpio_config_t dc_config = {
        .pin_bit_mask = 1ULL << BOARD_TFT_PIN_DC,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&dc_config), TAG, "configure display DC");

    gpio_config_t backlight_config = {
        .pin_bit_mask = 1ULL << BOARD_TFT_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&backlight_config), TAG, "configure backlight");
    gpio_set_level(BOARD_TFT_PIN_BL, 0);

    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_TFT_PIN_MOSI,
        .miso_io_num = BOARD_TFT_PIN_MISO,
        .sclk_io_num = BOARD_TFT_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz =
            BOARD_DISPLAY_WIDTH * BOARD_DISPLAY_BUFFER_ROWS * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_TFT_SPI_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "initialize display SPI bus");
    spi_device_interface_config_t device = {
        .clock_speed_hz = 40000000,
        .mode = 0,
        .spics_io_num = BOARD_TFT_PIN_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(BOARD_TFT_SPI_HOST, &device, &tft),
                        TAG, "add ILI9341 to SPI bus");

    gpio_set_level(BOARD_TFT_PIN_BL, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(send_command(0x01, NULL, 0), TAG, "reset ILI9341");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(send_command(0x28, NULL, 0), TAG, "disable ILI9341");
    ESP_RETURN_ON_ERROR(send_command(0xCF, (uint8_t[]){0x00, 0xC1, 0x30}, 3), TAG, "ILI9341 init");
    ESP_RETURN_ON_ERROR(send_command(0xED, (uint8_t[]){0x64, 0x03, 0x12, 0x81}, 4), TAG, "ILI9341 init");
    ESP_RETURN_ON_ERROR(send_command(0xE8, (uint8_t[]){0x85, 0x00, 0x78}, 3), TAG, "ILI9341 init");
    ESP_RETURN_ON_ERROR(send_command(0xCB, (uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5), TAG, "ILI9341 init");
    ESP_RETURN_ON_ERROR(send_command(0xF7, (uint8_t[]){0x20}, 1), TAG, "ILI9341 init");
    ESP_RETURN_ON_ERROR(send_command(0xEA, (uint8_t[]){0x00, 0x00}, 2), TAG, "ILI9341 init");
    ESP_RETURN_ON_ERROR(send_command(0xC0, (uint8_t[]){0x10}, 1), TAG, "ILI9341 init");
    ESP_RETURN_ON_ERROR(send_command(0xC1, (uint8_t[]){0x00}, 1), TAG, "ILI9341 init");
    ESP_RETURN_ON_ERROR(send_command(0xC5, (uint8_t[]){0x30, 0x30}, 2), TAG, "ILI9341 init");
    ESP_RETURN_ON_ERROR(send_command(0xC7, (uint8_t[]){0xB7}, 1), TAG, "ILI9341 init");
    ESP_RETURN_ON_ERROR(send_command(0x3A, (uint8_t[]){0x55}, 1), TAG, "ILI9341 init");
    ESP_RETURN_ON_ERROR(send_command(0x36, (uint8_t[]){0x08}, 1), TAG, "ILI9341 init");
    ESP_RETURN_ON_ERROR(send_command(0xB1, (uint8_t[]){0x00, 0x1A}, 2), TAG, "ILI9341 init");
    ESP_RETURN_ON_ERROR(send_command(0xB6, (uint8_t[]){0x08, 0x82, 0x27}, 3), TAG, "ILI9341 init");
    ESP_RETURN_ON_ERROR(send_command(0xF2, (uint8_t[]){0x00}, 1), TAG, "ILI9341 init");
    ESP_RETURN_ON_ERROR(send_command(0x26, (uint8_t[]){0x01}, 1), TAG, "ILI9341 init");
    ESP_RETURN_ON_ERROR(send_command(0xE0,
        (uint8_t[]){0x0F, 0x2A, 0x28, 0x08, 0x0E, 0x08, 0x54, 0xA9,
                    0x43, 0x0A, 0x0F, 0x00, 0x00, 0x00, 0x00}, 15),
        TAG, "ILI9341 init");
    ESP_RETURN_ON_ERROR(send_command(0xE1,
        (uint8_t[]){0x00, 0x15, 0x17, 0x07, 0x11, 0x06, 0x2B, 0x56,
                    0x3C, 0x05, 0x10, 0x0F, 0x3F, 0x3F, 0x0F}, 15),
        TAG, "ILI9341 init");
    ESP_RETURN_ON_ERROR(send_command(0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0x3F}, 4), TAG, "ILI9341 set window");
    ESP_RETURN_ON_ERROR(send_command(0x2A, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4), TAG, "ILI9341 set window");
    ESP_RETURN_ON_ERROR(send_command(0x11, NULL, 0), TAG, "ILI9341 sleep out");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(send_command(0x36, (uint8_t[]){0x28}, 1), TAG, "ILI9341 orientation");
    ESP_RETURN_ON_ERROR(send_command(0x21, NULL, 0), TAG, "ILI9341 inversion");
    ESP_RETURN_ON_ERROR(send_command(0x29, NULL, 0), TAG, "ILI9341 display on");
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

static void flush_ili9341(lv_display_t *display, const lv_area_t *area,
                          const uint8_t *pixels)
{
    uint8_t range[4];
    uint8_t command;
    int width = area->x2 - area->x1 + 1;
    int height = area->y2 - area->y1 + 1;
    command = 0x2A;
    esp_err_t err = send_bytes(false, &command, 1);
    range[0] = area->x1 >> 8;
    range[1] = area->x1;
    range[2] = area->x2 >> 8;
    range[3] = area->x2;
    if (err == ESP_OK) err = send_bytes(true, range, sizeof(range));
    command = 0x2B;
    if (err == ESP_OK) err = send_bytes(false, &command, 1);
    range[0] = area->y1 >> 8;
    range[1] = area->y1;
    range[2] = area->y2 >> 8;
    range[3] = area->y2;
    if (err == ESP_OK) err = send_bytes(true, range, sizeof(range));
    command = 0x2C;
    if (err == ESP_OK) err = send_bytes(false, &command, 1);
    if (err == ESP_OK) {
        err = send_bytes(true, pixels, (size_t)width * height * sizeof(uint16_t));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display flush failed: %s", esp_err_to_name(err));
    }
    lv_display_flush_ready(display);
}
#elif CONFIG_TID_BOARD_JC3248W535C
static esp_lcd_panel_handle_t panel;

static bool color_transfer_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *event_data,
                                void *user_context)
{
    (void)io;
    (void)event_data;
    lv_display_flush_ready(user_context);
    return false;
}

static esp_err_t initialize_axs15231b(lv_display_t *display)
{
    gpio_config_t backlight_config = {
        .pin_bit_mask = 1ULL << BOARD_TFT_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&backlight_config), TAG, "configure backlight");
    gpio_set_level(BOARD_TFT_PIN_BL, 0);

    spi_bus_config_t bus = AXS15231B_PANEL_BUS_QSPI_CONFIG(
        BOARD_TFT_PIN_CLK, BOARD_TFT_PIN_D0, BOARD_TFT_PIN_D1,
        BOARD_TFT_PIN_D2, BOARD_TFT_PIN_D3,
        BOARD_DISPLAY_WIDTH * BOARD_DISPLAY_BUFFER_ROWS * sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_TFT_SPI_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "initialize AXS15231B QSPI bus");

    esp_lcd_panel_io_spi_config_t io_config =
        AXS15231B_PANEL_IO_QSPI_CONFIG(BOARD_TFT_PIN_CS, color_transfer_done, display);
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
                            (esp_lcd_spi_bus_handle_t)BOARD_TFT_SPI_HOST,
                            &io_config, &io),
                        TAG, "create AXS15231B panel IO");

    axs15231b_vendor_config_t vendor_config = {
        .flags = {.use_qspi_interface = 1},
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_axs15231b(io, &panel_config, &panel),
                        TAG, "create AXS15231B panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset AXS15231B");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "initialize AXS15231B");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(panel, true), TAG,
                        "rotate AXS15231B to landscape");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel, false, true), TAG,
                        "set AXS15231B orientation");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG,
                        "enable AXS15231B");
    gpio_set_level(BOARD_TFT_PIN_BL, 1);
    return ESP_OK;
}

static void flush_axs15231b(lv_display_t *display, const lv_area_t *area,
                            const uint8_t *pixels)
{
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                                               area->x2 + 1, area->y2 + 1,
                                               pixels);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display flush failed: %s", esp_err_to_name(err));
        lv_display_flush_ready(display);
    }
}
#endif

esp_err_t display_hw_init(lv_display_t *display)
{
#if CONFIG_TID_BOARD_CYD
    (void)display;
    return initialize_ili9341();
#elif CONFIG_TID_BOARD_JC3248W535C
    return initialize_axs15231b(display);
#endif
}

void display_hw_flush(lv_display_t *display, const lv_area_t *area,
                      const uint8_t *pixels)
{
#if CONFIG_TID_BOARD_CYD
    flush_ili9341(display, area, pixels);
#elif CONFIG_TID_BOARD_JC3248W535C
    flush_axs15231b(display, area, pixels);
#endif
}
