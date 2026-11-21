#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_format.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "memory.h"

#include "image_cache.h"

#include "display.h"
#include "ui.h"

#include "esp_crt_bundle.h"

static const char *TAG = "ota";

#define OTA_MANIFEST_JSON_SIZE 2048

static ota_status_t s_status = OTA_STATUS_IDLE;
static ota_status_callback_t s_callback = NULL;
static char s_error[256] = {0};
static ota_manifest_t s_current_manifest = {0};
static uint32_t s_download_progress = 0;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool overflow;
} response_buffer_t;

typedef struct {
    uint32_t *bytes_written;
    const esp_partition_t *partition;
    uint8_t *sha256_data;
    size_t sha256_len;
    size_t sha256_capacity;
    uint32_t total_size;
} download_ctx_t;

static void set_status(ota_status_t new_status, const char *msg)
{
    s_status = new_status;
    if (msg)
    {
        strncpy(s_error, msg, sizeof(s_error) - 1);
        s_error[sizeof(s_error) - 1] = '\0';
    }
    ESP_LOGI(TAG, "Status: %d - %s", new_status, msg ? msg : "");
    if (s_callback)
    {
        s_callback(new_status, msg ? msg : "");
    }
}

static esp_err_t json_event_handler(esp_http_client_event_t *evt)
{
    response_buffer_t *response = (response_buffer_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !response || evt->data_len <= 0)
    {
        return ESP_OK;
    }
    size_t incoming = (size_t)evt->data_len;
    size_t available = response->capacity - response->length - 1;
    if (incoming > available)
    {
        if (available)
        {
            memcpy(response->data + response->length, evt->data, available);
            response->length += available;
            response->data[response->length] = '\0';
        }
        response->overflow = true;
        return ESP_OK;
    }
    memcpy(response->data + response->length, evt->data, incoming);
    response->length += incoming;
    response->data[response->length] = '\0';
    return ESP_OK;
}

static bool http_get_json(const char *url, char *output, size_t output_size)
{
    response_buffer_t response = {
        .data = output,
        .capacity = output_size,
    };
    output[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = json_event_handler,
        .user_data = &response,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        return false;
    }

    memory_tls_lock();
    esp_err_t err = esp_http_client_perform(client);
    memory_tls_unlock();
    int status = esp_http_client_get_status_code(client);
    bool success = err == ESP_OK && status >= 200 && status < 300 && !response.overflow;

    if (!success && !response.overflow)
    {
        ESP_LOGW(TAG, "GET JSON failed: %s, HTTP %d, URL %s",
                 esp_err_to_name(err), status, url);
        if (err != ESP_OK)
        {
            memory_log_heap(TAG, "after failed HTTPS request");
        }
    }
    else if (response.overflow)
    {
        ESP_LOGW(TAG, "JSON response truncated at %u bytes", (unsigned)response.length);
    }

    esp_http_client_cleanup(client);
    return success;
}

static bool parse_manifest(const char *json, ota_manifest_t *manifest)
{
    cJSON *root = cJSON_Parse(json);
    if (!root)
    {
        ESP_LOGE(TAG, "Failed to parse manifest JSON");
        return false;
    }

    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *sha256 = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    cJSON *file = cJSON_GetObjectItemCaseSensitive(root, "file");
    cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");

    bool ok = cJSON_IsString(version) && cJSON_IsString(sha256) &&
              cJSON_IsString(file) && cJSON_IsNumber(size);

    if (ok)
    {
        strncpy(manifest->version, version->valuestring, sizeof(manifest->version) - 1);
        manifest->version[sizeof(manifest->version) - 1] = '\0';
        strncpy(manifest->sha256, sha256->valuestring, sizeof(manifest->sha256) - 1);
        manifest->sha256[sizeof(manifest->sha256) - 1] = '\0';
        strncpy(manifest->file, file->valuestring, sizeof(manifest->file) - 1);
        manifest->file[sizeof(manifest->file) - 1] = '\0';
        manifest->size = (uint32_t)size->valuedouble;
        manifest->available = true;
    }
    else
    {
        manifest->available = false;
        ESP_LOGW(TAG, "Manifest missing required fields");
    }

    cJSON_Delete(root);
    return ok;
}

