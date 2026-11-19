#include "display.h"

#include <string.h>

#include "data.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "german_font.h"
#include "lvgl.h"
#include "image/lv_image_decoder_private.h"
#include "memory.h"
#include "display_hw.h"
#include "touch.h"

#define LVGL_LINE_CAPACITY 256
#define PHOTO_FS_LETTER 'P'
#define PHOTO_FS_PATH "P:photo.jpg"
#define EVENT_THUMBNAIL_WIDTH 108
#define EVENT_THUMBNAIL_HEIGHT 81
#define EVENT_THUMBNAIL_PIXEL_COUNT \
    (EVENT_THUMBNAIL_WIDTH * EVENT_THUMBNAIL_HEIGHT)
#define EVENT_THUMBNAIL_BYTES \
    (EVENT_THUMBNAIL_PIXEL_COUNT * sizeof(uint16_t))

static const char *TAG = "display";
static lv_display_t *lv_display;
static lv_obj_t *screen;
static lv_obj_t *page_timer_arc;
static lv_indev_t *touch_indev;
static int touch_x;
static int touch_y;
static bool touch_pressed;
static size_t line_count;
static const uint8_t *photo_fs_data;
static size_t photo_fs_size;
static size_t photo_fs_position;
static lv_fs_drv_t photo_fs_driver;
static lv_point_precise_t line_points[LVGL_LINE_CAPACITY][2];
static uint8_t *display_buffer;

typedef struct {
    char url[DATA_EVENT_IMAGE_URL_LENGTH];
    uint16_t *pixels;
    uint64_t last_used;
    lv_image_dsc_t image;
} event_thumbnail_cache_entry_t;

static event_thumbnail_cache_entry_t event_thumbnail_cache[DATA_EVENT_COUNT];
static uint64_t event_thumbnail_use_counter;

static bool photo_fs_ready(lv_fs_drv_t *driver)
{
    (void)driver;
    return photo_fs_data && photo_fs_size;
}

static void *photo_fs_open(lv_fs_drv_t *driver, const char *path, lv_fs_mode_t mode)
{
    (void)driver;
    if ((mode & LV_FS_MODE_RD) == 0 || (mode & LV_FS_MODE_WR) != 0 ||
        (strcmp(path, "photo.jpg") != 0 && strcmp(path, "/photo.jpg") != 0) ||
        !photo_fs_ready(NULL)) {
        return NULL;
    }
    photo_fs_position = 0;
    return &photo_fs_position;
}

static lv_fs_res_t photo_fs_close(lv_fs_drv_t *driver, void *file)
{
    (void)driver;
    (void)file;
    return LV_FS_RES_OK;
}

static lv_fs_res_t photo_fs_read(lv_fs_drv_t *driver, void *file, void *buffer,
                                 uint32_t bytes_to_read, uint32_t *bytes_read)
{
    (void)driver;
    size_t *position = file;
    if (!position || !buffer || !bytes_read || *position > photo_fs_size) {
        return LV_FS_RES_INV_PARAM;
    }
    size_t remaining = photo_fs_size - *position;
    size_t count = bytes_to_read < remaining ? bytes_to_read : remaining;
    memcpy(buffer, photo_fs_data + *position, count);
    *position += count;
    *bytes_read = (uint32_t)count;
    return LV_FS_RES_OK;
}

static lv_fs_res_t photo_fs_seek(lv_fs_drv_t *driver, void *file, uint32_t position,
                                 lv_fs_whence_t whence)
{
    (void)driver;
    size_t *current_position = file;
    if (!current_position || whence != LV_FS_SEEK_SET || position > photo_fs_size) {
        return LV_FS_RES_INV_PARAM;
    }
    *current_position = position;
    return LV_FS_RES_OK;
}

static lv_fs_res_t photo_fs_tell(lv_fs_drv_t *driver, void *file, uint32_t *position)
{
    (void)driver;
    size_t *current_position = file;
    if (!current_position || !position) return LV_FS_RES_INV_PARAM;
    *position = (uint32_t)*current_position;
    return LV_FS_RES_OK;
}

