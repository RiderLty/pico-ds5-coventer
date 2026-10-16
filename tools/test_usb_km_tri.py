#!/usr/bin/env python3
"""Verify dev_enable's tri-state merge against the real usb_km.h (no USB stack needed)."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="pico-usb-km-test-") as directory:
        executable = Path(directory) / "test"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g",
            "-Wall", "-Wextra",
            "-I", str(ROOT / "src"),
            str(ROOT / "tools" / "test_usb_km_tri.c"),
            "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
