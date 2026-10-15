#!/usr/bin/env python3
"""Exercise real Lua, mailbox and log-KV against an emulated NOR Flash device."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="pico-lua-test-") as directory:
        tmp = Path(directory)
        mocks = {
            "pico.h": "#define PICO_FLASH_SIZE_BYTES (4u*1024u*1024u)\n",
            "pico/time.h": "#include <stdint.h>\nuint64_t time_us_64(void);\n",
            "pico/critical_section.h": """
                typedef int critical_section_t;
                static inline unsigned next_striped_spin_lock_num(void) { return 0; }
                static inline void critical_section_init_with_lock_num(critical_section_t *p, unsigned n) { (void)p; (void)n; }
                static inline void critical_section_enter_blocking(critical_section_t *p) { (void)p; }
                static inline void critical_section_exit(critical_section_t *p) { (void)p; }
            """,
            "pico/mutex.h": "typedef int mutex_t;\nstatic inline void mutex_init(mutex_t *p) { (void)p; }\n",
        }
        for name, body in mocks.items():
            path = tmp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(body)
        lua = ROOT / "lib/lua-5.4.6/src"
        kv = ROOT / "lib/kvstore"
        sources = [str(path) for path in lua.glob("*.c") if path.name not in ("lua.c", "luac.c")]
        sources += [str(kv / "src" / name) for name in ("kvstore.c", "kvstore_logkvs.c", "blockdevice_stage.c", "crc32_ansi.c")]
        sources += [str(ROOT / "src" / name) for name in ("lua_store.c", "lua_core.c", "lua_binder.c")]
        executable = tmp / "test"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", str(tmp), "-I", str(ROOT / "src"), "-I", str(lua),
            "-I", str(kv / "include"), "-I", str(ROOT / "lib/hidkit/include"),
            "-I", str(ROOT / "lib/hidkit/src"),
            str(ROOT / "tools/test_lua_scripts.c"), *sources, "-lm", "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True, env={**os.environ,
            "ASAN_OPTIONS": "detect_leaks=0", "UBSAN_OPTIONS": "halt_on_error=1"})


if __name__ == "__main__":
    main()
