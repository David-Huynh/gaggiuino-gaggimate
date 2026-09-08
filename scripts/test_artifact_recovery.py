"""Run real shot codecs, storage and recovery against an in-memory filesystem."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    build = ROOT / ".pio/host-artifact"
    build.mkdir(parents=True, exist_ok=True)
    cxx = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
    compiler = [cxx] if cxx else [sys.executable, "-m", "ziglang", "c++"]
    if not cxx and not importlib.util.find_spec("ziglang"):
        raise SystemExit("Install a C++17 compiler or use uv run --with ziglang.")
    json_header = ROOT / ".pio/libdeps/display-headless-uart-n16r8/ArduinoJson/src/ArduinoJson.h"
    if not json_header.exists():
        json_header = next((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"), None)
    if json_header is None:
        raise SystemExit("Install the project's PlatformIO dependencies first (ArduinoJson is required).")
    # Only the Settings-free taste codec methods are needed; compile them unchanged.
    taste = (ROOT / "src/display/plugins/autotuning/AutoTuningTasteGoalJson.cpp").read_text()
    methods = []
    for signature in ("bool parseTasteGoal(", "void writeTasteGoal("):
        start = taste.index(signature)
        methods.append(taste[start:taste.index("\n}", start) + 2])
    generated = build / "taste.cpp"
    generated.write_text('#include <display/plugins/autotuning/AutoTuningTasteGoalJson.h>\nnamespace AutoTuning {\n' +
                         "\n".join(methods) + "\n}\n")
    command = compiler + ["-x", "c++", "-std=c++17", "-pthread", "-DGAGGIMATE_SIM", "-DARDUINO=10819"]
    for folder in (ROOT / "test/host_artifact/stubs", ROOT / "sim/platform", ROOT / "sim/platform/arduino",
                   ROOT / "src", json_header.parent):
        command += ["-I", str(folder)]
    sources = ["test/host_artifact/test.cpp", "sim/platform/arduino/WString.cpp",
               "sim/platform/arduino/Print.cpp", "sim/platform/arduino/Stream.cpp",
               "sim/platform/arduino/stdlib_noniso.c", "sim/platform/noniso_extra.c", "src/display/core/StorageCoordinator.cpp",
               "src/display/core/AutoTuningModels.cpp", "src/display/plugins/autotuning/AutoTuningJsonCodec.cpp",
               "src/display/plugins/autotuning/local/CompletedShotArtifactStore.cpp",
               "src/display/util/AtomicFile.cpp", "src/display/util/LittleFSUtil.cpp"]
    binary = build / ("test.exe" if os.name == "nt" else "test")
    command += [str(ROOT / source) for source in sources] + [str(generated), "-o", str(binary)]
    subprocess.run(command, check=True, cwd=ROOT)
    subprocess.run([str(binary)], check=True, timeout=30, cwd=ROOT)


if __name__ == "__main__":
    main()
