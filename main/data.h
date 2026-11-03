#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define DATA_TEXT_LENGTH 1200
#define DATA_DAYS 3
#define TIDE_SAMPLES 12

typedef struct {
    char date[11];
    int weather_code;
    float low_c;
    float high_c;
    int rain_probability;
} weather_day_t;

typedef struct {
    bool weather_valid;
    bool tide_valid;
    bool home_info_valid;
    bool calendar_valid;
    bool from_cache;
    float temperature_c;
    float feels_like_c;
    float wind_kmh;
    float gust_kmh;
    float precipitation_mm;
    int humidity_percent;
    int weather_code;
    char updated_at[20];
    char next_tide[48];
    float tide_height_m;
    float tide_samples[TIDE_SAMPLES];
    weather_day_t days[DATA_DAYS];
    char home_info[DATA_TEXT_LENGTH];
    char calendar[DATA_TEXT_LENGTH];
} app_data_t;

esp_err_t data_init(void);
void data_snapshot(app_data_t *out);
void data_request_refresh(void);
