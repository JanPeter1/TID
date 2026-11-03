#include "touch.h"

#include "display.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"

#define TOUCH_HOST SPI3_HOST
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK 25
#define TOUCH_CS 33
#define TOUCH_IRQ 36
#define TOUCH_MIN_X 240
#define TOUCH_MAX_X 3800
#define TOUCH_MIN_Y 200
#define TOUCH_MAX_Y 3800

static const char *TAG = "touch";
static spi_device_handle_t touch_device;
static bool reported_touch;

esp_err_t touch_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = TOUCH_MOSI,
        .miso_io_num = TOUCH_MISO,
        .sclk_io_num = TOUCH_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(TOUCH_HOST, &bus, SPI_DMA_DISABLED),
                        TAG, "initialize touch SPI bus");

    gpio_config_t irq_config = {
        .pin_bit_mask = 1ULL << TOUCH_IRQ,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&irq_config), TAG, "configure XPT2046 IRQ");

    spi_device_interface_config_t device = {
        .clock_speed_hz = 2500000,
        .mode = 0,
        .spics_io_num = TOUCH_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(TOUCH_HOST, &device, &touch_device),
                        TAG, "add XPT2046 to SPI bus");
    ESP_LOGI(TAG, "XPT2046 ready on SPI3");
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
    if (!x || !y || gpio_get_level(TOUCH_IRQ) != 0) {
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

    *x = clamp_map(raw_y, TOUCH_MIN_Y, TOUCH_MAX_Y, DISPLAY_WIDTH);
    *y = clamp_map(raw_x, TOUCH_MIN_X, TOUCH_MAX_X, DISPLAY_HEIGHT);
    if (!reported_touch) {
        ESP_LOGI(TAG, "touch raw=%u,%u screen=%d,%d", raw_x, raw_y, *x, *y);
        reported_touch = true;
    }
    return true;
}