static lv_color_t color_from_rgb565(uint16_t color)
{
    uint8_t red = ((color >> 11) & 0x1F) * 255 / 31;
    uint8_t green = ((color >> 5) & 0x3F) * 255 / 63;
    uint8_t blue = (color & 0x1F) * 255 / 31;
    return lv_color_make(red, green, blue);
}

static void set_scaled_position(lv_obj_t *object, int x, int y)
{
    lv_obj_set_pos(object, UI_SCALE_X(x), UI_SCALE_Y(y));
}

static void set_scaled_size(lv_obj_t *object, int width, int height)
{
    lv_obj_set_size(object, UI_SCALE_X(width), UI_SCALE_Y(height));
}

static void flush_display(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    display_hw_flush(display, area, pixels);
}

static uint32_t lvgl_tick(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int x = 0;
    int y = 0;
    touch_pressed = touch_driver_read(&x, &y);
    if (touch_pressed) {
        touch_x = x;
        touch_y = y;
    }
    data->state = touch_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = touch_x;
    data->point.y = touch_y;
}

esp_err_t display_init(void)
{
    lv_init();
    lv_fs_drv_init(&photo_fs_driver);
    photo_fs_driver.letter = PHOTO_FS_LETTER;
    photo_fs_driver.ready_cb = photo_fs_ready;
    photo_fs_driver.open_cb = photo_fs_open;
    photo_fs_driver.close_cb = photo_fs_close;
    photo_fs_driver.read_cb = photo_fs_read;
    photo_fs_driver.seek_cb = photo_fs_seek;
    photo_fs_driver.tell_cb = photo_fs_tell;
    lv_fs_drv_register(&photo_fs_driver);
    lv_tick_set_cb(lvgl_tick);
    lv_display = lv_display_create(BOARD_DISPLAY_WIDTH, BOARD_DISPLAY_HEIGHT);
    if (!lv_display) return ESP_ERR_NO_MEM;
    lv_display_set_color_format(lv_display, LV_COLOR_FORMAT_RGB565_SWAPPED);
    size_t buffer_size = (size_t)BOARD_DISPLAY_WIDTH *
                         BOARD_DISPLAY_BUFFER_ROWS * sizeof(uint16_t);
    memory_allocation_t allocation = MEMORY_ALLOC_DMA;
    lv_display_render_mode_t render_mode = LV_DISPLAY_RENDER_MODE_PARTIAL;
#if CONFIG_TID_BOARD_JC3248W535C
    buffer_size = (size_t)BOARD_DISPLAY_WIDTH * BOARD_DISPLAY_HEIGHT *
                  sizeof(uint16_t);
    allocation = MEMORY_ALLOC_LARGE;
    render_mode = LV_DISPLAY_RENDER_MODE_FULL;
#endif
    display_buffer = memory_alloc(buffer_size, allocation);
    if (!display_buffer) return ESP_ERR_NO_MEM;
    lv_display_set_buffers(lv_display, display_buffer, NULL, buffer_size,
                           render_mode);
    lv_display_set_flush_cb(lv_display, flush_display);
    ESP_RETURN_ON_ERROR(display_hw_init(lv_display), TAG,
                        "initialize display hardware");
    screen = lv_screen_active();
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, color_from_rgb565(0x0841), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    touch_indev = lv_indev_create();
    if (!touch_indev) return ESP_ERR_NO_MEM;
    lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(touch_indev, lv_display);
    lv_indev_set_read_cb(touch_indev, lvgl_touch_read);
    lv_refr_now(lv_display);
#if CONFIG_TID_BOARD_JC3248W535C
    ESP_LOGI(TAG, "%s ready at %ux%u with LVGL full rendering", BOARD_NAME,
             BOARD_DISPLAY_WIDTH, BOARD_DISPLAY_HEIGHT);
#else
    ESP_LOGI(TAG, "%s ready at %ux%u with LVGL partial rendering", BOARD_NAME,
             BOARD_DISPLAY_WIDTH, BOARD_DISPLAY_HEIGHT);
#endif
    return ESP_OK;
}

