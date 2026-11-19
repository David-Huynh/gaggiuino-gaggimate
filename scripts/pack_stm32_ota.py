"""Sign a relocated STM32F411CE ELF for UART OTA or first installation over ST-Link."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile

from sign_release import load_private_key, raw_public_key

FLASH_BASE = 0x08000000
ACTIVE_MANIFEST = 0x0800C000
APPLICATION = 0x08010000
MAX_IMAGE = 192 * 1024
SRAM_START = 0x20000000
SRAM_END = 0x20020000
MAGIC = b"GM411OT\x01"
COMMITTED = 0x474D4F54


def flash_image(elf, toolchain, address, capacity, target):
    def arm_tool(name):
        return str(toolchain / f"arm-none-eabi-{name}") if toolchain else f"arm-none-eabi-{name}"

    info = subprocess.check_output([arm_tool("objdump"), "-h", str(elf)], text=True)
    for row in info.splitlines():
        fields = row.split()
        if len(fields) >= 5 and fields[1] == ".isr_vector" and all(
            int(location, 16) == address for location in fields[3:5]
        ):
            break
    else:
        raise ValueError(f"Expected the {target} vector table at 0x{address:08x}.")

    with tempfile.TemporaryDirectory() as directory:
        binary = Path(directory) / "firmware.bin"
        subprocess.run([arm_tool("objcopy"), "-O", "binary", str(elf), str(binary)], check=True)
        image = binary.read_bytes()

    if not 8 <= len(image) <= capacity:
        raise ValueError(f"The {target} image must fit in {capacity} bytes.")
    stack, entry = struct.unpack_from("<II", image)
    if (not SRAM_START < stack <= SRAM_END or stack % 8 or not entry & 1
            or not address + 8 <= (entry & ~1) < address + len(image)):
        raise ValueError(f"Invalid {target} stack or reset vector.")
    return image + b"\xff" * (-len(image) % 4)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--toolchain", type=Path, help="directory containing arm-none-eabi-objcopy and objdump")
    parser.add_argument("--bootloader", type=Path, help="include the bootloader ELF in an ST-Link .bin at 0x08000000")
    keys = parser.add_mutually_exclusive_group(required=True)
    keys.add_argument("--private-key", type=Path)
    keys.add_argument("--private-key-env")
    args = parser.parse_args()
    if args.bootloader and args.output.suffix.lower() != ".bin":
        parser.error("Use a .bin output for first installation over ST-Link.")
    key = load_private_key(args)
    try:
        image = flash_image(args.elf, args.toolchain, APPLICATION, MAX_IMAGE, "stm32f4-ota-app")
        manifest = MAGIC + struct.pack("<I", len(image)) + hashlib.sha512(image).digest()
        signed_manifest = manifest + key.sign(manifest)
        if args.bootloader:
            loader = flash_image(
                args.bootloader, args.toolchain, FLASH_BASE, ACTIVE_MANIFEST - FLASH_BASE, "stm32f4-bootloader"
            )
            public_key = raw_public_key(key.public_key())
            if public_key not in loader or public_key not in image:
                raise ValueError("Rebuild both ELFs with the public key corresponding to the signing key.")
            metadata = signed_manifest + struct.pack("<I", COMMITTED)
            prefix = loader.ljust(ACTIVE_MANIFEST - FLASH_BASE, b"\xff") + metadata
            output = prefix.ljust(APPLICATION - FLASH_BASE, b"\xff") + image
        else:
            output = signed_manifest + image
        args.output.write_bytes(output)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
    image_type = "ST-Link boot image at 0x08000000" if args.bootloader else "Signed STM32 UART update"
    print(f"{image_type} ({len(output)} bytes) written to {args.output}")


if __name__ == "__main__":
    main()
