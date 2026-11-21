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
#include "german_font.h"
#include "info_pages.h"
#include "ota.h"
#include "touch.h"
#include "web_config.h"
#include "footer_icons.h"
#include "memory.h"
#include "ui.h"

#define COLOR_BACKGROUND 0x0841
#define COLOR_PANEL 0x10A3
#define COLOR_MUTED 0xA514
#define COLOR_ORANGE 0xFD20
#define COLOR_GREEN 0x07E0
#define COLOR_WHITE 0xFFFF
#define IMAGE_SLOT_TRAY_X 176
#define IMAGE_SLOT_TRAY_Y 54
#define IMAGE_SLOT_TRAY_WIDTH 112
#define IMAGE_SLOT_TRAY_HEIGHT 85
#define IMAGE_SLOT_X 184          /* (176 + (112‑96)/2)  – centered */
#define IMAGE_SLOT_Y 56           /* (54 + (85‑81)/2)   – centered */
#define PAGE_ROTATE_INTERVAL_MS 60000
#define SPLASH_DURATION_MS 60000

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
static const uint8_t subpage_counts[PAGE_COUNT] = {1, 1, 2, 3, 0};
static bool setup_credentials_visible;
static bool qr_visible;
static bool photo_request_pending;
static bool sleep_pending;
static int calendar_month_offset;
static int calendar_scroll_offset;
static TickType_t setup_visible_until;
static TickType_t touch_started_at;
static int touch_start_x;
static int touch_start_y;
static TickType_t setup_touch_started_at;
static app_data_t *snapshot;
static app_data_t *previous;

