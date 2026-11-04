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
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "info_pages.h"
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
static const uint8_t subpage_counts[PAGE_COUNT] = {1, 3, 2, 3, 0};
static bool setup_credentials_visible;
static bool qr_visible;
static bool sleep_pending;
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
    display_fill_round_rect(2, 96, 24, 64, COLOR_PANEL);
    display_fill_round_rect(294, 96, 24, 64, COLOR_PANEL);
    display_text_bg(7, 114, "<", COLOR_ORANGE, COLOR_PANEL, 2);
    display_text_bg(299, 114, ">", COLOR_ORANGE, COLOR_PANEL, 2);
}

static uint8_t page_subpage_count(page_t page, const app_data_t *data)
{
    if (page == PAGE_CURRENT) {
        uint8_t count = data->event_count;
        if (count > DATA_EVENT_COUNT) count = 0;
        return count ? count : 1;
    }
    if (page == PAGE_INFO) return (uint8_t)info_page_count;
    return subpage_counts[page];
}

static void draw_subpage_indicator(const app_data_t *data)
{
    char indicator[12];
    snprintf(indicator, sizeof(indicator), "%u/%u",
             active_subpage[active_page] + 1,
             page_subpage_count(active_page, data));
    display_text_bg(151, 225, indicator, COLOR_ORANGE, COLOR_PANEL, 1);
}

static void draw_wifi_icon(int center_x, int center_y, uint16_t color, int rssi)
{
    int active_bars = rssi >= -60 ? 4 : rssi >= -70 ? 3 : rssi >= -80 ? 2 : 1;
    for (int i = 0; i < 4; i++) {
        int height = 3 + i * 2;
        display_fill_rect(center_x + i * 3, center_y + 5 - height, 2, height,
                          config_wifi_connected() && i < active_bars
                              ? color : COLOR_MUTED);
    }
}

static void draw_qr_icon(int x, int y)
{
    display_fill_rect(x - 6, y - 6, 12, 12, COLOR_WHITE);
    display_fill_rect(x - 5, y - 5, 4, 4, COLOR_PANEL);
    display_fill_rect(x - 4, y - 4, 2, 2, COLOR_WHITE);
    display_fill_rect(x + 1, y - 5, 4, 4, COLOR_PANEL);
    display_fill_rect(x + 2, y - 4, 2, 2, COLOR_WHITE);
    display_fill_rect(x - 5, y + 1, 4, 4, COLOR_PANEL);
    display_fill_rect(x - 4, y + 2, 2, 2, COLOR_WHITE);
    display_fill_rect(x + 1, y + 1, 2, 2, COLOR_PANEL);
    display_fill_rect(x + 4, y + 4, 2, 2, COLOR_PANEL);
}

static void draw_close_icon(int x, int y)
{
    display_line(x - 4, y - 4, x + 4, y + 4, COLOR_WHITE);
    display_line(x - 4, y + 4, x + 4, y - 4, COLOR_WHITE);
}

static void draw_sleep_icon(int x, int y)
{
    display_fill_circle(x, y, 6, COLOR_WHITE);
    display_fill_circle(x + 3, y - 3, 6, COLOR_PANEL);
}

static void draw_setup_icon(int x, int y, uint16_t color)
{
    display_fill_circle(x, y, 5, color);
    display_fill_rect(x - 2, y - 8, 4, 3, color);
    display_fill_rect(x - 2, y + 5, 4, 3, color);
    display_fill_rect(x - 8, y - 2, 3, 4, color);
    display_fill_rect(x + 5, y - 2, 3, 4, color);
    display_fill_rect(x - 6, y - 6, 2, 2, color);
    display_fill_rect(x + 4, y - 6, 2, 2, color);
    display_fill_rect(x - 6, y + 4, 2, 2, color);
    display_fill_rect(x + 4, y + 4, 2, 2, color);
    display_fill_circle(x, y, 2, COLOR_PANEL);
}

