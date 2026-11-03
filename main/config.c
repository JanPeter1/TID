#include "config.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";
static const char *NVS_NAMESPACE = "texel";
static device_config_t stored_config;
static char setup_ssid[24];
static bool connected;
static bool setup_ap_active;
static bool sntp_started;
static volatile bool sntp_synced;
static int active_network = -1;
static int network_attempts;

static void time_sync_notification(struct timeval *tv)
{
    (void)tv;
    sntp_synced = true;
}

static void start_sntp(void)
{
    if (sntp_started) return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "nl.pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification);
    esp_sntp_init();
    sntp_started = true;
    ESP_LOGI(TAG, "SNTP started after Wi-Fi obtained an IP address");
}

static bool configure_network(int start_index)
{
    for (int offset = 0; offset < CONFIG_WIFI_NETWORKS; offset++) {
        int index = (start_index + offset) % CONFIG_WIFI_NETWORKS;
        if (!stored_config.ssid[index][0]) continue;
        wifi_config_t station = {0};
        size_t ssid_length = strnlen(stored_config.ssid[index], sizeof(station.sta.ssid));
        size_t password_length = strnlen(stored_config.wifi_password[index],
                                         sizeof(station.sta.password));
        memcpy(station.sta.ssid, stored_config.ssid[index], ssid_length);
        memcpy(station.sta.password, stored_config.wifi_password[index], password_length);
        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &station);
        if (err == ESP_OK) {
            active_network = index;
            network_attempts++;
            err = esp_wifi_connect();
            if (err == ESP_OK) return true;
            ESP_LOGW(TAG, "Could not start Wi-Fi connection: %s", esp_err_to_name(err));
            network_attempts--;
        } else {
            ESP_LOGW(TAG, "Could not apply saved Wi-Fi network: %s", esp_err_to_name(err));
        }
    }
    return false;
}

static bool has_saved_wifi(void)
{
    for (int i = 0; i < CONFIG_WIFI_NETWORKS; i++) {
        if (stored_config.ssid[i][0]) return true;
    }
    return false;
}

static void wifi_retry_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (!connected && has_saved_wifi()) {
            network_attempts = 0;
            configure_network((active_network + 1 + CONFIG_WIFI_NETWORKS) % CONFIG_WIFI_NETWORKS);
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        connected = false;
        if (network_attempts < CONFIG_WIFI_NETWORKS) {
            configure_network((active_network + 1 + CONFIG_WIFI_NETWORKS) % CONFIG_WIFI_NETWORKS);
        } else {
            network_attempts = 0;
            ESP_LOGW(TAG, "All saved Wi-Fi networks failed; will retry");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        connected = true;
        network_attempts = 0;
        ESP_LOGI(TAG, "Connected to Wi-Fi");
        start_sntp();
    }
}

esp_err_t config_load(device_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(out, 0, sizeof(*out));
        return ESP_OK;
    }
    if (err != ESP_OK) return err;
    size_t size = sizeof(*out);
    err = nvs_get_blob(handle, "device", out, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(out, 0, sizeof(*out));
        return ESP_OK;
    }
    if (err == ESP_OK && size != sizeof(*out)) return ESP_ERR_INVALID_SIZE;
    return err;
}

esp_err_t config_save(const device_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "open config storage");
    esp_err_t err = nvs_set_blob(handle, "device", config, sizeof(*config));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) memcpy(&stored_config, config, sizeof(stored_config));
    return err;
}

void config_clear(device_config_t *config)
{
    if (config) memset(config, 0, sizeof(*config));
}

static void make_setup_credentials(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(setup_ssid, sizeof(setup_ssid), "Texel-Info-%02X%02X",
             mac[4], mac[5]);
}

const char *config_ap_ssid(void) { return setup_ssid; }
bool config_wifi_connected(void) { return connected; }
bool config_setup_ap_active(void) { return setup_ap_active; }
bool config_ntp_synced(void) { return sntp_synced; }

esp_err_t config_start_setup_ap(void)
{
    if (setup_ap_active) return ESP_OK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "enable setup AP mode");
    wifi_config_t ap = {
        .ap = {
            .ssid_len = 0,
            .channel = 1,
            .max_connection = 2,
            .authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = {.required = false},
        },
    };
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", setup_ssid);
    ap.ap.ssid_len = strlen(setup_ssid);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_LOGE(TAG, "Could not configure open setup AP: %s", esp_err_to_name(err));
        return err;
    }

    setup_ap_active = true;
    ESP_LOGW(TAG, "Open setup AP started as %s",
             setup_ssid);
    return ESP_OK;
}

esp_err_t config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase incompatible NVS");
        ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "initialize NVS");
    } else {
        ESP_RETURN_ON_ERROR(err, TAG, "initialize NVS");
    }
    ESP_RETURN_ON_ERROR(config_load(&stored_config), TAG, "load config");
    make_setup_credentials();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "initialize network interface");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "create event loop");
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_config), TAG, "initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL), TAG, "register Wi-Fi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL), TAG, "register IP events");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi");
    configure_network(0);
    if (xTaskCreate(wifi_retry_task, "wifi_retry", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Setup AP is disabled; tap SETUP on the display to enable it");
    return ESP_OK;
}
