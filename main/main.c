#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "data.h"
#include "display.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "touch.h"
#include "web_config.h"

#define COLOR_BACKGROUND 0x0841
#define COLOR_PANEL 0x10A3
#define COLOR_MUTED 0xA514
#define COLOR_ORANGE 0xFD20
#define COLOR_GREEN 0x07E0
#define COLOR_WHITE 0xFFFF

typedef enum {
    PAGE_CURRENT,
    PAGE_CALENDAR,
    PAGE_PHOTOS,
    PAGE_WEATHER,
    PAGE_INFO,
    PAGE_COUNT
} page_t;

static page_t active_page;
static uint8_t active_subpage[PAGE_COUNT];
static const uint8_t subpage_counts[PAGE_COUNT] = {3, 3, 2, 3, 3};
static bool setup_credentials_visible;
static TickType_t setup_visible_until;
static TickType_t touch_started_at;

static void draw_icon(int index, int center_x, int center_y, uint16_t color)
{
    switch (index) {
    case PAGE_CURRENT:
        display_circle(center_x, center_y, 4, color);
        display_line(center_x, center_y - 10, center_x, center_y - 7, color);
        display_line(center_x, center_y + 7, center_x, center_y + 10, color);
        display_line(center_x - 10, center_y, center_x - 7, center_y, color);
        display_line(center_x + 7, center_y, center_x + 10, center_y, color);
        display_line(center_x - 7, center_y - 7, center_x - 5, center_y - 5, color);
        display_line(center_x + 5, center_y + 5, center_x + 7, center_y + 7, color);
        display_line(center_x + 5, center_y - 5, center_x + 7, center_y - 7, color);
        display_line(center_x - 7, center_y + 7, center_x - 5, center_y + 5, color);
        break;
    case PAGE_CALENDAR:
        display_line(center_x - 9, center_y - 7, center_x + 9, center_y - 7, color);
        display_line(center_x - 9, center_y - 7, center_x - 9, center_y + 8, color);
        display_line(center_x + 9, center_y - 7, center_x + 9, center_y + 8, color);
        display_line(center_x - 9, center_y + 8, center_x + 9, center_y + 8, color);
        display_line(center_x - 9, center_y - 2, center_x + 9, center_y - 2, color);
        display_line(center_x - 4, center_y - 10, center_x - 4, center_y - 5, color);
        display_line(center_x + 4, center_y - 10, center_x + 4, center_y - 5, color);
        display_fill_rect(center_x - 5, center_y + 1, 2, 2, color);
        display_fill_rect(center_x + 1, center_y + 1, 2, 2, color);
        break;
    case PAGE_PHOTOS:
        display_line(center_x - 10, center_y - 7, center_x + 10, center_y - 7, color);
        display_line(center_x - 10, center_y + 7, center_x + 10, center_y + 7, color);
        display_line(center_x - 10, center_y - 7, center_x - 10, center_y + 7, color);
        display_line(center_x + 10, center_y - 7, center_x + 10, center_y + 7, color);
        display_fill_circle(center_x + 5, center_y - 3, 2, color);
        display_line(center_x - 8, center_y + 5, center_x - 2, center_y - 1, color);
        display_line(center_x - 2, center_y - 1, center_x + 2, center_y + 3, color);
        display_line(center_x + 2, center_y + 3, center_x + 5, center_y + 1, color);
        break;
    case PAGE_WEATHER:
        display_circle(center_x - 4, center_y - 3, 4, color);
        display_line(center_x - 4, center_y - 10, center_x - 4, center_y - 8, color);
        display_line(center_x - 11, center_y - 3, center_x - 9, center_y - 3, color);
        display_line(center_x + 1, center_y - 3, center_x + 3, center_y - 3, color);
        display_circle(center_x + 3, center_y + 1, 5, color);
        display_circle(center_x - 3, center_y + 3, 5, color);
        display_line(center_x - 8, center_y + 7, center_x + 9, center_y + 7, color);
        break;
    case PAGE_INFO:
        display_circle(center_x, center_y, 9, color);
        display_fill_rect(center_x - 1, center_y - 1, 2, 7, color);
        display_fill_circle(center_x, center_y - 5, 1, color);
        break;
    default:
        break;
    }
}