static void draw_status(void)
{
    display_fill_rect(0, 224, DISPLAY_WIDTH, 16, COLOR_PANEL);
    char time_text[9] = "--:-- ";
    char date_text[11] = "--.--.----";
    time_t now = time(NULL);
    struct tm local_time;
    if (now >= 1704067200 && localtime_r(&now, &local_time)) {
        strftime(time_text, sizeof(time_text), "%H:%M", &local_time);
        strcat(time_text, " ");
        strftime(date_text, sizeof(date_text), "%d.%m.%Y", &local_time);
    }
    display_text_bg(4, 225, time_text, COLOR_ORANGE, COLOR_PANEL, 1);
    display_text_bg(48, 225, date_text, COLOR_WHITE, COLOR_PANEL, 1);
    int rssi = config_wifi_rssi();
    uint16_t wifi_color = !config_wifi_connected() ? 0xF800
        : rssi >= -60 ? COLOR_GREEN : rssi >= -75 ? 0xFFE0 : 0xF800;
    draw_wifi_icon(122, 232, wifi_color, rssi);
    if (setup_credentials_visible && xTaskGetTickCount() < setup_visible_until) {
        display_fill_rect(0, 194, DISPLAY_WIDTH, 30, COLOR_ORANGE);
        display_text_bg(4, 194, config_ap_ssid(), COLOR_BACKGROUND, COLOR_ORANGE, 1);
        display_text_bg(4, 209, "HTTP://192.168.4.1 OHNE LOGIN",
                        COLOR_BACKGROUND, COLOR_ORANGE, 1);
    } else {
        setup_credentials_visible = false;
    }
    if (qr_visible) {
        draw_close_icon(241, 232);
    } else {
        draw_qr_icon(241, 232);
    }
    draw_sleep_icon(272, 232);
    draw_setup_icon(303, 232,
                    config_setup_ap_active() ? COLOR_ORANGE : COLOR_WHITE);
}

static size_t utf8_sequence_length(const unsigned char *text)
{
    if (text[0] < 0x80) return 1;
    if ((text[0] & 0xE0) == 0xC0) return 2;
    if ((text[0] & 0xF0) == 0xE0) return 3;
    if ((text[0] & 0xF8) == 0xF0) return 4;
    return 1;
}

