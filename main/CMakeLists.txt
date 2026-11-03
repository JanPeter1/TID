idf_component_register(
    SRCS "main.c" "display.c" "german_font.c" "touch.c" "data.c" "config.c" "web_config.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif esp_http_server esp_http_client nvs_flash
             app_update esp_partition cjson esp-tls esp_driver_gpio esp_driver_spi
             esp_timer lwip lvgl
)
