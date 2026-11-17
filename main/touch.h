#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t touch_driver_init(void);
bool touch_driver_read(int *x, int *y);
esp_err_t touch_driver_enable_deep_sleep_wakeup(void);
