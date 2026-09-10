"""Test stale-bundle detection with a fake npm build and the real asset packer."""
import gzip
import io
import contextlib
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from build_webui import ensure_bundle


class WebUiBuildTests(unittest.TestCase):
    def test_firmware_hook_sanitizes_unicode_output_and_propagates_build_failure(self):
        hook = Path(__file__).with_name("embed_webui_pre.py")
        for code in (0, 1):
            result = subprocess.CompletedProcess([], code, stdout="Build \U0001f33c\n")
            with patch("subprocess.run", return_value=result) as run, contextlib.redirect_stdout(io.StringIO()) as output:
                def invoke():
                    runpy.run_path(str(hook), init_globals={
                        "Import": lambda _: None, "env": {"PROJECT_DIR": str(hook.parent.parent)},
                    })
                if code:
                    with self.assertRaises(subprocess.CalledProcessError):
                        invoke()
                else:
                    invoke()
                output.getvalue().encode("ascii")
                self.assertIn("--if-needed", run.call_args.args[0])

    def test_source_changes_and_corrupt_outputs_rebuild_but_unchanged_builds_skip(self):
        with tempfile.TemporaryDirectory() as directory, patch("build_webui.shutil.which", return_value="npm"):
            root = Path(directory)
            for folder in ("web/src", "web/node_modules", "web/dist", "scripts"):
                (root / folder).mkdir(parents=True)
            for name in ("build_webui.py", "embed_webui.py"):
                (root / "scripts" / name).write_text("packer fixture")
            source = root / "web/src/app.jsx"
            source.write_text("old protocol")
            calls = []

            def build(command, **kwargs):
                calls.append(command)
                self.assertEqual(command, ["npm", "run", "build"])
                self.assertTrue(kwargs["check"])
                (root / "web/dist/index.html").write_bytes(source.read_bytes())

            self.assertTrue(ensure_bundle(root, run=build))
            blob = root / "src/display/webassets/web_ui.bin"
            self.assertEqual(gzip.decompress(blob.read_bytes()), b"old protocol")
            self.assertEqual((root / "web/dist/index.html").read_bytes(), b"old protocol")
            self.assertFalse(ensure_bundle(root, run=build))
            self.assertEqual(len(calls), 1)
            source.write_text("confirmed recipe protocol")
            self.assertTrue(ensure_bundle(root, run=build))
            self.assertEqual(gzip.decompress(blob.read_bytes()), b"confirmed recipe protocol")
            blob.write_bytes(b"stale or damaged bundle")
            self.assertTrue(ensure_bundle(root, run=build))
            self.assertEqual(gzip.decompress(blob.read_bytes()), b"confirmed recipe protocol")

            # A failed rebuild must fail the firmware build, not bless old assets.
            source.write_text("newer protocol")
            stamp = root / "src/display/webassets/web_ui_build.json"
            previous = stamp.read_bytes()

            def failed_build(*args, **kwargs):
                raise subprocess.CalledProcessError(1, "npm run build")

            with self.assertRaises(subprocess.CalledProcessError):
                ensure_bundle(root, run=failed_build)
            self.assertEqual(stamp.read_bytes(), previous)
            self.assertTrue(ensure_bundle(root, run=build))
            self.assertEqual(gzip.decompress(blob.read_bytes()), b"newer protocol")


if __name__ == "__main__":
    unittest.main()
