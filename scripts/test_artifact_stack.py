"""Check ESP32 stack-frame budgets for the saved-shot boot-crash path after pio build."""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / ".pio/build/display-headless-uart-n16r8")
    parser.add_argument("--objdump", type=Path)
    args = parser.parse_args()
    objdump = args.objdump
    if objdump is None:
        candidates = list((Path.home() / ".platformio/packages/toolchain-xtensa-esp32s3/bin").glob(
            "xtensa-esp32s3-elf-objdump*"))
        if not candidates:
            raise SystemExit("Specify --objdump or install the PlatformIO ESP32-S3 toolchain.")
        objdump = candidates[0]
    budgets = {
        "AutoTuningJsonCodec::parseShotRecord(": ("src/display/plugins/autotuning/AutoTuningJsonCodec.cpp.o", 1536),
        "(anonymous namespace)::decodeArtifact(": (
            "src/display/plugins/autotuning/local/CompletedShotArtifactStore.cpp.o", 1280),
        "(anonymous namespace)::validArtifactFile(": (
            "src/display/plugins/autotuning/local/CompletedShotArtifactStore.cpp.o", 256),
    }
    disassemblies = {}
    total = 0
    for name, (relative, limit) in budgets.items():
        if relative not in disassemblies:
            disassemblies[relative] = subprocess.check_output(
                [str(objdump), "-d", "-C", str(args.build_dir / relative)], text=True)
        active = False
        sizes = []
        for line in disassemblies[relative].splitlines():
            if re.match(r"^[0-9a-f]+ <.*>:$", line):
                active = ("<" + name) in line
            elif active:
                frame = re.search(r"\bentry\s+a1,\s*(0x[0-9a-f]+|[0-9]+)", line)
                if frame:
                    sizes.append(int(frame.group(1), 0))
                    active = False
        assert len(sizes) == 1, f"Expected one frame for {name}, found {sizes}"
        assert sizes[0] <= limit, f"{name}: {sizes[0]} bytes exceeds {limit}-byte budget"
        total += sizes[0]
        print(f"PASS {name[:-1]}: {sizes[0]} bytes (budget {limit})")
    assert total <= 3072, f"Nested decoder frames consume {total} bytes of the 8192-byte Arduino stack"
    print(f"PASS combined saved-shot decoder frames: {total} bytes; device stack high-water still requires hardware validation")


if __name__ == "__main__":
    main()