static void draw_tabs(void)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        int x = i * 64;
        uint16_t color = i == active_page ? COLOR_ORANGE : COLOR_PANEL;
        display_fill_rect(x + 2, 2, 60, 29, color);
        draw_icon(i, x + 32, 16, COLOR_WHITE);
    }
}

static void draw_side_buttons(void)
{
    display_fill_rect(2, 34, 24, 188, COLOR_PANEL);
    display_fill_rect(294, 34, 24, 188, COLOR_PANEL);
    display_text_bg(8, 119, "<", COLOR_ORANGE, COLOR_PANEL, 2);
    display_text_bg(300, 119, ">", COLOR_ORANGE, COLOR_PANEL, 2);
}

static void draw_subpage_indicator(void)
{
    char indicator[12];
    snprintf(indicator, sizeof(indicator), "%u/%u",
             active_subpage[active_page] + 1, subpage_counts[active_page]);
    display_text_bg(151, 210, indicator, COLOR_ORANGE, COLOR_BACKGROUND, 1);
}

static void draw_status(void)
{
    display_fill_rect(0, 224, DISPLAY_WIDTH, 16, COLOR_PANEL);
    char time_text[8] = "--:--";
    char date_text[11] = "--.--.----";
    time_t now = time(NULL);
    struct tm local_time;
    if (now >= 1704067200 && localtime_r(&now, &local_time)) {
        strftime(time_text, sizeof(time_text), "%H:%M", &local_time);
        strftime(date_text, sizeof(date_text), "%d.%m.%Y", &local_time);
    }
    display_text_bg(4, 228, time_text, COLOR_ORANGE, COLOR_PANEL, 1);
    display_text_bg(36, 228, date_text, COLOR_WHITE, COLOR_PANEL, 1);
    display_text_bg(106, 228, config_wifi_connected() ? "WIFI" : "OFF",
                    config_wifi_connected() ? COLOR_GREEN : COLOR_ORANGE, COLOR_PANEL, 1);
    display_text_bg(148, 228, "4X TAEGLICH", COLOR_WHITE, COLOR_PANEL, 1);
    if (setup_credentials_visible && xTaskGetTickCount() < setup_visible_until) {
        display_fill_rect(0, 204, DISPLAY_WIDTH, 20, COLOR_ORANGE);
        display_text_bg(4, 205, config_ap_ssid(), COLOR_BACKGROUND, COLOR_ORANGE, 1);
        display_text_bg(4, 214, "HTTP://192.168.4.1 OHNE LOGIN",
                        COLOR_BACKGROUND, COLOR_ORANGE, 1);
    } else {
        setup_credentials_visible = false;
        display_text_bg(272, 228, config_setup_ap_active() ? "AP OPEN" : "SETUP",
                        COLOR_ORANGE, COLOR_PANEL, 1);
    }
}

static void draw_wrapped_text(const char *text, int x, int y, int max_lines,
                              uint16_t color, size_t page_offset)
{
    if (!text || !text[0]) {
        display_text(x, y, "KEINE DATEN", COLOR_MUTED, 1);
        return;
    }
    int line = 0;
    int column = 0;
    char buffer[52];
    const char *cursor = text + (page_offset < strlen(text) ? page_offset : strlen(text));
    for (; *cursor && line < max_lines; cursor++) {
        char ch = *cursor;
        if (ch == ' ' && column == 0) continue;
        if (column >= 40 || ch == '\n') {
            buffer[column] = '\0';
            display_text(x, y + line * 12, buffer, color, 1);
            line++;
            column = 0;
            if (ch == '\n' || ch == ' ') continue;
        }
        buffer[column++] = ch;
    }
    if (column && line < max_lines) {
        buffer[column] = '\0';
        display_text(x, y + line * 12, buffer, color, 1);
    }
}

