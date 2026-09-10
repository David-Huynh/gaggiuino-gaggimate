"""Check Xtensa stack frames after a PlatformIO display build.

This catches large automatic artifact/decoder and history-buffer frames behind
the recipe prompt and startup recovery crashes. It measures frames, not total runtime
stack usage; recovery and confirmation must also be exercised on hardware.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def frame_size(output, name):
    frames = []
    for symbol, size in re.findall(r"^\w+ <([^\n]+)>:\n\s*[^\n]*entry\s+a1,\s*(\S+)", output, re.MULTILINE):
        if symbol.startswith(name + "(") and "::{lambda" not in symbol:
            frames.append(int(size, 0))
    if not frames:
        raise ValueError(f"Missing Xtensa stack frame for {name}")
    # Count the largest compiler clone, never a small nested lambda's frame.
    return max(frames)


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
    budgets = {
        "LocalAutoTuningStorePlugin": {
            "drainStoredShots": 256, "drainPromptEvents": 256,
            "handleDoseConfirmation": 1024, "loadCommittedShot": 512,
            "recoverPendingDoseConfirmation": 512, "recoverCommittedArtifacts": 512,
            "persistShot": 512, "correctShot": 1024, "dispatchStoredShot": 1024,
            "prepareShotReprocess": 1280, "prepareShotComplete": 1280,
            "processShotDeliveryAck": 1024, "dispatchPendingCommunityUploads": 512,
        },
        "ShotHistoryPlugin": {"ensureProjection": 1024},
    }
    frames = {}
    for plugin, methods in budgets.items():
        artifact = ROOT / f".pio/build/{args.environment}/src/display/plugins/{plugin}.cpp.o"
        output = subprocess.check_output([str(objdump), "-d", "-C", str(artifact)], text=True)
        for method, budget in methods.items():
            name = f"{plugin}::{method}"
            size = frame_size(output, name)
            frames[name] = size
            if size > budget:
                raise SystemExit(f"{name}: {size} bytes exceeds {budget}-byte frame budget")
            print(f"PASS {name}: {size} bytes (budget {budget})")
    # These two frames overlap during boot and normal recovery. Leave room for
    # setup/worker frames, filesystem internals and codec calls on the 8 KB task.
    recovery_frames = (frames["LocalAutoTuningStorePlugin::recoverCommittedArtifacts"] +
                       frames["ShotHistoryPlugin::ensureProjection"])
    if recovery_frames > 1536:
        raise SystemExit(f"Combined recovery/projection frames exceed 1536 bytes: {recovery_frames}")
    print(f"PASS combined recovery/projection frames: {recovery_frames} bytes (budget 1536)")


if __name__ == "__main__":
    main()