static esp_err_t download_event_handler(esp_http_client_event_t *evt)
{
    download_ctx_t *ctx = (download_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0 && ctx)
    {
        uint32_t written = *ctx->bytes_written;

        esp_err_t err = esp_partition_write(ctx->partition, written, evt->data, evt->data_len);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Partition write failed at offset %u: %s",
                     (unsigned)written, esp_err_to_name(err));
            return ESP_FAIL;
        }

        if (ctx->sha256_len + evt->data_len <= ctx->sha256_capacity)
        {
            memcpy(ctx->sha256_data + ctx->sha256_len, evt->data, evt->data_len);
            ctx->sha256_len += evt->data_len;
        }

        *ctx->bytes_written = written + evt->data_len;
        if (ctx->total_size > 0)
        {
            s_download_progress = (*ctx->bytes_written * 1000) / ctx->total_size;
        }
        ESP_LOGD(TAG, "Downloaded %u/%u bytes",
                 (unsigned)*ctx->bytes_written,
                 (unsigned)ctx->sha256_capacity);
        ui_redraw();
    }
    return ESP_OK;
}

static bool download_firmware(const ota_manifest_t *manifest, const esp_partition_t *partition,
                              uint32_t *bytes_written, uint8_t *sha256_data,
                              size_t sha256_capacity, size_t *sha256_len)
{
    char firmware_url[256];
    snprintf(firmware_url, sizeof(firmware_url), "%s%s", OTA_FIRMWARE_BASE_URL, manifest->file);

    ESP_LOGI(TAG, "Downloading firmware from: %s", firmware_url);

    uint8_t *buffer = memory_alloc(8192, MEMORY_ALLOC_LARGE);
    if (!buffer)
    {
        set_status(OTA_STATUS_FAILED, "Buffer allocation failed");
        return false;
    }
    (void)buffer;

    *bytes_written = 0;
    *sha256_len = 0;

    download_ctx_t ctx = {
        .bytes_written = bytes_written,
        .partition = partition,
        .sha256_data = sha256_data,
        .sha256_len = 0,
        .sha256_capacity = sha256_capacity,
        .total_size = manifest->size,
    };

    esp_http_client_config_t config = {
        .url = firmware_url,
        .event_handler = download_event_handler,
        .user_data = &ctx,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .buffer_size = 4096,
        .buffer_size_tx = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        set_status(OTA_STATUS_FAILED, "HTTP client init failed");
        return false;
    }

    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    set_status(OTA_STATUS_DOWNLOADING, "Downloading firmware...");
    memory_tls_lock();
    esp_err_t err = esp_http_client_perform(client);
    memory_tls_unlock();
    int status = esp_http_client_get_status_code(client);

    *sha256_len = ctx.sha256_len;

    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300)
    {
        snprintf(s_error, sizeof(s_error), "Download failed: %s, HTTP %d",
                 esp_err_to_name(err), status);
        ESP_LOGE(TAG, "%s", s_error);
        return false;
    }

    if (*bytes_written != manifest->size)
    {
        snprintf(s_error, sizeof(s_error), "Incomplete download: %u/%u bytes",
                 (unsigned)*bytes_written, (unsigned)manifest->size);
        ESP_LOGE(TAG, "%s", s_error);
        return false;
    }

    ESP_LOGI(TAG, "Firmware download complete: %u bytes", (unsigned)*bytes_written);
    return true;
}

