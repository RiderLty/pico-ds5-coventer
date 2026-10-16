#ifndef DS5_BT_REPORT_H
#define DS5_BT_REPORT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// L2CAP HID frames include the 0xA1/0xA2 transaction byte in their CRC.
bool ds5_bt_report_to_usb(const uint8_t *packet, size_t size, uint8_t usb[64]);
void ds5_bt_make_init_output(uint8_t packet[143]);
#endif
