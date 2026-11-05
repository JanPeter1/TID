#include "data.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define RESPONSE_LIMIT 16384
#define EVENT_RESPONSE_LIMIT (16U * 1024U)
#define PHOTO_MANIFEST_LIMIT 8192
#define REFRESH_INTERVAL_MS (6U * 60U * 60U * 1000U)
#define PHOTO_MANIFEST_URL "https://familie-riegel.de/tx94/photos/manifest.json"
#define PHOTO_BASE_URL "https://familie-riegel.de/tx94/photos/"
#define CALENDAR_TERMS_URL \
    "https://familie-riegel.de/tx94/content/plugins/occcal/Texelkalender_termine.txt"
#define CALENDAR_GROUPS_URL \
    "https://familie-riegel.de/tx94/content/plugins/occcal/Texelkalender_gruppen.txt"
#define PHOTO_BIT BIT1
#define CACHE_PARTITION "content_cache"
#define CACHE_NAMESPACE "content"
#define CACHE_KEY "last_good"

static const char *TAG = "data";
static app_data_t current_data;
static SemaphoreHandle_t data_lock;
static EventGroupHandle_t refresh_events;
static uint8_t *photo_jpeg_buffer;
static size_t photo_jpeg_size;
static size_t photo_jpeg_capacity;
static uint32_t photo_request_serial;
static bool photo_jpeg_in_use;
static bool photo_release_pending;
static bool photo_requested;
static char last_photo_path[DATA_PHOTO_PATH_LENGTH];
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
    size_t internal_heap_before =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest_before =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    bool success = err == ESP_OK && status >= 200 && status < 300 &&
                   !response.overflow;
    if (response.overflow) {
        ESP_LOGW(TAG, "Response truncated at %u bytes: %s (HTTP %d)",
                 (unsigned)response.length, url, status);
    }
    if (!success && !response.overflow) {
        ESP_LOGW(TAG, "GET failed: %s, HTTP %d, URL %s",
                 esp_err_to_name(err), status, url);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Internal heap before HTTPS request: %u bytes free, "
                     "%u-byte largest block",
                     (unsigned)internal_heap_before,
                     (unsigned)internal_largest_before);
        }
    }
    esp_http_client_cleanup(client);
    return success;
}

static void finish_photo_buffer_use(void)
{
    xSemaphoreTake(data_lock, portMAX_DELAY);
    photo_jpeg_in_use = false;
    if (photo_release_pending) {
        free(photo_jpeg_buffer);
        photo_jpeg_buffer = NULL;
        photo_jpeg_capacity = 0;
        photo_jpeg_size = 0;
        photo_release_pending = false;
    }
    xSemaphoreGive(data_lock);
}

