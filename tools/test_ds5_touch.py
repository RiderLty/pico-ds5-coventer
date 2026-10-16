#!/usr/bin/env python3
"""Verify actual DS5 touch output bytes and decode through the existing input parser."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="pico-ds5-touch-") as directory:
    tmp = Path(directory)
    (tmp / "pico").mkdir()
    (tmp / "pico/time.h").write_text("")
    executable = tmp / "test"
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(tmp), "-I", str(ROOT / "src"),
        "-I", str(ROOT / "lib/hidkit/include"), "-I", str(ROOT / "lib/hidkit/src"),
        str(ROOT / "tools/test_ds5_touch.c"), str(ROOT / "src/ds5_pad.c"),
        "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
