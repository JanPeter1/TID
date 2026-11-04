# Texel-Info-Display

ESP-IDF firmware for the ESP32-2432S028 CYD (320 x 240 ILI9341 display and XPT2046 touch), using LVGL 9.6. The interface has five icon tabs and rounded side controls for browsing subpages. Product requirements and remaining decisions are in [PRD.md](PRD.md).

## Current implementation

- LVGL 9.6 renders the interface to the ILI9341 through a partial RGB565 SPI flush buffer. XPT2046 samples are registered as an LVGL pointer input device; screen navigation is handled in the UI task.
- Montserrat is used for crisp 12 px and 24 px text. Anti-aliased Noto Sans glyphs add German `äöüÄÖÜßé`; the font subset's SIL Open Font License is in `assets/fonts/OFL.txt`.
- Open-Meteo weather and marine requests, cached content, and periodic refresh every six hours. Fetching starts only after the station has obtained an IP address; a pending initial refresh waits for DHCP instead of attempting DNS while associating. The weather section has pages for current conditions, the next five days, and modeled high/low water with a 12-hour trend.
- Weather conditions and daily forecast rows include small WMO-code icons rendered as LVGL objects.
- VVV events are fetched from the page's public GraphQL endpoint; up to 12 today/tomorrow timeslots are cached and shown one per subpage. The compact query requests titles, places, detail slugs, and times only.
- Photo processing generates 256 x 137 baseline JPEGs with a 24 KB limit and a JSON manifest. The firmware downloads a random entry when a photo subpage is opened and decodes it through LVGL's low-memory TJPGD decoder.
- The status bar shows Wi-Fi connection quality through colored signal bars, without a numeric RSSI value. The Wi-Fi icon sits left of the icon-only setup (gear), QR, and deep-sleep controls. LVGL's QR widget opens the source URL for the selected subpage; touch on the XPT2046 wakes the device from deep sleep.
- NVS-backed Wi-Fi/calendar configuration through a local setup portal; the larger content cache uses its own flash NVS partition.
- House information is compiled from `assets/info/pages.txt`; paragraphs, bullet lists, and optional fixed-size images become offline-ready subpages with source-specific QR links.
- SNTP time synchronization after the station receives an IP address; local time follows `Europe/Amsterdam`. The status bar shows time in orange followed by date in white.
- Stack protection and light heap poisoning are enabled to help catch the reported intermittent FreeRTOS/LWIP crash; the TCP/IP stack is increased to 8 KB. Device stability still needs verification after flashing.
- Dual OTA application partitions exist, but OTA update logic is not implemented.

VVV sightseeing recommendations are not implemented. The calendar still uses a minimal HTML-to-text conversion.

## Editing house information

Edit `assets/info/pages.txt`; each `#` heading starts a subpage. Add one `@url` source link, an optional `@image source/<datei>` (optional `fit` or `contain` mode), then plain paragraphs or `- ` list items. Separate pages with `---`. The build-time generator checks that each page fits the display, scales referenced files from `assets/info/source/` to fixed 80 x 60 RGB565 files under `assets/info/images/`, and embeds the result in firmware. It reuses scaled files unless the source is newer or the output is missing or invalid. Original downloads from `familie-riegel.de/tx94/userfiles/images/` are kept under `assets/info/source/`.

## Prepare and deploy photos

Place source photos in `assets/photos/source/` and run `python tools/process_photos.py`. Install dependencies with `python -m pip install -r tools/requirements-photos.txt`. Supported sources include JPEG, PNG, WebP, BMP, and HEIC/HEIF. The script applies EXIF orientation, fits each photo to the display area, and writes baseline JPEGs under `assets/photos/images/` with `manifest.json` containing relative paths, capture dates, and titles. Dates use EXIF `DateTimeOriginal` when available and otherwise the file creation date. Titles use a reverse-geocoded EXIF GPS location; without GPS or if lookup fails, the title falls back to the filename or coordinates. Reverse lookups use Nominatim/OpenStreetMap, are rate-limited, and are cached in the manifest between runs.