void display_process(void)
{
    lv_timer_handler();
}

bool display_touch_read(int *x, int *y)
{
    if (x) *x = (touch_x * UI_SCALE_X_DEN) / UI_SCALE_X_NUM;
    if (y) *y = (touch_y * UI_SCALE_Y_DEN) / UI_SCALE_Y_NUM;
    return touch_pressed;
}

static lv_obj_t *new_object(void)
{
    if (!screen) {
        ESP_LOGE(TAG, "Cannot draw before LVGL display initialization");
        return NULL;
    }

    lv_obj_t *object = lv_obj_create(screen);
    if (!object) {
        ESP_LOGE(TAG, "LVGL could not allocate a display object");
        return NULL;
    }
    lv_obj_remove_style_all(object);
    lv_obj_set_clickable(object, false);
    lv_obj_set_scrollable(object, false);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    return object;
}

void display_fill_rect(int x, int y, int width, int height, uint16_t color)
{
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > DISPLAY_WIDTH) width = DISPLAY_WIDTH - x;
    if (y + height > DISPLAY_HEIGHT) height = DISPLAY_HEIGHT - y;
    if (width <= 0 || height <= 0) return;

    lv_obj_t *object = new_object();
    if (!object) return;
    set_scaled_position(object, x, y);
    set_scaled_size(object, width, height);
    lv_obj_set_style_bg_color(object, color_from_rgb565(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, 0, 0);
}

void display_fill_rect_opa(int x, int y, int width, int height, uint16_t color,
                           uint8_t opacity)
{
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > DISPLAY_WIDTH) width = DISPLAY_WIDTH - x;
    if (y + height > DISPLAY_HEIGHT) height = DISPLAY_HEIGHT - y;
    if (width <= 0 || height <= 0) return;

    lv_obj_t *object = new_object();
    if (!object) return;
    set_scaled_position(object, x, y);
    set_scaled_size(object, width, height);
    lv_obj_set_style_bg_color(object, color_from_rgb565(color), 0);
    lv_obj_set_style_bg_opa(object, opacity, 0);
    lv_obj_set_style_radius(object, 0, 0);
}

void display_fill_round_rect(int x, int y, int width, int height, uint16_t color)
{
    display_fill_round_rect_radius(x, y, width, height, height / 2, color);
}

void display_fill_round_rect_radius(int x, int y, int width, int height,
                                    int radius, uint16_t color)
{
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > DISPLAY_WIDTH) width = DISPLAY_WIDTH - x;
    if (y + height > DISPLAY_HEIGHT) height = DISPLAY_HEIGHT - y;
    if (width <= 0 || height <= 0) return;

    lv_obj_t *object = new_object();
    if (!object) return;
    set_scaled_position(object, x, y);
    set_scaled_size(object, width, height);
    lv_obj_set_style_bg_color(object, color_from_rgb565(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, radius, 0);
}

void display_clear(uint16_t color)
{
    if (!screen) return;
    lv_obj_clean(screen);
    line_count = 0;
    page_timer_arc = NULL;
    lv_obj_set_style_bg_color(screen, color_from_rgb565(color), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
}

void display_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    if (!screen) return;
    if (line_count >= LVGL_LINE_CAPACITY) {
        ESP_LOGE(TAG, "LVGL line capacity exceeded");
        return;
    }

    x0 = UI_SCALE_X(x0);
    y0 = UI_SCALE_Y(y0);
    x1 = UI_SCALE_X(x1);
    y1 = UI_SCALE_Y(y1);
    int left = x0 < x1 ? x0 : x1;
    int top = y0 < y1 ? y0 : y1;
    lv_point_precise_t *points = line_points[line_count++];
    points[0].x = x0 - left;
    points[0].y = y0 - top;
    points[1].x = x1 - left;
    points[1].y = y1 - top;

    lv_obj_t *object = lv_line_create(screen);
    if (!object) {
        line_count--;
        ESP_LOGE(TAG, "LVGL could not allocate a line object");
        return;
    }
    lv_obj_remove_style_all(object);
    lv_obj_set_clickable(object, false);
    lv_obj_set_scrollable(object, false);
    lv_obj_set_pos(object, left, top);
    lv_obj_set_size(object, (x0 > x1 ? x0 - x1 : x1 - x0) + 1,
                    (y0 > y1 ? y0 - y1 : y1 - y0) + 1);
    lv_obj_set_style_line_color(object, color_from_rgb565(color), 0);
    lv_obj_set_style_line_width(object, 1, 0);
    lv_obj_set_style_line_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_line_rounded(object, true, 0);
    lv_line_set_points(object, points, 2);
}

