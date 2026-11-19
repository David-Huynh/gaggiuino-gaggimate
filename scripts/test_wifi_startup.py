"""Exercise the firmware's Wi-Fi startup and retry methods with a fake station."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


def main():
    build = ROOT / ".pio/host-wifi"
    build.mkdir(parents=True, exist_ok=True)
    controller = (ROOT / "src/display/core/Controller.cpp").read_text(encoding="utf-8")
    watchdog = (ROOT / "src/display/plugins/WifiStaWatchdogPlugin.cpp").read_text(encoding="utf-8")
    source = build / "wifi.cpp"
    source.write_text(
        '#include "test.cpp"\n' + "\n\n".join((
            method(controller, "void Controller::setupWifi("),
            method(controller, "void Controller::handleWifiEvents("),
            method(watchdog, "void WifiStaWatchdogPlugin::setup("),
            method(watchdog, "void WifiStaWatchdogPlugin::loop("),
        )), encoding="utf-8",
    )
    cxx = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
    compiler = [cxx] if cxx else [sys.executable, "-m", "ziglang", "c++"]
    binary = build / ("test.exe" if os.name == "nt" else "test")
    paths = ("test/host_artifact/stubs", "sim/platform", "sim/platform/arduino", "src", "test/host_wifi")
    command = compiler + ["-x", "c++", "-std=c++17", "-pthread"]
    for path in paths:
        command.extend(["-I", str(ROOT / path)])
    arduino = ROOT / "sim/platform/arduino"
    command += [str(source), str(arduino / "WString.cpp"), str(arduino / "stdlib_noniso.c"),
                str(ROOT / "sim/platform/noniso_extra.c"), "-o", str(binary)]
    subprocess.run(command, check=True, cwd=ROOT)
    subprocess.run([str(binary)], check=True, cwd=ROOT, timeout=20)


if __name__ == "__main__":
    main()