Only missing or source-newer images are regenerated. To upload, copy `.env.example` to `.env`, enter the FTP password, then run `python tools/process_photos.py --upload`. This uploads `manifest.json` and all generated JPEGs to `ftp://familie-riegel-de@ftp.familie-riegel.de/tx94/photos/`. The `.env` file is git-ignored; never commit FTP credentials. Set `FTP_USE_TLS=true` in `.env` if the server supports explicit FTPS. The device reads the manifest and JPEG files from the corresponding HTTPS path `https://familie-riegel.de/tx94/photos/`.

## Build and flash
Plain FTP is unencrypted, including credentials and image contents; use a trusted network or enable FTPS when supported.

1. Install ESP-IDF 6.1 and open this project in VS Code with the Espressif ESP-IDF extension.
2. Select target `esp32`.
3. Run **ESP-IDF: Build Project**, then **ESP-IDF: Flash**.

Use the ESP-IDF extension for project operations. The project requires the ESP-IDF 6 split components `esp_driver_gpio` and `esp_driver_spi`; LVGL and the other managed components are declared in `main/idf_component.yml`.

## Initial Wi-Fi setup

1. Tap the gear icon at the bottom right of the display. The setup access point is off by default; tapping starts the open AP and shows its SSID.
2. Connect to that network and open `http://192.168.4.1/`. The portal does not require a login.
3. Enter the home Wi-Fi and calendar credentials. They are stored in NVS; the setup AP remains enabled until reboot.

The other bottom-right icons display a QR code for the active page or put the display into deep sleep. Touch the screen to wake it.

The setup network and portal are both open and use HTTP without TLS. Anyone nearby can connect and change settings while setup mode is active. Enable setup only briefly in a trusted environment. NVS and device flash are not encrypted; someone with physical flash access may recover stored credentials.

## Data sources and limits

- Weather: Open-Meteo Forecast API at 53.124279, 4.812830. The display shows current temperature, wind/gusts, precipitation, humidity, and a five-day daily forecast. Requests run on startup when connected and every six hours thereafter. Open-Meteo is for non-commercial use and requires CC BY attribution.
- Marine: Open-Meteo Marine API. The firmware finds the next modeled local water-level extremum after the current forecast time and shows it with a 12-hour trend. Coastal model levels are approximate and must not be used for navigation or safety decisions.
- House information: compile-time pages and scaled thumbnails are in `assets/info/`; their QR codes open the corresponding source page. Calendar: endpoint is in `main/data.c`, credentials are entered through setup, and HTML parsing is currently a simple text fallback.
- Events: VVV Texel German event overview. A compact GraphQL request retrieves timeslots in the local date range; matching entries are filtered to today and tomorrow and up to 12 are stored.
- Photos: the processor writes `manifest.json` and size-limited JPEGs; the firmware fetches a random photo when a photo subpage is opened. Place names are reverse-geocoded with Nominatim/OpenStreetMap from EXIF GPS coordinates.

HTTPS data requests use the ESP-IDF certificate bundle. Do not commit Wi-Fi or calendar secrets.

## Source layout

- `main/main.c`: UI rendering, navigation, status clock, and touch event handling.
- `main/display.c` / `main/display.h`: ILI9341 SPI initialization, LVGL display flush, pointer input, and drawing helpers.
- `main/german_font.c` / `main/german_font.h`: Noto Sans glyph subset and Montserrat fallbacks; see `assets/fonts/OFL.txt`.
- `main/touch.c` / `main/touch.h`: XPT2046 SPI3 input and coordinate mapping.
- `main/config.c` / `main/config.h`: Wi-Fi, NVS settings, setup AP, and SNTP startup.
- `main/web_config.c` / `main/web_config.h`: local setup portal.
- `main/data.c` / `main/data.h`: remote requests, parsing, refresh task, and cache.
- `assets/info/pages.txt` and `tools/generate_info_pages.py`: source format and build-time generator for the offline information pages.
- `assets/photos/source/`, `tools/process_photos.py`, and `tools/requirements-photos.txt`: source images, photo processor, and its Python dependencies.
- `partitions.csv`: 4 MB flash layout with dual OTA application slots and a dedicated content-cache NVS partition. Flash the partition table along with the firmware after changing this layout.
