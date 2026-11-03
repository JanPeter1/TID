#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t touch_init(void);
bool touch_read(int *x, int *y);
esp_err_t touch_enable_deep_sleep_wakeup(void);
