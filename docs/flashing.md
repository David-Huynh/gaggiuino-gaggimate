# Flashing Guide

The web UI is embedded in the firmware image. Normal firmware and WebUI updates
therefore require one firmware upload. LittleFS contains profiles and shot
history, not the web application.

## Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- [Node.js + npm](https://nodejs.org/) (for building the web UI)

## Step 1 - Build the web UI

Run this before compiling firmware whenever files in `web/` change:

```bash
scripts/build_webui.sh
```

The generated, gzipped assets are embedded under `src/display/webassets/` by
the firmware build.

## Step 2 - Flash the firmware

Select the matching PlatformIO environment and click **Upload**, or use the
CLI. For an ESP32-S3 N16R8 communicating with the controller over UART:

```bash
pio run -e display-headless-uart-n16r8 -t upload
```

Other UART variants:

```bash
# ESP32-S3 DevKitC-1 N8, no PSRAM
pio run -e display-headless-uart -t upload

# N8 with hardware scale support disabled
pio run -e display-headless-uart-no-hwscale -t upload
```

## Step 3 - Initialize or restore LittleFS when needed

Uploading LittleFS is optional for normal firmware and WebUI updates. Use it
for a fresh installation when seed profiles are wanted, or restore profiles
from a backup after changing partition layouts:

```bash
pio run -e display-headless-uart-n16r8 -t uploadfs
```

Changing from the N8 partition table to N16R8 requires one USB firmware upload,
which writes the new bootloader and partition table. Back up profiles first;
the moved LittleFS partition will be reformatted or restored. Do not attempt the
initial partition migration through OTA. Subsequent N16R8 firmware updates can
use OTA and leave LittleFS untouched.

## Environments

| Environment | Board | Notes |
|---|---|---|
| `display-headless-uart` | ESP32-S3 DevKitC-1 N8 | 8 MB flash, no PSRAM, UART |
| `display-headless-uart-n16r8` | ESP32-S3 DevKitC-1 N16R8 | 16 MB flash, 8 MB OPI PSRAM, UART |
| `display-headless-uart-no-hwscale` | ESP32-S3 DevKitC-1 N8 | UART with hardware scale support disabled |
| `display-headless` | LilyGo T-RGB | Headless build using the N16R8 display board |
| `display-headless-8m` | Seeed XIAO ESP32-S3 | 8 MB flash variant |
| `display-headless-4m` | ESP32-S3 SuperMini | 4 MB flash variant |
| `display` | LilyGo T-RGB | Full display build |
| `controller` | GaggiMate Controller | Controller firmware |
| `stm32f4-no-hwscale` | Black Pill STM32F411CE | STM32 controller without hardware scales |

## Initial Setup (headless)

After flashing the firmware:

1. Power on the device; it will broadcast a WiFi AP named **GaggiMate**.
2. Connect your phone or laptop to that network.
3. Open the captive portal, or navigate to `http://4.4.4.1/`.
4. Enter your home WiFi credentials and save.
5. The device restarts and connects to your network.
