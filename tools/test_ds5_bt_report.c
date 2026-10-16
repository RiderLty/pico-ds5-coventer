#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ds5_bt_report.h"
#include "layouts/gamepad_ds_edge.h"
#include "fixture.h"

int main(void)
{
    uint8_t usb[64], packet[79], output[143];
    assert(ds5_bt_report_to_usb(fixture, sizeof(fixture), usb));
    assert(usb[0] == 1 && !memcmp(usb + 1, fixture + 3, 63));
    gamepad_state_t state;
    assert(ds_edge_parse(usb, sizeof(usb), &state));
    assert(state.ls_x == 0 && state.ls_y == 32512 && state.lt == 32640);
    assert(state.buttons & (1u << 5)); // Cross
    assert(state.buttons & DS_EDGE_BTN_FN1);
    assert(state.gyro_x == 1234 && state.accel_x == 8192);
    assert((state.touchpad.meta & 3) == 1);
    assert(state.touchpad.x[0] == 0x123 && state.touchpad.y[0] == 0x234);
    for (unsigned size = 0; size < 79; ++size)
        assert(!ds5_bt_report_to_usb(fixture, size, usb));
    assert(!ds5_bt_report_to_usb(NULL, 79, usb));
    assert(!ds5_bt_report_to_usb(fixture, 79, NULL));
    // Corruption anywhere in header, common body, tail or CRC must be rejected.
    for (unsigned i = 0; i < 79; ++i) {
        memcpy(packet, fixture, 79); packet[i] ^= 1;
        assert(!ds5_bt_report_to_usb(packet, 79, usb));
    }
    memcpy(packet, fixture, 79); packet[2] |= 2;
    assert(!ds5_bt_report_to_usb(packet, 79, usb));
    ds5_bt_make_init_output(output);
    assert(output[0] == 0xa2 && output[1] == 0x32 && output[2] == 0x10);
    // Independent golden CRC from Python zlib over the reference mode-init bytes.
    assert(output[139] == 0x28 && output[140] == 0xda &&
           output[141] == 0xa1 && output[142] == 0x79);
    puts("PASS: Bluetooth CRC/length/corruption, DS5/Edge axes/buttons/motion/touch, init output");
}
