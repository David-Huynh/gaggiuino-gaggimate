"""Sign a relocated STM32F411CE ELF for the UART bootloader."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile

from sign_release import load_private_key

APPLICATION = 0x08010000
MAX_IMAGE = 192 * 1024
SRAM_START = 0x20000000
SRAM_END = 0x20020000
MAGIC = b"GM411OT\x01"


def relocated_image(elf, toolchain):
    def arm_tool(name):
        return str(toolchain / f"arm-none-eabi-{name}") if toolchain else f"arm-none-eabi-{name}"

    info = subprocess.check_output([arm_tool("objdump"), "-h", str(elf)], text=True)
    for row in info.splitlines():
        fields = row.split()
        if len(fields) >= 5 and fields[1] == ".isr_vector":
            if all(int(address, 16) == APPLICATION for address in fields[3:5]):
                break
    else:
        raise ValueError("Expected the stm32f4-ota-app vector table at 0x08010000.")

    with tempfile.TemporaryDirectory() as directory:
        binary = Path(directory) / "application.bin"
        subprocess.run([arm_tool("objcopy"), "-O", "binary", str(elf), str(binary)], check=True)
        image = binary.read_bytes()

    if not 8 <= len(image) <= MAX_IMAGE:
        raise ValueError(f"STM32 image must fit in {MAX_IMAGE} bytes.")
    stack, entry = struct.unpack_from("<II", image)
    if (not SRAM_START < stack <= SRAM_END or stack % 8 or not entry & 1
            or not APPLICATION + 8 <= (entry & ~1) < APPLICATION + len(image)):
        raise ValueError("Invalid application stack or reset vector.")
    return image + b"\xff" * (-len(image) % 4)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--toolchain", type=Path, help="directory containing arm-none-eabi-objcopy and objdump")
    keys = parser.add_mutually_exclusive_group(required=True)
    keys.add_argument("--private-key", type=Path)
    keys.add_argument("--private-key-env")
    args = parser.parse_args()
    try:
        image = relocated_image(args.elf, args.toolchain)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")
    key = load_private_key(args)
    manifest = MAGIC + struct.pack("<I", len(image)) + hashlib.sha512(image).digest()
    args.output.write_bytes(manifest + key.sign(manifest) + image)
    print(f"Signed STM32 image: {args.output} ({len(image)} firmware bytes)")


if __name__ == "__main__":
    main()
