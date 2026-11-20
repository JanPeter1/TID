#include "image_cache.h"

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "memory.h"

#define IMAGE_CACHE_BASE_PATH "/image_cache"
#define IMAGE_CACHE_PARTITION_LABEL "image_cache"
#define IMAGE_CACHE_MAX_USAGE_PERCENT 60

static const char *TAG = "image_cache";
static bool s_mounted = false;
static SemaphoreHandle_t s_lock;

static uint32_t url_hash(const char *url)
{
    uint32_t hash = 5381;
    for (const char *p = url; *p; p++) {
        hash = ((hash << 5) + hash) + (unsigned char)*p;
    }
    return hash;
}

static void make_filename(const char *url, char *out, size_t out_size)
{
    uint32_t h = url_hash(url);
    snprintf(out, out_size, "%s/%08" PRIx32 ".bin", IMAGE_CACHE_BASE_PATH, h);
}

esp_err_t image_cache_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = IMAGE_CACHE_BASE_PATH,
        .partition_label = IMAGE_CACHE_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    struct stat st;
    if (stat(IMAGE_CACHE_BASE_PATH, &st) != 0) {
        if (mkdir(IMAGE_CACHE_BASE_PATH, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create cache dir");
            return ESP_FAIL;
        }
    }

    s_mounted = true;
    ESP_LOGI(TAG, "Image cache initialized at %s", IMAGE_CACHE_BASE_PATH);
    return ESP_OK;
}

bool image_cache_has(const char *url)
{
    if (!s_mounted || !url) return false;
    char path[128];
    make_filename(url, path, sizeof(path));
    struct stat st;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int exists = stat(path, &st) == 0;
    xSemaphoreGive(s_lock);
    return exists;
}

esp_err_t image_cache_read(const char *url, uint16_t **pixels, size_t *size)
{
    if (!s_mounted || !url || !pixels || !size) return ESP_ERR_INVALID_ARG;
    char path[128];
    make_filename(url, path, sizeof(path));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    FILE *f = fopen(path, "rb");
    if (!f) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > EVENT_THUMBNAIL_BYTES) {
        fclose(f);
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_SIZE;
    }
    *pixels = memory_alloc((size_t)fsize, MEMORY_ALLOC_LARGE);
    if (!*pixels) {
        fclose(f);
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    size_t read = fread(*pixels, 1, (size_t)fsize, f);
    fclose(f);
    xSemaphoreGive(s_lock);
    if (read != (size_t)fsize) {
        memory_free(*pixels);
        *pixels = NULL;
        return ESP_FAIL;
    }
    *size = (size_t)fsize;
    return ESP_OK;
}

esp_err_t image_cache_write(const char *url, const uint16_t *pixels, size_t size)
{
    if (!s_mounted || !url || !pixels || size != EVENT_THUMBNAIL_BYTES) return ESP_ERR_INVALID_ARG;
    char path[128];
    make_filename(url, path, sizeof(path));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    FILE *f = fopen(path, "wb");
    if (!f) {
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    size_t written = fwrite(pixels, 1, size, f);
    fclose(f);
    xSemaphoreGive(s_lock);
    if (written != size) {
        unlink(path);
        return ESP_FAIL;
    }

    image_cache_evict_if_needed();
    return ESP_OK;
}

static void get_usage(size_t *used_bytes, size_t *total_bytes)
{
    *used_bytes = 0;
    *total_bytes = 0;
    lfs_t *lfs = NULL;
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, IMAGE_CACHE_PARTITION_LABEL);
    if (!part) return;
    esp_littlefs_get_lfs(part->label, &lfs);
    if (!lfs) return;
    lfs_soff_t used = lfs_fs_size(lfs);
    if (used >= 0) *used_bytes = (size_t)used;
    *total_bytes = part->size;
}

void image_cache_evict_if_needed(void)
{
    if (!s_mounted) return;

    size_t used, total;
    get_usage(&used, &total);
    if (total == 0) return;

    uint32_t usage_percent = (used * 100) / total;
    if (usage_percent < IMAGE_CACHE_MAX_USAGE_PERCENT) return;

    ESP_LOGI(TAG, "Cache usage %" PRIu32 "%% > %d%%, evicting oldest", usage_percent, IMAGE_CACHE_MAX_USAGE_PERCENT);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    DIR *dir = opendir(IMAGE_CACHE_BASE_PATH);
    if (!dir) {
        xSemaphoreGive(s_lock);
        return;
    }

    struct dirent *entry;
    struct stat oldest_st;
    char oldest_path[128] = {0};
    bool found = false;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        char path[128];
        snprintf(path, sizeof(path), "%s/%s", IMAGE_CACHE_BASE_PATH, entry->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (!found || st.st_mtime < oldest_st.st_mtime) {
            oldest_st = st;
            snprintf(oldest_path, sizeof(oldest_path), "%s", path);
            found = true;
        }
    }
    closedir(dir);

    if (found) {
        unlink(oldest_path);
        ESP_LOGD(TAG, "Evicted %s", oldest_path);
    }
    xSemaphoreGive(s_lock);
}

void image_cache_clear(void)
{
    if (!s_mounted) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    DIR *dir = opendir(IMAGE_CACHE_BASE_PATH);
    if (!dir) {
        xSemaphoreGive(s_lock);
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG) continue;
        char path[128];
        snprintf(path, sizeof(path), "%s/%s", IMAGE_CACHE_BASE_PATH, entry->d_name);
        unlink(path);
    }
    closedir(dir);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Image cache cleared");
}