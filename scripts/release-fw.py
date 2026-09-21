#!/usr/bin/env python3
"""Compile WT32 firmware, publish ota/, then commit and push.

Windows:
  python scripts/release-fw.py
Linux / macOS:
  python3 scripts/release-fw.py

Flags:
  -y / --yes       do not prompt before commit+push
  -m MESSAGE       commit message (default: Release firmware VERSION)
  --no-push        compile and update files, skip git
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIRMWARE = ROOT / "firmware"
CONFIG_H = FIRMWARE / "include" / "cof_config.h"
MANIFEST = ROOT / "ota" / "manifest.json"
PIO_BIN = FIRMWARE / ".pio" / "build" / "wt32-eth01" / "firmware.bin"
OTA_BIN = ROOT / "ota" / "firmware.bin"
# Devices flashed before the ota/ move still poll the old path. Keep both
# manifests byte-identical until the fleet has been updated. Remove this once
# every device runs a firmware whose COF_MANIFEST_URL points at ota/.
LEGACY_MANIFEST = ROOT / "actual_version" / "manifest.json"


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    print("+", " ".join(cmd), flush=True)
    return subprocess.run(cmd, cwd=kwargs.pop("cwd", ROOT), check=True, **kwargs)


def find_pio() -> str:
    for name in ("pio", "platformio"):
        found = shutil.which(name)
        if found:
            return found
    home = Path.home()
    candidates = [
        home / ".platformio" / "penv" / "Scripts" / "pio.exe",
        home / ".platformio" / "penv" / "Scripts" / "pio.cmd",
        home / ".platformio" / "penv" / "bin" / "pio",
    ]
    for path in candidates:
        if path.is_file():
            return str(path)
    sys.exit("pio not found. Install PlatformIO or add it to PATH.")


def firmware_version() -> str:
    text = CONFIG_H.read_text(encoding="utf-8")
    match = re.search(r'#define\s+COF_FIRMWARE_VERSION\s+"([^"]+)"', text)
    if not match:
        sys.exit(f"COF_FIRMWARE_VERSION not found in {CONFIG_H}")
    return match.group(1)


def confirm(prompt: str) -> bool:
    try:
        answer = input(prompt).strip().lower()
    except EOFError:
        return False
    return answer in {"y", "yes"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-y", "--yes", action="store_true")
    parser.add_argument("-m", "--message")
    parser.add_argument("--no-push", action="store_true")
    args = parser.parse_args()

    version = firmware_version()
    print(f"Firmware version: {version}", flush=True)

    run([find_pio(), "run"], cwd=FIRMWARE)
    if not PIO_BIN.is_file():
        sys.exit(f"missing build output: {PIO_BIN}")

    OTA_BIN.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(PIO_BIN, OTA_BIN)
    sha256 = hashlib.sha256(OTA_BIN.read_bytes()).hexdigest()
    print(f"Copied {OTA_BIN.relative_to(ROOT)}  sha256={sha256}", flush=True)

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    manifest.setdefault("firmware", {})
    manifest["firmware"]["version"] = version
    manifest["firmware"]["sha256"] = sha256
    MANIFEST.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Updated {MANIFEST.relative_to(ROOT)} -> {version}", flush=True)

    # Keep the pre-move path working for devices still polling actual_version/.
    if LEGACY_MANIFEST.parent.is_dir():
        LEGACY_MANIFEST.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        print(f"Mirrored {LEGACY_MANIFEST.relative_to(ROOT)} -> {version}", flush=True)

    if args.no_push:
        print("Skipping git (--no-push).")
        return 0

    paths_to_add = [str(OTA_BIN.relative_to(ROOT)), str(MANIFEST.relative_to(ROOT))]
    if LEGACY_MANIFEST.is_file():
        paths_to_add.append(str(LEGACY_MANIFEST.relative_to(ROOT)))
    run(["git", "add", *paths_to_add])
    run(["git", "status", "--short"])

    message = args.message or f"Release firmware {version}"
    if not args.yes:
        print(f"Commit message: {message}")
        if not confirm("Commit and push to origin? [y/N] "):
            print("Aborted.")
            return 0

    env = os.environ.copy()
    result = subprocess.run(
        ["git", "diff", "--cached", "--quiet"],
        cwd=ROOT,
        env=env,
    )
    if result.returncode == 0:
        print("Nothing to commit (firmware already published).")
        run(["git", "push", "-u", "origin", "HEAD"])
        return 0

    run(["git", "commit", "-m", message], env=env)
    run(["git", "push", "-u", "origin", "HEAD"], env=env)
    print("Done. On the VPS: cd /opt/callonfail && git pull")
    print("Then press OTA on the Flask device page.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
