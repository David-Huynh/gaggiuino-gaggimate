"""Build and embed the current web UI without modifying web/dist in place."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

from embed_webui import pack


def fingerprint(root):
    web = root / "web"
    inputs = [p for p in web.iterdir() if p.is_file()]
    for folder in (web / "src", web / "public"):
        inputs.extend(p for p in folder.rglob("*") if p.is_file())
    inputs.extend(root / "scripts" / name for name in ("build_webui.py", "embed_webui.py"))
    digest = hashlib.sha256()
    for path in sorted(inputs):
        digest.update(path.relative_to(root).as_posix().encode())
        digest.update(b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    for key in ("GAGGIMATE_DISABLE_HARDWARE_SCALE", "VITE_DISABLE_HARDWARE_SCALE"):
        digest.update(f"{key}={os.environ.get(key, '')}\n".encode())
    return digest.hexdigest()


def output_hashes(out):
    return {name: hashlib.sha256((out / name).read_bytes()).hexdigest()
            for name in ("web_ui.bin", "web_ui_manifest.h", "web_ui_blob.S")}


def ensure_bundle(root, *, force=False, install=False, run=subprocess.run):
    out = root / "src/display/webassets"
    stamp = out / "web_ui_build.json"
    inputs = fingerprint(root)
    if not force and not install:
        try:
            previous = json.loads(stamp.read_text())
            if previous == {"inputs": inputs, "outputs": output_hashes(out)}:
                print("webui: embedded bundle matches current sources")
                return False
        except (OSError, ValueError):
            pass
    npm = shutil.which("npm")
    if not npm:
        raise RuntimeError("Node/npm is required to rebuild the changed web UI")
    web = root / "web"
    if install:
        run([npm, "ci"], cwd=web, check=True)
    if not (web / "node_modules").is_dir():
        raise RuntimeError("Web dependencies are missing; run npm ci in web/ before building firmware")
    run([npm, "run", "build"], cwd=web, check=True)
    if not (web / "dist/index.html").is_file():
        raise RuntimeError("Web build did not produce dist/index.html")
    entries = pack(str(web / "dist"), str(out), compress=True)
    stamp.write_text(json.dumps({"inputs": inputs, "outputs": output_hashes(out)}))
    print(f"webui: embedded {len(entries)} assets from current sources")
    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--if-needed", action="store_true")
    parser.add_argument("--install", action="store_true")
    args = parser.parse_args()
    ensure_bundle(Path(__file__).resolve().parents[1], force=not args.if_needed, install=args.install)
