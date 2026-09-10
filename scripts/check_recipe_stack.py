"""Check Xtensa stack frames after a PlatformIO display build.

This catches the large automatic artifact/decoder frames behind the recipe
prompt stack-canary crash. It measures individual frames, not total runtime
stack usage; recovery and confirmation must also be exercised on hardware.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--environment", default="display-headless-uart-n16r8")
    parser.add_argument("--objdump")
    args = parser.parse_args()
    objdump = args.objdump or shutil.which("xtensa-esp32s3-elf-objdump")
    if not objdump:
        folder = Path.home() / ".platformio/packages/toolchain-xtensa-esp32s3/bin"
        objdump = next(folder.glob("xtensa-esp32s3-elf-objdump*"), None)
    if not objdump:
        parser.error("Pass --objdump with the Xtensa toolchain executable")
    artifact = ROOT / f".pio/build/{args.environment}/src/display/plugins/LocalAutoTuningStorePlugin.cpp.o"
    output = subprocess.check_output([str(objdump), "-d", "-C", str(artifact)], text=True)
    budgets = {"drainStoredShots": 256, "drainPromptEvents": 256,
               "handleDoseConfirmation": 1024, "loadCommittedShot": 512,
               "recoverPendingDoseConfirmation": 512}
    for method, budget in budgets.items():
        pattern = (r"^\w+ <LocalAutoTuningStorePlugin::" + method +
                   r"\([^\n]*\)(?: const)?>:\n\s*[^\n]*entry\s+a1,\s*(\S+)")
        match = re.search(pattern, output, re.MULTILINE)
        if not match:
            raise SystemExit(f"Missing Xtensa stack frame for {method}")
        size = int(match.group(1), 0)
        if size > budget:
            raise SystemExit(f"{method}: {size} bytes exceeds {budget}-byte frame budget")
        print(f"PASS {method}: {size} bytes (budget {budget})")


if __name__ == "__main__":
    main()
