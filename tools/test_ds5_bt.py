#!/usr/bin/env python3
"""Host tests of the actual BT report bridge and lifecycle, with real BTstack event layouts."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BT = ROOT / 'lib/pico-sdk/lib/btstack'
with tempfile.TemporaryDirectory(prefix='pico-ds5-bt-') as directory:
    tmp = Path(directory)
    (tmp / 'pico').mkdir()
    (tmp / 'hardware').mkdir()
    (tmp / 'pico/cyw43_arch.h').write_text('''
#include <stdint.h>
#include <stdbool.h>
#define CYW43_WL_GPIO_LED_PIN 0
#define PICO_FLASH_SIZE_BYTES (4u * 1024u * 1024u)
typedef uint64_t absolute_time_t;
absolute_time_t get_absolute_time(void);
uint32_t to_ms_since_boot(absolute_time_t time);
int cyw43_arch_init(void);
void cyw43_arch_poll(void);
void cyw43_arch_gpio_put(unsigned pin, bool value);
''')
    (tmp / 'pico/btstack_flash_bank.h').write_text('#define PICO_FLASH_BANK_TOTAL_SIZE 8192u\n')
    (tmp / 'hardware/flash.h').write_text('#define FLASH_SECTOR_SIZE 4096u\n')
    # Independent fixtures/CRC built by Python's standard CRC implementation.
    import zlib
    packet = bytearray(79)
    packet[:3] = bytes([0xa1, 0x31, 0x01])
    packet[3:7] = bytes([128, 255, 128, 128])
    packet[7:9] = bytes([255, 0])
    packet[10:14] = bytes([0x28, 0, 0x10, 0])  # Cross and Edge FN1
    packet[18:20] = (1234).to_bytes(2, 'little')
    packet[24:26] = (8192).to_bytes(2, 'little')
    packet[35:43] = bytes([0, 0x23, 0x41, 0x23, 0x81, 0x7f, 0xe7, 0x3a])
    packet[75:] = zlib.crc32(packet[:75]).to_bytes(4, 'little')
    (tmp / 'fixture.h').write_text('static const uint8_t fixture[79] = {' +
        ','.join(map(str, packet)) + '};\n')
    compiler = os.environ.get('CC', 'cc')
    common = [compiler, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
              '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
              '-DENABLE_DS5_BT=1', '-I', str(tmp), '-I', str(ROOT / 'src'),
              '-I', str(BT / 'src')]
    for name, sources, extra in [
        ('report', [ROOT / 'tools/test_ds5_bt_report.c', ROOT / 'src/ds5_bt_report.c'],
         ['-I', str(ROOT / 'lib/hidkit/src'), '-I', str(ROOT / 'lib/hidkit/include')]),
        ('lifecycle', [ROOT / 'tools/test_ds5_bt.c', ROOT / 'src/ds5_bt_report.c'], [])
    ]:
        executable = tmp / name
        subprocess.run(common + extra + list(map(str, sources)) + ['-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
