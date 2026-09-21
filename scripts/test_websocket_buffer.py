"""Compile/run the firmware receive buffer on the host, without network or hardware."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
build = ROOT / ".pio/host-websocket"
build.mkdir(parents=True, exist_ok=True)
cxx = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
compiler = [cxx] if cxx else [sys.executable, "-m", "ziglang", "c++"]
binary = build / ("test.exe" if os.name == "nt" else "test")
subprocess.run(compiler + ["-std=c++17", "-pthread", "-I", str(ROOT / "src"),
                          str(ROOT / "test/host_websocket/test.cpp"), "-o", str(binary)], check=True)
subprocess.run([str(binary)], check=True, timeout=30)
