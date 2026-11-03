# Project Guidelines

## Project

- Firmware for the ESP32-2432S028 CYD using ESP-IDF 6.1, an ILI9341 display, and an XPT2046 touch controller.
- The screen is 320 x 240 in landscape. TFT uses SPI2; touch uses a separate SPI3 bus (MOSI 32, MISO 39, SCLK 25, CS 33, IRQ 36).
- User-facing text is German. Local date/time uses the `Europe/Amsterdam` timezone.
- See [README.md](README.md) for setup/build instructions and [PRD.md](PRD.md) for product requirements and unfinished work.

## Architecture

- `main/main.c`: UI/page state, navigation logic, and clock/status rendering.
- `main/display.c`: ILI9341 SPI driver, LVGL display/flush and pointer registration, and LVGL drawing helpers.
- `main/touch.c`: low-level XPT2046 driver; LVGL reads it through the pointer callback in `display.c`.
- `main/german_font.c`: Noto Sans glyph subset for German umlauts, with Montserrat fallbacks. Preserve the SIL Open Font License in `assets/fonts/OFL.txt`.
- `main/config.c`: Wi-Fi station, NVS configuration, on-demand setup AP, and SNTP startup.
- `main/web_config.c`: setup portal; it is available without login only while the setup AP is enabled.
- `main/data.c`: remote data retrieval, parsing, caching, and periodic refresh.
- Keep network work out of the UI loop. Protect shared application data with its existing mutex.
- Run LVGL timers, display updates, and all LVGL object operations from the UI task only. Do not call LVGL from data/network tasks.

## Safety and configuration

- Never add Wi-Fi or calendar passwords, tokens, or other credentials to tracked files, logs, or documentation. Configure them on-device through the setup portal.
- The setup AP and portal are intentionally open and use HTTP. Keep the AP off except during brief setup in a trusted environment.
- Keep TLS certificate validation enabled for HTTPS requests.
- Do not place large response buffers or other large objects on task stacks. In particular, keep stack usage within the configured FreeRTOS task size; allocate large network buffers on the heap and handle allocation failure explicitly.
- Surface network, allocation, and peripheral errors through ESP-IDF logging or returned `esp_err_t` values; do not silently report success after failures.

## Build and validation

- Use the VS Code Espressif ESP-IDF extension for ESP-IDF operations. Select target `esp32`, then run **ESP-IDF: Build Project**.
- Flash and hardware-test LVGL flush colors/orientation, text spacing and umlauts, touch navigation, Wi-Fi setup, and NTP behavior on the actual CYD when changes affect those paths.
- There is no host-side automated test suite currently. Keep changes focused and update README/PRD when supported behavior or requirements change.
