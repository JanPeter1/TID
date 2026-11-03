#include "data.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define RESPONSE_LIMIT 16384
#define REFRESH_INTERVAL_MS (6U * 60U * 60U * 1000U)
#define CACHE_NAMESPACE "content"
#define CACHE_KEY "last_good"

static const char *TAG = "data";
static app_data_t current_data;
static SemaphoreHandle_t data_lock;
static EventGroupHandle_t refresh_events;
#define REFRESH_BIT BIT0

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool overflow;
} response_buffer_t;

static esp_err_t response_event(esp_http_client_event_t *event)
{
    response_buffer_t *response = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !response || event->data_len <= 0) {
        return ESP_OK;
    }
    size_t incoming = (size_t)event->data_len;
    if (response->length + incoming >= response->capacity) {
        response->overflow = true;
        return ESP_FAIL;
    }
    memcpy(response->data + response->length, event->data, incoming);
    response->length += incoming;
    response->data[response->length] = '\0';
    return ESP_OK;
}

static bool http_get(const char *url, const char *username, const char *password,
                     char *output, size_t output_size)
{
    response_buffer_t response = {
        .data = output,
        .capacity = output_size,
    };
    output[0] = '\0';
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = response_event,
        .user_data = &response,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    if (username && username[0]) {
        config.username = username;
        config.password = password;
        config.auth_type = HTTP_AUTH_TYPE_BASIC;
    }
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    bool success = err == ESP_OK && status >= 200 && status < 300 && !response.overflow;
    if (!success) {
        ESP_LOGW(TAG, "GET failed: %s, HTTP %d%s", esp_err_to_name(err), status,
                 response.overflow ? ", response exceeded limit" : "");
    }
    esp_http_client_cleanup(client);
    return success;
}

