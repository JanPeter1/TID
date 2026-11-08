# Agent Configuration

This project currently does not use background agents for automation or task delegation. All functionality is implemented in the main ESP-IDF firmware application.

## No Agents Required

The Texel-Info-Display firmware uses a monolithic approach with all logic contained within the FreeRTOS tasks. There are no subagents, background processes managed via Copilot, or external automation tools.

## Architecture Summary

### Main Application (`main/main.c`)
- UI state management and page navigation
- Clock rendering and status display (German time zone)
- LVGL timer handling and object updates
- Touch event processing through XPT2046 driver

### Display subsystem (`main/display.c`, `main/display_hw.c`)
- ILI9341 SPI driver over SPI2 (CYD) or AXS15231B over QSPI (JC3248W535C)
- RGB565 partial buffer flush with color/orientation handling
- Pointer registration and LVGL touch callback integration

### Touch subsystem (`main/touch.c`, `main/touch_hw.c`)
- XPT2046 low-level SPI driver (CYD: SPI3 pins 32/39/25/33/36)
- AXS15231B I²C driver (JC3248W535C) through esp_lcd_touch
- LVGL pointer callback integration

### Data management (`main/data.c`, `main/data.h`)
- Remote HTTP fetching from Open-Meteo, Marine, VVV GraphQL, and calendar APIs
- Local cache management with NVS-backed configuration
- Periodic refresh task (every 6 hours)
- Photo JSON manifest parsing and JPEG download

### UI rendering (`main/ui.c`, `main/ui.h`)
- LVGL object lifecycle: create, update, delete
- German text rendering with Noto Sans umlauts and Montserrat fallbacks
- Navigation logic through LVGL buttons and touch events

### Configuration (`main/config.c`, `main/web_config.c`)
- Wi-Fi station mode and NVS storage
- Setup AP (HTTP, no login) for configuration portal
- SNTP time synchronization after IP acquisition
- QR code generation for external links

### Memory management (`main/memory.c`, `main/memory.h`)
- Stack/heap allocation with board-specific PSRAM awareness
- Configurable limits per component based on BOARD_* macros
- Explicit allocation failure handling through esp_err_t

## Build Requirements

- ESP-IDF 6.1 with VS Code extension
- Target: `esp32` (CYD) or `esp32s3` (JC3248W535C)
- Components from `main/idf_component.yml`: LVGL 9.6, esp_lcd_driver_ili9341/axs15231b, esp_lcd_touch_xpt2046/esp_lcd_touch_i8080

## Safety Guidelines

- TLS enabled for all HTTPS requests
- Credentials configured through setup portal only (never in code)
- Error surface via ESP-IDF logging or returned `esp_err_t` values
- Large buffers allocated from heap, not task stack

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
