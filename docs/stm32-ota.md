# STM32F411CE UART bootloader

The development builds use the Black Pill's existing USART2 connection on PA2 and PA3 at 115200 baud. The host simulator exercises the firmware's sender, frame parser, signature checks and installer with a simulated UART and flash. It needs no boards.

## Run the simulation

```sh
python scripts/test_stm32_ota.py
```

On a host without a C and C++ compiler, use `uv run --with ziglang python scripts/test_stm32_ota.py`. The first run downloads the checksum-pinned Monocypher release to `.pio`. Subsequent runs use the cached archive.

The test sends a signed firmware image, checks that a malformed or forged request cannot enter the loader, and retries dropped UART replies. A flash model interrupts operations before, during and after each erase and word write in an update. A cut before staging commits leaves the old application bootable. A cut during installation leaves the verified staged image available so the loader can complete installation after power returns.

## Build the ARM firmware for simulation

Use the following public test key for this local build only.

```sh
export GAGGIMATE_STM32_OTA_PUBLIC_KEY=d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a
pio run -e stm32f4-bootloader -e stm32f4-ota-app
python scripts/test_stm32_ota.py --image .pio/build/stm32f4-ota-app/firmware.bin
```

In PowerShell, set the key with `$env:GAGGIMATE_STM32_OTA_PUBLIC_KEY = '…'`. The test signs and transfers the resulting binary. The regular `stm32f4` target builds the manual-flash controller image.

## Flash layout

| Sectors | Start | Contents |
| --- | --- | --- |
| 0–2 | `0x08000000` | Bootloader, up to 48 KiB |
| 3 | `0x0800C000` | Signed manifest and commit marker for the active image |
| 4–5 | `0x08010000` | Application and vector table, up to 192 KiB |
| 6–7 | `0x08040000` | Staged manifest and full signed application |

The relocated app accepts a signed BEGIN while its UART control session is connected. The app shuts down its outputs, writes a one-time reset flag to reserved SRAM and requests a software reset. The loader holds the heater, pump and valve GPIOs low. It verifies the manifest again, stages the image and checks the full SHA-512 digest before writing the commit marker. The sender retries BEGIN across the reset and waits for each acknowledgement. Acknowledging FINISH means staging is durable. Installation happens on reboot. If an update times out with an intact active image, the loader returns to that image.

The loader verifies the active image and vector table before booting. It installs a verified staged image when the active copy is missing, damaged or has a different manifest. This recovers interrupted installs. A verified app that crashes before it can accept another update needs ST-Link recovery.

With the signing dependency used by `scripts/sign_release.py`, `scripts/pack_stm32_ota.py` packages a relocated application ELF as a signed manifest followed by the image. Pass `--toolchain` if the ARM tools are outside PATH. The loader and OTA app must be built with the matching public key.

## First installation for a bench test

Use an STM32F411CE with 512 KiB of flash. Unplug the machine from mains and bench-power the controller with the heater and pump loads disconnected. Build both OTA targets with a local signing key for this board.

Pass the bootloader ELF to the existing packer to combine the loader, signed active manifest and relocated app into one ST-Link image.

```sh
python scripts/pack_stm32_ota.py .pio/build/stm32f4-ota-app/firmware.elf stm32-ota-bench.bin \
  --bootloader .pio/build/stm32f4-bootloader/firmware.elf --private-key path/to/your-signing-key.pem
```

Use STM32CubeProgrammer over ST-Link/SWD to read and save 524,288 bytes from `0x08000000`. When the backup is complete, program the combined `.bin` at `0x08000000` with verification enabled and reset the controller. Program the saved backup at the same address to restore the original controller.

Confirm the heater and pump control lines stay off through reset. To check the UART connection, use an ESP32 build that speaks controller protocol `10016`. [STM32CubeProgrammer SWD command reference](https://dev.st.com/stm32cube-docs/prog/2.23.0/en/docs/markup/CubeProg_Command_Lines.html)

## Integration status

Connecting the shared sender to the ESP32 updater and verifying the controller's version after reboot are the next integration steps. The simulation covers the flash protocol and recovery decisions. GPIO levels during reset, voltage drop behavior and actual flash timing need a hardware check before enabling deployment.
