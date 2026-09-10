# September 2026 upstream integration

Merged upstream `071e8899` (47 commits since the previous common ancestor).

## User-visible changes

- OTA retries interrupted downloads, resumes with validated HTTP ranges, and relaxes/restores watchdog deadlines during flash writes. Failed updates report a failure rather than a finished state.
- The embedded web UI is updated with the firmware. OTA does not overwrite profiles or shot storage.
- WebSocket status separates changed machine state from live telemetry. UART hardware-scale readings, pending brew starts, EspressoRL prompts and durable feedback remain available.
- Upstream button combinations, hold-to-flush, brew warnings, pressure offset, pumped-water telemetry, and shot-analyzer improvements are integrated.
- Saved-shot recovery writes actual elapsed milliseconds. New artifacts preserve cumulative pumped water; older artifacts represent it as unknown and the analyzer falls back to flow integration.

## Updating this fork

The display and controller now use **fork protocol version 7**. Upstream uses sensor field 7 for pumped water; fork UART diagnostics move to field 8. Heater power uses the upstream boiler field. Version mismatches continue to block machine control.

For the UART N16R8 machine, install matching `display-headless-uart-n16r8` ESP32 and `stm32f4` controller firmware together. STM32 updates still require the existing USB/ST-Link procedure; the Bluetooth controller OTA mechanism cannot update an STM32 over UART.

ESP32 OTA continues to select release assets from `David-Huynh/gaggiuino-gaggimate`, including the UART N16R8-specific image. This merge does not publish a release. Future OTA availability requires publishing the matching fork assets. Do not use stock upstream firmware for the custom UART machine.

## Boundaries and deliberate integration choices

All EspressoRL ingestion, optimization, cloud upload and repository ports are preserved. No Python optimizer or container deployment change is required by this merge. The optional cumulative-water sample belongs to the firmware's canonical capture model; the machine adapter obtains it, the local artifact adapter stores it, and the history adapter projects it. Missing values are never manufactured as measured zeroes.

WebUIPlugin retains ownership of the fork's WebSocket endpoint and durable feedback paths. Upstream's functional status split, warning messages and OTA behavior were ported into it; the structural WebSocketHandler extraction is deferred to a separate refactor. Cached state replay uses atomic shared-pointer access across the socket and loop tasks.

Shot log v7 reserves cumulative-water value 65535 as unknown in recovered artifacts, with finite measurements capped at 65534. The bundled parser handles this sentinel and continues reading v5/v6 logs.

## Validation

- PlatformIO: display, display-headless-uart-n16r8, stm32f4.
- `uv run --with ziglang python scripts/test_upstream_native.py`: 23 OTA retry/resume cases, 13 button cases, 2 pump estimator cases and 4 autotune cases. Requires installed PlatformIO Unity dependencies.
- `uv run --with ziglang python scripts/test_artifact_recovery.py`: actual persisted-artifact recovery/projection, missing/invalid pumped water and irregular timestamps.
- `uv run --with ziglang python scripts/test_brew_start.py`: real tare driver and controller start/stop orchestration, including warning override/readiness isolation.
- `uv run --with ziglang python scripts/test_storage_ownership.py`: host/FreeRTOS task ownership and flash/process exclusion.
- `node scripts/test_upstream_web.mjs`: sparse status and shot-log parsing. Requires Node 22.15+ or 24 and installed web dependencies.
- Web production build, ESLint, prompt browser tests at 320x568, 390x844, 844x390 and 1280x900; firmware stack-frame budgets.

Host checks cannot verify ESP32 TLS/flash timing or physical pump/UART behavior. Before a release, exercise boot with saved shots, manual/automatic shot endings, feedback replay, and interrupted/retried OTA on hardware. The upstream POSIX socket chaos suite remains part of the GitHub release/nightly build gate; it was not run on this Windows host.

Next slice: validate the matching firmware on the machine, then publish fork release assets for subsequent ESP32 OTA updates. The WebSocket class extraction can follow independently.
