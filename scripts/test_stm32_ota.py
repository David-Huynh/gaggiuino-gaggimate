"""Run the firmware update sender and installer against simulated UART and flash."""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys

from stm32_ota_dependencies import ROOT, monocypher_sources


def compiler(variable, names, zig):
    native = os.environ.get(variable) or next((path for name in names if (path := shutil.which(name))), None)
    if native:
        return [native]
    if importlib.util.find_spec("ziglang"):
        return [sys.executable, "-m", "ziglang", zig]
    raise SystemExit("Install C and C++ compilers or run with uv run --with ziglang python scripts/test_stm32_ota.py.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, help="also transfer a real relocated STM32 firmware binary")
    parser.add_argument("--package", type=Path, help="also transfer a package signed with the RFC 8032 fixture key")
    args = parser.parse_args()
    if args.package and not args.image:
        parser.error("--package requires --image")
    cc = compiler("CC", ("gcc", "clang"), "cc")
    cxx = compiler("CXX", ("g++", "clang++"), "c++")
    crypto = monocypher_sources()
    core = ROOT / "src/stm32-ota"
    build = ROOT / ".pio/host-stm32-ota"
    build.mkdir(parents=True, exist_ok=True)
    includes = [flag for path in (crypto, crypto / "optional", core, ROOT / "lib/NanoPbComm/src")
                for flag in ("-I", str(path))]
    objects = []
    for source in (crypto / "monocypher.c", crypto / "optional/monocypher-ed25519.c"):
        obj = build / (source.stem + ".o")
        subprocess.run(cc + ["-O2", *includes, "-c", str(source), "-o", str(obj)], check=True, cwd=ROOT)
        objects.append(obj)
    binary = build / ("test.exe" if os.name == "nt" else "test")
    sources = [core / "Stm32Boot.cpp", core / "Stm32BootWire.cpp", ROOT / "test/host_stm32_ota/test.cpp"]
    subprocess.run(
        cxx + ["-O2", "-std=c++17", "-Wall", "-Wextra", "-Werror", *includes,
               *map(str, sources + objects), "-o", str(binary)],
        check=True,
        cwd=ROOT,
    )
    command = [str(binary)]
    if args.image:
        command.append(str(args.image.resolve(strict=True)))
    if args.package:
        command.append(str(args.package.resolve(strict=True)))
    subprocess.run(command, check=True, cwd=ROOT, timeout=120)


if __name__ == "__main__":
    main()