static bool http_get_photo_file(const char *url, uint32_t expected_size,
                                uint32_t request_serial, size_t *written)
{
    if (expected_size < 4 || expected_size > DATA_PHOTO_MAX_BYTES) return false;
    xSemaphoreTake(data_lock, portMAX_DELAY);
    if (request_serial != photo_request_serial) {
        xSemaphoreGive(data_lock);
        return false;
    }
    photo_jpeg_in_use = true;
    xSemaphoreGive(data_lock);

    size_t required_capacity = (size_t)expected_size + 1;
    if (photo_jpeg_capacity != required_capacity) {
        uint8_t *resized = realloc(photo_jpeg_buffer, required_capacity);
        if (!resized) {
            ESP_LOGE(TAG, "Could not allocate %u-byte photo buffer",
                     (unsigned)required_capacity);
            finish_photo_buffer_use();
            return false;
        }
        photo_jpeg_buffer = resized;
        photo_jpeg_capacity = required_capacity;
    }
    response_buffer_t response = {
        .data = (char *)photo_jpeg_buffer,
        .capacity = required_capacity,
    };
    photo_jpeg_buffer[0] = 0;
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = response_event,
        .user_data = &response,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        finish_photo_buffer_use();
        return false;
    }
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t internal_free_before = heap_caps_get_free_size(internal_caps);
    size_t internal_largest_before = heap_caps_get_largest_free_block(internal_caps);
    esp_err_t monitor_err = heap_caps_monitor_local_minimum_free_size_start();
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    size_t internal_minimum_during = monitor_err == ESP_OK
        ? heap_caps_get_minimum_free_size(internal_caps) : 0;
    size_t internal_free_during = heap_caps_get_free_size(internal_caps);
    size_t internal_largest_during = heap_caps_get_largest_free_block(internal_caps);
    if (monitor_err == ESP_OK) {
        esp_err_t stop_err = heap_caps_monitor_local_minimum_free_size_stop();
        if (stop_err != ESP_OK) {
            ESP_LOGW(TAG, "Could not stop local heap monitoring: %s",
                     esp_err_to_name(stop_err));
        }
    } else {
        ESP_LOGW(TAG, "Could not start local heap monitoring: %s",
                 esp_err_to_name(monitor_err));
    }
    bool success = err == ESP_OK && status >= 200 && status < 300 &&
                   !response.overflow && response.length == expected_size &&
                   photo_jpeg_buffer[0] == 0xFF && photo_jpeg_buffer[1] == 0xD8;
    if (!success) {
        ESP_LOGW(TAG, "Photo download failed: %s, HTTP %d, bytes %u, overflow %d",
                 esp_err_to_name(err), status, (unsigned)response.length,
                 response.overflow);
    }
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "Photo TLS heap: JPEG buffer %u bytes, before %u free/%u largest, "
             "during %u free/%u largest, minimum %u, after cleanup %u free/%u largest",
             (unsigned)required_capacity,
             (unsigned)internal_free_before, (unsigned)internal_largest_before,
             (unsigned)internal_free_during, (unsigned)internal_largest_during,
             (unsigned)internal_minimum_during,
             (unsigned)heap_caps_get_free_size(internal_caps),
             (unsigned)heap_caps_get_largest_free_block(internal_caps));
    if (!success) {
        finish_photo_buffer_use();
        return false;
    }
    *written = response.length;
    finish_photo_buffer_use();
    return true;
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
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
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

static bool photo_entry_fields(cJSON *entry, const char **path,
                               const char **date, const char **title,
                               uint32_t *photo_size)
{
    *path = json_string(entry, "path");
    *date = json_string(entry, "date");
    *title = json_string(entry, "title");
    cJSON *size = object_item(entry, "size");
    if (!*path || !*date || !*title || !(*title)[0] ||
        strlen(*path) >= DATA_PHOTO_PATH_LENGTH || strlen(*date) != 10 ||
        (*date)[4] != '-' || (*date)[7] != '-' || strstr(*path, "..") ||
        !cJSON_IsNumber(size) || size->valuedouble < 4 ||
        size->valuedouble > DATA_PHOTO_MAX_BYTES ||
        size->valuedouble != (double)size->valueint) {
        return false;
    }
    *photo_size = (uint32_t)size->valueint;
    size_t path_length = strlen(*path);
    if (path_length < 5 || strcmp(*path + path_length - 4, ".jpg") != 0 ||
        (*path)[0] == '.') return false;
    for (const unsigned char *character = (const unsigned char *)*path;
         *character; character++) {
        if (!((*character >= 'A' && *character <= 'Z') ||
              (*character >= 'a' && *character <= 'z') ||
              (*character >= '0' && *character <= '9') ||
              *character == '-' || *character == '_' || *character == '.')) {
            return false;
        }
    }
    return true;
}

