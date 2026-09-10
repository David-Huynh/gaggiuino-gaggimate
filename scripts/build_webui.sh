#!/usr/bin/env bash
set -euo pipefail

# Build the web UI and embed it into the display firmware (GM-106).
#
# The bundle no longer ships in the LittleFS image (/w). It is gzipped and packed
# into a single blob that scripts/embed_webui.py turns into firmware-embedded,
# memory-mapped flash. LittleFS now holds only profiles (/p) and shot history
# (/h), so OTA never touches user data. data/p (seed profiles) is still staged
# for the fresh-install filesystem image.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Seed profiles still go into the filesystem image used for fresh USB installs.
mkdir -p "$ROOT/data/p"

# Build, compress and record source/output hashes for the firmware pre-build check.
python "$ROOT/scripts/build_webui.py" --install
