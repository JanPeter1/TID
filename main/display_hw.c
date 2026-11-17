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
#include "freertos/semphr.h"
#include "driver/ledc.h"
#include "memory.h"

static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t color_transfer_done_sem;
static uint16_t *axs_transfer_buffer;

static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xBB, (uint8_t []){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5}, 8, 0},
    {0xA0, (uint8_t []){0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00}, 17, 0},
    {0xA2, (uint8_t []){0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80, 0x80, 0x80, 0x20, 0xf9, 0x10, 0x02, 0xff, 0xff, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A}, 31, 0},
    {0xD0, (uint8_t []){0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15, 0x42, 0xC2, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x60, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12}, 30, 0},
    {0xA3, (uint8_t []){0xA0, 0x06, 0xAa, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55}, 22, 0},
    {0xC1, (uint8_t []){0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0d, 0x00, 0xFF, 0x40}, 30, 0},
    {0xC3, (uint8_t []){0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01}, 11, 0},
    {0xC4, (uint8_t []){0x00, 0x24, 0x33, 0x80, 0x00, 0xea, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50}, 29, 0},
    {0xC5, (uint8_t []){0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00}, 23, 0},
    {0xC6, (uint8_t []){0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22}, 20, 0},
    {0xC7, (uint8_t []){0x50, 0x32, 0x28, 0x00, 0xa2, 0x80, 0x8f, 0x00, 0x80, 0xff, 0x07, 0x11, 0x9c, 0x67, 0xff, 0x24, 0x0c, 0x0d, 0x0e, 0x0f}, 20, 0},
    {0xC9, (uint8_t []){0x33, 0x44, 0x44, 0x01}, 4, 0},
    {0xCF, (uint8_t []){0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0x88, 0x00, 0x65, 0x09, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08}, 27, 0},
    {0xD5, (uint8_t []){0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92, 0x74, 0x04, 0x08, 0x6A, 0x04, 0x46, 0x03, 0x03, 0x03, 0x03, 0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00}, 30, 0},
    {0xD6, (uint8_t []){0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00, 0x01, 0x83, 0x07, 0x07, 0x00, 0x07, 0x07, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00}, 30, 0},
    {0xD7, (uint8_t []){0x03, 0x01, 0x0b, 0x09, 0x0f, 0x0d, 0x1E, 0x1F, 0x18, 0x1d, 0x1f, 0x19, 0x40, 0x8E, 0x04, 0x00, 0x20, 0xA0, 0x1F}, 19, 0},
    {0xD8, (uint8_t []){0x02, 0x00, 0x0a, 0x08, 0x0e, 0x0c, 0x1E, 0x1F, 0x18, 0x1d, 0x1f, 0x19}, 12, 0},
    {0xD9, (uint8_t []){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDD, (uint8_t []){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDF, (uint8_t []){0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90}, 8,  0},
    {0xE0, (uint8_t []){0x3B, 0x28, 0x10, 0x16, 0x0c, 0x06, 0x11, 0x28, 0x5c, 0x21, 0x0D, 0x35, 0x13, 0x2C, 0x33, 0x28, 0x0D}, 17, 0},
    {0xE1, (uint8_t []){0x37, 0x28, 0x10, 0x16, 0x0b, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14, 0x2C, 0x33, 0x28, 0x0F}, 17, 0},
    {0xE2, (uint8_t []){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE3, (uint8_t []){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F}, 17, 0},
    {0xE4, (uint8_t []){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE5, (uint8_t []){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0F}, 17, 0},
    {0xA4, (uint8_t []){0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30}, 16, 0},
    {0xA4, (uint8_t []){0x85, 0x85, 0x95, 0x85}, 4, 0},
    {0xBB, (uint8_t []){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 0},
    {0x13, (uint8_t []){0x00}, 0, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x2C, (uint8_t []){0x00, 0x00, 0x00, 0x00}, 4, 0},
};

static bool color_transfer_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *event_data,
                                void *user_context)
{
    (void)io;
    (void)event_data;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_context,
                          &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

static esp_err_t initialize_axs15231b(lv_display_t *display)
{
    (void)display;
    size_t transfer_buffer_size = (size_t)BOARD_DISPLAY_WIDTH *
                                  BOARD_DISPLAY_BUFFER_ROWS * sizeof(uint16_t);
    color_transfer_done_sem = xSemaphoreCreateBinary();
    if (!color_transfer_done_sem) return ESP_ERR_NO_MEM;
    axs_transfer_buffer = memory_alloc(transfer_buffer_size, MEMORY_ALLOC_DMA);
    if (!axs_transfer_buffer) return ESP_ERR_NO_MEM;

    // Initialize backlight with LEDC PWM
    const ledc_channel_config_t backlight_channel = {
        .gpio_num = BOARD_TFT_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = 1,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = 1,
        .duty = 0,
        .hpoint = 0
    };
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = 1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&backlight_timer), TAG, "configure backlight timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&backlight_channel), TAG, "configure backlight channel");
    ledc_set_duty(LEDC_LOW_SPEED_MODE, 1, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, 1);

    spi_bus_config_t bus = AXS15231B_PANEL_BUS_QSPI_CONFIG(
        BOARD_TFT_PIN_CLK, BOARD_TFT_PIN_D0, BOARD_TFT_PIN_D1,
        BOARD_TFT_PIN_D2, BOARD_TFT_PIN_D3,
        transfer_buffer_size);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_TFT_SPI_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "initialize AXS15231B QSPI bus");

    esp_lcd_panel_io_spi_config_t io_config =
        AXS15231B_PANEL_IO_QSPI_CONFIG(BOARD_TFT_PIN_CS, color_transfer_done,
                                       color_transfer_done_sem);
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
                            (esp_lcd_spi_bus_handle_t)BOARD_TFT_SPI_HOST,
                            &io_config, &io),
                        TAG, "create AXS15231B panel IO");

    axs15231b_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {.use_qspi_interface = 1},
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_TFT_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_axs15231b(io, &panel_config, &panel),
                        TAG, "create AXS15231B panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset AXS15231B");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "initialize AXS15231B");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, false), TAG,
                        "enable AXS15231B");

    // Turn on backlight at 100%
    ledc_set_duty(LEDC_LOW_SPEED_MODE, 1, 1023);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, 1);
    return ESP_OK;
}

