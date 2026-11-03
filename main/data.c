#include "data.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define RESPONSE_LIMIT 16384
#define EVENT_RESPONSE_LIMIT 32768
#define REFRESH_INTERVAL_MS (6U * 60U * 60U * 1000U)
#define CACHE_PARTITION "content_cache"
#define CACHE_NAMESPACE "content"
#define CACHE_KEY "last_good"

static const char *TAG = "data";
static app_data_t current_data;
static SemaphoreHandle_t data_lock;
static EventGroupHandle_t refresh_events;
#define REFRESH_BIT BIT0

static void html_to_text(char *html);

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
    size_t available = response->capacity - response->length - 1;
    if (incoming > available) {
        if (available) {
            memcpy(response->data + response->length, event->data, available);
            response->length += available;
            response->data[response->length] = '\0';
        }
        response->overflow = true;
        return ESP_OK;
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
    bool success = err == ESP_OK && status >= 200 && status < 300;
    if (response.overflow) {
        ESP_LOGW(TAG, "Response truncated at %u bytes: %s (HTTP %d)",
                 (unsigned)response.length, url, status);
    }
    if (!success) {
        ESP_LOGW(TAG, "GET failed: %s, HTTP %d, URL %s",
                 esp_err_to_name(err), status, url);
    }
    esp_http_client_cleanup(client);
    return success;
}

static bool http_post_json(const char *url, const char *body,
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
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;

    esp_err_t err = esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK) err = esp_http_client_set_header(client, "x-site-language", "de");
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Origin", "https://www.texel.net");
    if (err == ESP_OK) {
        err = esp_http_client_set_header(client, "Referer",
                                         "https://www.texel.net/de/veranstaltungsuebersicht/");
    }
    if (err == ESP_OK) err = esp_http_client_set_post_field(client, body, strlen(body));
    if (err == ESP_OK) err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    bool success = err == ESP_OK && status >= 200 && status < 300;
    if (!success) {
        ESP_LOGW(TAG, "POST failed: %s, HTTP %d, URL %s",
                 esp_err_to_name(err), status, url);
        if (response.length) {
            ESP_LOGW(TAG, "POST response body: %.*s",
                     (int)(response.length < 240 ? response.length : 240), output);
        }
    }
    if (response.overflow) {
        ESP_LOGW(TAG, "POST response truncated at %u bytes: %s",
                 (unsigned)response.length, url);
    }
    esp_http_client_cleanup(client);
    return success && !response.overflow;
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

