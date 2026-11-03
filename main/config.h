#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define CONFIG_WIFI_NETWORKS 3
#define CONFIG_SSID_SIZE 33
#define CONFIG_PASSWORD_SIZE 65
#define CONFIG_CALENDAR_USER_SIZE 65
#define CONFIG_CALENDAR_PASSWORD_SIZE 129

typedef struct {
    char ssid[CONFIG_WIFI_NETWORKS][CONFIG_SSID_SIZE];
    char wifi_password[CONFIG_WIFI_NETWORKS][CONFIG_PASSWORD_SIZE];
    char calendar_user[CONFIG_CALENDAR_USER_SIZE];
    char calendar_password[CONFIG_CALENDAR_PASSWORD_SIZE];
} device_config_t;

esp_err_t config_init(void);
esp_err_t config_load(device_config_t *out);
esp_err_t config_save(const device_config_t *config);
void config_clear(device_config_t *config);
const char *config_ap_ssid(void);
bool config_wifi_connected(void);
int config_wifi_rssi(void);
bool config_setup_ap_active(void);
bool config_ntp_synced(void);
esp_err_t config_start_setup_ap(void);
