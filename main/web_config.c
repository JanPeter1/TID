#include "web_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"

static const char *TAG = "web_config";
static httpd_handle_t server;

static bool setup_ap_required(httpd_req_t *request)
{
    if (config_setup_ap_active()) return true;
    httpd_resp_set_status(request, "503 Service Unavailable");
    httpd_resp_sendstr(request, "Setup-Portal ist ausgeschaltet.");
    return false;
}

static esp_err_t root_get(httpd_req_t *request)
{
    if (!setup_ap_required(request)) return ESP_OK;
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    const char *page =
        "<!doctype html><html lang='de'><meta name='viewport' content='width=device-width'>"
        "<title>Texel-Display Setup</title><style>body{font:18px sans-serif;max-width:560px;margin:2em auto;padding:0 1em;background:#101827;color:#eee}"
        "input{box-sizing:border-box;width:100%;padding:.7em;margin:.3em 0 1em}button{padding:.8em;background:#ff9800;border:0}</style>"
        "<h1>Texel-Display</h1><p>Netze eintragen. Leere Felder bleiben unveraendert. "
        "Passwoerter werden nicht wieder angezeigt.</p><form method='post'>"
        "<h2>WLAN (bis zu 3 Netze)</h2>";
    httpd_resp_sendstr_chunk(request, page);
    for (int i = 0; i < CONFIG_WIFI_NETWORKS; i++) {
        char section[256];
        snprintf(section, sizeof(section),
                 "<label>WLAN %d - SSID</label><input name='ssid%d' maxlength='32'>"
                 "<label>WLAN %d - Passwort</label><input name='pass%d' type='password' maxlength='64'>",
                 i + 1, i, i + 1, i);
        httpd_resp_sendstr_chunk(request, section);
    }
    httpd_resp_sendstr_chunk(request,
        "<button type='submit'>Speichern und WLAN neu verbinden</button></form></html>");
    return httpd_resp_sendstr_chunk(request, NULL);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *text)
{
    char *read = text;
    char *write = text;
    while (*read) {
        if (*read == '+') {
            *write++ = ' ';
            read++;
        } else if (*read == '%' && hex_value(read[1]) >= 0 && hex_value(read[2]) >= 0) {
            *write++ = (char)((hex_value(read[1]) << 4) | hex_value(read[2]));
            read += 3;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
}

static void read_field(const char *body, const char *name, char *out, size_t out_size)
{
    out[0] = '\0';
    size_t name_length = strlen(name);
    const char *cursor = body;
    while (cursor && *cursor) {
        const char *end = strchr(cursor, '&');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if (length > name_length && strncmp(cursor, name, name_length) == 0 &&
            cursor[name_length] == '=') {
            size_t value_length = length - name_length - 1;
            if (value_length >= out_size) value_length = out_size - 1;
            memcpy(out, cursor + name_length + 1, value_length);
            out[value_length] = '\0';
            url_decode(out);
            return;
        }
        cursor = end ? end + 1 : NULL;
    }
}

static esp_err_t root_post(httpd_req_t *request)
{
    if (!setup_ap_required(request)) return ESP_OK;
    if (request->content_len <= 0 || request->content_len > 2048) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Ungueltige Formularlaenge");
        return ESP_FAIL;
    }
    char *body = calloc(1, request->content_len + 1);
    if (!body) return ESP_ERR_NO_MEM;
    int received = httpd_req_recv(request, body, request->content_len);
    if (received != request->content_len) {
        free(body);
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Formular unvollstaendig");
        return ESP_FAIL;
    }

    device_config_t config;
    esp_err_t err = config_load(&config);
    if (err != ESP_OK) {
        free(body);
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Konfiguration nicht lesbar");
        return err;
    }

    for (int i = 0; i < CONFIG_WIFI_NETWORKS; i++) {
        char key[12];
        char value[CONFIG_SSID_SIZE];
        snprintf(key, sizeof(key), "ssid%d", i);
        read_field(body, key, value, sizeof(value));
        if (value[0]) {
            snprintf(config.ssid[i], sizeof(config.ssid[i]), "%s", value);
            snprintf(key, sizeof(key), "pass%d", i);
            read_field(body, key, config.wifi_password[i], sizeof(config.wifi_password[i]));
        }
    }
    free(body);

    err = config_save(&config);
    if (err != ESP_OK) {
        config_clear(&config);
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Speichern fehlgeschlagen");
        return err;
    }
    wifi_config_t station = {0};
    bool has_network = false;
    for (int i = 0; i < CONFIG_WIFI_NETWORKS; i++) {
        if (config.ssid[i][0]) {
            size_t ssid_length = strnlen(config.ssid[i], sizeof(station.sta.ssid));
            size_t password_length = strnlen(config.wifi_password[i],
                                             sizeof(station.sta.password));
            memcpy(station.sta.ssid, config.ssid[i], ssid_length);
            memcpy(station.sta.password, config.wifi_password[i], password_length);
            has_network = true;
            break;
        }
    }
    config_clear(&config);
    if (has_network) {
        esp_wifi_disconnect();
        err = esp_wifi_set_config(WIFI_IF_STA, &station);
        if (err != ESP_OK) {
            httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "WLAN-Einstellungen konnten nicht aktiviert werden");
            return err;
        }
        err = esp_wifi_connect();
        if (err != ESP_OK) {
            httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "WLAN-Verbindung konnte nicht gestartet werden");
            return err;
        }
    }
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_sendstr(request,
        "<!doctype html><html lang='de'><meta charset='utf-8'><h1>Gespeichert</h1>"
        "<p>Das Geraet verbindet WLAN neu. Diese Seite kann geschlossen werden.</p></html>");
    ESP_LOGI(TAG, "Settings updated through the setup portal");
    return ESP_OK;
}

esp_err_t web_config_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "start config web server");
    httpd_uri_t get = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    httpd_uri_t post = {.uri = "/", .method = HTTP_POST, .handler = root_post};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get), TAG, "register config page");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &post), TAG, "register config submit");
    return ESP_OK;
}