static const char *json_string(cJSON *object, const char *key)
{
    cJSON *item = object_item(object, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool event_dates(char today[11], char tomorrow[11])
{
    time_t now = time(NULL);
    struct tm local_now = {0};
    if (now < 1704067200 || !localtime_r(&now, &local_now)) {
        char updated_at[20] = {0};
        xSemaphoreTake(data_lock, portMAX_DELAY);
        snprintf(updated_at, sizeof(updated_at), "%s", current_data.updated_at);
        xSemaphoreGive(data_lock);
        int year, month, day;
        if (sscanf(updated_at, "%4d-%2d-%2d", &year, &month, &day) != 3) {
            return false;
        }
        local_now.tm_year = year - 1900;
        local_now.tm_mon = month - 1;
        local_now.tm_mday = day;
        local_now.tm_hour = 12;
        now = mktime(&local_now);
        if (now < 0 || !localtime_r(&now, &local_now)) return false;
    }

    strftime(today, 11, "%Y-%m-%d", &local_now);
    local_now.tm_mday++;
    time_t next_day = mktime(&local_now);
    if (next_day < 0 || !localtime_r(&next_day, &local_now)) return false;
    strftime(tomorrow, 11, "%Y-%m-%d", &local_now);
    return true;
}

static void copy_event_field(char *destination, size_t size, const char *source)
{
    if (!source || !source[0] || size == 0) {
        if (size) destination[0] = '\0';
        return;
    }
    snprintf(destination, size, "%s", source);
    html_to_text(destination);
}

static bool parse_event_response(const char *json, const char *today, const char *tomorrow)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *data = root ? object_item(root, "data") : NULL;
    cJSON *activities = data ? object_item(data, "activities") : NULL;
    cJSON *hits = activities ? object_item(activities, "hits") : NULL;
    if (!cJSON_IsArray(hits) || object_item(root, "errors")) {
        cJSON_Delete(root);
        return false;
    }

    texel_event_t events[DATA_EVENT_COUNT] = {0};
    uint8_t event_count = 0;
    for (cJSON *hit = hits->child; hit && event_count < DATA_EVENT_COUNT; hit = hit->next) {
        const char *title = json_string(hit, "name");
        const char *slug = json_string(hit, "slug");
        const char *place = json_string(hit, "place");
        cJSON *timeslots = object_item(hit, "timeslots");
        if (!title || !slug || !cJSON_IsArray(timeslots)) continue;

        for (cJSON *slot = timeslots->child;
             slot && event_count < DATA_EVENT_COUNT; slot = slot->next) {
            const char *date = json_string(slot, "startDateTime");
            if (!date || strlen(date) < 16) continue;
            uint8_t day_offset;
            if (strncmp(date, today, 10) == 0) day_offset = 0;
            else if (strncmp(date, tomorrow, 10) == 0) day_offset = 1;
            else continue;

            texel_event_t event = {0};
            snprintf(event.date, sizeof(event.date), "%.10s", date);
            snprintf(event.time, sizeof(event.time), "%.5s", date + 11);
            copy_event_field(event.title, sizeof(event.title), title);
            copy_event_field(event.place, sizeof(event.place), place);
            snprintf(event.url, sizeof(event.url),
                     "https://www.texel.net/de/aktivitat/%s/", slug);
            event.day_offset = day_offset;

            bool duplicate = false;
            for (uint8_t i = 0; i < event_count; i++) {
                if (strcmp(events[i].date, event.date) == 0 &&
                    strcmp(events[i].time, event.time) == 0 &&
                    strcmp(events[i].title, event.title) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) events[event_count++] = event;
        }
    }

    xSemaphoreTake(data_lock, portMAX_DELAY);
    memcpy(current_data.events, events, sizeof(events));
    current_data.event_count = event_count;
    current_data.events_valid = true;
    current_data.from_cache = false;
    xSemaphoreGive(data_lock);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Parsed %u VVV events for today and tomorrow", event_count);
    return true;
}

static void fetch_events(void)
{
    char today[11] = {0};
    char tomorrow[11] = {0};
    if (!event_dates(today, tomorrow)) {
        ESP_LOGW(TAG, "Cannot determine local date for VVV events");
        return;
    }
    static const char query[] =
        "query ActivitySearch($params: ActivitySearchParamsInput!, $limit: Int, "
        "$offset: Int, $order: ActivitySearchOrderEnum!, $startDate: String!, "
        "$endDate: String!) { activities(params: $params, limit: $limit, "
        "offset: $offset, order: $order) { ... on ActivitySearchResult { "
        "totalHits hits { ... on Activity { name slug place "
        "timeslots(startDate: $startDate, endDate: $endDate) { "
        "startDateTime endDateTime } } } } } }";
    cJSON *request = cJSON_CreateObject();
    cJSON *variables = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    if (!request || !variables || !params) {
        ESP_LOGE(TAG, "Could not allocate VVV query");
        cJSON_Delete(request);
        cJSON_Delete(variables);
        cJSON_Delete(params);
        return;
    }
    cJSON_AddStringToObject(request, "operationName", "ActivitySearch");
    cJSON_AddStringToObject(request, "query", query);
    cJSON_AddNumberToObject(variables, "limit", 40);
    cJSON_AddStringToObject(variables, "order", "RANDOM");
    cJSON_AddStringToObject(params, "startDate", today);
    cJSON_AddStringToObject(params, "endDate", tomorrow);
    cJSON_AddItemToObject(params, "targetAudiences", cJSON_CreateArray());
    cJSON_AddItemToObject(params, "themeSlugs", cJSON_CreateArray());
    cJSON_AddItemToObject(variables, "params", params);
    cJSON_AddStringToObject(variables, "startDate", today);
    cJSON_AddStringToObject(variables, "endDate", tomorrow);
    cJSON_AddItemToObject(request, "variables", variables);
    char *body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!body) {
        ESP_LOGE(TAG, "Could not serialize VVV query");
        return;
    }
    char *response = calloc(1, EVENT_RESPONSE_LIMIT);
    if (!response) {
        ESP_LOGE(TAG, "Could not allocate VVV response buffer");
        free(body);
        return;
    }
    if (http_post_json("https://texel-gateway.travelbase.nl/graphql",
                       body, response, EVENT_RESPONSE_LIMIT)) {
        if (!parse_event_response(response, today, tomorrow)) {
            ESP_LOGW(TAG, "VVV GraphQL response did not contain event data");
        }
    }
    free(response);
    free(body);
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
    char forecast_from[20] = {0};
    xSemaphoreTake(data_lock, portMAX_DELAY);
    snprintf(forecast_from, sizeof(forecast_from), "%s", current_data.updated_at);
    xSemaphoreGive(data_lock);
    if (!forecast_from[0]) {
        time_t now = time(NULL);
        struct tm local_now;
        if (now >= 1704067200 && localtime_r(&now, &local_now)) {
            strftime(forecast_from, sizeof(forecast_from), "%Y-%m-%dT%H:%M", &local_now);
        }
    }

    int start_index = 0;
    if (forecast_from[0]) {
        for (int i = 0; i < count; i++) {
            cJSON *time = cJSON_GetArrayItem(times, i);
            if (cJSON_IsString(time) && time->valuestring &&
                strcmp(time->valuestring, forecast_from) >= 0) {
                start_index = i;
                break;
            }
        }
    }
    int peak_index = -1;
    float peak_level = 0;
    bool high_water = false;
    float samples[TIDE_SAMPLES];
    for (int i = 0; i < TIDE_SAMPLES; i++) samples[i] = NAN;
    int sample_count = count - start_index < TIDE_SAMPLES
        ? count - start_index : TIDE_SAMPLES;
    for (int i = 0; i < sample_count; i++) {
        cJSON *item = cJSON_GetArrayItem(levels, start_index + i);
        samples[i] = cJSON_IsNumber(item) ? (float)item->valuedouble : NAN;
    }
    for (int i = start_index + 1; i < count - 1; i++) {
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
        char tide_date[12] = "--.-- --:--";
        if (cJSON_IsString(time) && time->valuestring &&
            strlen(time->valuestring) >= 16) {
            snprintf(tide_date, sizeof(tide_date), "%.2s.%.2s %.5s",
                     time->valuestring + 8, time->valuestring + 5,
                     time->valuestring + 11);
        }
        xSemaphoreTake(data_lock, portMAX_DELAY);
        snprintf(current_data.next_tide, sizeof(current_data.next_tide),
                 "%s %s %.2fm",
                 high_water ? "HOCHWASSER" : "NIEDRIGWASSER",
                 tide_date, peak_level);
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
    char *response = calloc(1, RESPONSE_LIMIT);
    if (!response) {
        ESP_LOGE(TAG, "Could not allocate HTTP response buffer");
        return;
    }
    const char *weather_url =
        "https://api.open-meteo.com/v1/forecast?latitude=53.124279&longitude=4.812830"
        "&current=temperature_2m,apparent_temperature,relative_humidity_2m,precipitation,weather_code,wind_speed_10m,wind_gusts_10m"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
        "&forecast_days=6&timezone=Europe%2FAmsterdam";
    const char *marine_url =
        "https://marine-api.open-meteo.com/v1/marine?latitude=53.124279&longitude=4.812830"
        "&hourly=sea_level_height_msl&forecast_days=3&timezone=Europe%2FAmsterdam";

    if (http_get(weather_url, NULL, NULL, response, RESPONSE_LIMIT)) parse_weather(response);
    if (http_get(marine_url, NULL, NULL, response, RESPONSE_LIMIT)) parse_tides(response);
    fetch_events();

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
    free(response);
    ESP_LOGI(TAG, "Content refresh finished");
}

static void load_cache(void)
{
    nvs_handle_t handle;
    if (nvs_open_from_partition(CACHE_PARTITION, CACHE_NAMESPACE,
                                NVS_READONLY, &handle) != ESP_OK) return;
    size_t size = sizeof(current_data);
    if (nvs_get_blob(handle, CACHE_KEY, &current_data, &size) != ESP_OK ||
        size != sizeof(current_data)) {
        memset(&current_data, 0, sizeof(current_data));
        ESP_LOGW(TAG, "No valid saved content cache");
    } else {
        if (current_data.event_count > DATA_EVENT_COUNT) {
            current_data.event_count = 0;
            current_data.events_valid = false;
            ESP_LOGW(TAG, "Ignoring invalid cached event count");
        }
        current_data.from_cache = true;
        ESP_LOGI(TAG, "Restored cached content");
    }
    nvs_close(handle);
}

static void save_cache(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(CACHE_PARTITION, CACHE_NAMESPACE,
                                            NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not open content cache: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(handle, CACHE_KEY, &current_data, sizeof(current_data));
    if (err == ESP_OK) err = nvs_commit(handle);
    if (err != ESP_OK) ESP_LOGW(TAG, "Could not save content cache: %s", esp_err_to_name(err));
    nvs_close(handle);
}

static void refresh_task(void *unused)
{
    (void)unused;
    TickType_t last_refresh = 0;
    bool refresh_pending = true;
    bool waiting_for_ip_logged = false;
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(refresh_events, REFRESH_BIT, pdTRUE,
                                                pdFALSE, pdMS_TO_TICKS(1000));
        TickType_t now = xTaskGetTickCount();
        if (bits & REFRESH_BIT) refresh_pending = true;
        if (now - last_refresh >= pdMS_TO_TICKS(REFRESH_INTERVAL_MS)) {
            refresh_pending = true;
        }
        if (!refresh_pending) continue;

        if (!config_wifi_connected()) {
            if (!waiting_for_ip_logged) {
                ESP_LOGI(TAG, "Waiting for Wi-Fi IP before fetching content");
                waiting_for_ip_logged = true;
            }
            continue;
        }

        waiting_for_ip_logged = false;
        fetch_all();
        xSemaphoreTake(data_lock, portMAX_DELAY);
        save_cache();
        xSemaphoreGive(data_lock);
        ESP_LOGI(TAG, "Refresh task minimum remaining stack: %u bytes",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        last_refresh = xTaskGetTickCount();
        refresh_pending = false;
    }
}

esp_err_t data_init(void)
{
    data_lock = xSemaphoreCreateMutex();
    refresh_events = xEventGroupCreate();
    if (!data_lock || !refresh_events) return ESP_ERR_NO_MEM;
    esp_err_t err = nvs_flash_init_partition(CACHE_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Resetting incompatible content cache partition");
        ESP_RETURN_ON_ERROR(nvs_flash_erase_partition(CACHE_PARTITION),
                            TAG, "erase content cache partition");
        err = nvs_flash_init_partition(CACHE_PARTITION);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not initialize content cache partition: %s",
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Content cache partition ready; cache record size is %u bytes",
             (unsigned)sizeof(current_data));
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
