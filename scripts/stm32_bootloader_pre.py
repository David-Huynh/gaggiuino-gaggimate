import os
from pathlib import Path
import sys

Import("env")

project = Path(env["PROJECT_DIR"])
sys.path.insert(0, str(project / "scripts"))
from stm32_ota_dependencies import monocypher_sources

try:
    key = bytes.fromhex(os.environ.get("GAGGIMATE_STM32_OTA_PUBLIC_KEY", ""))
    if len(key) != 32 or not any(key):
        raise ValueError
except ValueError:
    raise SystemExit("Set GAGGIMATE_STM32_OTA_PUBLIC_KEY to a 32-byte Ed25519 public key in hex.")

build = Path(env.subst("$BUILD_DIR"))
build.mkdir(parents=True, exist_ok=True)
header = build / "stm32_ota_key.h"
key_bytes = ", ".join(f"0x{byte:02x}" for byte in key)
contents = f"#pragma once\nconstexpr uint8_t STM32_OTA_KEY[32] = {{{key_bytes}}};\n"
if not header.is_file() or header.read_text() != contents:
    header.write_text(contents)

monocypher = monocypher_sources()
core = project / "src/stm32-ota"
env.Append(CPPPATH=[str(build), str(core), str(monocypher), str(monocypher / "optional"),
                   str(project / "lib/NanoPbComm/src")])
env.BuildSources("$BUILD_DIR/stm32-boot-core", str(core))
env.BuildSources("$BUILD_DIR/monocypher", str(monocypher), src_filter=["+<*.c>", "+<optional/*.c>"])
