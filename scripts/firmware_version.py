"""Version used by both the Nightly firmware and its release metadata."""

import os
import re
import subprocess


STABLE_TAG = re.compile(r"v([0-9]+)\.([0-9]+)\.([0-9]+)")
# Earlier fork builds used v1.8.<run number>, including v1.8.36.
MIN_NIGHTLY_SERIES = (1, 9, 0)


def nightly_version(run_number, tags):
    if not run_number.isdecimal():
        raise ValueError("A numerical GitHub run number is required")
    stable_versions = [
        tuple(map(int, match.groups()))
        for tag in tags
        if (match := STABLE_TAG.fullmatch(tag))
    ]
    major, minor, patch = max(stable_versions, default=(0, 0, 0))
    base = max(MIN_NIGHTLY_SERIES, (major, minor, patch + 1))
    return "v" + ".".join(map(str, base)) + f"-nightly.{run_number}"


if __name__ == "__main__":
    tags = subprocess.check_output(
        ["git", "tag", "--merged", "HEAD", "--list", "v*"], text=True
    ).splitlines()
    version = nightly_version(os.environ["GITHUB_RUN_NUMBER"], tags)
    commit = subprocess.check_output(["git", "rev-parse", "--short=8", "HEAD"], text=True).strip()
    print(f"{version}+g{commit}")
