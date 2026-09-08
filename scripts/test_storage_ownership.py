"""Compile real storage coordination with host and simulated FreeRTOS identities."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    build = ROOT / ".pio/host-storage"
    build.mkdir(parents=True, exist_ok=True)
    cxx = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
    if cxx:
        compiler = [cxx]
    elif importlib.util.find_spec("ziglang"):
        compiler = [sys.executable, "-m", "ziglang", "c++"]
    else:
        raise SystemExit("Install a C++17 compiler or use uv run --with ziglang.")
    for platform in ("host", "esp32"):
        binary = build / (platform + (".exe" if os.name == "nt" else ""))
        command = compiler + ["-std=c++17", "-pthread"]
        if platform == "esp32":
            command += ["-DARDUINO_ARCH_ESP32", "-I", str(ROOT / "test/host_storage/stubs")]
        command += ["-I", str(ROOT / "src/display/core"),
                    str(ROOT / "test/host_storage/test.cpp"),
                    str(ROOT / "src/display/core/StorageCoordinator.cpp"), "-o", str(binary)]
        subprocess.run(command, check=True, cwd=ROOT)
        print(f"Testing {platform} identity:", flush=True)
        subprocess.run([str(binary)], check=True, timeout=20, cwd=ROOT)
        if platform == "esp32":
            rejected = subprocess.run([str(binary), "wrong-owner"], timeout=20,
                                      capture_output=True, text=True, cwd=ROOT)
            assert rejected.returncode != 0 and "flashOwner == currentFlashOwner()" in rejected.stderr, rejected
            print("PASS storage: release by the wrong FreeRTOS task is rejected", flush=True)


if __name__ == "__main__":
    main()
