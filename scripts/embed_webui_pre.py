#
# PlatformIO pre-build hook (display envs).
#
# Verify source and output hashes before every display build. An old UI can
# speak an incompatible protocol even when the firmware itself compiles.
#
import os
import subprocess
import sys

Import("env")  # noqa: F821 -- provided by PlatformIO/SCons

subprocess.check_call([sys.executable, os.path.join(env["PROJECT_DIR"], "scripts", "generate_machine_profile_schema.py")])

project_dir = env["PROJECT_DIR"]  # noqa: F821
builder = os.path.join(project_dir, "scripts", "build_webui.py")
# PlatformIO's Windows log forwarding can use cp1252. Vite/plugin output
# includes Unicode symbols, so normalize this subprocess's log at the boundary.
result = subprocess.run([sys.executable, builder, "--if-needed"],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, encoding="utf-8", errors="replace")
print(result.stdout.encode("ascii", errors="replace").decode("ascii"))
result.check_returncode()

# Also track the bytes included by web_ui_blob.S.
blob_bin = os.path.join(project_dir, "src", "display", "webassets", "web_ui.bin")
blob_obj = os.path.join(env.subst("$BUILD_DIR"), "src", "display", "webassets", "web_ui_blob.S.o")
env.Depends(blob_obj, blob_bin)