static const char *weather_description(int code)
{
    if (code == 0) return "SONNIG";
    if (code == 1 || code == 2) return "LICHTE WOLKEN";
    if (code == 3) return "BEWOELKT";
    if (code == 45 || code == 48) return "NEBEL";
    if (code >= 51 && code <= 67) return "REGEN";
    if (code >= 71 && code <= 77) return "SCHNEE";
    if (code >= 80 && code <= 82) return "REGENSCHAUER";
    if (code >= 95) return "GEWITTER";
    return "WETTER";
}

static void draw_weather(const app_data_t *data, uint8_t subpage)
{
    if (subpage == 2) {
        display_text(34, 48, "HOCH- UND NIEDRIGWASSER", COLOR_ORANGE, 1);
        display_text(34, 65, "MODELLWERTE - NUR ORIENTIERUNG", COLOR_MUTED, 1);
        if (!data->tide_valid) {
            draw_wrapped_text("Gezeitendaten werden geladen oder sind nicht erreichbar.",
                              34, 90, 5, COLOR_WHITE, 0);
            return;
        }
        display_text(34, 92, data->next_tide, COLOR_GREEN, 1);
        float minimum = data->tide_samples[0];
        float maximum = minimum;
        for (int i = 1; i < TIDE_SAMPLES; i++) {
            if (!isfinite(data->tide_samples[i])) continue;
            if (data->tide_samples[i] < minimum) minimum = data->tide_samples[i];
            if (data->tide_samples[i] > maximum) maximum = data->tide_samples[i];
        }
        float span = maximum - minimum;
        display_line(38, 165, 282, 165, COLOR_MUTED);
        for (int i = 0; i < TIDE_SAMPLES; i++) {
            if (!isfinite(data->tide_samples[i])) continue;
            int height = span > 0.01f
                ? 10 + (int)(38.0f * (data->tide_samples[i] - minimum) / span)
                : 28;
            display_fill_rect(42 + i * 19, 165 - height, 10, height,
                              i == 0 ? COLOR_ORANGE : COLOR_GREEN);
        }
        display_text(34, 181, "OPEN-METEO MARINE - KUSTENMODELL", COLOR_MUTED, 1);
        return;
    }

    display_text(34, 48, subpage == 0 ? "AKTUELLES WETTER" : "WETTERVORHERSAGE",
                 COLOR_ORANGE, 1);
    display_text(220, 48, data->updated_at[11] ? data->updated_at + 11 : "--:--", COLOR_MUTED, 1);
    display_text(34, 60, "WEATHER BY OPEN-METEO.COM", COLOR_MUTED, 1);
    if (!data->weather_valid) {
        draw_wrapped_text("Wetterdaten werden geladen oder sind nicht erreichbar.",
                          34, 82, 5, COLOR_WHITE, 0);
        return;
    }
    char line[52];
    if (subpage == 0) {
        snprintf(line, sizeof(line), "%.1f C GEFUEHLT %.1f C",
                 data->temperature_c, data->feels_like_c);
        display_text(34, 82, line, COLOR_WHITE, 1);
        snprintf(line, sizeof(line), "%s", weather_description(data->weather_code));
        display_text(34, 105, line, COLOR_ORANGE, 1);
        snprintf(line, sizeof(line), "WIND %.0f KM/H BOEEN %.0f",
                 data->wind_kmh, data->gust_kmh);
        display_text(34, 123, line, COLOR_WHITE, 1);
        snprintf(line, sizeof(line), "LUFT %d%% REGEN %.1fMM",
                 data->humidity_percent, data->precipitation_mm);
        display_text(34, 141, line, COLOR_WHITE, 1);
    } else {
        display_text(34, 79, "DIE NAECHSTEN TAGE", COLOR_WHITE, 1);
        for (int i = 0; i < DATA_DAYS; i++) {
            if (!data->days[i].date[0]) continue;
            snprintf(line, sizeof(line), "%.5s %.0f/%.0fC REGEN %d%%",
                     data->days[i].date + 5,
                     data->days[i].high_c, data->days[i].low_c,
                     data->days[i].rain_probability);
            display_text(34, 101 + i * 22, line, i == 0 ? COLOR_ORANGE : COLOR_WHITE, 1);
        }
    }
}