esp_err_t ota_init(void)
{
    ESP_LOGI(TAG, "OTA init");

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();

    ESP_LOGI(TAG, "Running partition: %s @ 0x%08x (%u KB)",
             running->label, (unsigned)running->address,
             (unsigned)(running->size / 1024));
    ESP_LOGI(TAG, "Boot partition: %s @ 0x%08x (%u KB)",
             boot->label, (unsigned)boot->address,
             (unsigned)(boot->size / 1024));

    esp_ota_img_states_t ota_state;
    esp_err_t err = esp_ota_get_state_partition(running, &ota_state);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Running partition state: %d", ota_state);
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY)
        {
            ESP_LOGW(TAG, "Previous OTA update pending verification, marking as valid");
            err = esp_ota_mark_app_valid_cancel_rollback();
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to mark app valid: %s", esp_err_to_name(err));
            }
        }
    }

    return ESP_OK;
}

#define OTA_MANIFEST_JSON_SIZE 2048

esp_err_t ota_check_update(ota_manifest_t *manifest)
{
    ESP_LOGI(TAG, "Checking for firmware update at %s", OTA_MANIFEST_URL);

    set_status(OTA_STATUS_CHECKING, "Checking for updates...");

    char *json = memory_alloc(OTA_MANIFEST_JSON_SIZE, MEMORY_ALLOC_LARGE);
    if (!json)
    {
        set_status(OTA_STATUS_FAILED, "Memory allocation failed");
        return ESP_ERR_NO_MEM;
    }

    /* Right after boot the content refresh competes for internal heap; retry. */
    bool ok = false;
    for (int attempt = 1; attempt <= 3 && !ok; attempt++)
    {
        ok = http_get_json(OTA_MANIFEST_URL, json, OTA_MANIFEST_JSON_SIZE);
        if (!ok && attempt < 3)
        {
            ESP_LOGW(TAG, "Manifest fetch attempt %d failed, retrying in 15 s", attempt);
            vTaskDelay(pdMS_TO_TICKS(15000));
        }
    }
    if (!ok)
    {
        memory_free(json);
        set_status(OTA_STATUS_FAILED, "Failed to fetch manifest");
        return ESP_FAIL;
    }

    bool parsed = parse_manifest(json, manifest);
    memory_free(json);

    if (!parsed)
    {
        set_status(OTA_STATUS_FAILED, "Failed to parse manifest");
        return ESP_FAIL;
    }

    const char *current_version = ota_get_version();
    ESP_LOGI(TAG, "Current version: %s, Available: %s", current_version, manifest->version);

    if (strcmp(current_version, manifest->version) == 0)
    {
        ESP_LOGI(TAG, "Already on latest version");
        set_status(OTA_STATUS_NO_UPDATE, "Already on latest version");
        manifest->available = false;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "New version available: %s", manifest->version);
    set_status(OTA_STATUS_UPDATE_AVAILABLE, "Update available");
    memcpy(&s_current_manifest, manifest, sizeof(ota_manifest_t));
    return ESP_OK;
}

