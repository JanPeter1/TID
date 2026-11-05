set(INFO_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../assets/info")
set(INFO_INPUT "${INFO_ASSET_DIR}/pages.txt")
set(INFO_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/info_pages_generated.c")
set(SPLASH_IMAGE "${CMAKE_CURRENT_SOURCE_DIR}/../assets/photos/source/IMG_20261102_081843.jpg")
file(GLOB INFO_SOURCE_IMAGES "${INFO_ASSET_DIR}/source/*")

idf_component_register(
    SRCS "touch.c" "main.c" "ui.c" "display.c" "display_hw.c" "german_font.c"
         "data.c" "config.c" "web_config.c" "memory.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif esp_http_server esp_http_client nvs_flash
             app_update esp_partition cjson esp-tls esp_driver_gpio esp_driver_spi
             esp_driver_i2c esp_lcd esp_timer lwip lvgl esp_lcd_axs15231b
)

add_custom_target(generate_info_pages
             COMMAND "${PYTHON}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_info_pages.py"
                     --input "${INFO_INPUT}"
                     --assets "${INFO_ASSET_DIR}"
                     --splash-image "${SPLASH_IMAGE}"
                     --output "${INFO_OUTPUT}"
             DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_info_pages.py"
                 "${INFO_INPUT}"
                 "${SPLASH_IMAGE}"
                 ${INFO_SOURCE_IMAGES}
             VERBATIM
)
set_source_files_properties("${INFO_OUTPUT}" PROPERTIES GENERATED TRUE)
target_sources(${COMPONENT_LIB} PRIVATE "${INFO_OUTPUT}")
add_dependencies(${COMPONENT_LIB} generate_info_pages)
