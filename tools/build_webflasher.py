"""Build the firmware and package it for the browser flasher in webflasher/.

    python tools/build_webflasher.py

Creates webflasher/firmware.bin: bootloader, partition table and application merged into
one image for flash offset 0. The gap where the settings are stored is filled with empty
data, so flashing this image also deletes the stored configuration.
Also writes webflasher/version.json, which the page shows. The version text comes from
the environment variable FW_VERSION (set by the GitHub workflow).
"""
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENV = "esp32s3cam"
BUILD = ROOT / ".pio" / "build" / ENV
PACKAGES = Path.home() / ".platformio" / "packages"
OUT = ROOT / "webflasher"

subprocess.run([sys.executable, "-m", "platformio", "run", "-d", str(ROOT), "-e", ENV], check=True)

subprocess.run(
    [
        sys.executable, str(PACKAGES / "tool-esptoolpy" / "esptool.py"),
        "--chip", "esp32s3", "merge_bin",
        "-o", str(OUT / "firmware.bin"),
        "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB",
        "0x0", str(BUILD / "bootloader.bin"),
        "0x8000", str(BUILD / "partitions.bin"),
        "0xe000", str(PACKAGES / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin"),
        "0x10000", str(BUILD / "firmware.bin"),
    ],
    check=True,
)

version = {
    "version": os.environ.get("FW_VERSION", "local build"),
    "built": time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime()),
}
(OUT / "version.json").write_text(json.dumps(version) + "\n")
print(f"\nWeb flasher ready: {version['version']}, {version['built']}")
print("Serve it with:  python -m http.server 8000 --directory webflasher")
print("Then open:      http://localhost:8000/")