static void append_utf8(char **destination, uint32_t codepoint)
{
    if (codepoint <= 0x7F) {
        *(*destination)++ = (char)codepoint;
    } else if (codepoint <= 0x7FF) {
        *(*destination)++ = (char)(0xC0 | (codepoint >> 6));
        *(*destination)++ = (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0xFFFF &&
               !(codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        *(*destination)++ = (char)(0xE0 | (codepoint >> 12));
        *(*destination)++ = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        *(*destination)++ = (char)(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0x10FFFF) {
        *(*destination)++ = (char)(0xF0 | (codepoint >> 18));
        *(*destination)++ = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        *(*destination)++ = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        *(*destination)++ = (char)(0x80 | (codepoint & 0x3F));
    } else {
        *(*destination)++ = '?';
    }
}

static bool decode_html_entity(const char *text, uint32_t *codepoint,
                              size_t *consumed)
{
    static const struct {
        const char *entity;
        uint32_t codepoint;
    } entities[] = {
        {"&amp;", '&'}, {"&apos;", '\''}, {"&gt;", '>'}, {"&lt;", '<'},
        {"&quot;", '"'}, {"&nbsp;", ' '}, {"&auml;", 0x00E4},
        {"&Auml;", 0x00C4}, {"&ouml;", 0x00F6}, {"&Ouml;", 0x00D6},
        {"&uuml;", 0x00FC}, {"&Uuml;", 0x00DC}, {"&szlig;", 0x00DF},
        {"&eacute;", 0x00E9},
        {"&euro;", 0x20AC}, {"&ndash;", 0x2013}, {"&mdash;", 0x2014},
        {"&hellip;", 0x2026}, {"&rsquo;", 0x2019}, {"&lsquo;", 0x2018},
        {"&rdquo;", 0x201D}, {"&ldquo;", 0x201C},
    };

    if (text[0] != '&') return false;
    for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); i++) {
        size_t length = strlen(entities[i].entity);
        if (strncmp(text, entities[i].entity, length) == 0) {
            *codepoint = entities[i].codepoint;
            *consumed = length;
            return true;
        }
    }

    if (text[1] != '#') return false;
    size_t index = 2;
    unsigned int base = 10;
    if (text[index] == 'x' || text[index] == 'X') {
        base = 16;
        index++;
    }
    uint32_t value = 0;
    size_t digits = 0;
    while (text[index] && text[index] != ';') {
        unsigned char ch = (unsigned char)text[index];
        unsigned int digit;
        if (ch >= '0' && ch <= '9') digit = ch - '0';
        else if (base == 16 && ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
        else if (base == 16 && ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
        else return false;
        if (digit >= base || value > (0x10FFFFU - digit) / base) return false;
        value = value * base + digit;
        digits++;
        index++;
    }
    if (!digits || text[index] != ';' || value == 0 ||
        (value >= 0xD800 && value <= 0xDFFF)) {
        return false;
    }
    *codepoint = value;
    *consumed = index + 1;
    return true;
}

static size_t valid_utf8_sequence_length(const unsigned char *text)
{
    unsigned char first = text[0];
    if (first < 0x80) return 1;

    size_t length;
    uint32_t codepoint;
    if (first >= 0xC2 && first <= 0xDF) {
        length = 2;
        codepoint = first & 0x1F;
    } else if (first >= 0xE0 && first <= 0xEF) {
        length = 3;
        codepoint = first & 0x0F;
    } else if (first >= 0xF0 && first <= 0xF4) {
        length = 4;
        codepoint = first & 0x07;
    } else {
        return 0;
    }

    for (size_t i = 1; i < length; i++) {
        if (!text[i] || (text[i] & 0xC0) != 0x80) return 0;
        codepoint = (codepoint << 6) | (text[i] & 0x3F);
    }
    if ((length == 3 && codepoint < 0x800) ||
        (length == 4 && codepoint < 0x10000) ||
        codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        return 0;
    }
    return length;
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
    size_t source_length = strlen(source);
    size_t copy_length = source_length < size - 1 ? source_length : size - 1;
    if (copy_length < source_length) {
        while (copy_length &&
               ((unsigned char)source[copy_length] & 0xC0) == 0x80) {
            copy_length--;
        }
    }
    memcpy(destination, source, copy_length);
    destination[copy_length] = '\0';
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

    texel_event_t *events = calloc(DATA_EVENT_COUNT, sizeof(*events));
    if (!events) {
        ESP_LOGE(TAG, "Could not allocate VVV event array");
        cJSON_Delete(root);
        return false;
    }
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

    for (uint8_t i = 1; i < event_count; i++) {
        texel_event_t event = events[i];
        uint8_t insert_at = i;
        while (insert_at > 0) {
            texel_event_t *previous = &events[insert_at - 1];
            int date_order = strcmp(previous->date, event.date);
            if (date_order < 0 ||
                (date_order == 0 && strcmp(previous->time, event.time) <= 0)) {
                break;
            }
            events[insert_at] = *previous;
            insert_at--;
        }
        events[insert_at] = event;
    }

    xSemaphoreTake(data_lock, portMAX_DELAY);
    memset(current_data.events, 0, sizeof(current_data.events));
    memcpy(current_data.events, events, event_count * sizeof(*events));
    current_data.event_count = event_count;
    current_data.events_valid = true;
    current_data.from_cache = false;
    xSemaphoreGive(data_lock);
    free(events);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Parsed %u VVV events for today and tomorrow", event_count);
    return true;
}

static void log_vvv_response(const char *response)
{
    size_t length = 0;
    while (length < EVENT_RESPONSE_LIMIT && response[length]) length++;
    ESP_LOGI(TAG, "VVV response (%u bytes):", (unsigned)length);

    for (size_t offset = 0; offset < length;) {
        size_t chunk_length = length - offset;
        if (chunk_length > 256) chunk_length = 256;
        while (chunk_length > 0 && offset + chunk_length < length &&
               ((unsigned char)response[offset + chunk_length] & 0xC0) == 0x80) {
            chunk_length--;
        }
        if (!chunk_length) chunk_length = length - offset;
        ESP_LOGI(TAG, "VVV response [%u]: %.*s",
                 (unsigned)offset, (int)chunk_length, response + offset);
        offset += chunk_length;
    }
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
    cJSON_AddNumberToObject(variables, "limit", DATA_EVENT_COUNT);
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
    bool response_ok = http_post_json("https://texel-gateway.travelbase.nl/graphql",
                                      body, response, EVENT_RESPONSE_LIMIT);
    log_vvv_response(response);
    if (response_ok) {
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
            uint32_t codepoint;
            size_t entity_length;
            if (*read == '&' &&
                decode_html_entity(read, &codepoint, &entity_length)) {
                append_utf8(&write, codepoint);
                read += entity_length - 1;
            } else {
                size_t sequence_length =
                    valid_utf8_sequence_length((const unsigned char *)read);
                if (!sequence_length) {
                    *write++ = '?';
                } else {
                    memmove(write, read, sequence_length);
                    write += sequence_length;
                    read += sequence_length - 1;
                }
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

static bool calendar_parse_uint(const char **cursor, char separator,
                                unsigned int *value)
{
    if (**cursor == '\0') return false;
    unsigned int parsed = 0;
    if (**cursor < '0' || **cursor > '9') return false;
    while (**cursor >= '0' && **cursor <= '9') {
        parsed = parsed * 10U + (unsigned int)(**cursor - '0');
        (*cursor)++;
    }
    if (**cursor != separator) return false;
    (*cursor)++;
    *value = parsed;
    return true;
}

static bool valid_calendar_date(unsigned int year, unsigned int month,
                               unsigned int day)
{
    static const uint8_t month_lengths[] =
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year < 2000 || year > UINT16_MAX || month < 1 || month > 12 || day < 1) {
        return false;
    }
    unsigned int max_day = month_lengths[month - 1];
    bool leap_year = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap_year) max_day++;
    return day <= max_day;
}

static bool parse_calendar_terms(const char *text, calendar_entry_t *entries,
                                 uint16_t *entry_count)
{
    *entry_count = 0;
    const char *cursor = text;
    while (*cursor) {
        while (*cursor == ';' || *cursor == '\r' || *cursor == '\n' ||
               *cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (!*cursor) break;

        unsigned int year, month, day, group_id;
        if (!calendar_parse_uint(&cursor, '-', &year) ||
            !calendar_parse_uint(&cursor, '-', &month) ||
            !calendar_parse_uint(&cursor, '=', &day)) {
            return false;
        }
        const char *group_start = cursor;
        if (*cursor < '0' || *cursor > '9') return false;
        group_id = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            group_id = group_id * 10U + (unsigned int)(*cursor - '0');
            cursor++;
        }
        if (cursor == group_start || !valid_calendar_date(year, month, day) ||
            group_id == 0 || group_id >= DATA_CALENDAR_GROUPS ||
            *entry_count >= DATA_CALENDAR_MAX_ENTRIES) {
            return false;
        }
        entries[*entry_count] = (calendar_entry_t) {
            .year = (uint16_t)year,
            .month = (uint8_t)month,
            .day = (uint8_t)day,
            .group_id = (uint8_t)group_id,
        };
        (*entry_count)++;
        if (*cursor && *cursor != ';' && *cursor != '\r' && *cursor != '\n' &&
            *cursor != ' ' && *cursor != '\t') {
            return false;
        }
    }
    return *entry_count > 0;
}

static bool parse_calendar_groups(
    const char *text,
    char names[DATA_CALENDAR_GROUPS][DATA_CALENDAR_GROUP_NAME_LENGTH])
{
    memset(names, 0, DATA_CALENDAR_GROUPS * DATA_CALENDAR_GROUP_NAME_LENGTH);
    const char *cursor = text;
    while (*cursor) {
        while (*cursor == ';' || *cursor == '\r' || *cursor == '\n' ||
               *cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (!*cursor) break;

        unsigned int group_id;
        if (!calendar_parse_uint(&cursor, '=', &group_id) ||
            group_id == 0 || group_id >= DATA_CALENDAR_GROUPS) {
            return false;
        }
        const char *name_start = cursor;
        while (*cursor && *cursor != ';' && *cursor != '\r' && *cursor != '\n') {
            cursor++;
        }
        size_t name_length = (size_t)(cursor - name_start);
        while (name_length && (name_start[name_length - 1] == ' ' ||
                               name_start[name_length - 1] == '\t')) {
            name_length--;
        }
        if (!name_length || name_length >= DATA_CALENDAR_GROUP_NAME_LENGTH) {
            return false;
        }
        memcpy(names[group_id], name_start, name_length);
    }
    return true;
}

static void fetch_calendar(void)
{
    char *response = calloc(1, RESPONSE_LIMIT);
    if (!response) {
        ESP_LOGE(TAG, "Could not allocate calendar response buffer");
        return;
    }

    calendar_entry_t entries[DATA_CALENDAR_MAX_ENTRIES] = {0};
    uint16_t entry_count = 0;
    char group_names[DATA_CALENDAR_GROUPS][DATA_CALENDAR_GROUP_NAME_LENGTH] = {0};
    bool success = http_get(CALENDAR_TERMS_URL, NULL, NULL, response, RESPONSE_LIMIT) &&
                   parse_calendar_terms(response, entries, &entry_count);
    if (!success) {
        ESP_LOGW(TAG, "Could not parse occupancy calendar terms");
        free(response);
        return;
    }

    memset(response, 0, RESPONSE_LIMIT);
    success = http_get(CALENDAR_GROUPS_URL, NULL, NULL, response, RESPONSE_LIMIT) &&
              parse_calendar_groups(response, group_names);
    free(response);
    if (!success) {
        ESP_LOGW(TAG, "Could not parse occupancy calendar groups");
        return;
    }
    for (uint16_t i = 0; i < entry_count; i++) {
        if (!group_names[entries[i].group_id][0]) {
            ESP_LOGW(TAG, "Occupancy calendar references unknown group %u",
                     (unsigned)entries[i].group_id);
            return;
        }
    }

    xSemaphoreTake(data_lock, portMAX_DELAY);
    memcpy(current_data.calendar_entries, entries, sizeof(entries));
    current_data.calendar_entry_count = entry_count;
    memcpy(current_data.calendar_group_names, group_names, sizeof(group_names));
    current_data.calendar_valid = true;
    current_data.from_cache = false;
    xSemaphoreGive(data_lock);
    ESP_LOGI(TAG, "Loaded %u occupancy calendar days", (unsigned)entry_count);
}

static void mark_photo_error(uint32_t request_serial)
{
    xSemaphoreTake(data_lock, portMAX_DELAY);
    if (request_serial == photo_request_serial) {
        current_data.photo_ready = false;
        current_data.photo_error = true;
        current_data.photo_generation++;
    }
    xSemaphoreGive(data_lock);
}

static void fetch_random_photo(uint32_t request_serial)
{
    char *manifest = calloc(1, PHOTO_MANIFEST_LIMIT);
    if (!manifest) {
        ESP_LOGE(TAG, "Could not allocate photo manifest buffer");
        mark_photo_error(request_serial);
        return;
    }
    if (!http_get(PHOTO_MANIFEST_URL, NULL, NULL, manifest,
                  PHOTO_MANIFEST_LIMIT)) {
        free(manifest);
        mark_photo_error(request_serial);
        return;
    }

    cJSON *root = cJSON_Parse(manifest);
    free(manifest);
    cJSON *images = object_item(root, "images");
    if (!cJSON_IsArray(images)) {
        ESP_LOGW(TAG, "Photo manifest has no images array");
        cJSON_Delete(root);
        mark_photo_error(request_serial);
        return;
    }

    size_t valid_count = 0;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, images) {
        const char *path;
        const char *date;
        const char *title;
        uint32_t photo_size;
        if (photo_entry_fields(entry, &path, &date, &title, &photo_size)) {
            valid_count++;
        }
    }
    if (!valid_count) {
        ESP_LOGW(TAG, "Photo manifest contains no usable images");
        cJSON_Delete(root);
        mark_photo_error(request_serial);
        return;
    }

    size_t selected_index = esp_random() % valid_count;
    char selected_path[DATA_PHOTO_PATH_LENGTH] = {0};
    char selected_date[DATA_PHOTO_DATE_LENGTH] = {0};
    char selected_title[DATA_PHOTO_TITLE_LENGTH] = {0};
    uint32_t selected_size = 0;
    for (size_t attempt = 0; attempt < 2; attempt++) {
        size_t valid_index = 0;
        cJSON_ArrayForEach(entry, images) {
            const char *path;
            const char *date;
            const char *title;
            uint32_t photo_size;
            if (!photo_entry_fields(entry, &path, &date, &title, &photo_size)) continue;
            if (valid_index++ != selected_index) continue;
            snprintf(selected_path, sizeof(selected_path), "%s", path);
            snprintf(selected_date, sizeof(selected_date), "%s", date);
            snprintf(selected_title, sizeof(selected_title), "%s", title);
            selected_size = photo_size;
            break;
        }
        if (valid_count == 1 || strcmp(selected_path, last_photo_path) != 0) break;
        selected_index = (selected_index + 1) % valid_count;
    }
    xSemaphoreTake(data_lock, portMAX_DELAY);
    if (request_serial == photo_request_serial) {
        current_data.photo_count = valid_count > UINT16_MAX
            ? UINT16_MAX : (uint16_t)valid_count;
        current_data.photo_number = selected_index >= UINT16_MAX
            ? UINT16_MAX : (uint16_t)(selected_index + 1);
        current_data.photo_generation++;
    }
    xSemaphoreGive(data_lock);
    cJSON_Delete(root);

    char url[sizeof(PHOTO_BASE_URL) + DATA_PHOTO_PATH_LENGTH];
    snprintf(url, sizeof(url), "%s%s", PHOTO_BASE_URL, selected_path);
    size_t downloaded_size = 0;
    if (!http_get_photo_file(url, selected_size, request_serial, &downloaded_size)) {
        mark_photo_error(request_serial);
        return;
    }

    xSemaphoreTake(data_lock, portMAX_DELAY);
    bool photo_ready = false;
    if (request_serial == photo_request_serial) {
        photo_jpeg_size = downloaded_size;
        current_data.photo_ready = true;
        current_data.photo_error = false;
        current_data.photo_generation++;
        snprintf(current_data.photo_title, sizeof(current_data.photo_title),
                 "%s", selected_title);
        snprintf(current_data.photo_date, sizeof(current_data.photo_date),
                 "%s", selected_date);
        snprintf(last_photo_path, sizeof(last_photo_path), "%s", selected_path);
        photo_ready = true;
    }
    xSemaphoreGive(data_lock);
    if (photo_ready) {
        const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        ESP_LOGI(TAG, "Loaded photo %s (%u bytes); retained-photo heap: %u free, "
                 "%u-byte largest block; another max-size JPEG needs %u bytes",
                 selected_path, (unsigned)downloaded_size,
                 (unsigned)heap_caps_get_free_size(internal_caps),
                 (unsigned)heap_caps_get_largest_free_block(internal_caps),
                 (unsigned)(DATA_PHOTO_MAX_BYTES + 1U));
    }
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
    free(response);
    fetch_events();
    fetch_calendar();
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
        if (current_data.calendar_entry_count > DATA_CALENDAR_MAX_ENTRIES) {
            current_data.calendar_entry_count = 0;
            current_data.calendar_valid = false;
            ESP_LOGW(TAG, "Ignoring invalid cached calendar entry count");
        }
        current_data.from_cache = true;
        current_data.photo_ready = false;
        current_data.photo_error = false;
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
    bool photo_pending = false;
    bool waiting_for_ip_logged = false;
    bool waiting_for_time_logged = false;
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(refresh_events, REFRESH_BIT | PHOTO_BIT,
                                                pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        TickType_t now = xTaskGetTickCount();
        if (bits & REFRESH_BIT) refresh_pending = true;
        if (bits & PHOTO_BIT) photo_pending = true;
        if (now - last_refresh >= pdMS_TO_TICKS(REFRESH_INTERVAL_MS)) {
            refresh_pending = true;
        }
        if (!refresh_pending && !photo_pending) continue;

        if (!config_wifi_connected()) {
            if (!waiting_for_ip_logged) {
                ESP_LOGI(TAG, "Waiting for Wi-Fi IP before fetching content");
                waiting_for_ip_logged = true;
            }
            continue;
        }

        waiting_for_ip_logged = false;
        if (!config_ntp_synced()) {
            if (!waiting_for_time_logged) {
                ESP_LOGI(TAG, "Waiting for NTP time synchronization before HTTPS requests");
                waiting_for_time_logged = true;
            }
            continue;
        }

        waiting_for_time_logged = false;
        if (refresh_pending) {
            fetch_all();
            xSemaphoreTake(data_lock, portMAX_DELAY);
            save_cache();
            xSemaphoreGive(data_lock);
            last_refresh = xTaskGetTickCount();
            refresh_pending = false;
        }
        if (photo_pending) {
            uint32_t request_serial;
            bool requested;
            xSemaphoreTake(data_lock, portMAX_DELAY);
            request_serial = photo_request_serial;
            requested = photo_requested;
            xSemaphoreGive(data_lock);
            photo_pending = false;
            if (requested) fetch_random_photo(request_serial);
        }
        ESP_LOGI(TAG, "Refresh task minimum remaining stack: %u bytes",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
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

void data_request_photo(void)
{
    if (!data_lock || !refresh_events) return;
    xSemaphoreTake(data_lock, portMAX_DELAY);
    photo_request_serial++;
    photo_requested = true;
    current_data.photo_ready = false;
    current_data.photo_error = false;
    current_data.photo_number = 0;
    current_data.photo_generation++;
    current_data.photo_title[0] = '\0';
    current_data.photo_date[0] = '\0';
    xSemaphoreGive(data_lock);
    xEventGroupSetBits(refresh_events, PHOTO_BIT);
}

void data_release_photo(void)
{
    if (!data_lock) return;
    xSemaphoreTake(data_lock, portMAX_DELAY);
    photo_request_serial++;
    photo_requested = false;
    current_data.photo_ready = false;
    current_data.photo_error = false;
    current_data.photo_generation++;
    current_data.photo_title[0] = '\0';
    current_data.photo_date[0] = '\0';
    if (photo_jpeg_in_use) {
        photo_release_pending = true;
    } else {
        free(photo_jpeg_buffer);
        photo_jpeg_buffer = NULL;
        photo_jpeg_capacity = 0;
        photo_jpeg_size = 0;
    }
    xSemaphoreGive(data_lock);
}

const uint8_t *data_photo_jpeg(size_t *size)
{
    if (!data_lock || !size) return NULL;
    xSemaphoreTake(data_lock, portMAX_DELAY);
    const uint8_t *jpeg = current_data.photo_ready ? photo_jpeg_buffer : NULL;
    *size = current_data.photo_ready ? photo_jpeg_size : 0;
    xSemaphoreGive(data_lock);
    return jpeg;
}
