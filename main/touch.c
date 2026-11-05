#include "touch.h"

#include "boards/board_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"

#if CONFIG_TID_BOARD_CYD
#include "driver/spi_master.h"
#elif CONFIG_TID_BOARD_JC3248W535C
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_axs15231b.h"
#endif

static const char *TAG = "touch";
static bool reported_touch;

#if CONFIG_TID_BOARD_CYD
static spi_device_handle_t touch_device;

esp_err_t touch_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_TOUCH_PIN_MOSI,
        .miso_io_num = BOARD_TOUCH_PIN_MISO,
        .sclk_io_num = BOARD_TOUCH_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_TOUCH_SPI_HOST, &bus,
                                           SPI_DMA_DISABLED),
                        TAG, "initialize touch SPI bus");

    gpio_config_t irq_config = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_PIN_IRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&irq_config), TAG, "configure XPT2046 IRQ");

    spi_device_interface_config_t device = {
        .clock_speed_hz = 2500000,
        .mode = 0,
        .spics_io_num = BOARD_TOUCH_PIN_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(BOARD_TOUCH_SPI_HOST, &device,
                                           &touch_device),
                        TAG, "add XPT2046 to SPI bus");
    ESP_LOGI(TAG, "XPT2046 ready");
    return ESP_OK;
}

static esp_err_t read_axis(uint8_t command, uint16_t *value)
{
    uint8_t tx[3] = {command, 0, 0};
    uint8_t rx[sizeof(tx)] = {0};
    spi_transaction_t transaction = {
        .length = sizeof(tx) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_polling_transmit(touch_device, &transaction);
    if (err != ESP_OK) return err;
    *value = (((uint16_t)rx[1] << 8) | rx[2]) >> 3 & 0x0FFF;
    return ESP_OK;
}

static int clamp_map(int value, int minimum, int maximum, int extent)
{
    if (value < minimum) value = minimum;
    if (value > maximum) value = maximum;
    return (value - minimum) * (extent - 1) / (maximum - minimum);
}

bool touch_read(int *x, int *y)
{
    if (!x || !y || gpio_get_level(BOARD_TOUCH_PIN_IRQ) != 0) {
        reported_touch = false;
        return false;
    }

    uint16_t raw_x = 0;
    uint16_t raw_y = 0;
    esp_err_t err = read_axis(0xD0, &raw_x);
    if (err == ESP_OK) err = read_axis(0x90, &raw_y);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "XPT2046 read failed: %s", esp_err_to_name(err));
        reported_touch = false;
        return false;
    }
    if (raw_x == 0 || raw_y == 0 || raw_x >= 4095 || raw_y >= 4095) {
        reported_touch = false;
        return false;
    }

    *x = clamp_map(raw_y, BOARD_TOUCH_MIN_Y, BOARD_TOUCH_MAX_Y,
                   UI_BASE_WIDTH);
    *y = clamp_map(raw_x, BOARD_TOUCH_MIN_X, BOARD_TOUCH_MAX_X,
                   UI_BASE_HEIGHT);
    if (!reported_touch) {
        ESP_LOGI(TAG, "touch raw=%u,%u screen=%d,%d", raw_x, raw_y, *x, *y);
        reported_touch = true;
    }
    return true;
}
#elif CONFIG_TID_BOARD_JC3248W535C
static i2c_master_bus_handle_t touch_i2c_bus;
static esp_lcd_panel_io_handle_t touch_io;
static esp_lcd_touch_handle_t touch_controller;

esp_err_t touch_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = BOARD_TOUCH_I2C_PORT,
        .sda_io_num = BOARD_TOUCH_PIN_SDA,
        .scl_io_num = BOARD_TOUCH_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &touch_i2c_bus),
                        TAG, "initialize touch I2C bus");

    esp_lcd_panel_io_i2c_config_t io_config =
        ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG_EX(400000);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(touch_i2c_bus, &io_config,
                                                 &touch_io),
                        TAG, "create AXS15231B touch IO");
    esp_lcd_touch_config_t config = {
        .x_max = BOARD_DISPLAY_WIDTH,
        .y_max = BOARD_DISPLAY_HEIGHT,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = BOARD_TOUCH_PIN_IRQ,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = true, .mirror_x = false, .mirror_y = true},
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_axs15231b(
                            touch_io, &config, &touch_controller),
                        TAG, "initialize AXS15231B touch controller");

    gpio_config_t irq_config = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_PIN_IRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&irq_config), TAG, "configure touch IRQ");
    ESP_LOGI(TAG, "AXS15231B capacitive touch ready");
    return ESP_OK;
}

bool touch_read(int *x, int *y)
{
    if (!x || !y) {
        reported_touch = false;
        return false;
    }

    esp_err_t err = esp_lcd_touch_read_data(touch_controller);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AXS15231B touch read failed: %s", esp_err_to_name(err));
        reported_touch = false;
        return false;
    }
    esp_lcd_touch_point_data_t point = {0};
    uint8_t point_count = 0;
    err = esp_lcd_touch_get_data(touch_controller, &point, &point_count, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AXS15231B touch data failed: %s", esp_err_to_name(err));
        reported_touch = false;
        return false;
    }
    if (point_count == 0) {
        reported_touch = false;
        return false;
    }

    *x = point.x;
    *y = point.y;
    if (!reported_touch) {
        ESP_LOGI(TAG, "touch screen=%d,%d", *x, *y);
        reported_touch = true;
    }
    return true;
}
#endif

esp_err_t touch_enable_deep_sleep_wakeup(void)
{
    if (gpio_get_level(BOARD_TOUCH_PIN_IRQ) == 0) return ESP_ERR_INVALID_STATE;
    return esp_sleep_enable_ext0_wakeup(BOARD_TOUCH_PIN_IRQ, 0);
}
