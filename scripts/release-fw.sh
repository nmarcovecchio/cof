#!/usr/bin/env bash
# Compile ESP32 firmware, update actual_version/, commit and push.
# From the repo root: ./scripts/release-fw.sh
set -euo pipefail
cd "$(dirname "$0")/.."
if command -v python3 >/dev/null 2>&1; then
  exec python3 scripts/release-fw.py "$@"
fi
exec python scripts/release-fw.py "$@"
