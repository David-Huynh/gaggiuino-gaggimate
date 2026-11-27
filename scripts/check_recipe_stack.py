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

    # CommunityUploadQueue runs on the 6 KB CloudHandoff task; keep the 4 KB flash chunk off the stack.
    queue_obj = ROOT / f".pio/build/{args.environment}/src/display/plugins/community/CommunityUploadQueue.cpp.o"
    if queue_obj.exists():
        q_out = subprocess.check_output([str(objdump), "-d", "-C", str(queue_obj)], text=True)
        q_sz = frame_size(q_out, "CommunityUploadQueue::readItemUnlocked")
        if q_sz > 512:
            raise SystemExit(f"CommunityUploadQueue::readItemUnlocked: {q_sz} bytes exceeds 512-byte frame budget")
        print(f"PASS CommunityUploadQueue::readItemUnlocked: {q_sz} bytes (budget 512)")

    # On-board EspressoRL live 4 Hz MPC runs on Arduino loopTask (8 KB stack) and between-shot CPBO
    # runs on the 10 KB Core-1 worker. Enforce tight per-frame and combined live-brew stack ceilings.
    build_root = ROOT / f".pio/build/{args.environment}"
    mpc_objs = list(build_root.glob("lib*/EspressoRLCore/CemGpMpc.cpp.o"))
    if mpc_objs:
        core_dir = mpc_objs[0].parent
        onboard_checks = [
            (build_root / "src/display/plugins/OnBoardEspressoRLPlugin.cpp.o",
             "OnBoardEspressoRLPlugin::onLiveShotSample", 768),
            (build_root / "src/display/plugins/OnBoardEspressoRLPlugin.cpp.o",
             "OnBoardEspressoRLPlugin::loadCommunityPrior", 512),
            (core_dir / "CemGpMpc.cpp.o",
             "EspressoRL::CemGpMpcController::computeCommand", 768),
            (core_dir / "HydraulicWorldModel.cpp.o",
             "EspressoRL::CombinedHydraulicPredictor::rolloutWithScratch", 256),
            (core_dir / "HydraulicWorldModel.cpp.o",
             "EspressoRL::FixedBasisVfeNystromGP::predictMean", 128),
            (core_dir / "EmbeddedCpbo.cpp.o",
             "EspressoRL::conditionalJndProbsBivariateTruncated", 1536),
            (core_dir / "Ed25519Verify.cpp.o",
             "EspressoRL::ed25519Verify", 1024),
        ]
        onboard_frames = {}
        out_cache = {}
        for obj_path, symbol, budget in onboard_checks:
            key = str(obj_path)
            if key not in out_cache:
                out_cache[key] = subprocess.check_output([str(objdump), "-d", "-C", key], text=True)
            size = frame_size(out_cache[key], symbol)
            onboard_frames[symbol] = size
            if size > budget:
                raise SystemExit(f"{symbol}: {size} bytes exceeds {budget}-byte frame budget")
            print(f"PASS {symbol}: {size} bytes (budget {budget})")
        live_brew_frames = (
            onboard_frames["OnBoardEspressoRLPlugin::onLiveShotSample"] +
            onboard_frames["EspressoRL::CemGpMpcController::computeCommand"] +
            onboard_frames["EspressoRL::CombinedHydraulicPredictor::rolloutWithScratch"]
        )
        if live_brew_frames > 1536:
            raise SystemExit(f"Combined live-brew MPC frames exceed 1536 bytes: {live_brew_frames}")
        print(f"PASS combined live-brew MPC frames: {live_brew_frames} bytes (budget 1536)")


if __name__ == "__main__":
    main()
