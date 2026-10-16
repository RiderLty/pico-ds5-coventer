#!/usr/bin/env python3
"""Exercise real WinUSB framing/protocol with TinyUSB's buffered short-packet rules."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="pico-winusb-test-") as directory:
        tmp = Path(directory)
        mocks = {
            "pico/time.h": "#include <stdint.h>\nuint64_t time_us_64(void);\nuint64_t get_absolute_time(void);\nuint32_t to_ms_since_boot(uint64_t t);\n",
            "pico/critical_section.h": """
                typedef int critical_section_t;
                static inline unsigned next_striped_spin_lock_num(void) { return 0; }
                static inline void critical_section_init_with_lock_num(critical_section_t *p, unsigned n) { (void)p; (void)n; }
                static inline void critical_section_enter_blocking(critical_section_t *p) { (void)p; }
                static inline void critical_section_exit(critical_section_t *p) { (void)p; }
            """,
            "tusb.h": """
                #include <stdint.h>
                uint32_t tud_vendor_write_available(void);
                uint32_t tud_vendor_write(const void *buffer, uint32_t len);
                uint32_t tud_vendor_write_flush(void);
                uint32_t tud_vendor_read(void *buffer, uint32_t len);
            """,
        }
        for name, body in mocks.items():
            path = tmp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(body)
        executable = tmp / "test"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g",
            "-Wall", "-Wextra", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", str(tmp), "-I", str(ROOT / "src"),
            str(ROOT / "tools/test_usb_io.c"), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True, env={**os.environ,
            "ASAN_OPTIONS": "detect_leaks=0", "UBSAN_OPTIONS": "halt_on_error=1"})


if __name__ == "__main__":
    main()
