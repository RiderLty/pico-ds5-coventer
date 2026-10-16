#!/usr/bin/env python3
"""Host-side checks of the firmware's real USB descriptor callbacks (no board needed)."""
import ctypes as C
import os
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CONFIG = """
#define CFG_TUSB_MCU OPT_MCU_NONE
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUD_ENABLED 1
#define CFG_TUH_ENABLED 0
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED
#define CFG_TUD_HID 3
#define CFG_TUD_VENDOR 1
#define CFG_TUD_ENDPOINT0_SIZE 64
#define TUP_DCD_ENDPOINT_MAX 16
"""
STUBS = """
#include "tusb.h"
#include "ds5_pad.h"
#include "kbd_hid.h"
#include "mouse_hid.h"
unsigned test_mask;
unsigned test_setup_stage = CONTROL_STAGE_SETUP;
unsigned test_length;
uint8_t test_data[512];
void pico_get_unique_board_id_string(char *out, unsigned len) {
    const char serial[] = "0123456789ABCDEF";
    if (len > sizeof serial) len = sizeof serial;
    memcpy(out, serial, len);
    out[len - 1] = 0;
}
bool usb_km_kbd_enabled(void) { return (test_mask & 1) != 0; }
bool usb_km_mouse_enabled(void) { return (test_mask & 2) != 0; }
bool usb_km_io_enabled(void) { return (test_mask & 4) != 0; }
bool kbd_hid_enabled(void) { return usb_km_kbd_enabled(); }
uint8_t kbd_hid_instance(void) { return 1 + usb_km_mouse_enabled(); }
void kbd_hid_set_led(uint8_t leds) { (void)leds; }
uint8_t const *kbd_hid_report_desc(void) { return NULL; }
uint8_t const *mouse_hid_report_desc(void) { return NULL; }
void ds5_pad_build_report(uint8_t *out) { memset(out, 0, DS_REPORT_LEN); }
void debug_log(const char *fmt, ...) { (void)fmt; }
bool tud_control_xfer(uint8_t port, tusb_control_request_t const *req,
                      void *buf, uint16_t len) {
    (void)port;
    test_length = len < req->wLength ? len : req->wLength;
    memcpy(test_data, buf, test_length);
    return true;
}
"""


