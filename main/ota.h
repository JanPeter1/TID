#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define OTA_MANIFEST_URL \
    "https://familie-riegel.de/tx94/firmware/manifest.json"
#define OTA_FIRMWARE_BASE_URL \
    "https://familie-riegel.de/tx94/firmware/"
#define OTA_FIRMWARE_FILE "firmware.bin"
#define OTA_FIRMWARE_MAX_SIZE (1U * 1024U * 1024U)  /* 1 MiB */

#define OTA_VERSION_MAX_LENGTH 32
#define OTA_SHA256_MAX_LENGTH 64

typedef struct {
    char version[OTA_VERSION_MAX_LENGTH];
    char sha256[OTA_SHA256_MAX_LENGTH + 1];
    char file[OTA_FIRMWARE_MAX_SIZE > 0 ? 128 : 128];
    uint32_t size;
    bool available;
} ota_manifest_t;

typedef enum {
    OTA_STATUS_IDLE,
    OTA_STATUS_CHECKING,
    OTA_STATUS_UPDATE_AVAILABLE,
    OTA_STATUS_NO_UPDATE,
    OTA_STATUS_DOWNLOADING,
    OTA_STATUS_WRITING,
    OTA_STATUS_SUCCESS,
    OTA_STATUS_FAILED,
} ota_status_t;

typedef void (*ota_status_callback_t)(ota_status_t status, const char *message);

/* Initialize OTA subsystem. Must be called after config_init and data_init. */
esp_err_t ota_init(void);

/* Check for a firmware update. Downloads and parses the manifest. */
esp_err_t ota_check_update(ota_manifest_t *manifest);

/* Download and install the firmware update described by manifest.
 * This function blocks until the update completes or fails.
 * On success the device will reboot automatically.
 * Returns ESP_OK if the update was triggered, ESP_ERR_INVALID_STATE if no
 * update is available, or an error code on failure. */
esp_err_t ota_start_update(const ota_manifest_t *manifest);

/* Get the current OTA status. */
ota_status_t ota_get_status(void);

/* Get the current download progress (0-100 percent). */
uint8_t ota_get_progress(void);

/* Set a callback that is invoked when the OTA status changes. */
void ota_set_status_callback(ota_status_callback_t callback);

/* Get the current firmware version string. */
const char *ota_get_version(void);

/* Get the last error message. */
const char *ota_get_error(void);