void display_circle(int center_x, int center_y, int radius, uint16_t color)
{
    if (radius <= 0) return;
    lv_obj_t *object = new_object();
    if (!object) return;
    int x_radius = UI_SCALE_X(radius);
    int y_radius = UI_SCALE_Y(radius);
    int diameter_x = x_radius * 2 + 1;
    int diameter_y = y_radius * 2 + 1;
    set_scaled_position(object, center_x - radius, center_y - radius);
    lv_obj_set_size(object, diameter_x, diameter_y);
    lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(object, color_from_rgb565(color), 0);
    lv_obj_set_style_border_width(object, 1, 0);
}

void display_fill_circle(int center_x, int center_y, int radius, uint16_t color)
{
    if (radius <= 0) return;
    lv_obj_t *object = new_object();
    if (!object) return;
    int diameter_x = UI_SCALE_X(radius) * 2 + 1;
    int diameter_y = UI_SCALE_Y(radius) * 2 + 1;
    set_scaled_position(object, center_x - radius, center_y - radius);
    lv_obj_set_size(object, diameter_x, diameter_y);
    lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(object, color_from_rgb565(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
}

void display_set_page_timer(uint8_t elapsed_seconds)
{
    if (!screen) return;
    if (!page_timer_arc) {
        page_timer_arc = lv_arc_create(screen);
        if (!page_timer_arc) {
            ESP_LOGE(TAG, "LVGL could not allocate the page timer arc");
            return;
        }
        lv_obj_remove_style_all(page_timer_arc);
        set_scaled_size(page_timer_arc, 14, 14);
        set_scaled_position(page_timer_arc, 207, 225);
        lv_obj_set_clickable(page_timer_arc, false);
        lv_obj_set_scrollable(page_timer_arc, false);
        lv_arc_set_rotation(page_timer_arc, 270);
        lv_arc_set_bg_angles(page_timer_arc, 0, 360);
        lv_obj_set_style_arc_color(page_timer_arc, color_from_rgb565(0xA514),
                                   LV_PART_MAIN);
        lv_obj_set_style_arc_opa(page_timer_arc, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_arc_width(page_timer_arc, 2, LV_PART_MAIN);
        lv_obj_set_style_arc_color(page_timer_arc, color_from_rgb565(0xFD20),
                                   LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(page_timer_arc, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(page_timer_arc, 2, LV_PART_INDICATOR);
        lv_obj_remove_style(page_timer_arc, NULL, LV_PART_KNOB);
    }

    if (elapsed_seconds > 60) elapsed_seconds = 60;
    lv_arc_set_angles(page_timer_arc, 0, (int32_t)elapsed_seconds * 6);
}

static void tide_chart_draw_event(lv_event_t *event)
{
    lv_draw_task_t *task = lv_event_get_draw_task(event);
    if (!task || lv_draw_task_get_type(task) != LV_DRAW_TASK_TYPE_LINE) return;

    lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(task);
    if (!base || base->part != LV_PART_ITEMS) return;

    lv_draw_line_dsc_t *line = lv_draw_task_get_line_dsc(task);
    if (!line || !line->points || line->point_cnt < 2) return;

    lv_area_t chart_area;
    lv_obj_get_coords(lv_event_get_target_obj(event), &chart_area);

    lv_draw_triangle_dsc_t triangle;
    lv_draw_triangle_dsc_init(&triangle);
    triangle.color = color_from_rgb565(0x7DFF);
    triangle.opa = LV_OPA_COVER;

    lv_draw_rect_dsc_t rectangle;
    lv_draw_rect_dsc_init(&rectangle);
    rectangle.bg_color = color_from_rgb565(0x7DFF);
    rectangle.bg_opa = LV_OPA_COVER;
    rectangle.border_width = 0;
    rectangle.radius = 0;

    for (uint32_t i = 0; i + 1 < line->point_cnt; i++) {
        lv_point_precise_t p1 = line->points[i];
        lv_point_precise_t p2 = line->points[i + 1];
        if (p1.x == LV_DRAW_LINE_POINT_NONE || p1.y == LV_DRAW_LINE_POINT_NONE ||
            p2.x == LV_DRAW_LINE_POINT_NONE || p2.y == LV_DRAW_LINE_POINT_NONE) {
            continue;
        }

        int32_t lower_y = LV_MAX(p1.y, p2.y);
        triangle.p[0] = p1;
        triangle.p[1] = p2;
        triangle.p[2].x = p1.y < p2.y ? p1.x : p2.x;
        triangle.p[2].y = lower_y;
        lv_draw_triangle(base->layer, &triangle);

        if (p1.x < p2.x && lower_y < chart_area.y2) {
            lv_area_t fill_area = {
                .x1 = p1.x,
                .y1 = lower_y,
                .x2 = p2.x - 1,
                .y2 = chart_area.y2,
            };
            lv_draw_rect(base->layer, &rectangle, &fill_area);
        }
    }

    lv_draw_line(base->layer, line);
}

esp_err_t display_draw_tide_chart(const int32_t *values, size_t count,
                                  int32_t minimum, int32_t maximum)
{
    if (!screen || !values || count < 2 || maximum < minimum) {
        return ESP_ERR_INVALID_ARG;
    }

    lv_obj_t *chart = lv_chart_create(screen);
    if (!chart) {
        ESP_LOGE(TAG, "LVGL could not allocate the tide chart");
        return ESP_ERR_NO_MEM;
    }

    lv_obj_remove_style_all(chart);
    set_scaled_position(chart, 42, 119);
    set_scaled_size(chart, 236, 50);
    lv_obj_set_clickable(chart, false);
    lv_obj_set_scrollable(chart, false);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chart, 0, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_rounded(chart, true, LV_PART_ITEMS);
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);

    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, (uint32_t)count);
    lv_chart_set_div_line_count(chart, 0, 0);
    if (maximum == minimum) {
        minimum--;
        maximum++;
    }
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, minimum, maximum);

    lv_chart_series_t *series =
        lv_chart_add_series(chart, color_from_rgb565(0xFD20),
                            LV_CHART_AXIS_PRIMARY_Y);
    if (!series) {
        ESP_LOGE(TAG, "LVGL could not allocate the tide chart data");
        lv_obj_delete(chart);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < count; i++) {
        lv_chart_set_next_value(chart, series, values[i]);
    }

    lv_obj_add_event_cb(chart, tide_chart_draw_event,
                        LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_set_send_draw_task_events(chart, true);
    lv_chart_refresh(chart);
    return ESP_OK;
}

esp_err_t display_draw_qr_code(int x, int y, int size, const char *data)
{
    if (!screen || !data || size <= 0 || x < 0 || y < 0 ||
        x + size > DISPLAY_WIDTH || y + size > DISPLAY_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    lv_obj_t *qr = lv_qrcode_create(screen);
    if (!qr) return ESP_ERR_NO_MEM;
    lv_qrcode_set_size(qr, UI_SCALE_X(size));
    lv_qrcode_set_quiet_zone(qr, true);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    set_scaled_position(qr, x, y);
    if (lv_qrcode_update(qr, data, (uint32_t)strlen(data)) != LV_RESULT_OK) {
        lv_obj_delete(qr);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t display_draw_image(int x, int y, const lv_image_dsc_t *image)
{
    if (!screen || !image || !image->data || !image->header.w || !image->header.h) {
        return ESP_ERR_INVALID_ARG;
    }
    int width = (int)image->header.w;
    int height = (int)image->header.h;
    if (x < 0 || y < 0 || x + width > DISPLAY_WIDTH ||
        y + height > DISPLAY_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    lv_obj_t *object = lv_image_create(screen);
    if (!object) return ESP_ERR_NO_MEM;
    lv_image_set_src(object, image);
    set_scaled_position(object, x, y);
    lv_image_set_scale_x(object,
        (uint32_t)(UI_SCALE_X_NUM * LV_SCALE_NONE / UI_SCALE_X_DEN));
    lv_image_set_scale_y(object,
        (uint32_t)(UI_SCALE_Y_NUM * LV_SCALE_NONE / UI_SCALE_Y_DEN));
    return ESP_OK;
}

static event_thumbnail_cache_entry_t *find_event_thumbnail(const char *url)
{
    if (!url || !url[0]) return NULL;
    for (size_t i = 0; i < DATA_EVENT_COUNT; i++) {
        event_thumbnail_cache_entry_t *entry = &event_thumbnail_cache[i];
        if (entry->url[0] && strcmp(entry->url, url) == 0) {
            entry->last_used = ++event_thumbnail_use_counter;
            return entry;
        }
    }
    return NULL;
}

bool display_event_thumbnail_cached(const char *url)
{
    return find_event_thumbnail(url) != NULL;
}

static esp_err_t decode_event_thumbnail(const uint8_t *data, size_t size,
                                        uint16_t *pixels)
{
    photo_fs_data = data;
    photo_fs_size = size;

    lv_image_decoder_dsc_t decoder;
    lv_image_decoder_args_t args = {.no_cache = true};
    if (lv_image_decoder_open(&decoder, PHOTO_FS_PATH, &args) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "LVGL could not open event JPEG (%u bytes)",
                 (unsigned)size);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t source_width = decoder.header.w;
    uint32_t source_height = decoder.header.h;
    if (!source_width || !source_height ||
        decoder.header.cf != LV_COLOR_FORMAT_RGB888) {
        lv_image_decoder_close(&decoder);
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t scaled_width;
    uint32_t scaled_height;
    if ((uint64_t)EVENT_THUMBNAIL_WIDTH * source_height <=
        (uint64_t)EVENT_THUMBNAIL_HEIGHT * source_width) {
        scaled_width = EVENT_THUMBNAIL_WIDTH;
        scaled_height = source_height * EVENT_THUMBNAIL_WIDTH / source_width;
    } else {
        scaled_height = EVENT_THUMBNAIL_HEIGHT;
        scaled_width = source_width * EVENT_THUMBNAIL_HEIGHT / source_height;
    }
    if (!scaled_width) scaled_width = 1;
    if (!scaled_height) scaled_height = 1;

    int32_t source_x[EVENT_THUMBNAIL_WIDTH];
    int32_t source_y[EVENT_THUMBNAIL_HEIGHT];
    int offset_x = (EVENT_THUMBNAIL_WIDTH - (int)scaled_width) / 2;
    int offset_y = (EVENT_THUMBNAIL_HEIGHT - (int)scaled_height) / 2;
    for (int x = 0; x < EVENT_THUMBNAIL_WIDTH; x++) {
        source_x[x] = x < offset_x || x >= offset_x + (int)scaled_width
            ? -1
            : (int32_t)((uint32_t)(x - offset_x) * source_width / scaled_width);
    }
    for (int y = 0; y < EVENT_THUMBNAIL_HEIGHT; y++) {
        source_y[y] = y < offset_y || y >= offset_y + (int)scaled_height
            ? -1
            : (int32_t)((uint32_t)(y - offset_y) * source_height / scaled_height);
    }
    memset(pixels, 0, EVENT_THUMBNAIL_BYTES);

    lv_area_t full_area = {
        .x1 = 0,
        .y1 = 0,
        .x2 = (lv_coord_t)source_width - 1,
        .y2 = (lv_coord_t)source_height - 1,
    };
    lv_area_t decoded_area = {
        .x1 = LV_COORD_MIN,
        .y1 = LV_COORD_MIN,
        .x2 = LV_COORD_MIN,
        .y2 = LV_COORD_MIN,
    };
    uint32_t max_areas = ((source_width + 7U) / 8U) *
                         ((source_height + 7U) / 8U);
    bool complete = false;
    esp_err_t err = ESP_FAIL;
    for (uint32_t i = 0; i < max_areas; i++) {
        if (lv_image_decoder_get_area(&decoder, &full_area, &decoded_area) !=
            LV_RESULT_OK) {
            break;
        }
        const lv_draw_buf_t *area = decoder.decoded;
        if (!area || !area->data || area->header.cf != LV_COLOR_FORMAT_RGB888 ||
            area->header.stride < (uint32_t)lv_area_get_width(&decoded_area) * 3U) {
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }

        const uint8_t *rgb = area->data;
        uint32_t stride = area->header.stride;
        for (int y = 0; y < EVENT_THUMBNAIL_HEIGHT; y++) {
            int32_t sy = source_y[y];
            if (sy < decoded_area.y1 || sy > decoded_area.y2) continue;
            for (int x = 0; x < EVENT_THUMBNAIL_WIDTH; x++) {
                int32_t sx = source_x[x];
                if (sx < decoded_area.x1 || sx > decoded_area.x2) continue;
                const uint8_t *pixel =
                    rgb + (size_t)(sy - decoded_area.y1) * stride +
                    (size_t)(sx - decoded_area.x1) * 3U;
                pixels[y * EVENT_THUMBNAIL_WIDTH + x] =
                    lv_color_to_u16(lv_color_make(pixel[0], pixel[1], pixel[2]));
            }
        }

        if (decoded_area.x2 + 1 >= (lv_coord_t)source_width &&
            decoded_area.y2 + 1 >= (lv_coord_t)source_height) {
            complete = true;
            err = ESP_OK;
            break;
        }
    }
    lv_image_decoder_close(&decoder);
    return complete ? err : ESP_FAIL;
}

static event_thumbnail_cache_entry_t *event_thumbnail_cache_slot(void)
{
    event_thumbnail_cache_entry_t *oldest = &event_thumbnail_cache[0];
    for (size_t i = 0; i < DATA_EVENT_COUNT; i++) {
        event_thumbnail_cache_entry_t *entry = &event_thumbnail_cache[i];
        if (!entry->url[0]) return entry;
        if (entry->last_used < oldest->last_used) oldest = entry;
    }
    return oldest;
}

esp_err_t display_draw_event_thumbnail(int x, int y, const char *url,
                                       const uint8_t *data, size_t size)
{
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;
    size_t url_length = strnlen(url, DATA_EVENT_IMAGE_URL_LENGTH);
    if (url_length >= DATA_EVENT_IMAGE_URL_LENGTH) return ESP_ERR_INVALID_SIZE;

    event_thumbnail_cache_entry_t *entry = find_event_thumbnail(url);
    if (entry) return display_draw_image(x, y, &entry->image);
    if (!data || size < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        return ESP_ERR_NOT_FOUND;
    }

    /* free any previously cached thumbnail to keep heap usage minimal */
    for (size_t i = 0; i < DATA_EVENT_COUNT; i++) {
        if (event_thumbnail_cache[i].pixels) {
            memory_free(event_thumbnail_cache[i].pixels);
            event_thumbnail_cache[i].pixels = NULL;
        }
    }

    uint16_t *pixels = memory_alloc(EVENT_THUMBNAIL_BYTES, MEMORY_ALLOC_LARGE);
    if (!pixels) return ESP_ERR_NO_MEM;
    esp_err_t err = decode_event_thumbnail(data, size, pixels);
    if (err != ESP_OK) {
        memory_free(pixels);
        ESP_LOGW(TAG, "Could not create event thumbnail: %s",
                 esp_err_to_name(err));
        return err;
    }

    entry = event_thumbnail_cache_slot();
    memory_free(entry->pixels);
    entry->pixels = pixels;
    memcpy(entry->url, url, url_length + 1);
    entry->last_used = ++event_thumbnail_use_counter;
    entry->image = (lv_image_dsc_t) {
        .header = {
            .magic = LV_IMAGE_HEADER_MAGIC,
            .cf = LV_COLOR_FORMAT_RGB565,
            .w = EVENT_THUMBNAIL_WIDTH,
            .h = EVENT_THUMBNAIL_HEIGHT,
            .stride = EVENT_THUMBNAIL_WIDTH * sizeof(uint16_t),
        },
        .data_size = EVENT_THUMBNAIL_BYTES,
        .data = (const uint8_t *)pixels,
    };
    return display_draw_image(x, y, &entry->image);
}

static esp_err_t draw_jpeg(int x, int y, const uint8_t *data, size_t size)
{
    if (!screen || !data || size < 4 ||
        data[0] != 0xFF || data[1] != 0xD8) {
        return ESP_ERR_INVALID_ARG;
    }
    photo_fs_data = data;
    photo_fs_size = size;
    lv_image_header_t header = {0};
    if (lv_image_decoder_get_info(PHOTO_FS_PATH, &header) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "LVGL could not decode JPEG image (%u bytes)",
                 (unsigned)size);
        return ESP_ERR_NOT_SUPPORTED;
    }
    lv_obj_t *object = lv_image_create(screen);
    if (!object) return ESP_ERR_NO_MEM;
    lv_image_set_src(object, PHOTO_FS_PATH);
    set_scaled_position(object, x, y);
    return ESP_OK;
}

esp_err_t display_draw_jpeg(int x, int y, const uint8_t *data, size_t size)
{
    return draw_jpeg(x, y, data, size);
}

esp_err_t display_draw_photo_jpeg(int x, int y, const uint8_t *data, size_t size)
{
    return draw_jpeg(x, y, data, size);
}

static const lv_font_t *font_for_scale(uint8_t scale)
{
    return scale == 2 ? &lv_font_german_24 : &lv_font_german_12;
}

static void display_label(int x, int y, int width, int height, const char *text,
                          uint16_t foreground, uint16_t background,
                          uint8_t scale, bool transparent)
{
    if (!screen || !text || !text[0] || !scale || scale > 2) return;
    lv_obj_t *label = lv_label_create(screen);
    if (!label) {
        ESP_LOGE(TAG, "LVGL could not allocate a text label");
        return;
    }
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    set_scaled_position(label, x, y);
    if (width > 0 && height > 0) {
        set_scaled_size(label, width, height);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_set_style_text_font(label, font_for_scale(scale), 0);
    lv_obj_set_style_text_color(label, color_from_rgb565(foreground), 0);
    lv_obj_set_style_bg_color(label, color_from_rgb565(background), 0);
    lv_obj_set_style_bg_opa(label, transparent ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    lv_obj_set_style_border_width(label, 0, 0);
}

void display_text_bg(int x, int y, const char *text, uint16_t foreground,
                     uint16_t background, uint8_t scale)
{
    display_label(x, y, 0, 0, text, foreground, background, scale, false);
}

void display_text(int x, int y, const char *text, uint16_t color, uint8_t scale)
{
    display_label(x, y, 0, 0, text, color, 0x0841, scale, true);
}

void display_text_box(int x, int y, int width, int height, const char *text,
                      uint16_t color, uint8_t scale)
{
    display_label(x, y, width, height, text, color, 0x0841, scale, true);
}

void display_draw_frame(void)
{
    display_clear(0x0841);
    display_fill_rect(0, 0, DISPLAY_WIDTH, 32, 0x10A3);
}

void display_draw_message(const char *title, const char *body)
{
    display_draw_frame();
    display_text_bg(12, 6, title, 0xFFFF, 0x10A3, 2);
    display_text(12, 52, body, 0xFD20, 1);
}
