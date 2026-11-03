#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define DATA_TEXT_LENGTH 1200
#define DATA_DAYS 6
#define TIDE_SAMPLES 12
#define DATA_EVENT_COUNT 12
#define DATA_EVENT_TITLE_LENGTH 72
#define DATA_EVENT_PLACE_LENGTH 48
#define DATA_EVENT_DESCRIPTION_LENGTH 120
#define DATA_EVENT_URL_LENGTH 128

typedef struct {
    char date[11];
    char time[6];
    char title[DATA_EVENT_TITLE_LENGTH];
    char place[DATA_EVENT_PLACE_LENGTH];
    char description[DATA_EVENT_DESCRIPTION_LENGTH];
    char url[DATA_EVENT_URL_LENGTH];
    uint8_t day_offset;
} texel_event_t;

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
    bool events_valid;
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
    uint8_t event_count;
    texel_event_t events[DATA_EVENT_COUNT];
    char home_info[DATA_TEXT_LENGTH];
    char calendar[DATA_TEXT_LENGTH];
} app_data_t;

esp_err_t data_init(void);
void data_snapshot(app_data_t *out);
void data_request_refresh(void);
