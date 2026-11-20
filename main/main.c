#include <stdlib.h>
#include <time.h>

#include "config.h"
#include "data.h"
#include "display.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "image_cache.h"
#include "memory.h"
#include "ota.h"
#include "touch.h"
#include "ui.h"
#include "web_config.h"

#define OTA_NIGHTLY_HOUR 3

static TickType_t delay_until_next_nightly_check(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    if (!localtime_r(&now, &tm_now)) {
        return pdMS_TO_TICKS(3600000);
    }
    int current_seconds = tm_now.tm_hour * 3600 + tm_now.tm_min * 60 + tm_now.tm_sec;
    int target_seconds = OTA_NIGHTLY_HOUR * 3600;
    int delay_seconds = target_seconds - current_seconds;
    if (delay_seconds <= 0) {
        delay_seconds += 86400;
    }
    return pdMS_TO_TICKS((uint64_t)delay_seconds * 1000);
}

void ota_task(void *unused)
{
    (void)unused;

    while (!config_wifi_connected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* Wait for SNTP to sync time before doing HTTPS certificate validation */
    while (!config_ntp_synced()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ota_manifest_t manifest = {0};
    esp_err_t err = ota_check_update(&manifest);
    if (err == ESP_OK && manifest.available) {
        ESP_LOGI("ota", "Initial check: update available, version %s", manifest.version);
    }

    vTaskDelay(delay_until_next_nightly_check());

    while (true) {
        while (!config_wifi_connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        while (!config_ntp_synced()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        ota_manifest_t manifest = {0};
        esp_err_t err = ota_check_update(&manifest);
        if (err == ESP_OK && manifest.available) {
            ESP_LOGI("ota", "Nightly check: update available, version %s", manifest.version);
        } else {
            ESP_LOGI("ota", "Nightly check: %s",
                     err == ESP_OK ? "no update" : esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(86400000ULL));
    }
}

void app_main(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();

    memory_tls_init();
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(image_cache_init());
    ESP_ERROR_CHECK(touch_driver_init());
    ESP_ERROR_CHECK(config_init());
    ESP_ERROR_CHECK(web_config_start());
    ESP_ERROR_CHECK(data_init());
    ESP_ERROR_CHECK(ota_init());
    xTaskCreate(ota_task, "ota_task", 16384, NULL, 5, NULL);
    ESP_ERROR_CHECK(ui_start());
}
