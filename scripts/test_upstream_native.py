"""Run upstream OTA and button unit suites with a host C++ compiler (including Windows)."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

def main():
    build = ROOT / ".pio/host-upstream"
    build.mkdir(parents=True, exist_ok=True)
    cxx = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
    compiler = [cxx] if cxx else [sys.executable, "-m", "ziglang", "c++"]
    if not cxx and not importlib.util.find_spec("ziglang"):
        raise SystemExit("Use uv run --with ziglang python scripts/test_upstream_native.py")
    unity = next((ROOT / ".pio/libdeps").glob("*/Unity/src/unity.c"), None)
    if unity is None:
        raise SystemExit("Install PlatformIO dependencies first (Unity is required).")
    for suite in ("test_ota_download", "test_button_handler", "test_puckflow_latch", "test_autotune_simc"):
        binary = build / (suite + (".exe" if os.name == "nt" else ""))
        command = compiler + ["-x", "c++", "-std=c++17", "-DOTA_LOG_LEVEL=0"]
        for folder in (ROOT / "src", ROOT / "lib/OTA/src", ROOT / "test/ota_common", ROOT / "lib/NayrodPID/src",
                       ROOT / "test/test_puckflow_latch/native_stubs", unity.parent):
            command += ["-I", str(folder)]
        command += [str(ROOT / "test" / suite / (suite + ".cpp")), str(unity), "-o", str(binary)]
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=60)

if __name__ == "__main__":
    main()
