"""Real session, heater/PID and thermocouple code with fake RTOS, link and sensors."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[1]

def main():
    build = ROOT / ".pio/host-heater"
    build.mkdir(parents=True, exist_ok=True)
    cxx = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
    compiler = [cxx] if cxx else [sys.executable, "-m", "ziglang", "c++"]
    if not cxx and not importlib.util.find_spec("ziglang"):
        raise SystemExit("Run with uv --with ziglang or install a C++17 compiler.")
    nanopb_header = next((ROOT / ".pio/libdeps").glob("*/Nanopb/pb.h"), None)
    if nanopb_header is None:
        raise SystemExit("Run pio run -e stm32f4 first to install Nanopb.")
    nanopb = nanopb_header.parent
    candidates = (ROOT / ".pio/build").glob("*/nanopb/generated-src/gaggimate.pb.h")
    generated_header = next((path for path in candidates if "peer_session_id" in path.read_text()), None)
    if generated_header is None:
        raise SystemExit("Run pio run -e stm32f4 first to generate the protocol.")
    generated = generated_header.parent
    folders = [ROOT/"test/host_heater/stubs", ROOT/"lib/NanoPbComm/src", generated, nanopb,
               ROOT/"lib/GaggiMateController/src/peripherals", ROOT/"lib/NayrodPID/src"]
    command = compiler + ["-x", "c++", "-std=c++17", "-pthread", "-fno-access-control", "-DARDUINO_ARCH_STM32"]
    for folder in folders:
        command += ["-I", str(folder)]
    sources = [ROOT/"test/host_heater/test.cpp", ROOT/"lib/NanoPbComm/src/Endpoint.cpp",
        ROOT/"lib/NanoPbComm/src/uart/UartTransport.cpp",
        generated/"gaggimate.pb.c", nanopb/"pb_common.c", nanopb/"pb_encode.c", nanopb/"pb_decode.c",
        ROOT/"lib/GaggiMateController/src/peripherals/Heater.cpp",
        ROOT/"lib/GaggiMateController/src/peripherals/Max31855Thermocouple.cpp",
        ROOT/"lib/NayrodPID/src/SimplePID/SimplePID.cpp", ROOT/"lib/NayrodPID/src/Autotune/Autotune.cpp"]
    binary = build/("test.exe" if os.name=="nt" else "test")
    command += [str(path) for path in sources] + ["-o", str(binary)]
    subprocess.run(command, check=True, cwd=ROOT)
    subprocess.run([str(binary)], check=True, timeout=30, cwd=ROOT)

if __name__ == "__main__":
    main()