static void draw_wrapped_text(const char *text, int x, int y, int max_lines,
                              uint16_t color, size_t page_offset)
{
    if (!text || !text[0]) {
        display_text(x, y, "KEINE DATEN", COLOR_MUTED, 1);
        return;
    }
    int line = 0;
    int columns = 0;
    size_t bytes = 0;
    char buffer[140];
    const char *cursor = text;
    for (size_t skipped = 0; *cursor && skipped < page_offset; skipped++) {
        cursor += utf8_sequence_length((const unsigned char *)cursor);
    }
    while (*cursor && line < max_lines) {
        unsigned char lead = (unsigned char)*cursor;
        size_t character_bytes = utf8_sequence_length((const unsigned char *)cursor);
        if (lead == '\n' || columns >= 34) {
            buffer[bytes] = '\0';
            if (bytes) display_text(x, y + line * 14, buffer, color, 1);
            line++;
            columns = 0;
            bytes = 0;
            if (lead == '\n') {
                cursor++;
                continue;
            }
            if (line >= max_lines) break;
        }
        if (lead == ' ' && columns == 0) {
            cursor += character_bytes;
            continue;
        }
        memcpy(buffer + bytes, cursor, character_bytes);
        bytes += character_bytes;
        columns++;
        cursor += character_bytes;
    }
    if (bytes && line < max_lines) {
        buffer[bytes] = '\0';
        display_text(x, y + line * 14, buffer, color, 1);
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

static void draw_weather_icon(int code, int x, int y, int scale)
{
    const uint16_t sun = COLOR_ORANGE;
    const uint16_t cloud = 0xBFFF;
    const uint16_t rain = 0x07FF;
    bool sunny = code == 0 || code == 1 || code == 2;
    bool cloudy = code == 2 || code == 3 || code == 45 || code == 48 ||
                  (code >= 51 && code <= 82) || code >= 95;

    if (sunny) {
        int sx = x - 5 * scale;
        int sy = y - 4 * scale;
        display_circle(sx, sy, 5 * scale, sun);
        for (int angle = 0; angle < 8; angle++) {
            int dx[] = {0, 1, 1, 1, 0, -1, -1, -1};
            int dy[] = {-1, -1, 0, 1, 1, 1, 0, -1};
            display_line(sx + dx[angle] * 8 * scale,
                         sy + dy[angle] * 8 * scale,
                         sx + dx[angle] * 10 * scale,
                         sy + dy[angle] * 10 * scale, sun);
        }
    }
    if (cloudy) {
        int cx = sunny ? x + 3 * scale : x;
        int cy = y + (sunny ? 2 : 0) * scale;
        display_fill_circle(cx - 5 * scale, cy, 5 * scale, cloud);
        display_fill_circle(cx, cy - 4 * scale, 7 * scale, cloud);
        display_fill_circle(cx + 6 * scale, cy, 5 * scale, cloud);
        display_fill_rect(cx - 5 * scale, cy, 16 * scale, 5 * scale, cloud);
    }
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        for (int i = 0; i < 3; i++) {
            int dx = x - 6 * scale + i * 6 * scale;
            display_line(dx, y + 8 * scale, dx - 2 * scale,
                         y + 12 * scale, rain);
        }
    } else if (code >= 71 && code <= 77) {
        for (int i = 0; i < 3; i++) {
            int sx = x - 6 * scale + i * 6 * scale;
            int sy = y + 10 * scale;
            display_line(sx - 2 * scale, sy, sx + 2 * scale, sy, rain);
            display_line(sx, sy - 2 * scale, sx, sy + 2 * scale, rain);
        }
    } else if (code >= 95) {
        int sx = x;
        int sy = y + 7 * scale;
        display_line(sx + 2 * scale, sy, sx - 2 * scale, sy + 5 * scale, sun);
        display_line(sx - 2 * scale, sy + 5 * scale, sx + 1 * scale, sy + 5 * scale, sun);
        display_line(sx + 1 * scale, sy + 5 * scale, sx - 2 * scale, sy + 10 * scale, sun);
    } else if (code == 45 || code == 48) {
        for (int i = 0; i < 3; i++) {
            int fy = y + 7 * scale + i * 3 * scale;
            display_line(x - 9 * scale, fy, x + 9 * scale, fy, rain);
        }
    }
}

static void draw_weather(const app_data_t *data, uint8_t subpage)
{
    if (subpage == 2) {
        display_text(34, 40, "GEZEITEN AUF TEXEL", COLOR_ORANGE, 1);
        display_text(34, 55, "OPEN-METEO MARINE-MODELL", COLOR_MUTED, 1);
        if (!data->tide_valid) {
            draw_wrapped_text("Gezeitendaten werden geladen oder sind nicht erreichbar.",
                              34, 80, 5, COLOR_WHITE, 0);
            return;
        }
        display_text(34, 78, data->next_tide, COLOR_GREEN, 1);
        display_text(34, 104, "WASSERSTAND - NAECHSTE 12 STUNDEN",
                     COLOR_WHITE, 1);
        float minimum = INFINITY;
        float maximum = -INFINITY;
        int valid_samples = 0;
        for (int i = 0; i < TIDE_SAMPLES; i++) {
            if (!isfinite(data->tide_samples[i])) continue;
            if (data->tide_samples[i] < minimum) minimum = data->tide_samples[i];
            if (data->tide_samples[i] > maximum) maximum = data->tide_samples[i];
            valid_samples++;
        }
        float span = maximum - minimum;
        display_line(38, 170, 282, 170, COLOR_MUTED);
        for (int i = 0; i < TIDE_SAMPLES; i++) {
            if (!isfinite(data->tide_samples[i])) continue;
            int height = span > 0.01f
                ? 8 + (int)(40.0f * (data->tide_samples[i] - minimum) / span)
                : 28;
            display_fill_rect(42 + i * 19, 170 - height, 10, height,
                              i == 0 ? COLOR_ORANGE : COLOR_GREEN);
        }
        if (valid_samples) {
            char level_label[32];
            snprintf(level_label, sizeof(level_label), "MIN %.2fM     MAX %.2fM",
                     minimum, maximum);
            display_text(54, 180, level_label, COLOR_MUTED, 1);
        } else {
            display_text(54, 180, "KEINE VERLAUFSWERTE", COLOR_MUTED, 1);
        }
        display_text(34, 194, "MODELLWERTE - KEINE NAVIGATIONSDATEN",
                     COLOR_MUTED, 1);
        return;
    }

    display_text(34, 40, subpage == 0 ? "JETZT AUF TEXEL" : "VORSCHAU - NÄCHSTE 5 TAGE",
                 COLOR_ORANGE, 1);
    display_text(220, 40, data->updated_at[11] ? data->updated_at + 11 : "--:--",
                 COLOR_MUTED, 1);
    display_text(34, 54, "OPEN-METEO.COM", COLOR_MUTED, 1);
    if (!data->weather_valid) {
        draw_wrapped_text("Wetterdaten werden geladen oder sind nicht erreichbar.",
                          34, 76, 5, COLOR_WHITE, 0);
        return;
    }
    char line[52];
    if (subpage == 0) {
        draw_weather_icon(data->weather_code, 255, 103, 2);
        snprintf(line, sizeof(line), "%.1f C", data->temperature_c);
        display_text(34, 76, line, COLOR_WHITE, 2);
        snprintf(line, sizeof(line), "GEFÜHLT %.1f C", data->feels_like_c);
        display_text(34, 101, line, COLOR_MUTED, 1);
        display_text(34, 122, weather_description(data->weather_code), COLOR_ORANGE, 1);
        snprintf(line, sizeof(line), "WIND %.0f KM/H  BOEEN %.0f",
                 data->wind_kmh, data->gust_kmh);
        display_text(34, 143, line, COLOR_WHITE, 1);
        snprintf(line, sizeof(line), "REGEN %.1f MM", data->precipitation_mm);
        display_text(34, 164, line, COLOR_GREEN, 1);
        snprintf(line, sizeof(line), "LUFTFEUCHTE %d%%", data->humidity_percent);
        display_text(34, 185, line, COLOR_WHITE, 1);
    } else {
        int displayed_days = 0;
        for (int i = 1; i < DATA_DAYS; i++) {
            if (!data->days[i].date[0]) continue;
            draw_weather_icon(data->days[i].weather_code, 47,
                              83 + displayed_days * 23, 1);
            snprintf(line, sizeof(line), "%.5s %-12s %.0f/%.0fC %d%%",
                     data->days[i].date + 5,
                     weather_description(data->days[i].weather_code),
                     data->days[i].high_c, data->days[i].low_c,
                     data->days[i].rain_probability);
            display_text(63, 75 + displayed_days * 23, line,
                         displayed_days == 0 ? COLOR_ORANGE : COLOR_WHITE, 1);
            displayed_days++;
        }
        if (displayed_days == 0) {
            display_text(34, 82, "VORHERSAGE NICHT VERFUEGBAR", COLOR_MUTED, 1);
        }
    }
}

static void current_page_url(const app_data_t *data, char *url, size_t url_size)
{
    if (active_page == PAGE_CURRENT && data->event_count &&
        active_subpage[active_page] < data->event_count) {
        const char *event_url = data->events[active_subpage[active_page]].url;
        if (strncmp(event_url, "https://", 8) == 0 ||
            strncmp(event_url, "http://", 7) == 0) {
            snprintf(url, url_size, "%s", event_url);
            return;
        }
        if (event_url[0] == '/') {
            snprintf(url, url_size, "https://www.texel.net%s", event_url);
            return;
        }
        if (event_url[0]) {
            snprintf(url, url_size, "https://www.texel.net/%s", event_url);
            return;
        }
    }

    switch (active_page) {
    case PAGE_CURRENT:
        snprintf(url, url_size, "%s", "https://www.texel.net/de/veranstaltungsuebersicht/");
        break;
    case PAGE_CALENDAR:
        snprintf(url, url_size, "%s", "https://familie-riegel.de/tx94/?Texel-Kalender");
        break;
    case PAGE_PHOTOS:
        snprintf(url, url_size, "%s", "https://familie-riegel.de/tx94/photos/320x200");
        break;
    case PAGE_WEATHER:
        snprintf(url, url_size, "%s", active_subpage[active_page] == 2
                 ? "https://open-meteo.com/en/docs/marine-weather-api"
                 : "https://open-meteo.com/en/docs");
        break;
    case PAGE_INFO:
        snprintf(url, url_size, "%s",
                 info_pages[active_subpage[active_page]].url);
        break;
    default:
        snprintf(url, url_size, "%s", "https://www.texel.net/");
        break;
    }
}

static void draw_qr_code(const char *url)
{
    esp_err_t err = display_draw_qr_code(97, 55, 126, url);
    if (err != ESP_OK) {
        ESP_LOGE("ui", "Could not generate QR code: %s", esp_err_to_name(err));
        display_text(70, 110, "QR-CODE NICHT VERFUEGBAR", COLOR_ORANGE, 1);
    }
}

static void draw_info_page(void)
{
    const info_page_t *page = &info_pages[active_subpage[PAGE_INFO]];
    display_text(34, 40, page->title, COLOR_ORANGE, 1);
    if (page->image) {
        display_fill_rect(200, 65, 84, 64, COLOR_PANEL);
        esp_err_t err = display_draw_image(202, 67, page->image);
        if (err != ESP_OK) {
            ESP_LOGE("ui", "Could not draw info image: %s", esp_err_to_name(err));
        }
    }
    for (size_t i = 0; i < INFO_PAGE_MAX_LINES && page->lines[i]; i++) {
        display_text(34, 61 + (int)i * 14, page->lines[i], COLOR_WHITE, 1);
    }
}

static void draw_page(const app_data_t *data)
{
    display_draw_frame();
    draw_tabs();
    if (qr_visible) {
        static const char *page_names[PAGE_COUNT] = {
            "HEUTE", "KALENDER", "FOTOS", "WETTER", "INFO"
        };
        char url[DATA_EVENT_URL_LENGTH + 32];
        char title[32];
        current_page_url(data, url, sizeof(url));
        snprintf(title, sizeof(title), "QR-CODE: %s", page_names[active_page]);
        display_fill_rect(28, 34, 266, 188, COLOR_BACKGROUND);
        display_text(34, 40, title, COLOR_ORANGE, 1);
        draw_qr_code(url);
        draw_wrapped_text(url, 34, 185, 2, COLOR_MUTED, 0);
        draw_side_buttons();
        draw_status();
        draw_subpage_indicator(data);
        return;
    }
    switch (active_page) {
    case PAGE_CURRENT:
        if (data->event_count && active_subpage[active_page] < data->event_count) {
            const texel_event_t *event = &data->events[active_subpage[active_page]];
            display_text(34, 40, event->day_offset ? "MORGEN AUF TEXEL" : "HEUTE AUF TEXEL",
                         COLOR_ORANGE, 1);
            char event_date[32];
            snprintf(event_date, sizeof(event_date), "%.2s.%.2s.%.4s  %s",
                     event->date + 8, event->date + 5, event->date,
                     event->time[0] ? event->time : "ZEIT N.N.");
            display_text(34, 56, event_date, COLOR_MUTED, 1);
            draw_wrapped_text(event->title, 34, 76, 4, COLOR_WHITE, 0);
            if (event->place[0]) {
                display_text(34, 134, "ORT", COLOR_ORANGE, 1);
                draw_wrapped_text(event->place, 34, 146, 2, COLOR_WHITE, 0);
            }
            if (event->description[0]) {
                draw_wrapped_text(event->description, 34,
                                  event->place[0] ? 172 : 142, 3, COLOR_MUTED, 0);
            }
        } else if (data->events_valid) {
            display_text(34, 40, "HEUTE & MORGEN", COLOR_ORANGE, 1);
            display_text(34, 70, "KEINE VERANSTALTUNGEN GEFUNDEN", COLOR_WHITE, 1);
        } else {
            display_text(34, 40, "VERANSTALTUNGEN", COLOR_ORANGE, 1);
            display_text(34, 70, "VVV-DATEN WERDEN GELADEN", COLOR_WHITE, 1);
        }
        display_text(34, 194, "QUELLE: TEXEL.NET", COLOR_MUTED, 1);
        break;
    case PAGE_CALENDAR:
        {
            char title[32];
            snprintf(title, sizeof(title), "TEXELKALENDER - TEIL %u",
                     active_subpage[active_page] + 1);
            display_text(34, 40, title, COLOR_ORANGE, 1);
        }
        display_text(34, 56, data->calendar_valid ? "LETZTER ABRUF:" : "WARTET AUF ABRUF",
                     COLOR_MUTED, 1);
        draw_wrapped_text(data->calendar, 34, 74, 10, COLOR_WHITE,
                          (size_t)active_subpage[active_page] * 340);
        break;
    case PAGE_PHOTOS:
        display_text(34, 40, active_subpage[active_page] == 0
                     ? "FOTOS VON TEXEL - 1" : "FOTOS VON TEXEL - 2",
                     COLOR_ORANGE, 1);
        display_fill_rect(32, 60, 256, 137, COLOR_PANEL);
        display_text(46, 78, active_subpage[active_page] == 0
                     ? "ZUFALLSFOTO 1" : "ZUFALLSFOTO 2", COLOR_WHITE, 1);
        display_text(46, 97, "NOCH NICHT BEREIT", COLOR_ORANGE, 1);
        display_text(46, 120, "JPEG 320 X 200 PX", COLOR_MUTED, 1);
        display_text(46, 142, "MANIFEST URL FESTLEGEN", COLOR_MUTED, 1);
        break;
    case PAGE_WEATHER:
        draw_weather(data, active_subpage[active_page]);
        break;
    case PAGE_INFO:
        draw_info_page();
        break;
    default:
        break;
    }
    draw_side_buttons();
    draw_status();
    draw_subpage_indicator(data);
}

static void app_main_task(void *unused)
{
    (void)unused;
    static app_data_t snapshot;
    static app_data_t previous;
    bool was_touched = false;
    bool refresh_sent = false;
    page_t previous_page = PAGE_COUNT;
    uint8_t previous_subpage = UINT8_MAX;
    bool previous_qr_visibility = false;
    bool previous_setup_visibility = false;
    int previous_clock_minute = -1;
    bool previous_clock_valid = false;
    bool ntp_sync_logged = false;
    TickType_t last_stack_report = xTaskGetTickCount();
    while (true) {
        display_process();
        int x = 0;
        int y = 0;
        bool touched = display_touch_read(&x, &y);
        TickType_t now = xTaskGetTickCount();
        data_snapshot(&snapshot);
        uint8_t active_subpage_count = page_subpage_count(active_page, &snapshot);
        if (active_subpage[active_page] >= active_subpage_count) {
            active_subpage[active_page] = active_subpage_count - 1;
        }
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
        if (touched && !was_touched && y >= 40 && y <= 218) {
            if (x <= 28) {
                active_subpage[active_page] =
                    (active_subpage[active_page] + active_subpage_count - 1) %
                    active_subpage_count;
            } else if (x >= 292) {
                active_subpage[active_page] =
                    (active_subpage[active_page] + 1) % active_subpage_count;
            }
        }
        if (touched && !was_touched && y >= 224 && x >= 226 && x < 258) {
            qr_visible = !qr_visible;
        }
        if (touched && !was_touched && y >= 224 && x >= 258 && x < 290) {
            sleep_pending = true;
            ESP_LOGI("ui", "Entering deep sleep after touch release");
        }
        if (touched && !was_touched && y >= 224 && x >= 290) {
            esp_err_t err = config_start_setup_ap();
            if (err == ESP_OK) {
                setup_credentials_visible = true;
                setup_visible_until = now + pdMS_TO_TICKS(30000);
            } else {
                ESP_LOGE("ui", "Could not start setup AP: %s", esp_err_to_name(err));
            }
        }
        was_touched = touched;
        if (sleep_pending && !touched) {
            esp_err_t err = touch_enable_deep_sleep_wakeup();
            if (err != ESP_OK) {
                ESP_LOGE("ui", "Could not enable touch wakeup: %s", esp_err_to_name(err));
                sleep_pending = false;
            } else {
                display_fill_rect(0, 34, DISPLAY_WIDTH, 188, COLOR_BACKGROUND);
                display_text(70, 105, "BERÜHREN ZUM AUFWACHEN",
                             COLOR_ORANGE, 1);
                display_process();
                vTaskDelay(pdMS_TO_TICKS(250));
                esp_deep_sleep_start();
            }
        }

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
                            qr_visible != previous_qr_visibility ||
                            show_setup != previous_setup_visibility ||
                            memcmp(&snapshot, &previous, sizeof(snapshot)) != 0;
        if (page_changed) {
            draw_page(&snapshot);
            if (snapshot.from_cache) {
                display_text_bg(176, 225, "CACHE", COLOR_ORANGE, COLOR_PANEL, 1);
            }
            previous = snapshot;
            previous_page = active_page;
            previous_subpage = active_subpage[active_page];
            previous_qr_visibility = qr_visible;
            previous_setup_visibility = show_setup;
        } else if (clock_changed) {
            draw_page(&snapshot);
            if (snapshot.from_cache) {
                display_text_bg(176, 225, "CACHE", COLOR_ORANGE, COLOR_PANEL, 1);
            }
        }
        previous_clock_minute = clock_minute;
        previous_clock_valid = clock_valid;
        if (now - last_stack_report >= pdMS_TO_TICKS(60000)) {
            ESP_LOGI("ui", "Display task minimum remaining stack: %u bytes",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            last_stack_report = now;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
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
    ESP_ERROR_CHECK(xTaskCreate(app_main_task, "display", 12288, NULL, 5, NULL) == pdPASS
                        ? ESP_OK : ESP_ERR_NO_MEM);
}