static void draw_page(const app_data_t *data)
{
    display_draw_frame();
    draw_tabs();
    switch (active_page) {
    case PAGE_CURRENT:
        if (active_subpage[active_page] == 0) {
            display_text(34, 48, "TERMINE HEUTE", COLOR_ORANGE, 1);
            display_text(34, 73, "VVV-QUELLE NOCH FESTZULEGEN", COLOR_WHITE, 1);
            display_text(34, 95, "VERANSTALTUNGEN AUF TEXEL", COLOR_MUTED, 1);
        } else if (active_subpage[active_page] == 1) {
            display_text(34, 48, "TERMINE MORGEN", COLOR_ORANGE, 1);
            display_text(34, 73, "VVV-QUELLE NOCH FESTZULEGEN", COLOR_WHITE, 1);
            display_text(34, 95, "VERANSTALTUNGEN AUF TEXEL", COLOR_MUTED, 1);
        } else {
            display_text(34, 48, "ORTE & SEHENSWUERDIGKEITEN", COLOR_ORANGE, 1);
            display_text(34, 73, "TIPPS UND AUSFLUGSZIELE", COLOR_WHITE, 1);
            display_text(34, 95, "VVV-INHALTE NOCH NICHT GELADEN", COLOR_MUTED, 1);
        }
        display_text(34, 171, "AKTUALISIERUNG AUTOMATISCH", COLOR_MUTED, 1);
        break;
    case PAGE_CALENDAR:
        {
            char title[32];
            snprintf(title, sizeof(title), "TEXELKALENDER - TEIL %u",
                     active_subpage[active_page] + 1);
            display_text(34, 48, title, COLOR_ORANGE, 1);
        }
        display_text(34, 64, data->calendar_valid ? "LETZTER ABRUF:" : "WARTET AUF ABRUF",
                     COLOR_MUTED, 1);
        draw_wrapped_text(data->calendar, 34, 82, 10, COLOR_WHITE,
                          (size_t)active_subpage[active_page] * 400);
        break;
    case PAGE_PHOTOS:
        display_text(34, 48, active_subpage[active_page] == 0
                     ? "FOTOS VON TEXEL - 1" : "FOTOS VON TEXEL - 2",
                     COLOR_ORANGE, 1);
        display_fill_rect(32, 68, 256, 137, COLOR_PANEL);
        display_text(46, 86, active_subpage[active_page] == 0
                     ? "ZUFALLSFOTO 1" : "ZUFALLSFOTO 2", COLOR_WHITE, 1);
        display_text(46, 105, "NOCH NICHT BEREIT", COLOR_ORANGE, 1);
        display_text(46, 128, "JPEG 320 X 200 PX", COLOR_MUTED, 1);
        display_text(46, 150, "MANIFEST URL FESTLEGEN", COLOR_MUTED, 1);
        break;
    case PAGE_WEATHER:
        draw_weather(data, active_subpage[active_page]);
        break;
    case PAGE_INFO:
        {
            char title[32];
            snprintf(title, sizeof(title), "HAUS-INFOS - TEIL %u",
                     active_subpage[active_page] + 1);
            display_text(34, 48, title, COLOR_ORANGE, 1);
        }
        draw_wrapped_text(data->home_info, 34, 70, 11, COLOR_WHITE,
                          (size_t)active_subpage[active_page] * 440);
        break;
    default:
        break;
    }
    draw_side_buttons();
    draw_subpage_indicator();
    draw_status();
}

