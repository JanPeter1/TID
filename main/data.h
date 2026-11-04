#pragma once

#include <stdbool.h>
#include <stddef.h>
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
#define DATA_PHOTO_TITLE_LENGTH 64
#define DATA_PHOTO_DATE_LENGTH 11
#define DATA_PHOTO_PATH_LENGTH 64
#define DATA_PHOTO_MAX_BYTES (12U * 1024U)

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
    bool calendar_valid;
    bool events_valid;
    bool from_cache;
    bool photo_ready;
    bool photo_error;
    uint16_t photo_count;
    uint16_t photo_number;
    uint32_t photo_generation;
    float temperature_c;
    float feels_like_c;
    float wind_kmh;
    float gust_kmh;
    float precipitation_mm;
    int humidity_percent;
    int weather_code;
    char updated_at[20];
    char next_tide[48];
    char photo_title[DATA_PHOTO_TITLE_LENGTH];
    char photo_date[DATA_PHOTO_DATE_LENGTH];
    float tide_height_m;
    float tide_samples[TIDE_SAMPLES];
    weather_day_t days[DATA_DAYS];
    uint8_t event_count;
    texel_event_t events[DATA_EVENT_COUNT];
    char calendar[DATA_TEXT_LENGTH];
} app_data_t;

esp_err_t data_init(void);
void data_snapshot(app_data_t *out);
void data_request_refresh(void);
void data_request_photo(void);
void data_release_photo(void);
const uint8_t *data_photo_jpeg(size_t *size);
