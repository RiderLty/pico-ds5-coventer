#include "ds5_bt_report.h"
#include <string.h>

static uint32_t crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    while (size--) {
        crc ^= *data++;
        for (unsigned i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool ds5_bt_report_to_usb(const uint8_t *packet, size_t size, uint8_t usb[64])
{
    // Standard full report: HID DATA header + 78-byte Bluetooth report.
    // Short 0x01 reports and audio/microphone payloads have different layouts.
    if (!packet || !usb || size != 79 || packet[0] != 0xa1 ||
        packet[1] != 0x31 || (packet[2] & 2u)) return false;
    uint32_t received = (uint32_t)packet[75] | ((uint32_t)packet[76] << 8) |
                        ((uint32_t)packet[77] << 16) | ((uint32_t)packet[78] << 24);
    if (received != crc32(packet, size - 4)) return false;
    usb[0] = 0x01;
    // BT seq/tag is outside the common DS5 input body; touch coordinates retain
    // their native contact/X-low/XY-nibble/Y-high byte order and scale.
    memcpy(usb + 1, packet + 3, 63);
    return true;
}

void ds5_bt_make_init_output(uint8_t packet[143])
{
    // DS5Dongle / pico-hid-mapper mode-init sequence: output 0x32 / 0x10.
    // This enables full 0x31 input reports on controllers sending short 0x01.
    static const uint8_t state[] = {
        0x90, 0x3f,
        0xfd, 0xf7, 0, 0, 0x7f, 0x7f,
        0xff, 9, 0, 15, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 10,
        7, 0, 0, 2, 1, 0, 31, 0, 27
    };
    memset(packet, 0, 143);
    packet[0] = 0xa2;
    packet[1] = 0x32;
    packet[2] = 0x10;
    memcpy(packet + 3, state, sizeof(state));
    uint32_t crc = crc32(packet, 139);
    for (unsigned i = 0; i < 4; ++i) packet[139 + i] = (uint8_t)(crc >> (8 * i));
}
