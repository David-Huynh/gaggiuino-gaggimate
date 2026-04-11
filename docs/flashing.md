# Flashing Guide

Flashing GaggiMate requires two separate uploads: the **firmware** and the **filesystem** (web UI). Both must be flashed for the device to work correctly. Skipping the filesystem upload results in a blank web interface.

## Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- [Node.js + npm](https://nodejs.org/) (for building the web UI)

---

## Step 1 — Build the web UI

The web UI must be built before the filesystem image can be created. Run this once, and again whenever you change files in `web/`.

**Linux / macOS / Git Bash on Windows:**
```bash
scripts/build_spiffs.sh
```

After this step, `data/w/` will contain the gzipped web app files.

---

## Step 2 — Flash the firmware

In PlatformIO, select your environment (e.g. `display-headless-uart`) and click **Upload**, or via CLI:

```bash
pio run -e display-headless-uart -t upload
```

---

## Step 3 — Flash the filesystem (web UI)

This is a **separate upload** from the firmware. In PlatformIO, click **Upload Filesystem Image**, or via CLI:

```bash
pio run -e display-headless-uart -t uploadfs
```

> **Important:** You must redo this step any time you rebuild the web UI (Step 1).

---

## Environments

| Environment | Board | Notes |
|---|---|---|
| `display-headless-uart` | ESP32-S3 DevKitC-1 | Headless, communicates with controller over UART |
| `display-headless` | LilyGo T-RGB | Headless with display board |
| `display-headless-8m` | Seeed XIAO ESP32-S3 | 8MB flash variant |
| `display-headless-4m` | ESP32-S3 SuperMini | 4MB flash variant |
| `display` | LilyGo T-RGB | Full display build |
| `controller` | GaggiMate Controller | Controller firmware (no web UI) |

---

## Initial Setup (headless)

After flashing both firmware and filesystem:

1. Power on the device — it will broadcast a WiFi AP named **GaggiMate**
2. Connect your phone or laptop to that network
3. A captive portal will open automatically (or navigate to `http://4.4.4.1/`)
4. Enter your home WiFi credentials and save
5. The device will restart and connect to your network
