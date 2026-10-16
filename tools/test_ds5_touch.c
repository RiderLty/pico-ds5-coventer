// Real DS5 output -> real hidkit input parser, plus independent wire-byte fixtures.
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "ds5_pad.h"
#include "input_queue.h"
#include "layouts/gamepad_ds5.h"

static uint8_t report[1 + DS_REPORT_LEN];
static void build(void) {
    ds5_pad_tick(); report[0] = 1; ds5_pad_build_report(report + 1);
}
static void feed(uint16_t x0,uint16_t y0,uint16_t x1,uint16_t y1,uint8_t flags) {
    uint8_t p[INQ_TOUCH_PAYLOAD] = {x0,x0>>8,y0,y0>>8,x1,x1>>8,y1,y1>>8,flags};
    ds5_pad_feed_touch(p,sizeof p); build();
}
static gamepad_state_t parse(void) {
    gamepad_state_t out;
    assert(ds5_parse(report,sizeof report,&out));
    return out;
}
int main(void) {
    _Static_assert(sizeof(struct dualsense_input_report)==63,"USB body must be 63 bytes");
    _Static_assert(offsetof(struct dualsense_input_report,points_1)==32,"touch slot0 offset");
    _Static_assert(offsetof(struct dualsense_input_report,points_2)==36,"touch slot1 offset");
    ds5_pad_init(); build();
    assert(report[33]==0x80 && report[37]==0x80);
    gamepad_state_t out=parse(); assert(!(out.touchpad.meta & 3));
    feed(0x123,0x234,1919,942,3);
    // [ID/active, Xlo, Ylo4|Xhi4, Yhi8], ID1 differs from ID0.
    const uint8_t expected[]={0x00,0x23,0x41,0x23, 0x01,0x7f,0xe7,0x3a};
    assert(!memcmp(report+33,expected,sizeof expected));
    out=parse(); assert((out.touchpad.meta&3)==3);
    assert(out.touchpad.x[0]==0x123 && out.touchpad.y[0]==0x234);
    assert(out.touchpad.x[1]==1919 && out.touchpad.y[1]==942);
    // The low X byte used to be interpreted as flags: cross bit7 and nibble boundaries.
    const uint16_t coords[]={0,1,15,16,127,128,255,256,942,943,1919,1920,4095};
    for(unsigned i=0;i<sizeof coords/sizeof *coords;i++)
        for(unsigned j=0;j<sizeof coords/sizeof *coords;j++) {
            feed(coords[i],coords[j],coords[j],coords[i],3); out=parse();
            assert(out.touchpad.x[0]==coords[i] && out.touchpad.y[0]==coords[j]);
            assert(out.touchpad.x[1]==coords[j] && out.touchpad.y[1]==coords[i]);
            assert(report[33]==0 && report[37]==1); // IDs stay stable during movement
        }
    feed(1234,567,800,900,3);
    feed(0,0,800,900,2); // absent source coordinates are zero
    const uint8_t released[]={0x80,0xd2,0x74,0x23};
    assert(!memcmp(report+33,released,4));
    assert(report[37]==1); out=parse(); assert((out.touchpad.meta&3)==2);
    feed(100,200,800,900,3); assert(report[33]==2 && report[37]==1);
    // Tracking counter wraps in 7 bits without colliding with the other held finger.
    for(unsigned i=0;i<300;i++) {
        feed(0,0,800,900,2); feed(100,200,800,900,3);
        assert(!(report[33]&0x80) && report[33]!=report[37] && report[37]==1);
    }
    feed(65535,65535,0,0,1); out=parse();
    assert(out.touchpad.x[0]==4095 && out.touchpad.y[0]==4095);
    uint8_t disconnect[INQ_PAD_PAYLOAD]={0};
    ds5_pad_feed_pad(disconnect,sizeof disconnect); build(); out=parse();
    assert(!(out.touchpad.meta&3));
    assert((report[33]&0x80) && (report[37]&0x80));
    feed(1920,943,0,0,1); out=parse();
    assert((out.touchpad.meta&3)==1 && out.touchpad.x[0]==1920 && out.touchpad.y[0]==943);
    puts("PASS: DS5 wire fixtures, real hidkit roundtrip, coordinate boundaries, stable/unique/wrapping IDs, release, disconnect, saturation");
}