esp_err_t ota_start_update(const ota_manifest_t *manifest)
{
    if (!manifest || !manifest->available)
    {
        ESP_LOGE(TAG, "No valid update manifest provided");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    ESP_LOGI(TAG, "Current: %s, Target: %s", running->label, next->label);
    ESP_LOGI(TAG, "Firmware size: %u bytes, Partition size: %u bytes",
             (unsigned)manifest->size, (unsigned)next->size);

    if (manifest->size > next->size)
    {
        snprintf(s_error, sizeof(s_error), "Firmware too large: %u > %u",
                 (unsigned)manifest->size, (unsigned)next->size);
        set_status(OTA_STATUS_FAILED, s_error);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Buffer for SHA256 verification (we hash the downloaded data directly) */
    uint8_t *sha256_buffer = memory_alloc(manifest->size > sizeof(uint32_t)
                                              ? manifest->size
                                              : sizeof(uint32_t),
                                          MEMORY_ALLOC_LARGE);
    if (!sha256_buffer)
    {
        set_status(OTA_STATUS_FAILED, "SHA256 buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    set_status(OTA_STATUS_DOWNLOADING, "Downloading firmware...");

    esp_err_t err = esp_partition_erase_range(next, 0, next->size);
    if (err != ESP_OK)
    {
        snprintf(s_error, sizeof(s_error), "Partition erase failed: %s", esp_err_to_name(err));
        set_status(OTA_STATUS_FAILED, s_error);
        memory_free(sha256_buffer);
        return err;
    }

    uint32_t bytes_written = 0;
    size_t sha256_len = 0;
    bool ok = download_firmware(manifest, next, &bytes_written,
                                sha256_buffer, manifest->size, &sha256_len);
    if (!ok)
    {
        memory_free(sha256_buffer);
        return ESP_FAIL;
    }

    set_status(OTA_STATUS_WRITING, "Verifying SHA256...");

    psa_hash_operation_t op = psa_hash_operation_init();
    psa_status_t status = psa_hash_setup(&op, PSA_ALG_SHA_256);
    if (status != PSA_SUCCESS)
    {
        ESP_LOGE(TAG, "psa_hash_setup failed: %d", status);
        memory_free(sha256_buffer);
        set_status(OTA_STATUS_FAILED, "SHA256 setup failed");
        return ESP_FAIL;
    }
    status = psa_hash_update(&op, sha256_buffer, bytes_written);
    if (status != PSA_SUCCESS)
    {
        ESP_LOGE(TAG, "psa_hash_update failed: %d", status);
        psa_hash_abort(&op);
        memory_free(sha256_buffer);
        set_status(OTA_STATUS_FAILED, "SHA256 update failed");
        return ESP_FAIL;
    }
    uint8_t hash[32];
    size_t hash_len = 0;
    status = psa_hash_finish(&op, hash, sizeof(hash), &hash_len);
    if (status != PSA_SUCCESS || hash_len != 32)
    {
        ESP_LOGE(TAG, "psa_hash_finish failed: %d", status);
        psa_hash_abort(&op);
        memory_free(sha256_buffer);
        set_status(OTA_STATUS_FAILED, "SHA256 finish failed");
        return ESP_FAIL;
    }
    char calc_hex[65] = {0};
    for (int i = 0; i < 32; i++)
    {
        snprintf(calc_hex + i * 2, 3, "%02x", hash[i]);
    }
    bool verified = strcasecmp(calc_hex, manifest->sha256) == 0;
    if (!verified)
    {
        ESP_LOGE(TAG, "SHA256 mismatch: expected %s, got %s", manifest->sha256, calc_hex);
    }
    else
    {
        ESP_LOGI(TAG, "SHA256 verified OK");
    }
    memory_free(sha256_buffer);

    if (!verified)
    {
        set_status(OTA_STATUS_FAILED, "SHA256 verification failed");
        return ESP_FAIL;
    }

    set_status(OTA_STATUS_WRITING, "Setting boot partition...");

    err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK)
    {
        snprintf(s_error, sizeof(s_error), "Set boot partition failed: %s",
                 esp_err_to_name(err));
        set_status(OTA_STATUS_FAILED, s_error);
        return err;
    }

    ESP_LOGI(TAG, "Boot partition set to %s, rebooting...", next->label);
    set_status(OTA_STATUS_SUCCESS, "Update successful, rebooting...");

    vTaskDelay(pdMS_TO_TICKS(500));
    image_cache_clear();
    esp_restart();

    return ESP_OK;
}

ota_status_t ota_get_status(void)
{
    return s_status;
}

uint8_t ota_get_progress(void)
{
    return (uint8_t)(s_download_progress / 10);
}

void ota_set_status_callback(ota_status_callback_t callback)
{
    s_callback = callback;
}

const char *ota_get_version(void)
{
    static char version[OTA_VERSION_MAX_LENGTH] = {0};

    if (version[0] == '\0')
    {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_app_desc_t app_desc;
        if (esp_ota_get_partition_description(running, &app_desc) == ESP_OK)
        {
            strncpy(version, app_desc.version, sizeof(version) - 1);
            version[sizeof(version) - 1] = '\0';
        }
        else
        {
            strcpy(version, "unknown");
        }
    }

    return version;
}

const char *ota_get_error(void)
{
    return s_error[0] ? s_error : "No error";
}