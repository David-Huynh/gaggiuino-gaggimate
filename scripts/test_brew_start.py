"""Run real tare code with fake hardware; no machine or network required.

Uses CXX / g++ / clang++, or `uv run --with ziglang python scripts/test_brew_start.py`.
Controller methods are extracted unchanged to exercise orchestration without
linking the UI, MQTT, or complete ESP32 runtime. Missing signatures fail loudly.
"""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio" / "host-brew-start"


def method(source, signature):
    start = source.index(signature)
    end = source.index("\n}", start) + 2
    return source[start:end]


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    source = (ROOT / "src/display/core/Controller.cpp").read_text(encoding="utf-8")
    signatures = [
        "bool Controller::armHardwareScaleBrewTare()",
        "void Controller::cancelHardwareScaleBrewTare(",
        "void Controller::pollHardwareScaleBrewTare()",
        "bool Controller::deactivateLocked(",
        "void Controller::loopLogic()",
        "void Controller::activate(bool",
        "void Controller::deactivate()",
        "void Controller::startProcess(",
        "bool Controller::startProcessLocked(",
        "void Controller::clear()",
        "void Controller::clearLocked(",
        "void Controller::handleBrewButton(",
        "void Controller::deactivateStandby()",
    ]
    (BUILD / "controller_methods.inc").write_text("\n\n".join(method(source, s) for s in signatures), encoding="utf-8")
    cxx = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
    if cxx:
        command = [cxx]
    elif importlib.util.find_spec("ziglang"):
        command = [sys.executable, "-m", "ziglang", "c++"]
    else:
        raise SystemExit("Install a C++17 compiler, set CXX, or run via uv --with ziglang.")
    command += ["-std=c++17", "-fno-access-control", "-pthread", "-DARDUINO_ARCH_STM32"]
    for folder in [BUILD, ROOT / "test/host_brew_start/stubs", ROOT / "src/display/core",
                   ROOT / "lib/GaggiMateController/src/peripherals", ROOT / "lib/NanoPbComm/src"]:
        command += ["-I", str(folder)]
    for path in ["test/host_brew_start/test.cpp", "src/display/core/StorageCoordinator.cpp",
                 "lib/GaggiMateController/src/peripherals/HX711Scale.cpp",
                 "lib/GaggiMateController/src/peripherals/DualScaleFilter.cpp"]:
        command.append(str(ROOT / path))
    binary = BUILD / ("test.exe" if os.name == "nt" else "test")
    command += ["-o", str(binary)]
    subprocess.run(command, check=True, cwd=ROOT)
    subprocess.run([str(binary)], check=True, timeout=20, cwd=ROOT)


if __name__ == "__main__":
    main()
