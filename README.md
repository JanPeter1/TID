# Texel-Info-Display

ESP-IDF firmware for the ESP32-2432S028 CYD (ILI9341 display and XPT2046 touch) described in [PRD.md](PRD.md). The screen is landscape 320x240 with icon tabs and left/right buttons that cycle through subpages of the selected section.

## Status

This is the first hardware-oriented project baseline. It includes a native SPI display/touch driver, weather and marine data retrieval, a protected local configuration page, and a five-section interface with multiple subpages per section. The source-defined VVV page, exact HTML selectors, photo manifest, and signed OTA manifest are not yet known, so those sections are intentionally marked as unavailable rather than showing fabricated content.

## Build and flash

1. Install Espressif ESP-IDF (the project uses the ESP-IDF extension/toolchain, not Arduino).
2. Open this folder in VS Code and select target `esp32`.
3. Use **ESP-IDF: Build Project**, then **ESP-IDF: Flash**.
4. On first boot, tap **SETUP** at the bottom right. The device starts an open setup access point and displays its SSID and portal address. Connect to that network and open `http://192.168.4.1/`; no login is required.
5. Enter the home Wi-Fi details and calendar credentials. Settings are stored in NVS and are not part of the source tree.

The setup AP is off by default and starts only when **SETUP** is tapped. Both the AP and setup portal are open while enabled; portal routes return unavailable when the AP is off. The portal uses HTTP, not HTTPS, so enable the AP only while configuring the device and disconnect it afterward by rebooting.

## Data sources

- Weather: Open-Meteo Forecast API, location 53.124279, 4.812830, Europe/Amsterdam. Requests run at startup when Wi-Fi is available and then every six hours.
- Marine: Open-Meteo Marine API. The firmware finds the next local sea-level extremum as an approximate high/low-water event. Coastal model values are only indicative and are not for navigation.
- House information and calendar: HTML is fetched from the URLs in `main/data.c`; the calendar uses Basic authentication configured on the device. Current HTML conversion is a deliberately small text fallback, not a site-specific parser.
- VVV events and photos: disabled until the VVV URL/selectors and photo manifest schema are agreed. The expected photo files are JPEG, 320x200 pixels.

Weather data attribution is shown on the weather tab. Open-Meteo's free service is intended for non-commercial usage and requires CC BY attribution.

## Configuration and security notes

- Do not commit Wi-Fi or calendar credentials. Provision them through the device setup page; they are stored in NVS.
- NVS storage is not encrypted by this baseline. Anyone with physical access to flash may recover saved credentials. Enable ESP-IDF NVS encryption/flash encryption before treating the device as physically secure.
- HTTPS requests validate server certificates using ESP-IDF's certificate bundle.
- OTA is not enabled yet. The server needs a defined version manifest, firmware URL, signing/verification scheme, and an update process tested against the 4 MB partition layout before automatic installation is safe.
- The screen and touch pins follow the ESP_CoGo CYD setup. Confirm orientation, touch calibration, and display behavior on the actual board before unattended use.
- Touch uses the CYD's separate SPI3 bus (MOSI GPIO 32, MISO GPIO 39, SCLK GPIO 25, CS GPIO 33, IRQ GPIO 36); the TFT uses SPI2. SNTP starts after Wi-Fi obtains an IP address and logs successful synchronization outside the LWIP callback. The bottom status bar shows local date and time using the `Europe/Amsterdam` daylight-saving rules.

## Project layout

- `main/display.*`: ILI9341 initialization and basic text/shape drawing over SPI.
- `main/touch.*`: XPT2046 pressure and X/Y SPI sampling with initial touch-coordinate mapping.
- `main/config.*`: on-demand open setup AP and NVS-backed settings.
- `main/web_config.*`: password-protected setup form.
- `main/data.*`: Open-Meteo/Marine requests and website text retrieval.
- `partitions.csv`: dual OTA application slots for a 4 MB ESP32.

The GPIO and SPI APIs use the ESP-IDF 6 split components `esp_driver_gpio` and
`esp_driver_spi`, declared directly by the main component.