static void flush_axs15231b(lv_display_t *display, const lv_area_t *area,
                            const uint8_t *pixels)
{
    int width = area->x2 - area->x1 + 1;
    int height = area->y2 - area->y1 + 1;
    if (area->x1 != 0 || area->y1 != 0 ||
        width != BOARD_DISPLAY_WIDTH || height != BOARD_DISPLAY_HEIGHT) {
        ESP_LOGE(TAG, "Expected full-screen flush, got (%d,%d)-(%d,%d)",
                 area->x1, area->y1, area->x2, area->y2);
        lv_display_flush_ready(display);
        return;
    }

    const uint16_t *source_pixels = (const uint16_t *)pixels;
    for (int x = 0; x < width; x += BOARD_DISPLAY_BUFFER_ROWS) {
        int chunk_width = width - x;
        if (chunk_width > BOARD_DISPLAY_BUFFER_ROWS) {
            chunk_width = BOARD_DISPLAY_BUFFER_ROWS;
        }

        for (int column = 0; column < chunk_width; column++) {
            for (int y = 0; y < height; y++) {
                axs_transfer_buffer[column * height + height - y - 1] =
                    source_pixels[y * width + x + column];
            }
        }

        esp_err_t err = esp_lcd_panel_draw_bitmap(
            panel, 0, x, height, x + chunk_width, axs_transfer_buffer);
        if (err == ESP_OK) {
            xSemaphoreTake(color_transfer_done_sem, portMAX_DELAY);
        } else {
            ESP_LOGE(TAG, "Display flush failed: %s", esp_err_to_name(err));
            lv_display_flush_ready(display);
            return;
        }
    }

    lv_display_flush_ready(display);
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