static cJSON *object_item(cJSON *object, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

static double json_number(cJSON *object, const char *key, double fallback)
{
    cJSON *item = object_item(object, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static bool parse_weather(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    cJSON *current = object_item(root, "current");
    cJSON *daily = object_item(root, "daily");
    if (!cJSON_IsObject(current) || !cJSON_IsObject(daily)) {
        cJSON_Delete(root);
        return false;
    }

    xSemaphoreTake(data_lock, portMAX_DELAY);
    current_data.temperature_c = json_number(current, "temperature_2m", NAN);
    current_data.feels_like_c = json_number(current, "apparent_temperature", NAN);
    current_data.humidity_percent = (int)json_number(current, "relative_humidity_2m", -1);
    current_data.wind_kmh = json_number(current, "wind_speed_10m", NAN);
    current_data.gust_kmh = json_number(current, "wind_gusts_10m", NAN);
    current_data.precipitation_mm = json_number(current, "precipitation", NAN);
    current_data.weather_code = (int)json_number(current, "weather_code", -1);
    current_data.from_cache = false;

    cJSON *time = object_item(current, "time");
    if (cJSON_IsString(time) && time->valuestring) {
        snprintf(current_data.updated_at, sizeof(current_data.updated_at), "%.16s", time->valuestring);
    }
    cJSON *dates = object_item(daily, "time");
    cJSON *codes = object_item(daily, "weather_code");
    cJSON *lows = object_item(daily, "temperature_2m_min");
    cJSON *highs = object_item(daily, "temperature_2m_max");
    cJSON *rain = object_item(daily, "precipitation_probability_max");
    for (int i = 0; i < DATA_DAYS; i++) {
        cJSON *date = cJSON_GetArrayItem(dates, i);
        cJSON *code = cJSON_GetArrayItem(codes, i);
        cJSON *low = cJSON_GetArrayItem(lows, i);
        cJSON *high = cJSON_GetArrayItem(highs, i);
        cJSON *probability = cJSON_GetArrayItem(rain, i);
        snprintf(current_data.days[i].date, sizeof(current_data.days[i].date), "%s",
                 cJSON_IsString(date) ? date->valuestring : "--");
        current_data.days[i].weather_code = cJSON_IsNumber(code) ? code->valueint : -1;
        current_data.days[i].low_c = cJSON_IsNumber(low) ? (float)low->valuedouble : NAN;
        current_data.days[i].high_c = cJSON_IsNumber(high) ? (float)high->valuedouble : NAN;
        current_data.days[i].rain_probability = cJSON_IsNumber(probability) ? probability->valueint : -1;
    }
    current_data.weather_valid = isfinite(current_data.temperature_c);
    xSemaphoreGive(data_lock);
    cJSON_Delete(root);
    return true;
}

static bool parse_tides(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *hourly = root ? object_item(root, "hourly") : NULL;
    cJSON *times = hourly ? object_item(hourly, "time") : NULL;
    cJSON *levels = hourly ? object_item(hourly, "sea_level_height_msl") : NULL;
    if (!cJSON_IsArray(times) || !cJSON_IsArray(levels)) {
        cJSON_Delete(root);
        return false;
    }

    int count = cJSON_GetArraySize(times);
    int peak_index = -1;
    float peak_level = 0;
    bool high_water = false;
    float samples[TIDE_SAMPLES] = {0};
    int sample_count = count < TIDE_SAMPLES ? count : TIDE_SAMPLES;
    for (int i = 0; i < sample_count; i++) {
        cJSON *item = cJSON_GetArrayItem(levels, i);
        samples[i] = cJSON_IsNumber(item) ? (float)item->valuedouble : NAN;
    }
    for (int i = 1; i < count - 1; i++) {
        cJSON *before = cJSON_GetArrayItem(levels, i - 1);
        cJSON *here = cJSON_GetArrayItem(levels, i);
        cJSON *after = cJSON_GetArrayItem(levels, i + 1);
        if (!cJSON_IsNumber(before) || !cJSON_IsNumber(here) || !cJSON_IsNumber(after)) continue;
        float a = (float)before->valuedouble;
        float b = (float)here->valuedouble;
        float c = (float)after->valuedouble;
        if (b >= a && b > c) {
            peak_index = i;
            peak_level = b;
            high_water = true;
            break;
        }
        if (b <= a && b < c) {
            peak_index = i;
            peak_level = b;
            high_water = false;
            break;
        }
    }
    if (peak_index >= 0) {
        cJSON *time = cJSON_GetArrayItem(times, peak_index);
        xSemaphoreTake(data_lock, portMAX_DELAY);
        snprintf(current_data.next_tide, sizeof(current_data.next_tide),
                 "%s %s %.2fm", high_water ? "HOOGWATER" : "LAAGWATER",
                 cJSON_IsString(time) ? time->valuestring : "--", peak_level);
        current_data.tide_height_m = peak_level;
        memcpy(current_data.tide_samples, samples, sizeof(samples));
        current_data.tide_valid = true;
        current_data.from_cache = false;
        xSemaphoreGive(data_lock);
    }
    cJSON_Delete(root);
    return peak_index >= 0;
}

static void html_to_text(char *html)
{
    char *read = html;
    char *write = html;
    bool in_tag = false;
    while (*read) {
        if (*read == '<') {
            in_tag = true;
            *write++ = ' ';
        } else if (*read == '>') {
            in_tag = false;
        } else if (!in_tag) {
            if (read[0] == '&' && strncmp(read, "&nbsp;", 6) == 0) {
                *write++ = ' ';
                read += 5;
            } else if (read[0] == '&' && strncmp(read, "&amp;", 5) == 0) {
                *write++ = '&';
                read += 4;
            } else if (read[0] == '&' && strncmp(read, "&uuml;", 6) == 0) {
                *write++ = 'u';
                read += 5;
            } else if (read[0] == '&' && strncmp(read, "&auml;", 6) == 0) {
                *write++ = 'a';
                read += 5;
            } else if (read[0] == '&' && strncmp(read, "&ouml;", 6) == 0) {
                *write++ = 'o';
                read += 5;
            } else if (read[0] == '&' && strncmp(read, "&szlig;", 7) == 0) {
                *write++ = 's';
                *write++ = 's';
                read += 6;
            } else if ((unsigned char)*read < 0x80) {
                *write++ = *read;
            } else {
                *write++ = '?';
                while (((unsigned char)*read & 0xC0) == 0x80) read++;
            }
        }
        read++;
    }
    *write = '\0';
    char *source = html;
    write = html;
    bool previous_space = true;
    while (*source) {
        char ch = *source++;
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
        if (ch == ' ' && previous_space) continue;
        *write++ = ch;
        previous_space = ch == ' ';
    }
    *write = '\0';
}

static void fetch_page(const char *url, const char *username, const char *password,
                       char *destination, size_t destination_size, bool calendar)
{
    char *response = calloc(1, RESPONSE_LIMIT);
    if (!response) return;
    if (http_get(url, username, password, response, RESPONSE_LIMIT)) {
        html_to_text(response);
        xSemaphoreTake(data_lock, portMAX_DELAY);
        snprintf(destination, destination_size, "%s", response);
        if (calendar) current_data.calendar_valid = destination[0] != '\0';
        else current_data.home_info_valid = destination[0] != '\0';
        current_data.from_cache = false;
        xSemaphoreGive(data_lock);
    }
    free(response);
}

static void fetch_all(void)
{
    char response[RESPONSE_LIMIT];
    const char *weather_url =
        "https://api.open-meteo.com/v1/forecast?latitude=53.124279&longitude=4.812830"
        "&current=temperature_2m,apparent_temperature,relative_humidity_2m,precipitation,weather_code,wind_speed_10m,wind_gusts_10m"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
        "&forecast_days=5&timezone=Europe%2FAmsterdam";
    const char *marine_url =
        "https://marine-api.open-meteo.com/v1/marine?latitude=53.124279&longitude=4.812830"
        "&hourly=sea_level_height_msl&forecast_days=3&timezone=Europe%2FAmsterdam";

    if (http_get(weather_url, NULL, NULL, response, sizeof(response))) parse_weather(response);
    if (http_get(marine_url, NULL, NULL, response, sizeof(response))) parse_tides(response);

    device_config_t config;
    if (config_load(&config) == ESP_OK) {
        char user_info[DATA_TEXT_LENGTH];
        char calendar[DATA_TEXT_LENGTH];
        fetch_page("https://familie-riegel.de/tx94/", NULL, NULL, user_info, sizeof(user_info), false);
        if (config.calendar_user[0] && config.calendar_password[0]) {
            fetch_page("https://familie-riegel.de/tx94/?Texel-Kalender",
                       config.calendar_user, config.calendar_password,
                       calendar, sizeof(calendar), true);
        } else {
            ESP_LOGI(TAG, "Calendar credentials are not configured");
        }
        config_clear(&config);
    }
    ESP_LOGI(TAG, "Content refresh finished");
}

static void load_cache(void)
{
    nvs_handle_t handle;
    if (nvs_open(CACHE_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    size_t size = sizeof(current_data);
    if (nvs_get_blob(handle, CACHE_KEY, &current_data, &size) != ESP_OK ||
        size != sizeof(current_data)) {
        memset(&current_data, 0, sizeof(current_data));
        ESP_LOGW(TAG, "No valid saved content cache");
    } else {
        current_data.from_cache = true;
        ESP_LOGI(TAG, "Restored cached content");
    }
    nvs_close(handle);
}

static void save_cache(void)
{
    nvs_handle_t handle;
    if (nvs_open(CACHE_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    esp_err_t err = nvs_set_blob(handle, CACHE_KEY, &current_data, sizeof(current_data));
    if (err == ESP_OK) err = nvs_commit(handle);
    if (err != ESP_OK) ESP_LOGW(TAG, "Could not save content cache: %s", esp_err_to_name(err));
    nvs_close(handle);
}

static void refresh_task(void *unused)
{
    (void)unused;
    TickType_t last_refresh = xTaskGetTickCount();
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(refresh_events, REFRESH_BIT, pdTRUE,
                                                pdFALSE, pdMS_TO_TICKS(REFRESH_INTERVAL_MS));
        if (bits & REFRESH_BIT || (xTaskGetTickCount() - last_refresh) >= pdMS_TO_TICKS(REFRESH_INTERVAL_MS)) {
            if (esp_wifi_sta_get_ap_info(&(wifi_ap_record_t){0}) == ESP_OK) {
                fetch_all();
                xSemaphoreTake(data_lock, portMAX_DELAY);
                save_cache();
                xSemaphoreGive(data_lock);
                last_refresh = xTaskGetTickCount();
            } else {
                ESP_LOGW(TAG, "No Wi-Fi connection; keeping cached content");
                last_refresh = xTaskGetTickCount() - pdMS_TO_TICKS(REFRESH_INTERVAL_MS - 60000);
            }
        }
    }
}

esp_err_t data_init(void)
{
    data_lock = xSemaphoreCreateMutex();
    refresh_events = xEventGroupCreate();
    if (!data_lock || !refresh_events) return ESP_ERR_NO_MEM;
    load_cache();
    if (xTaskCreate(refresh_task, "content_refresh", 12288, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void data_snapshot(app_data_t *out)
{
    if (!out || !data_lock) return;
    xSemaphoreTake(data_lock, portMAX_DELAY);
    memcpy(out, &current_data, sizeof(current_data));
    xSemaphoreGive(data_lock);
}

void data_request_refresh(void)
{
    if (refresh_events) xEventGroupSetBits(refresh_events, REFRESH_BIT);
}