class Request(C.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [("type", C.c_uint8), ("code", C.c_uint8),
                ("value", C.c_uint16), ("index", C.c_uint16),
                ("length", C.c_uint16)]


def check(lib):
    lib.tud_descriptor_device_cb.restype = C.POINTER(C.c_uint8)
    lib.tud_descriptor_configuration_cb.restype = C.POINTER(C.c_uint8)
    lib.tud_descriptor_string_cb.restype = C.POINTER(C.c_uint16)
    lib.tud_vendor_control_xfer_cb.argtypes = [C.c_uint8, C.c_uint8, C.POINTER(Request)]
    lib.tud_vendor_control_xfer_cb.restype = C.c_bool
    lib.usb_km_apply.argtypes = [C.c_bool, C.c_bool, C.c_bool]
    mask = C.c_uint.in_dll(lib, "test_mask")
    size = C.c_uint.in_dll(lib, "test_length")
    setup_stage = C.c_uint.in_dll(lib, "test_setup_stage").value
    data = (C.c_uint8 * 512).in_dll(lib, "test_data")

    def query(index, value=0, recipient=0xC0, length=512, code=0x21):
        req = Request(recipient, code, value, index, length)
        size.value = 0
        if not lib.tud_vendor_control_xfer_cb(0, setup_stage, C.byref(req)):
            return None
        return bytes(data[:size.value])

    revisions = {}
    # Start with the actual power-on state, before usb_km_apply has ever run.
    for layout in [None, *range(8), 0, 7, 0, 4, 5, 6, 7, 4]:
        current = 0 if layout is None else layout
        kbd, mouse, io = [bool(current & bit) for bit in (1, 2, 4)]
        mask.value = current
        if layout is not None:
            lib.usb_km_apply(mouse, kbd, io)
        device = C.string_at(lib.tud_descriptor_device_cb(), 18)
        assert struct.unpack_from("<HH", device, 8) == (0x054C, 0x0CE6)
        revision = struct.unpack_from("<H", device, 12)[0]
        assert revision != 0x0101
        assert all(((revision >> shift) & 15) <= 9 for shift in (0, 4, 8, 12))
        assert revision == revisions.setdefault(current, revision)
        assert device[16] == (6 if io else 0)
        serial = lib.tud_descriptor_string_cb(6, 0)
        if io:
            assert serial
            assert C.string_at(serial, 34) == b'\x22\x03' + '0123456789ABCDEF'.encode('utf-16le')
        else:
            assert not serial

        signature = C.string_at(lib.tud_descriptor_string_cb(0xEE, 0), 18)
        assert signature == b"\x12\x03" + "MSFT100".encode("utf-16le") + b"\x21\0"

        ptr = lib.tud_descriptor_configuration_cb(0)
        config = C.string_at(ptr, ptr[2] | ptr[3] << 8)
        assert len(config) == 227 + 25 * (mouse + kbd) + 23 * io
        interfaces = []
        offset = 0
        while offset < len(config):
            length, kind = config[offset:offset + 2]
            assert length >= 2 and offset + length <= len(config)
            desc = config[offset:offset + length]
            if kind == 4 and desc[3] == 0:
                interfaces.append((desc[2], desc[5]))
            offset += length
        assert [number for number, _ in interfaces] == list(range(config[4]))

        compat = query(4)
        assert len(compat) == (40 if io else 16)
        assert struct.unpack_from("<IHHB", compat) == (len(compat), 0x0100, 4, int(io))
        assert compat[9:16] == bytes(7)
        assert query(4, length=16) == compat[:16]
        assert query(4, length=1) == compat[:1]
        assert query(4, length=0) == b""
        itf = 4 + mouse + kbd
        if io:
            assert interfaces[-1] == (itf, 0xFF)
            assert compat[16:26] == bytes([itf, 1]) + b"WINUSB\0\0"
            assert compat[26:] == bytes(14)
            props = query(5, itf, 0xC1)
            assert struct.unpack_from("<IHHH", props) == (len(props), 0x0100, 5, 1)
            section_size, kind, name_length = struct.unpack_from("<IIH", props, 10)
            assert section_size == len(props) - 10 and kind == 7
            assert props[20:20 + name_length].decode("utf-16le") == "DeviceInterfaceGUIDs\0"
            pos = 20 + name_length
            data_length = struct.unpack_from("<I", props, pos)[0]
            value = props[pos + 4:]
            assert len(value) == data_length
            assert value.decode("utf-16le") == "{56D3BAC2-F5C4-43BE-A690-71B61199C15E}\0\0"
            assert query(5, itf, 0xC1, 10) == props[:10]
            assert query(5, itf << 8, 0xC0) == props
            assert query(5, itf - 1, 0xC1) is None
        else:
            assert all(kind != 0xFF for _, kind in interfaces)
            assert query(5, itf, 0xC1) is None
        assert query(4, recipient=0xC1) is None
        assert query(4, value=1) is None
        assert query(4, recipient=0x40) is None
        assert query(4, code=0x22) is None
        assert query(6) is None
    assert len(set(revisions.values())) == 8
    print("PASS: boot, 8 layouts, repeated transitions, stable WinUSB serial/baseline identity, OS signature, Compatible ID, GUID properties, request validation")


def main():
    with tempfile.TemporaryDirectory(prefix="pico-usb-test-") as tmp:
        tmp = Path(tmp)
        config = tmp / "host_config.h"
        config.write_text(CONFIG)
        (tmp / "pico").mkdir()
        (tmp / "pico/unique_id.h").write_text('#define PICO_UNIQUE_BOARD_ID_SIZE_BYTES 8\nvoid pico_get_unique_board_id_string(char *out, unsigned len);\n')
        stubs = tmp / "stubs.c"
        stubs.write_text(STUBS)
        library = tmp / "descriptors.so"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-shared", "-fPIC",
            "-Wall", "-Wextra", "-Werror",
            f'-DCFG_TUSB_CONFIG_FILE="{config}"',
            "-I", str(tmp),
            "-I", str(ROOT / "lib/pico-sdk/lib/tinyusb/src"),
            "-I", str(ROOT / "src"),
            str(ROOT / "src/usb_descriptors.c"), str(stubs),
            "-o", str(library),
        ], check=True)
        check(C.CDLL(str(library)))


if __name__ == "__main__":
    main()
