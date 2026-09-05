# ESP32-S3 Clock with Screen

A touchscreen desktop dashboard for the **OSPTEK ESP32-S3-Touch-LCD-4 / ESP32-TPCB4**, with four pages: clock, Hong Kong MTR arrivals, nearby aircraft radar, and settings.

## Hardware

| Item | Specification |
| --- | --- |
| Module | ESP32-S3-WROOM-1-N16R8 |
| Memory | 16 MB Flash, 8 MB Octal PSRAM |
| Display | Approximately 3.95 inches, 480 x 480, ST7701S RGB, YDP395B003-V4 |
| Touch | FT5x06 capacitive touch |
| USB serial | CH340K |
| Framework | ESP-IDF 5.5.3, LVGL 9.2.2 |

This project targets the board above. Other ESP32-S3 boards may require different pin assignments, display timings, or memory settings.

## Features

### Clock

- Two oversized clock faces: staggered green digits and overlapping blue digits on black.
- 12/24-hour formats with leading zeros and linear fade transitions.
- A location label on the blue face, with a blinking icon while locating.
- Network status fades out about 10 seconds after connecting; connection or time-sync problems remain visible.
- Swipe between pages. Page indicators appear on touch and fade after about 3 seconds of inactivity.

### MTR Arrivals

- Admiralty on the Island Line and Sheung Shui on the East Rail Line.
- Separate station and direction selectors, remembering the last direction for each station.
- PIDS-style arrival information with Traditional Chinese text, an end-of-service notice, and Hong Kong weather.

### Aircraft Radar

- Nearby aircraft centered on an IP-based location or manually entered coordinates.
- Solid aircraft icons oriented along their tracks, with flight number, aircraft type, altitude, and speed.
- Tap an aircraft to expand its details; the panel returns to its compact form after 15 seconds.
- Data refreshes approximately every 30 seconds, with estimated movement between updates and fade transitions for large position corrections.
- Automatic label placement, alignment relative to each aircraft, and five text-size options.
- Search radii: **20, 50, 70, 100, 120, or 200 km**.
- Aircraft display limits: **5, 8, 10, 13, or 15**.
- A map background with subtle place labels and visible map attribution.

### Settings

Chinese/English interface, theme, brightness, screen timeout, time zone, automatic time synchronization, clock format and style, MTR station/direction, and radar preferences. Settings are saved in the device's NVS.

## Flash a Release

Download `esp32-s3-clock-with-screen-v1.0.0-flash.zip` from [Releases](https://github.com/LiuKinJohn/esp32-s3-clock-with-screen/releases), extract it, and follow the included [FLASHING.md](FLASHING.md). The package contains the bootloader, partition table, application firmware, instructions, and SHA256 checksums. It does not include personal settings or an NVS image.

For initial setup, connect to the device's `ESP32-Clock-Setup` hotspot using the default password `12345678`, then open `http://192.168.4.1` to configure your Wi-Fi. Set up the device on a trusted network. If IP-based location fails, enter coordinates on the setup page. IP location is approximate, not GPS; browser location access may be restricted by secure-context requirements.

## Build from Source

Use an initialized **ESP-IDF 5.5.3** terminal. On Windows, use a short ASCII project path without spaces.

```powershell
Copy-Item sdkconfig.release sdkconfig
idf.py build
# Replace PORT with your device's serial port, such as COM7.
idf.py -p PORT flash monitor
```

`sdkconfig.release` contains the complete release configuration, including the esp32s3 target. Do not first run `set-target`, which resets the configuration. Use this complete configuration rather than an old local config or only `sdkconfig.defaults`.

Dependencies are pinned by `main/idf_component.yml` and `dependencies.lock` and downloaded on the first build. Generated `managed_components/` and `build/` directories are not tracked. The required font and digit-image C resources are already included in `main/`.

```text
main/                 Application, display integration, UI, networking, and generated assets
tools/                Optional asset generators and comparison scripts, including older intermediates
sdkconfig.release     Complete release configuration
sdkconfig.defaults    Original project defaults
dependencies.lock     Pinned component versions
partitions.csv        Flash partition layout
```

## Optional Font Generation

Normal builds do not require Pillow, Node.js, or locally installed source fonts. Optional generators require tools such as Pillow and lv_font_conv, plus appropriately licensed source fonts. The blue clock generator reads its font path from `BLUE_CLOCK_FONT`.

Some original font subsets and temporary conversion tools are no longer available, so byte-for-byte asset regeneration is not guaranteed. Files in `tools/generated_fonts/` are older intermediates: **do not use them to overwrite the current fonts in `main/`**. Original local font files are not distributed. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Data and Privacy

The firmware uses ip-api for approximate location, MTR open arrival data, Hong Kong Observatory weather, ADSB.lol aircraft data, MapMap static maps, and Overpass place-name queries. A custom aircraft API-key setting is not currently implemented.

Location, map, and aircraft requests disclose the public IP address or query coordinates to the respective services. Availability, limits, and accuracy depend on those providers. Keep map attribution visible. This dashboard is not intended for navigation, air traffic control, or other safety-critical use.

The archive excludes personal Wi-Fi credentials, API keys, location records, serial logs, full-flash dumps, private user paths, and IDE settings. The default setup hotspot password is not a personal Wi-Fi password. Normal flashing does not erase saved settings; back up and deliberately clear NVS before sharing a configured device when needed.

## Status and Limitations

- The archived firmware passes an incremental build and has limited on-device testing; long-term stability and all search radii are not fully verified.
- The 20 km map path and map-to-aircraft scale alignment still need on-device checks.
- Network timeouts or rate limits may delay updates. Place labels can fall back to the location name or lack uncommon Chinese characters; maps reload after a restart.

This is a firmware archive for the specified board, not a universal SDK. Third-party code and fonts retain their respective rights and notices; the archive does not apply a single license to all resources.

If you have any questions, please contact this email: silicium@foxmail.com