static void draw_icon(int index, int center_x, int center_y, uint16_t color)
{
    switch (index) {
    case PAGE_CURRENT:
        display_circle(center_x, center_y, 9, color);
        display_line(center_x, center_y, center_x, center_y - 6, color);
        display_line(center_x, center_y, center_x + 5, center_y + 2, color);
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
    if (active_page == PAGE_CALENDAR) return;
    char indicator[12];
    if (active_page == PAGE_PHOTOS) {
        if (data->photo_count) {
            if (data->photo_number) {
                snprintf(indicator, sizeof(indicator), "%u/%u",
                         (unsigned)data->photo_number,
                         (unsigned)data->photo_count);
            } else {
                snprintf(indicator, sizeof(indicator), "--/%u",
                         (unsigned)data->photo_count);
            }
        } else {
            snprintf(indicator, sizeof(indicator), "--/--");
        }
    } else {
        snprintf(indicator, sizeof(indicator), "%u/%u",
                 active_subpage[active_page] + 1,
                 page_subpage_count(active_page, data));
    }
    display_text_bg(112, 225, indicator, COLOR_ORANGE, COLOR_PANEL, 1);
}

static void draw_wifi_icon(int center_x, int center_y, uint16_t color, int rssi)
{
    int active_bars = rssi >= -60 ? 4 : rssi >= -70 ? 3 : rssi >= -80 ? 2 : 1;
    for (int i = 0; i < 4; i++) {
        int height = 3 + i * 2;
        display_fill_rect(center_x - 6 + i * 3, center_y + 5 - height, 2, height,
                          config_wifi_connected() && i < active_bars
                              ? color : COLOR_MUTED);
    }
}

static void draw_qr_icon(int x, int y)
{
    /* 16x16 icon centered on x, aligned with the 16 px footer (starts at y 224). */
    (void)y;
    (void)display_draw_image(x - FOOTER_ICON_SIZE / 2, 224, &footer_icon_qr_white);
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
    (void)y;
    const lv_image_dsc_t *icon = color == COLOR_ORANGE
        ? &footer_icon_settings_orange : &footer_icon_settings_white;
    (void)display_draw_image(x - FOOTER_ICON_SIZE / 2, 224, icon);
}

/* Short OTA status text, or NULL while idle or after a successful update. */
static const char *ota_status_text(void)
{
    switch (ota_get_status()) {
        case OTA_STATUS_CHECKING: return "OTA: PRUEFE";
        case OTA_STATUS_UPDATE_AVAILABLE: return "OTA: UPDATE";
        case OTA_STATUS_NO_UPDATE: return "OTA: AKTUELL";
        case OTA_STATUS_DOWNLOADING: return "OTA: DOWNLOAD";
        case OTA_STATUS_WRITING: return "OTA: WRITE";
        case OTA_STATUS_FAILED: return "OTA: FEHLER";
        case OTA_STATUS_IDLE:
        case OTA_STATUS_SUCCESS:
            return NULL;
        default: return "OTA";
    }
}

/* Draw an OTA progress bar and status text while an update is in progress. */
static void draw_ota_progress(void)
{
    ota_status_t status = ota_get_status();
    if (status != OTA_STATUS_DOWNLOADING && status != OTA_STATUS_WRITING) {
        return;
    }
    const char *status_text = ota_status_text();
    uint8_t progress = ota_get_progress();

    const int bx = 40;
    const int by = 138;
    const int bw = 240;
    const int bh = 12;

    /* Frame */
    display_fill_rect(bx - 2, by - 2, bw + 4, bh + 4, COLOR_ORANGE);
    /* Background */
    display_fill_rect(bx, by, bw, bh, COLOR_BACKGROUND);
    /* Progress */
    int fill_w = (int)((uint32_t)bw * progress) / 100;
    if (fill_w > 0)
    {
        display_fill_rect(bx, by, fill_w, bh, COLOR_WHITE);
    }
    /* Percent text */
    char pct[8];
    snprintf(pct, sizeof(pct), "%u%%", (unsigned)progress);
    display_text(bx + bw / 2 - 12, by - 14, pct, COLOR_ORANGE, 1);
    /* Status text */
    if (status_text) {
        display_text(bx, by + bh + 4, status_text, COLOR_ORANGE, 1);
    }
}

/*
 * Setup banner above the status bar: three lines (SSID, URL, firmware version
 * plus OTA status). It must end at y 224 so it never overlaps the footer.
 */
static void draw_setup_banner(void)
{
    const int top = 179;
    display_fill_rect(0, top, DISPLAY_WIDTH, 224 - top, COLOR_ORANGE);
    display_text_bg(4, top, config_ap_ssid(), COLOR_BACKGROUND, COLOR_ORANGE, 1);
    display_text_bg(4, top + 15, "HTTP://192.168.4.1 OHNE LOGIN",
                    COLOR_BACKGROUND, COLOR_ORANGE, 1);
    char version_text[32];
    snprintf(version_text, sizeof(version_text), "FW %s", ota_get_version());
    display_text_bg(4, top + 30, version_text, COLOR_BACKGROUND, COLOR_ORANGE, 1);
    const char *ota_text = ota_status_text();
    if (ota_text) {
        display_text_bg(130, top + 30, ota_text, COLOR_BACKGROUND, COLOR_ORANGE, 1);
    }
}

static void draw_status(uint8_t timer_progress)
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
    display_set_page_timer(timer_progress);
    int rssi = config_wifi_rssi();
    uint16_t wifi_color = !config_wifi_connected() ? 0xF800
        : rssi >= -60 ? COLOR_GREEN : rssi >= -75 ? 0xFFE0 : 0xF800;
    draw_wifi_icon(186, 228, wifi_color, rssi);
    if (setup_credentials_visible && xTaskGetTickCount() < setup_visible_until) {
        draw_setup_banner();
    } else {
        setup_credentials_visible = false;
    }
    if (qr_visible) {
        draw_close_icon(242, 228);
    } else {
        draw_qr_icon(242, 228);
    }
    draw_sleep_icon(270, 228);
    draw_setup_icon(298, 228,
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

static uint32_t utf8_codepoint(const unsigned char *text, size_t length)
{
    if (length == 1) return text[0];
    if (length == 2) {
        return ((uint32_t)(text[0] & 0x1F) << 6) |
               (uint32_t)(text[1] & 0x3F);
    }
    if (length == 3) {
        return ((uint32_t)(text[0] & 0x0F) << 12) |
               ((uint32_t)(text[1] & 0x3F) << 6) |
               (uint32_t)(text[2] & 0x3F);
    }
    if (length == 4) {
        return ((uint32_t)(text[0] & 0x07) << 18) |
               ((uint32_t)(text[1] & 0x3F) << 12) |
               ((uint32_t)(text[2] & 0x3F) << 6) |
               (uint32_t)(text[3] & 0x3F);
    }
    return 0xFFFD;
}

static size_t utf8_safe_length(const char *text)
{
    size_t length = utf8_sequence_length((const unsigned char *)text);
    for (size_t i = 1; i < length; i++) {
        if (!text[i]) return 1;
    }
    return length;
}

static uint16_t text_glyph_width(const char *text, size_t length)
{
    uint32_t codepoint = utf8_codepoint((const unsigned char *)text, length);
    const char *next = text + length;
    size_t next_length = *next ? utf8_safe_length(next) : 0;
    uint32_t next_codepoint = next_length
        ? utf8_codepoint((const unsigned char *)next, next_length) : 0;
    return lv_font_get_glyph_width(&lv_font_german_12, codepoint,
                                   next_codepoint);
}

static void draw_wrapped_text_width(const char *text, int x, int y,
                                    int max_lines, int max_width,
                                    uint16_t color, size_t page_offset)
{
    if (!text || !text[0]) {
        display_text(x, y, "KEINE DATEN", COLOR_MUTED, 1);
        return;
    }
    int line = 0;
    const char *cursor = text;
    for (size_t skipped = 0; *cursor && skipped < page_offset; skipped++) {
        cursor += utf8_safe_length(cursor);
    }
    while (*cursor && line < max_lines) {
        while (*cursor == ' ') cursor++;
        if (!*cursor) break;

        const char *scan = cursor;
        const char *last_space = NULL;
        const char *line_end = NULL;
        const char *next_cursor = NULL;
        int width = 0;
        while (*scan && *scan != '\n') {
            size_t length = utf8_safe_length(scan);
            uint16_t glyph_width = text_glyph_width(scan, length);
            if (width + glyph_width > max_width) {
                if (last_space) {
                    line_end = last_space;
                    next_cursor = last_space + 1;
                    while (*next_cursor == ' ') next_cursor++;
                } else if (scan == cursor) {
                    line_end = scan + length;
                    next_cursor = line_end;
                } else {
                    line_end = scan;
                    next_cursor = scan;
                }
                break;
            }
            width += glyph_width;
            if (*scan == ' ') last_space = scan;
            scan += length;
        }
        if (!line_end) {
            line_end = scan;
            next_cursor = *scan == '\n' ? scan + 1 : scan;
        }

        size_t bytes = (size_t)(line_end - cursor);
        while (bytes && cursor[bytes - 1] == ' ') bytes--;
        if (bytes) {
            char buffer[140];
            if (bytes >= sizeof(buffer)) bytes = sizeof(buffer) - 1;
            memcpy(buffer, cursor, bytes);
            buffer[bytes] = '\0';
            display_text(x, y + line * 14, buffer, color, 1);
        }
        cursor = next_cursor;
        line++;
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

static const char *weekday_abbreviation(const char *date)
{
    static const char *weekdays[] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
    int year;
    int month;
    int day;
    if (!date || sscanf(date, "%4d-%2d-%2d", &year, &month, &day) != 3) {
        return "--";
    }
    struct tm calendar_date = {
        .tm_year = year - 1900,
        .tm_mon = month - 1,
        .tm_mday = day,
        .tm_hour = 12,
        .tm_isdst = -1,
    };
    if (mktime(&calendar_date) == (time_t)-1 ||
        calendar_date.tm_wday < 0 || calendar_date.tm_wday > 6) {
        return "--";
    }
    return weekdays[calendar_date.tm_wday];
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

static float tide_curve_value(const float *values, size_t count, size_t segment,
                              float t)
{
    float p0 = values[segment ? segment - 1 : segment];
    float p1 = values[segment];
    float p2 = values[segment + 1];
    float p3 = values[segment + 2 < count ? segment + 2 : segment + 1];
    float t2 = t * t;
    float t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
                   (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

static int tide_graph_y(float value, float minimum, float maximum)
{
    const int graph_bottom = 168;
    const int graph_height = 48;
    if (maximum - minimum < 0.01f) return graph_bottom - graph_height / 2;
    if (value < minimum) value = minimum;
    if (value > maximum) value = maximum;
    return graph_bottom - (int)lroundf(graph_height * (value - minimum) /
                                       (maximum - minimum));
}

static void draw_tide_graph(const float *values, size_t count,
                            float minimum, float maximum)
{
    const size_t steps_per_segment = 4;
    size_t chart_count = (count - 1) * steps_per_segment + 1;
    int32_t chart_values[(TIDE_SAMPLES - 1) * steps_per_segment + 1];
    if (chart_count > sizeof(chart_values) / sizeof(chart_values[0])) {
        ESP_LOGE("ui", "Too many tide samples for chart");
        return;
    }

    for (size_t i = 0; i < chart_count; i++) {
        size_t segment = i / steps_per_segment;
        float t = (float)(i % steps_per_segment) / steps_per_segment;
        if (i == chart_count - 1) {
            segment = count - 2;
            t = 1.0f;
        }
        float value = tide_curve_value(values, count, segment, t);
        if (value < minimum) value = minimum;
        if (value > maximum) value = maximum;
        chart_values[i] = (int32_t)lroundf(value * 100.0f);
    }

    esp_err_t err = display_draw_tide_chart(
        chart_values, chart_count, (int32_t)lroundf(minimum * 100.0f),
        (int32_t)lroundf(maximum * 100.0f));
    if (err != ESP_OK) {
        ESP_LOGE("ui", "Could not draw tide chart: %s", esp_err_to_name(err));
    }
}

static void draw_weather(const app_data_t *data, uint8_t subpage)
{
    if (subpage == 2) {
        display_text(34, 40, "GEZEITEN AUF TEXEL", COLOR_ORANGE, 1);
        if (!data->tide_valid) {
            draw_wrapped_text("Gezeitendaten werden geladen oder sind nicht erreichbar.",
                              34, 80, 5, COLOR_WHITE, 0);
            return;
        }
        display_text(34, 78, data->next_tide, COLOR_GREEN, 1);
        display_text(34, 104, "WASSERSTAND - NAECHSTE 12 STUNDEN",
                     COLOR_WHITE, 1);
        float values[TIDE_SAMPLES];
        float minimum = INFINITY;
        float maximum = -INFINITY;
        size_t valid_samples = 0;
        for (int i = 0; i < TIDE_SAMPLES; i++) {
            if (!isfinite(data->tide_samples[i])) continue;
            if (data->tide_samples[i] < minimum) minimum = data->tide_samples[i];
            if (data->tide_samples[i] > maximum) maximum = data->tide_samples[i];
            values[valid_samples++] = data->tide_samples[i];
        }
        if (valid_samples >= 2) {
            draw_tide_graph(values, valid_samples, minimum, maximum);
        } else if (valid_samples == 1) {
            display_fill_circle(160, tide_graph_y(values[0], minimum, maximum),
                                2, COLOR_ORANGE);
        }
        if (valid_samples) {
            char level_label[32];
            snprintf(level_label, sizeof(level_label), "MIN %.2fM     MAX %.2fM",
                     minimum, maximum);
            display_text(54, 180, level_label, COLOR_MUTED, 1);
        } else {
            display_text(54, 180, "KEINE VERLAUFSWERTE", COLOR_MUTED, 1);
        }
        return;
    }

    display_text(34, 40, subpage == 0 ? "JETZT AUF TEXEL" : "VORSCHAU - NÄCHSTE 5 TAGE",
                 COLOR_ORANGE, 1);
    display_text(252, 40, data->updated_at[11] ? data->updated_at + 11 : "--:--",
                 COLOR_MUTED, 1);
    if (!data->weather_valid) {
        draw_wrapped_text("Wetterdaten werden geladen oder sind nicht erreichbar.",
                          34, 76, 5, COLOR_WHITE, 0);
        return;
    }
    char line[52];
    if (subpage == 0) {
        draw_weather_icon(data->weather_code, 255, 103, 2);
        snprintf(line, sizeof(line), "%.1f °C", data->temperature_c);
        display_text(34, 76, line, COLOR_WHITE, 2);
        snprintf(line, sizeof(line), "GEFÜHLT %.1f °C", data->feels_like_c);
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
            int row_y = 78 + displayed_days * 27;
            uint16_t color = displayed_days == 0 ? COLOR_ORANGE : COLOR_WHITE;
            char temperature[16];
            char rain_probability[8];
            snprintf(temperature, sizeof(temperature), "%.0f/%.0f°C",
                     data->days[i].high_c, data->days[i].low_c);
            snprintf(rain_probability, sizeof(rain_probability), "%d%%",
                     data->days[i].rain_probability);
            draw_weather_icon(data->days[i].weather_code, 47, row_y + 8, 1);
            display_text(63, row_y, weekday_abbreviation(data->days[i].date),
                         color, 1);
            display_text(92, row_y,
                         weather_description(data->days[i].weather_code), color, 1);
            display_text(206, row_y, temperature, color, 1);
            display_text(259, row_y, rain_probability, color, 1);
            displayed_days++;
        }
        if (displayed_days == 0) {
            display_text(34, 82, "VORHERSAGE NICHT VERFUEGBAR", COLOR_MUTED, 1);
        }
    }
}

static const char *calendar_month_name(int month)
{
    static const char *months[] = {
        "Januar", "Februar", "März", "April", "Mai", "Juni",
        "Juli", "August", "September", "Oktober", "November", "Dezember",
    };
    return month >= 1 && month <= 12 ? months[month - 1] : "";
}

static int calendar_days_in_month(int year, int month)
{
    static const int month_lengths[] =
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int days = month_lengths[month - 1];
    bool leap_year = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap_year ? days + 1 : days;
}

static void calendar_month_layout(const app_data_t *data, int month_offset,
                                  int *year, int *month, int *first_weekday,
                                  int *days, int *rows)
{
    time_t now = time(NULL);
    struct tm local_time;
    if (now >= 1704067200 && localtime_r(&now, &local_time)) {
        *year = local_time.tm_year + 1900;
        *month = local_time.tm_mon + 1;
    } else if (data->calendar_entry_count) {
        *year = data->calendar_entries[0].year;
        *month = data->calendar_entries[0].month;
    } else {
        *year = 2026;
        *month = 1;
    }

    int month_index = *year * 12 + (*month - 1) + month_offset;
    *year = month_index / 12;
    *month = month_index % 12 + 1;
    if (*month <= 0) {
        *month += 12;
        (*year)--;
    }

    *days = calendar_days_in_month(*year, *month);
    struct tm first_day = {
        .tm_year = *year - 1900,
        .tm_mon = *month - 1,
        .tm_mday = 1,
        .tm_hour = 12,
        .tm_isdst = -1,
    };
    if (mktime(&first_day) == (time_t)-1) {
        *first_weekday = 0;
    } else {
        *first_weekday = (first_day.tm_wday + 6) % 7;
    }
    *rows = (*first_weekday + *days + 6) / 7;
}

static int calendar_scroll_limit(const app_data_t *data)
{
    int year, month, first_weekday, days, rows;
    calendar_month_layout(data, calendar_month_offset, &year, &month,
                          &first_weekday, &days, &rows);
    int content_height = rows * 30 - 2;
    const int viewport_height = 126;
    return content_height > viewport_height
        ? content_height - viewport_height : 0;
}

static const calendar_entry_t *calendar_booking_for(
    const app_data_t *data, int year, int month, int day)
{
    for (uint16_t i = 0; i < data->calendar_entry_count; i++) {
        const calendar_entry_t *entry = &data->calendar_entries[i];
        if (entry->year == year && entry->month == month && entry->day == day) {
            return entry;
        }
    }
    return NULL;
}

static const char *current_calendar_booking_name(const app_data_t *data)
{
    if (!data->calendar_valid || !config_ntp_synced()) return NULL;

    time_t now = time(NULL);
    struct tm local_time;
    if (now < 1704067200 || !localtime_r(&now, &local_time)) return NULL;

    const calendar_entry_t *booking = calendar_booking_for(
        data, local_time.tm_year + 1900, local_time.tm_mon + 1,
        local_time.tm_mday);
    if (!booking || booking->group_id >= DATA_CALENDAR_GROUPS ||
        !data->calendar_group_names[booking->group_id][0]) {
        return NULL;
    }
    return data->calendar_group_names[booking->group_id];
}

static uint16_t calendar_group_color(uint8_t group_id)
{
    static const uint16_t colors[DATA_CALENDAR_GROUPS] = {
        COLOR_PANEL, 0x04C0, 0xFE60, 0xB800, 0xCB20, 0x432C,
        0x18CE, 0x2444, 0x8004, 0x4810, 0x3902, 0x0451,
        0x10A3, 0x10A3, 0x10A3, 0x10A3,
    };
    return group_id < DATA_CALENDAR_GROUPS ? colors[group_id] : COLOR_PANEL;
}

static uint16_t calendar_group_text_color(uint8_t group_id)
{
    return group_id == 2 ? 0x2104 : COLOR_WHITE;
}

static void calendar_short_group_name(const char *name, char *short_name,
                                      size_t short_name_size)
{
    const char *plus = strchr(name, '+');
    if (plus) {
        const char *second_name = plus + 1;
        while (*second_name == ' ') second_name++;
        snprintf(short_name, short_name_size, "%c+%c.",
                 name[0], second_name[0]);
        return;
    }
    const char *space = strchr(name, ' ');
    if (space && space[1]) {
        size_t first_length = (size_t)(space - name);
        if (first_length <= 3) {
            snprintf(short_name, short_name_size, "%.*s %c.",
                     (int)first_length, name, space[1]);
        } else {
            snprintf(short_name, short_name_size, "%c. %c.",
                     name[0], space[1]);
        }
    } else {
        snprintf(short_name, short_name_size, "%.5s%s", name,
                 strlen(name) > 5 ? "." : "");
    }
}

static void draw_calendar(const app_data_t *data)
{
    static const char *weekdays[] = {"Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"};
    const int grid_left = 30;
    const int grid_top = 92;
    const int cell_width = 35;
    const int cell_height = 28;
    const int column_pitch = 37;
    const int row_pitch = 30;
    const int viewport_bottom = 218;

    int year, month, first_weekday, days, rows;
    calendar_month_layout(data, calendar_month_offset, &year, &month,
                          &first_weekday, &days, &rows);
    int scroll_limit = calendar_scroll_limit(data);
    if (calendar_scroll_offset < 0) calendar_scroll_offset = 0;
    if (calendar_scroll_offset > scroll_limit) calendar_scroll_offset = scroll_limit;

    char month_title[32];
    const char *month_name = calendar_month_name(month);
    snprintf(month_title, sizeof(month_title), "%s %d",
             month_name, year);
    display_text(30, 40, "BELEGUNGSKALENDER", COLOR_ORANGE, 1);
    display_text(30, 56, month_title, COLOR_WHITE, 1);
    if (calendar_month_offset == 0) {
        size_t title_characters = 0;
        for (const unsigned char *character =
                 (const unsigned char *)month_title;
             *character; character++) {
            if ((*character & 0xC0) != 0x80) title_characters++;
        }
        int title_width = (int)title_characters * 7;
        display_line(30, 70, 30 + title_width - 1, 70, COLOR_ORANGE);
    }
    for (int column = 0; column < 7; column++) {
        display_text_box(grid_left + column * column_pitch, 76,
                         cell_width, 15, weekdays[column],
                         column == 6 ? COLOR_ORANGE : COLOR_MUTED, 1);
    }

    time_t now = time(NULL);
    struct tm today;
    bool today_valid = now >= 1704067200 && localtime_r(&now, &today);
    int previous_month = month == 1 ? 12 : month - 1;
    int previous_year = month == 1 ? year - 1 : year;
    int previous_month_days = calendar_days_in_month(previous_year, previous_month);
    int total_days = rows * 7;

    for (int index = 0; index < total_days; index++) {
        int day = index - first_weekday + 1;
        int cell_year = year;
        int cell_month = month;
        bool in_month = day >= 1 && day <= days;
        if (day < 1) {
            day += previous_month_days;
            cell_year = previous_year;
            cell_month = previous_month;
        } else if (day > days) {
            day -= days;
            cell_month = month == 12 ? 1 : month + 1;
            cell_year = month == 12 ? year + 1 : year;
        }

        int row = index / 7;
        int column = index % 7;
        int x = grid_left + column * column_pitch;
        int y = grid_top + row * row_pitch - calendar_scroll_offset;
        if (y + cell_height <= grid_top || y >= viewport_bottom) continue;

        const calendar_entry_t *booking =
            calendar_booking_for(data, cell_year, cell_month, day);
        uint16_t background = !in_month ? COLOR_BACKGROUND
            : booking ? calendar_group_color(booking->group_id) : COLOR_PANEL;
        int clipped_y = y < grid_top ? grid_top : y;
        int clipped_bottom = y + cell_height > viewport_bottom
            ? viewport_bottom : y + cell_height;
        if (clipped_y != y || clipped_bottom != y + cell_height) {
            display_fill_rect(x, clipped_y, cell_width,
                              clipped_bottom - clipped_y, background);
            continue;
        }
        display_fill_round_rect_radius(x, y, cell_width, cell_height, 4,
                                       background);

        char day_text[3];
        snprintf(day_text, sizeof(day_text), "%d", day);
        uint16_t booking_text_color = booking
            ? calendar_group_text_color(booking->group_id) : COLOR_WHITE;
        uint16_t day_color = in_month ? booking_text_color : COLOR_MUTED;
        display_text_box(x + 1, y + 1, cell_width - 2, 14,
                         day_text, day_color, 1);

        if (booking && booking->group_id < DATA_CALENDAR_GROUPS) {
            char group_name[16];
            calendar_short_group_name(
                data->calendar_group_names[booking->group_id],
                group_name, sizeof(group_name));
            display_text_box(x + 1, y + 14, cell_width - 2, 14,
                             group_name, booking_text_color, 1);
        }
        if (today_valid && in_month && today.tm_year + 1900 == year &&
            today.tm_mon + 1 == month && today.tm_mday == day) {
            display_line(x, y, x + cell_width - 1, y, COLOR_ORANGE);
            display_line(x, y + cell_height - 1, x + cell_width - 1,
                         y + cell_height - 1, COLOR_ORANGE);
            display_line(x, y, x, y + cell_height - 1, COLOR_ORANGE);
            display_line(x + cell_width - 1, y, x + cell_width - 1,
                         y + cell_height - 1, COLOR_ORANGE);
        }
    }

    if (!data->calendar_valid) {
        display_text(38, 196, "BELEGUNGSDATEN WERDEN GELADEN",
                     COLOR_MUTED, 1);
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
        snprintf(url, url_size, "%s",
                 "https://open-meteo.com/en/forecast?latitude=53.124279&longitude=4.812830");
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

static bool draw_splash_screen(const app_data_t *data)
{
    display_clear(COLOR_BACKGROUND);
    esp_err_t err = display_draw_jpeg(0, 0, splash_image_data,
                                      splash_image_size);
    if (err != ESP_OK) {
        ESP_LOGE("ui", "Could not draw startup splash image: %s",
                 esp_err_to_name(err));
        return false;
    }

    const char *name = current_calendar_booking_name(data);
    if (name) {
        char welcome[DATA_CALENDAR_GROUP_NAME_LENGTH + 32];
        snprintf(welcome, sizeof(welcome), "Willkommen auf Texel, %s!\n<start>", name);
        display_fill_rect_opa(10, 178, 300, 52, 0x0000, 190);
        display_text_box(10, 189, 300, 42, welcome, COLOR_WHITE, 1);
    }

    const char *version = ota_get_version();
    char version_line[64];
    snprintf(version_line, sizeof(version_line), "v%s", version);
    display_text(10, 238, version_line, COLOR_MUTED, 1);

    return true;
}

static void draw_info_page(void)
{
    const info_page_t *page = &info_pages[active_subpage[PAGE_INFO]];
    display_text(34, 40, page->title, COLOR_ORANGE, 1);
    if (page->image) {
        display_fill_rect(IMAGE_SLOT_TRAY_X, IMAGE_SLOT_TRAY_Y,
                          IMAGE_SLOT_TRAY_WIDTH, IMAGE_SLOT_TRAY_HEIGHT,
                          COLOR_PANEL);
        esp_err_t err = display_draw_image(IMAGE_SLOT_X, IMAGE_SLOT_Y,
                                           page->image);
        if (err != ESP_OK) {
            ESP_LOGE("ui", "Could not draw info image: %s", esp_err_to_name(err));
        }
    }
    for (size_t i = 0; i < INFO_PAGE_MAX_LINES && page->lines[i]; i++) {
        display_text(34, 61 + (int)i * 14, page->lines[i], COLOR_WHITE, 1);
    }
    if (page->qr_data) {
        display_draw_qr_code(IMAGE_SLOT_X + IMAGE_SLOT_TRAY_WIDTH + 10,
                             IMAGE_SLOT_Y, 80, page->qr_data);
    }
}

static void draw_page(const app_data_t *data, uint8_t timer_progress,
                      bool splash_visible)
{
    if (splash_visible && draw_splash_screen(data)) return;

    display_draw_frame();
    draw_tabs();
    draw_ota_progress();
    if (qr_visible) {
        static const char *page_names[PAGE_COUNT] = {
            "HEUTE", "KALENDER", "FOTOS", "WETTER", "INFO"
        };
        char url[DATA_EVENT_URL_LENGTH + 32];
        char title[64];
        const char *page_name = active_page == PAGE_INFO
            ? info_pages[active_subpage[PAGE_INFO]].title
            : page_names[active_page];
        current_page_url(data, url, sizeof(url));
        const char *qr_target = url[0] ? url :
            (active_page == PAGE_INFO && info_pages[active_subpage[PAGE_INFO]].qr_data
                ? info_pages[active_subpage[PAGE_INFO]].qr_data : NULL);
        snprintf(title, sizeof(title), "QR-CODE: %s", page_name);
        display_fill_rect(28, 34, 266, 188, COLOR_BACKGROUND);
        display_text(34, 40, title, COLOR_ORANGE, 1);
        if (qr_target) {
            draw_qr_code(qr_target);
            if (url[0]) {
                draw_wrapped_text(url, 34, 185, 2, COLOR_MUTED, 0);
            }
        }
        draw_side_buttons();
        draw_status(timer_progress);
        draw_subpage_indicator(data);
        return;
    }
    bool show_event_image = false;
    bool event_image_error = false;
    int event_status_y = 194;
    switch (active_page) {
    case PAGE_CURRENT:
        if (data->event_count && active_subpage[active_page] < data->event_count) {
            const texel_event_t *event = &data->events[active_subpage[active_page]];
            bool downloaded_event_image = data->event_image_ready &&
                data->event_image_index == active_subpage[active_page];
            show_event_image = event->image_url[0] &&
                (downloaded_event_image ||
                 display_event_thumbnail_cached(event->image_url));
            event_image_error = data->event_image_error &&
                                data->event_image_index ==
                                    active_subpage[active_page];
            bool reserve_event_image = event->image_url[0] &&
                                       !event_image_error;
            if (reserve_event_image) event_status_y = 208;
            display_text(34, 40, event->day_offset ? "MORGEN AUF TEXEL" : "HEUTE AUF TEXEL",
                         COLOR_ORANGE, 1);
            char event_times[DATA_EVENT_TIMES_TEXT_LENGTH] = {0};
            uint8_t time_count = event->time_count;
            if (time_count > DATA_EVENT_TIMES_PER_EVENT) {
                time_count = DATA_EVENT_TIMES_PER_EVENT;
            }
            /*
            ESP_LOGI("ui", "Event %s has %u times, displaying %u",
                     event->title, event->time_count, time_count);
            */
            if (!time_count) {
                snprintf(event_times, sizeof(event_times), "%s", "ZEIT N.N.");
            } else {
                size_t used = 0;
                for (uint8_t i = 0; i < time_count; i++) {
                    int written = snprintf(event_times + used,
                                           sizeof(event_times) - used,
                                           "%s%02u:%02u", i ? ", " : "",
                                           (unsigned)(event->times[i] / 60U),
                                           (unsigned)(event->times[i] % 60U));
                    if (written < 0 || (size_t)written >=
                        sizeof(event_times) - used) {
                        break;
                    }
                    used += (size_t)written;
                }
            }
            char event_date[DATA_EVENT_TIMES_TEXT_LENGTH + 12];
            if (time_count > 1) {
                snprintf(event_date, sizeof(event_date), "%.2s.%.2s  %s",
                         event->date + 8, event->date + 5, event_times);
            } else {
                snprintf(event_date, sizeof(event_date), "%.2s.%.2s.%.4s  %s",
                         event->date + 8, event->date + 5, event->date,
                         event_times);
            }
            display_text(34, 56, event_date, COLOR_MUTED, 1);
            if (reserve_event_image) {
                draw_wrapped_text_width(event->title, 34, 76, 3, 164,
                                        COLOR_WHITE, 0);
            } else {
                draw_wrapped_text(event->title, 34, 76, 4, COLOR_WHITE, 0);
            }
            if (reserve_event_image) {
                display_fill_rect(IMAGE_SLOT_TRAY_X, IMAGE_SLOT_TRAY_Y,
                                  IMAGE_SLOT_TRAY_WIDTH, IMAGE_SLOT_TRAY_HEIGHT,
                                  COLOR_PANEL);
            }
            if (show_event_image) {
                size_t image_size = 0;
                const uint8_t *image = downloaded_event_image
                    ? data_event_image_jpeg(&image_size) : NULL;
                ESP_LOGD("ui", "Drawing event image: url=%s image=%p size=%u downloaded=%d cached=%d",
                         event->image_url, (const void *)image, image_size,
                         downloaded_event_image ? 1 : 0,
                         image ? 0 : 1);
                esp_err_t err = display_draw_event_thumbnail(
                    IMAGE_SLOT_X, IMAGE_SLOT_Y, event->image_url, image,
                    image_size);
                if (err != ESP_OK) {
                    ESP_LOGE("ui", "Could not display event image: %s",
                             esp_err_to_name(err));
                    event_image_error = true;
                }
            }
            if (reserve_event_image) {
                if (event->place[0]) {
                    display_text(34, 120, "ORT", COLOR_ORANGE, 1);
                    draw_wrapped_text_width(event->place, 34, 132, 2, 164,
                                            COLOR_WHITE, 0);
                }
                if (event->description[0]) {
                    draw_wrapped_text(event->description, 34,
                                      event->place[0] ? 162 : 134,
                                      3, COLOR_MUTED, 0);
                }
            } else {
                if (event->place[0]) {
                    display_text(34, 134, "ORT", COLOR_ORANGE, 1);
                    draw_wrapped_text(event->place, 34, 146, 2, COLOR_WHITE, 0);
                }
                if (event->description[0]) {
                    draw_wrapped_text(event->description, 34,
                                      event->place[0] ? 172 : 142, 3, COLOR_MUTED, 0);
                }
            }
        } else if (data->events_valid) {
            display_text(34, 40, "HEUTE & MORGEN", COLOR_ORANGE, 1);
            display_text(34, 70, "KEINE VERANSTALTUNGEN GEFUNDEN", COLOR_WHITE, 1);
        } else {
            display_text(34, 40, "VERANSTALTUNGEN", COLOR_ORANGE, 1);
            display_text(34, 70, "VVV-DATEN WERDEN GELADEN", COLOR_WHITE, 1);
        }
        if (!show_event_image || event_image_error) {
            const char *status = "QUELLE: TEXEL.NET";
            if (event_image_error) {
                status = "BILD NICHT VERFUEGBAR";
            } else if (active_subpage[active_page] < data->event_count &&
                       data->events[active_subpage[active_page]].image_url[0]) {
                status = "BILD WIRD GELADEN";
            }
            display_text(34, event_status_y, status, COLOR_MUTED, 1);
        }
        break;
    case PAGE_CALENDAR:
        draw_calendar(data);
        break;
    case PAGE_PHOTOS:
        display_text(34, 40, "FOTOS VON TEXEL", COLOR_ORANGE, 1);
        display_fill_rect(32, 60, 256, 137, COLOR_PANEL);
        if (data->photo_ready && !photo_request_pending) {
            size_t photo_size = 0;
            const uint8_t *photo = data_photo_jpeg(&photo_size);
            esp_err_t err = display_draw_photo_jpeg(
                BOARD_PHOTO_X, BOARD_PHOTO_Y, photo, photo_size);
            if (err != ESP_OK) {
                ESP_LOGE("ui", "Could not display photo: %s", esp_err_to_name(err));
                display_text(46, 112, "FOTO KANN NICHT ANGEZEIGT WERDEN",
                             COLOR_ORANGE, 1);
            } else {
                char caption[DATA_PHOTO_TITLE_LENGTH + DATA_PHOTO_DATE_LENGTH + 4];
                snprintf(caption, sizeof(caption), "%s - %s",
                         data->photo_title, data->photo_date);
                display_text(34, BOARD_PHOTO_CAPTION_Y, caption,
                             COLOR_MUTED, 1);
            }
        } else if (data->photo_error) {
            display_text(54, 112, "FOTO NICHT VERFUEGBAR", COLOR_ORANGE, 1);
        } else {
            display_text(70, 112, "FOTO WIRD GELADEN", COLOR_WHITE, 1);
        }
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
    draw_status(timer_progress);
    draw_subpage_indicator(data);
}

static void ui_task(void *unused)
{
    (void)unused;
    bool was_touched = false;
    bool refresh_sent = false;
    page_t previous_page = PAGE_COUNT;
    uint8_t previous_subpage = UINT8_MAX;
    bool previous_qr_visibility = false;
    bool previous_setup_visibility = false;
    bool previous_splash_visible = false;
    bool previous_event_image_page_active = false;
    int previous_timer_second = -1;
    int previous_clock_minute = -1;
    bool previous_clock_valid = false;
    ota_status_t previous_ota_status = OTA_STATUS_IDLE;
    uint8_t previous_ota_progress = UINT8_MAX;
    int previous_calendar_month = calendar_month_offset;
    int previous_calendar_scroll = calendar_scroll_offset;
    TickType_t last_auto_advance = xTaskGetTickCount();
    TickType_t splash_started = last_auto_advance;
    TickType_t last_stack_report = xTaskGetTickCount();
    bool splash_dismissed = false;
    while (true) {
        display_process();
        int x = 0;
        int y = 0;
        bool touched = display_touch_read(&x, &y);
        TickType_t now = xTaskGetTickCount();
        bool splash_visible = !splash_dismissed &&
            now - splash_started < pdMS_TO_TICKS(SPLASH_DURATION_MS);
        data_snapshot(snapshot);
        if (photo_request_pending && (snapshot->photo_ready || snapshot->photo_error)) {
            photo_request_pending = false;
        }
        uint8_t active_subpage_count = page_subpage_count(active_page, snapshot);
        if (active_subpage[active_page] >= active_subpage_count) {
            active_subpage[active_page] = active_subpage_count - 1;
        }
        if (splash_visible && touched && !was_touched) {
            splash_dismissed = true;
            splash_visible = false;
            refresh_sent = true;
        } else if (!splash_visible) {
            if (touched && !was_touched) {
                touch_started_at = now;
                touch_start_x = x;
                touch_start_y = y;
                refresh_sent = false;
            }
            if (touched && !refresh_sent &&
                now - touch_started_at > pdMS_TO_TICKS(1600) && y < 40) {
                data_request_refresh();
                refresh_sent = true;
            }
            if (touched && !was_touched && y < 38) {
                int selected = x / 64;
                if (selected >= 0 && selected < PAGE_COUNT) {
                    active_page = (page_t)selected;
                    active_subpage[active_page] = 0;
                    if (active_page == PAGE_CALENDAR) calendar_scroll_offset = 0;
                }
            }
            if (touched && !was_touched && y >= 40 && y <= 218) {
                if (active_page == PAGE_CALENDAR && x <= 28) {
                    calendar_month_offset--;
                    calendar_scroll_offset = 0;
                } else if (active_page == PAGE_CALENDAR && x >= 292) {
                    calendar_month_offset++;
                    calendar_scroll_offset = 0;
                } else if (active_page != PAGE_CALENDAR && x <= 28) {
                    active_subpage[active_page] =
                        (active_subpage[active_page] + active_subpage_count - 1) %
                        active_subpage_count;
                } else if (active_page != PAGE_CALENDAR && x >= 292) {
                    active_subpage[active_page] =
                        (active_subpage[active_page] + 1) % active_subpage_count;
                }
            }
            if (!touched && was_touched && active_page == PAGE_CALENDAR &&
                touch_start_x > 28 && touch_start_x < 292 &&
                touch_start_y >= 86 && touch_start_y <= 218) {
                int vertical_drag = touch_start_y - y;
                if (abs(vertical_drag) >= 12) {
                    calendar_scroll_offset += vertical_drag;
                    int scroll_limit = calendar_scroll_limit(snapshot);
                    if (calendar_scroll_offset < 0) calendar_scroll_offset = 0;
                    if (calendar_scroll_offset > scroll_limit) {
                        calendar_scroll_offset = scroll_limit;
                    }
                }
            }
            if (touched && !was_touched && y >= 228 && x >= 226 && x < 258) {
                qr_visible = !qr_visible;
            }
            if (touched && !was_touched && y >= 228 && x >= 258 && x < 290) {
                sleep_pending = true;
                ESP_LOGI("ui", "Entering deep sleep after touch release");
            }
            if (touched && !was_touched && y >= 228 && x >= 290) {
                setup_touch_started_at = now;
            }
            if (!touched && was_touched && y >= 228 && x >= 290 &&
                setup_touch_started_at != 0) {
                TickType_t held = now - setup_touch_started_at;
                setup_touch_started_at = 0;
                if (held >= pdMS_TO_TICKS(2000)) {
                    ota_manifest_t manifest = {0};
                    esp_err_t err = ota_check_update(&manifest);
                    if (err == ESP_OK && manifest.available) {
                        ESP_LOGI("ui", "Manual OTA check: update available %s, starting update",
                                 manifest.version);
                        ota_start_update(&manifest);
                    } else if (err == ESP_OK) {
                        ESP_LOGI("ui", "Manual OTA check: no update");
                    } else {
                        ESP_LOGW("ui", "Manual OTA check failed: %s",
                                 esp_err_to_name(err));
                    }
                } else if (config_setup_ap_active()) {
                    esp_err_t err = config_stop_setup_ap();
                    setup_credentials_visible = false;
                    if (err != ESP_OK) {
                        ESP_LOGE("ui", "Could not stop setup AP: %s",
                                 esp_err_to_name(err));
                    }
                } else {
                    esp_err_t err = config_start_setup_ap();
                    if (err == ESP_OK) {
                        setup_credentials_visible = true;
                        setup_visible_until = now + pdMS_TO_TICKS(30000);
                    } else {
                        ESP_LOGE("ui", "Could not start setup AP: %s",
                                 esp_err_to_name(err));
                    }
                }
            }
        }
        was_touched = touched;
        if (!splash_visible && previous_splash_visible) {
            last_auto_advance = now;
        }
        if (active_page != previous_page ||
            active_subpage[active_page] != previous_subpage ||
            (active_page == PAGE_CALENDAR &&
             (calendar_month_offset != previous_calendar_month ||
              calendar_scroll_offset != previous_calendar_scroll))) {
            last_auto_advance = now;
        }
        if (qr_visible) {
            last_auto_advance = now;
        } else if (now - last_auto_advance >=
                   pdMS_TO_TICKS(PAGE_ROTATE_INTERVAL_MS)) {
            active_subpage[active_page] =
                (active_subpage[active_page] + 1) % active_subpage_count;
            last_auto_advance = now;
        }
        if (sleep_pending && !touched) {
            esp_err_t err = touch_driver_enable_deep_sleep_wakeup();
            if (err == ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGW("ui", "Touch wakeup not supported on this board; using timer wakeup");
                esp_sleep_enable_timer_wakeup(60 * 1000000ULL);  // 60s fallback
            } else if (err != ESP_OK) {
                ESP_LOGE("ui", "Could not enable touch wakeup: %s", esp_err_to_name(err));
                sleep_pending = false;
            }
            if (err != ESP_ERR_INVALID_STATE) {
                display_fill_rect(0, 34, DISPLAY_WIDTH, 188, COLOR_BACKGROUND);
                display_text(70, 105, "BERÜHREN ZUM AUFWACHEN",
                             COLOR_ORANGE, 1);
                display_process();
                vTaskDelay(pdMS_TO_TICKS(250));
                esp_deep_sleep_start();
            }
            sleep_pending = false;
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
        uint32_t elapsed_seconds =
            (uint32_t)pdTICKS_TO_MS(now - last_auto_advance) / 1000U;
        if (elapsed_seconds >= PAGE_ROTATE_INTERVAL_MS / 1000U) {
            elapsed_seconds = PAGE_ROTATE_INTERVAL_MS / 1000U - 1;
        }
        uint8_t timer_progress = (uint8_t)elapsed_seconds;
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
                            splash_visible != previous_splash_visible ||
                            calendar_month_offset != previous_calendar_month ||
                            calendar_scroll_offset != previous_calendar_scroll ||
                            qr_visible != previous_qr_visibility ||
                            show_setup != previous_setup_visibility ||
                            memcmp(snapshot, previous, sizeof(*snapshot)) != 0;
        ota_status_t current_ota_status = ota_get_status();
        uint8_t current_ota_progress = ota_get_progress();
        bool ota_redraw = current_ota_status != previous_ota_status ||
                          current_ota_progress != previous_ota_progress;
        bool event_image_page_active =
            !splash_visible && !qr_visible && active_page == PAGE_CURRENT &&
            snapshot->events_valid && active_subpage[PAGE_CURRENT] < snapshot->event_count;
        if (page_changed || ota_redraw) {
            bool photo_page_changed = active_page == PAGE_PHOTOS &&
                (active_page != previous_page ||
                 active_subpage[PAGE_PHOTOS] != previous_subpage);
            bool leaving_photo_page = previous_page == PAGE_PHOTOS &&
                                      active_page != PAGE_PHOTOS;
            bool photo_prefetched = false;
            if (active_page != PAGE_PHOTOS) photo_request_pending = false;
            if (photo_page_changed) {
                photo_prefetched = data_promote_prefetched_photo();
                photo_request_pending = !photo_prefetched;
                if (photo_prefetched) data_snapshot(snapshot);
            }
            draw_page(snapshot, timer_progress, splash_visible);
            if (photo_page_changed && !photo_prefetched) {
                data_request_photo();
            }
            if (leaving_photo_page) data_release_photo();
            if (active_page == PAGE_PHOTOS && snapshot->photo_ready) {
                data_prefetch_next_photo();
            }
            if (event_image_page_active) {
                if (!display_event_thumbnail_cached(
                        snapshot->events[active_subpage[PAGE_CURRENT]].image_url)) {
                    data_request_event_image(active_subpage[PAGE_CURRENT]);
                }
            } else if (previous_event_image_page_active) {
                data_release_event_image();
            }
            if (snapshot->from_cache && !splash_visible) {
                display_text_bg(146, 225, "CACHE", COLOR_ORANGE, COLOR_PANEL, 1);
            }
            memcpy(previous, snapshot, sizeof(*snapshot));
            previous_page = active_page;
            previous_subpage = active_subpage[active_page];
            previous_calendar_month = calendar_month_offset;
            previous_calendar_scroll = calendar_scroll_offset;
            previous_qr_visibility = qr_visible;
            previous_setup_visibility = show_setup;
            previous_splash_visible = splash_visible;
            previous_ota_status = current_ota_status;
            previous_ota_progress = current_ota_progress;
        } else if (clock_changed) {
            draw_page(snapshot, timer_progress, splash_visible);
            if (snapshot->from_cache && !splash_visible) {
                display_text_bg(146, 225, "CACHE", COLOR_ORANGE, COLOR_PANEL, 1);
            }
            previous_splash_visible = splash_visible;
        } else if (previous_timer_second != timer_progress) {
            display_set_page_timer(timer_progress);
        }
        previous_timer_second = timer_progress;
        previous_event_image_page_active = event_image_page_active;
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

esp_err_t ui_start(void)
{
    snapshot = memory_alloc(sizeof(*snapshot), MEMORY_ALLOC_LARGE);
    if (!snapshot) return ESP_ERR_NO_MEM;
    previous = memory_alloc(sizeof(*previous), MEMORY_ALLOC_LARGE);
    if (!previous) {
        memory_free(snapshot);
        snapshot = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(ui_task, "display", BOARD_UI_TASK_STACK_SIZE, NULL, 5, NULL) !=
        pdPASS) {
        memory_free(snapshot);
        memory_free(previous);
        snapshot = NULL;
        previous = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
