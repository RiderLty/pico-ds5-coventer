// Actual protocol and WinUSB adapter; only TinyUSB's endpoint/FIFO is mocked.
#include <assert.h>
#include <stdio.h>
#include "../src/proto.c"
#include "../src/usb_io.c"

static uint8_t fifo[256], wire[4096];
static unsigned queued, wire_size, in_flight, flushes;
static bool endpoint_busy;
static uint8_t script_body[512];
static uint16_t script_size;

uint64_t time_us_64(void) { return 1234567; }
uint64_t get_absolute_time(void) { return time_us_64(); }
uint32_t to_ms_since_boot(uint64_t t) { return t / 1000; }
void debug_log(const char *fmt, ...) { (void)fmt; }
void uart_proto_init(void) {}
uart_proto_stats_t uart_proto_stats(void) { return (uart_proto_stats_t){0}; }
bool usb_km_kbd_enabled(void) { return false; }
bool usb_km_mouse_enabled(void) { return false; }
bool usb_km_io_enabled(void) { return true; }
void lua_script_request(proto_chan_t *ch, uint16_t id, const uint8_t *data, uint16_t len) {
    assert(len == 1 && data[0] == 3);
    assert(proto_chan_submit(ch, PROTO_CMD_SCRIPT, id, script_body, script_size));
}

uint32_t tud_vendor_read(void *buffer, uint32_t len) { (void)buffer; (void)len; return 0; }
uint32_t tud_vendor_write_available(void) { return sizeof fifo - queued; }
uint32_t tud_vendor_write_flush(void) {
    ++flushes;
    if (endpoint_busy || !queued) return 0;
    in_flight = queued < 64 ? queued : 64;
    assert(wire_size + in_flight <= sizeof wire);
    memcpy(wire + wire_size, fifo, in_flight);
    wire_size += in_flight;
    queued -= in_flight;
    memmove(fifo, fifo + in_flight, queued);
    endpoint_busy = true;
    return in_flight;
}
uint32_t tud_vendor_write(const void *buffer, uint32_t len) {
    assert(len <= tud_vendor_write_available());
    memcpy(fifo + queued, buffer, len);
    queued += len;
    // TinyUSB auto-flushes only when at least one full packet has accumulated.
    if (queued >= 64) tud_vendor_write_flush();
    return len;
}
static void complete_transfer(void) {
    if (!endpoint_busy) return;
    endpoint_busy = false;
    in_flight = 0;
    tud_vendor_write_flush(); // TinyUSB completion also flushes queued tails.
}
static void drain(void) {
    for (unsigned i = 0; i < 100; ++i) {
        usb_io_task();
        complete_transfer();
        if (!s_chan.used && !s_chan.tx_active && !queued && !endpoint_busy) return;
    }
    assert(!"reply stalled without later logs or requests");
}

// Decode independently and verify the entire reply, including escaped tails.
static void check_reply(uint16_t id, const uint8_t *body, unsigned size) {
    unsigned got = 0, chunks = 0, raw_size = 0;
    bool escape = false, ended = false;
    uint8_t raw[66];
    for (unsigned i = 0; i < wire_size; ++i) {
        uint8_t b = wire[i];
        if (b == 0xC0) {
            if (!raw_size) continue;
            assert(!escape && raw_size >= 4 && !ended);
            uint16_t crc = raw[raw_size-2] | (uint16_t)raw[raw_size-1] << 8;
            assert(crc16_update(0xFFFF, raw, raw_size-2) == crc);
            assert(raw[0] == chunks++);
            unsigned pos = 2;
            if (raw[1] & PROTO_F_START) {
                assert(chunks == 1 && raw[pos++] == PROTO_CMD_SCRIPT);
                assert((raw[pos] | (unsigned)raw[pos+1] << 8) == id);
                pos += 2;
                unsigned declared = 0, shift = 0;
                do {
                    b = raw[pos++]; declared |= (unsigned)(b & 127) << shift; shift += 7;
                } while (b & 128);
                assert(declared == size);
            }
            unsigned count = raw_size - 2 - pos;
            assert(got + count <= size && !memcmp(raw + pos, body + got, count));
            got += count;
            ended = (raw[1] & PROTO_F_END) != 0;
            raw_size = 0;
        } else if (escape) {
            assert(b == 0xDC || b == 0xDD);
            raw[raw_size++] = b == 0xDC ? 0xC0 : 0xDB;
            escape = false;
        } else if (b == 0xDB) escape = true;
        else { assert(raw_size < sizeof raw); raw[raw_size++] = b; }
    }
    assert(ended && got == size && !raw_size && !queued && !endpoint_busy);
}

int main(void) {
    usb_io_init();
    const unsigned sizes[] = {0, 1, 2, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 127, 128, 255, 256, 511, 512};
    for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; ++s) {
        usb_io_set_instance(1);
        wire_size = flushes = 0;
        script_size = sizes[s];
        for (unsigned i = 0; i < script_size; ++i) script_body[i] = (uint8_t)(i * 37 + 0xC0);
        // No DEBUG or Lua logs: RUN acknowledgments must flush on their own.
        const uint8_t run[] = {3, PROTO_CMD_SCRIPT, 42, 0, 1, 3};
        proto_chan_rx(&s_chan, run, sizeof run);
        drain();
        check_reply(42, script_body, script_size);
        assert(flushes > 0);
    }

    // Busy endpoint + insufficient FIFO capacity must retry the whole frame.
    usb_io_set_instance(1);
    wire_size = 0;
    queued = sizeof fifo - 1;
    endpoint_busy = true;
    assert(proto_chan_submit(&s_chan, PROTO_CMD_SCRIPT, 42, script_body, 512));
    usb_io_task();
    assert(s_chan.tx_active && s_chan.tx_off == 0 && s_seq == 0);
    queued = 0; endpoint_busy = false; // host resumes after the blocked transfer
    drain();
    check_reply(42, script_body, 512);
    puts("PASS: silent WinUSB RUN ACK, 0..512B replies, escaping, packet tails and endpoint backpressure");
}
