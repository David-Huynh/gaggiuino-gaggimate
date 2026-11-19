# November 2026 upstream integration

The merge incorporates 231 commits through upstream `818ca889`.

- Gaggimate Max support adds dual-boiler controls, a setup wizard and
  coordinated heater power. Brew profiles gain ratio-based stops and the
  display gains live charts.
- The UART transport now comes from upstream. Supported Gaggimate ESP32
  controller boards can receive OTA over UART. The Black Pill keeps its pins,
  baud rate, STM32 board support and correlated HX711 tare.
- The fork uses the upstream OTA downloader, embedded WebUI and firmware CA
  bundle. Release URLs and UART image selection point to the fork.
- The history log follows upstream v8, including SD storage, history tools and
  scale telemetry. Earlier v5 through v7 logs remain readable. Recovery
  retains valid older projections and respects bulk history deletion.
- The new WebUI retains EspressoRL's recipe memory, active recommendation
  validation, feedback receipts and prompt navigation. EspressoRL continues
  to receive canonical MQTT messages using the existing event contracts.

The UART payloads changed. Install the matching ESP32 display and STM32
controller images together when updating this machine. See the
[flashing guide](flashing.md) for the board targets and the
[heater startup notes](controller-heating-safety.md) for the session guards.

Nightly builds use the next patch series after the newest reachable stable tag,
with a minimum base of `v1.9.0` and the GitHub run number for ordering. This
keeps the new builds newer than the fork's older `v1.8.36` Nightly. The binary
and `version.txt` use the same version. The updater reports failed checks,
download retries and flash errors in the WebUI.

Use new firmware for the first cold boot, warm restart and one complete
brew-and-feedback cycle on the machine before publishing the fork's OTA assets.
