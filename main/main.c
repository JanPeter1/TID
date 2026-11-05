#include <stdlib.h>
#include <time.h>

#include "config.h"
#include "data.h"
#include "display.h"
#include "esp_err.h"
#include "touch.h"
#include "ui.h"
#include "web_config.h"

void app_main(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();

    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(touch_init());
    ESP_ERROR_CHECK(config_init());
    ESP_ERROR_CHECK(web_config_start());
    ESP_ERROR_CHECK(data_init());
    ESP_ERROR_CHECK(ui_start());
}