static void app_main_task(void *unused)
{
    (void)unused;
    app_data_t snapshot = {0};
    app_data_t previous = {0};
    bool was_touched = false;
    bool refresh_sent = false;
    page_t previous_page = PAGE_COUNT;
    uint8_t previous_subpage = UINT8_MAX;
    bool previous_setup_visibility = false;
    int previous_clock_minute = -1;
    bool previous_clock_valid = false;
    bool ntp_sync_logged = false;
    while (true) {
        int x = 0;
        int y = 0;
        bool touched = touch_read(&x, &y);
        TickType_t now = xTaskGetTickCount();
        if (touched && !was_touched) {
            touch_started_at = now;
            refresh_sent = false;
        }
        if (touched && !refresh_sent && now - touch_started_at > pdMS_TO_TICKS(1600)) {
            if (y < 40) {
                data_request_refresh();
                refresh_sent = true;
            }
        }
        if (touched && !was_touched && y < 38) {
            int selected = x / 64;
            if (selected >= 0 && selected < PAGE_COUNT) {
                active_page = (page_t)selected;
                active_subpage[active_page] = 0;
            }
        }
        if (touched && !was_touched && y >= 34 && y <= 222) {
            if (x <= 28) {
                active_subpage[active_page] =
                    (active_subpage[active_page] + subpage_counts[active_page] - 1) %
                    subpage_counts[active_page];
            } else if (x >= 292) {
                active_subpage[active_page] =
                    (active_subpage[active_page] + 1) % subpage_counts[active_page];
            }
        }
        if (touched && !was_touched && y > 218 && x > 260) {
            esp_err_t err = config_start_setup_ap();
            if (err == ESP_OK) {
                setup_credentials_visible = true;
                setup_visible_until = now + pdMS_TO_TICKS(30000);
            } else {
                ESP_LOGE("ui", "Could not start setup AP: %s", esp_err_to_name(err));
            }
        }
        was_touched = touched;

        if (!ntp_sync_logged && config_ntp_synced()) {
            time_t synced_at = time(NULL);
            struct tm synced_local_time;
            char synced_text[24] = "unknown";
            if (localtime_r(&synced_at, &synced_local_time)) {
                strftime(synced_text, sizeof(synced_text), "%Y-%m-%d %H:%M:%S",
                         &synced_local_time);
            }
            ESP_LOGI("clock", "NTP synchronized local time: %s", synced_text);
            ntp_sync_logged = true;
        }

        data_snapshot(&snapshot);
        bool show_setup = setup_credentials_visible && now < setup_visible_until;
        time_t wall_clock = time(NULL);
        struct tm local_time;
        bool clock_valid = wall_clock >= 1704067200 &&
                           localtime_r(&wall_clock, &local_time) != NULL;
        int clock_minute = clock_valid
            ? local_time.tm_yday * 1440 + local_time.tm_hour * 60 + local_time.tm_min
            : -1;
        bool clock_changed = clock_valid != previous_clock_valid ||
                             clock_minute != previous_clock_minute;
        bool page_changed = active_page != previous_page ||
                            active_subpage[active_page] != previous_subpage ||
                            show_setup != previous_setup_visibility ||
                            memcmp(&snapshot, &previous, sizeof(snapshot)) != 0;
        if (page_changed) {
            draw_page(&snapshot);
            if (snapshot.from_cache) {
                display_text_bg(222, 228, "CACHED", COLOR_ORANGE, COLOR_PANEL, 1);
            }
            previous = snapshot;
            previous_page = active_page;
            previous_subpage = active_subpage[active_page];
            previous_setup_visibility = show_setup;
        } else if (clock_changed) {
            draw_status();
        }
        previous_clock_minute = clock_minute;
        previous_clock_valid = clock_valid;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(touch_init());
    ESP_ERROR_CHECK(config_init());
    ESP_ERROR_CHECK(web_config_start());
    ESP_ERROR_CHECK(data_init());
    data_request_refresh();
    ESP_ERROR_CHECK(xTaskCreate(app_main_task, "display", 8192, NULL, 5, NULL) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
}
