set(INFO_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../assets/info")
set(INFO_INPUT "${INFO_ASSET_DIR}/pages.txt")
set(INFO_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/info_pages_generated.c")
file(GLOB INFO_SOURCE_IMAGES "${INFO_ASSET_DIR}/source/*")

idf_component_register(
    SRCS "main.c" "display.c" "german_font.c" "touch.c" "data.c" "config.c" "web_config.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif esp_http_server esp_http_client nvs_flash
             app_update esp_partition cjson esp-tls esp_driver_gpio esp_driver_spi
             esp_timer lwip lvgl
)

add_custom_target(generate_info_pages
             COMMAND "${PYTHON}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_info_pages.py"
                     --input "${INFO_INPUT}"
                     --assets "${INFO_ASSET_DIR}"
                     --output "${INFO_OUTPUT}"
             DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_info_pages.py"
                 "${INFO_INPUT}"
                 ${INFO_SOURCE_IMAGES}
             VERBATIM
)
set_source_files_properties("${INFO_OUTPUT}" PROPERTIES GENERATED TRUE)
target_sources(${COMPONENT_LIB} PRIVATE "${INFO_OUTPUT}")
add_dependencies(${COMPONENT_LIB} generate_info_pages)
