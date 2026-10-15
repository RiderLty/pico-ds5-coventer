#!/usr/bin/env python3
"""Run actual firmware protocol/UART code against simulated DMA and UART hardware."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MOCKS = r"""
#ifndef HOST_HARDWARE_H
#define HOST_HARDWARE_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef unsigned uint;
typedef int critical_section_t;
typedef uint64_t absolute_time_t;
static uint64_t mock_time = 1234567;
static inline uint64_t time_us_64(void) { return mock_time; }
static inline absolute_time_t get_absolute_time(void) { return mock_time; }
static inline uint32_t to_ms_since_boot(absolute_time_t t) { return t / 1000; }
static inline uint next_striped_spin_lock_num(void) { return 0; }
static inline void critical_section_init_with_lock_num(critical_section_t *c, uint n) { (void)c; (void)n; }
static inline void critical_section_enter_blocking(critical_section_t *c) { (void)c; }
static inline void critical_section_exit(critical_section_t *c) { (void)c; }
typedef struct { uint32_t dr, rsr; } uart_hw_t;
static uart_hw_t mock_uart;
#define uart0 0
#define UART_UARTRSR_BITS 15u
#define UART_IRQ_NUM(u) (u)
static inline uart_hw_t *uart_get_hw(uint u) { (void)u; return &mock_uart; }
static inline uint uart_get_dreq(uint u, bool tx) { (void)u; (void)tx; return 0; }
static inline void uart_set_irqs_enabled(uint u, bool rx, bool tx) { (void)u; (void)rx; (void)tx; }
static inline void irq_set_enabled(uint u, bool en) { (void)u; (void)en; }
typedef struct { uintptr_t write_addr; uint32_t transfer_count; } mock_dma_channel_t;
typedef struct { mock_dma_channel_t ch[16]; } mock_dma_t;
static mock_dma_t mock_dma;
#define dma_hw (&mock_dma)
static bool mock_busy;
typedef int dma_channel_config;
#define DMA_SIZE_8 0
#define __compiler_memory_barrier() __asm__ volatile ("" ::: "memory")
static inline void dma_channel_claim(uint n) { (void)n; }
static inline bool dma_channel_is_busy(uint n) { (void)n; return mock_busy; }
static inline uint32_t dma_encode_transfer_count_with_self_trigger(uint32_t n) { return n | (1u << 28); }
static inline void dma_channel_set_read_addr(uint n, const volatile void *a, bool t) { (void)n; (void)a; (void)t; }
static inline void dma_channel_set_write_addr(uint n, void *a, bool t) { (void)t; dma_hw->ch[n].write_addr = (uintptr_t)a; }
static inline void dma_channel_set_transfer_count(uint n, uint32_t c, bool t) { (void)t; dma_hw->ch[n].transfer_count = c; }
static inline void dma_channel_start(uint n) { (void)n; mock_busy = true; }
static inline dma_channel_config dma_channel_get_default_config(uint n) { (void)n; return 0; }
static inline void channel_config_set_transfer_data_size(dma_channel_config *c, int n) { (void)c; (void)n; }
static inline void channel_config_set_read_increment(dma_channel_config *c, bool v) { (void)c; (void)v; }
static inline void channel_config_set_write_increment(dma_channel_config *c, bool v) { (void)c; (void)v; }
static inline void channel_config_set_dreq(dma_channel_config *c, uint n) { (void)c; (void)n; }
static inline void channel_config_set_ring(dma_channel_config *c, bool w, uint n) { (void)c; (void)w; (void)n; }
static inline void channel_config_set_irq_quiet(dma_channel_config *c, bool v) { (void)c; (void)v; }
static inline void dma_channel_configure(uint n, const dma_channel_config *c, void *w,
                                        const volatile void *r, uint32_t count, bool trigger) {
    (void)c; (void)r;
    dma_channel_set_write_addr(n, w, false);
    dma_channel_set_transfer_count(n, count, false);
    mock_busy = trigger;
}
#endif
"""
TEST = r"""
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>
#include "proto.c"
#include "uart_proto.c"

void debug_log(const char *fmt, ...) { (void)fmt; }
bool usb_km_kbd_enabled(void) { return true; }
bool usb_km_mouse_enabled(void) { return true; }
bool usb_km_io_enabled(void) { return true; }

static uint8_t output[8 * 1024 * 1024 + 512];
static uint32_t output_len, output_total, complete_count;
static uint8_t output_cmd;
static uint16_t output_id;
static bool output_active;

// Independent receiver: no START may interrupt a message before its END.
static void capture(const uint8_t *chunk, uint16_t len) {
    uint8_t flags = *chunk++;
    --len;
    if (flags & PROTO_F_START) {
        assert(!output_active);
        output_active = true;
        output_cmd = *chunk++;
        output_id = chunk[0] | ((uint16_t)chunk[1] << 8);
        chunk += 2;
        len -= 3;
        output_total = 0;
        unsigned shift = 0;
        uint8_t b;
        do { b = *chunk++; --len; output_total |= (uint32_t)(b & 127) << shift; shift += 7; } while (b & 128);
        output_len = 0;
    }
    assert(output_active && output_len + len <= sizeof output);
    memcpy(output + output_len, chunk, len);
    output_len += len;
    if (flags & PROTO_F_END) {
        assert(output_len == output_total);
        if (output_cmd == PROTO_CMD_TEST) {
            uint32_t expected;
            memcpy(&expected, output + output_len - 4, 4);
            assert(crc32(0, output, output_len - 4) == expected);
        }
        output_active = false;
        complete_count++;
    }
}

static uint8_t serial_frame[256];
static uint16_t serial_len;
static bool serial_escape;
void debug_write_raw(const uint8_t *buf, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) {
        uint8_t b = buf[i];
        if (b == 0xC0) {
            if (serial_len) {
                assert(!serial_escape && serial_len >= 4);
                uint16_t crc = serial_frame[serial_len - 2] | serial_frame[serial_len - 1] << 8;
                assert(crc16_update(0xFFFF, serial_frame, serial_len - 2) == crc);
                capture(serial_frame + 1, serial_len - 3);
            }
            serial_len = 0; serial_escape = false;
        } else if (serial_escape) {
            assert(b == 0xDC || b == 0xDD);
            serial_frame[serial_len++] = b == 0xDC ? 0xC0 : 0xDB;
            serial_escape = false;
        } else if (b == 0xDB) serial_escape = true;
        else serial_frame[serial_len++] = b;
    }
}

static unsigned attempts;
static bool backpressure;
static bool ready(proto_chan_t *ch) { (void)ch; return true; }
static bool send_chunk(proto_chan_t *ch, const uint8_t *chunk, uint16_t len) {
    (void)ch;
    if (backpressure && ++attempts % 3 == 1) return false;
    capture(chunk, len);
    return true;
}
static const proto_transport_t transport = { "host", ready, send_chunk };

static uint32_t dma_produced;
static void dma_feed(const uint8_t *bytes, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) {
        uart_rx_ring[dma_produced & (UART_RX_RING_SIZE - 1)] = bytes[i];
        dma_produced = (dma_produced + 1) & UART_RX_DMA_MASK;
    }
    dma_hw->ch[UART_RX_DMA].transfer_count =
        dma_encode_transfer_count_with_self_trigger(UART_RX_DMA_PERIOD - dma_produced);
    dma_hw->ch[UART_RX_DMA].write_addr = (uintptr_t)uart_rx_ring + (dma_produced & (UART_RX_RING_SIZE - 1));
}

static void feed_test(proto_chan_t *ch, uint32_t size, bool serial) {
    uint8_t *input = malloc(size);
    assert(input && size >= 4);
    for (uint32_t i = 0; i < size - 4; ++i) input[i] = (uint8_t)(i * 37 + 0xC0);
    uint32_t crc = crc32(0, input, size - 4);
    memcpy(input + size - 4, &crc, 4);
    uint32_t off = 0;
    uint8_t seq = 0;
    while (off < size) {
        uint8_t chunk[62];
        uint8_t *p = chunk + 1;
        if (!off) {
            *p++ = PROTO_CMD_TEST; *p++ = 42; *p++ = 0;
            p = varint_put(p, size);
        }
        uint16_t n = (uint16_t)(chunk + sizeof chunk - p);
        if (n > size - off) n = (uint16_t)(size - off);
        chunk[0] = (!off ? PROTO_F_START : 0) | (off + n == size ? PROTO_F_END : 0);
        memcpy(p, input + off, n);
        uint16_t len = (uint16_t)(p + n - chunk);
        if (serial) {
            uint8_t raw[66], encoded[134];
            raw[0] = seq++;
            memcpy(raw + 1, chunk, len);
            uint16_t frame_crc = crc16_update(0xFFFF, raw, len + 1);
            raw[len + 1] = (uint8_t)frame_crc;
            raw[len + 2] = (uint8_t)(frame_crc >> 8);
            uint16_t wire_len = slip_encode(raw, len + 3, encoded);
            // Deliberately split inside frame boundaries, including escapes.
            for (uint16_t pos = 0; pos < wire_len;) {
                uint16_t take = wire_len - pos;
                if (take > 17) take = 17;
                dma_feed(encoded + pos, take);
                uart_proto_task();
                pos += take;
            }
        } else proto_chan_rx(ch, chunk, len);
        off += n;
    }
    assert(!ch->rx_active && !ch->stream_rx);
    free(input);
}

static void drain(proto_chan_t *ch, bool serial) {
    unsigned limit = 500000;
    while (ch->stream_tx || ch->tx_active || ch->used) {
        assert(limit--);
        if (serial) uart_proto_task(); else proto_chan_task(ch);
    }
    assert(!output_active);
}

int main(void) {
    proto_init();
    assert(crc16_update(0xFFFF, (const uint8_t *)"123456789", 9) == 0x29B1);
    const uint32_t sizes[] = {4, 5, 60, 61, 62, 63, 64, 65, 32*1024, 64*1024, 256*1024, 512*1024, 1024*1024, 2*1024*1024, 4*1024*1024, 8*1024*1024};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        proto_chan_t ch;
        proto_chan_init(&ch, &transport); ch.active = true;
        backpressure = true;
        unsigned before = complete_count;
        feed_test(&ch, sizes[i], false);
        drain(&ch, false);
        assert(complete_count == before + 1 && output_len == sizes[i]);
    }

    // A partial PRINT must finish with intact bytes before TEST begins.
    proto_chan_t ch;
    proto_chan_init(&ch, &transport); ch.active = true;
    uint8_t print[180]; memset(print, 0x5A, sizeof print);
    backpressure = false;
    proto_chan_submit(&ch, PROTO_CMD_PRINT, PROTO_ID_BROADCAST, print, sizeof print);
    proto_chan_task(&ch); assert(ch.tx_active);
    unsigned before = complete_count;
    feed_test(&ch, 1000, false);
    while (ch.tx_active) proto_chan_task(&ch);
    assert(complete_count == before + 1 && output_len == sizeof print);
    assert(memcmp(output, print, sizeof print) == 0);
    drain(&ch, false); assert(complete_count == before + 2);

    // Multi-chunk ring messages remain byte-exact across repeated ring wraps.
    uint8_t message[383];
    for (unsigned i = 0; i < sizeof message; ++i) message[i] = (uint8_t)(i * 73);
    for (unsigned i = 0; i < 24; ++i) {
        proto_chan_submit(&ch, PROTO_CMD_PRINT, PROTO_ID_BROADCAST, message, sizeof message);
        drain(&ch, false);
        assert(output_len == sizeof message && memcmp(output, message, sizeof message) == 0);
        assert(output_id == PROTO_ID_BROADCAST);
    }

    // A missing END can recover on the next START; a short TEST is rejected.
    uint8_t partial[] = {1, PROTO_CMD_TEST, 1, 0, 100, 42};
    proto_chan_rx(&ch, partial, sizeof partial); assert(ch.stream_rx);
    feed_test(&ch, 100, false); drain(&ch, false);
    uint8_t invalid[] = {3, PROTO_CMD_TEST, 1, 0, 0};
    proto_chan_rx(&ch, invalid, sizeof invalid); assert(!ch.stream_rx && !ch.stream_tx && !ch.rx_active);
    uint8_t bad_crc[] = {3, PROTO_CMD_TEST, 1, 0, 4, 1, 2, 3, 4};
    proto_chan_rx(&ch, bad_crc, sizeof bad_crc);
    assert(!ch.rx_active && !ch.stream_rx && !ch.stream_tx);
    uint8_t stale_cont[] = {2, 42};
    proto_chan_rx(&ch, stale_cont, sizeof stale_cont);
    feed_test(&ch, 100, false); drain(&ch, false);

    // Variable-length transports reject extra bytes instead of accepting padding.
    uint8_t padded[] = {1, PROTO_CMD_TEST, 1, 0, 4, 0, 0, 0, 0, 99, 99};
    proto_chan_rx(&ch, padded, sizeof padded);
    assert(!ch.stream_rx && !ch.rx_active);
    proto_chan_rx(&ch, stale_cont, sizeof stale_cont);
    assert(!ch.rx_active && !ch.stream_rx && !ch.stream_tx);
    drain(&ch, false);
    assert(output_cmd == PROTO_CMD_ERROR && output[1] == PROTO_ERR_LENGTH);

    // Correlated replies, explicit errors, reserved IDs, and strict lengths.
    uint8_t info[] = {3, PROTO_CMD_INFO, 165, 0, 0};
    proto_chan_rx(&ch, info, sizeof info); drain(&ch, false);
    assert(output_cmd == PROTO_CMD_INFO && output_id == 165);
    for (unsigned reserved = 0; reserved < 2; ++reserved) {
        info[2] = info[3] = reserved ? 255 : 0;
        before = complete_count;
        proto_chan_rx(&ch, info, sizeof info); drain(&ch, false);
        assert(complete_count == before);
    }
    uint8_t unknown[] = {3, 99, 165, 0, 0};
    proto_chan_rx(&ch, unknown, sizeof unknown); drain(&ch, false);
    assert(output_cmd == PROTO_CMD_ERROR && output_id == 165);
    assert(output[0] == 99 && output[1] == PROTO_ERR_COMMAND);
    uint8_t short_info[] = {3, PROTO_CMD_INFO, 165, 0, 1};
    proto_chan_rx(&ch, short_info, sizeof short_info); drain(&ch, false);
    assert(output_cmd == PROTO_CMD_ERROR && output[1] == PROTO_ERR_LENGTH);
    uint8_t huge[] = {1, PROTO_CMD_TEST, 165, 0, 0x81, 0x80, 0x80, 4};
    proto_chan_rx(&ch, huge, sizeof huge); drain(&ch, false);
    assert(output_cmd == PROTO_CMD_ERROR && output[1] == PROTO_ERR_LENGTH);
    proto_chan_rx(&ch, bad_crc, sizeof bad_crc); drain(&ch, false);
    assert(output_cmd == PROTO_CMD_ERROR && output[1] == PROTO_ERR_CRC);

    // A second TEST cannot overwrite an active streaming response.
    feed_test(&ch, 1000, false);
    uint8_t busy[] = {1, PROTO_CMD_TEST, 165, 0, 4};
    proto_chan_rx(&ch, busy, sizeof busy);
    assert(ch.stream_msgid == 42 && ch.tx_total == 1000);
    drain(&ch, false);
    assert(output_cmd == PROTO_CMD_ERROR && output_id == 165 && output[1] == PROTO_ERR_BUSY);

    // Logs cannot use the reply reserve. RESET clears abandoned TX and ACKs.
    while (proto_chan_submit(&ch, PROTO_CMD_PRINT, PROTO_ID_BROADCAST, message, sizeof message)) {}
    assert(PROTO_RING_SIZE - ch.used >= PROTO_REPLY_RESERVE);
    assert(proto_chan_submit(&ch, PROTO_CMD_INFO, 165, message, sizeof message));
    drain(&ch, false);
    feed_test(&ch, 1000, false);
    uint8_t reset[] = {3, PROTO_CMD_RESET, 165, 0, 0};
    proto_chan_rx(&ch, reset, sizeof reset); drain(&ch, false);
    assert(output_cmd == PROTO_CMD_RESET && output_id == 165 && output_len == 0);
    assert(!ch.stream_tx && !ch.tx_active && !ch.rx_active);
    assert(!proto_chan_submit(&ch, PROTO_CMD_PRINT, 0, message, sizeof message));

    uart_proto_init(); uart_proto_task();
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
        before = complete_count;
        s_rx_seq_valid = false;
        feed_test(&g_uart_chan, sizes[i], true);
        drain(&g_uart_chan, true);
        assert(complete_count == before + 1 && output_len == sizes[i]);
    }
    assert(s_stats.crc_errors == 0 && s_stats.ring_discard_bytes == 0);

    // A valid 3000-byte backlog fits in the 4096-byte ring and must survive.
    uint8_t delimiters[UART_RX_RING_SIZE + 16]; memset(delimiters, 0xC0, sizeof delimiters);
    dma_feed(delimiters, 3000);
    for (unsigned i = 0; i < 3; ++i) uart_proto_task();
    assert(uart_rx_tail == dma_produced && s_stats.ring_discard_bytes == 0);
    // A full lap + 16 bytes must be detected even when the write address looks near-empty.
    dma_feed(delimiters, sizeof delimiters); uart_proto_task();
    assert(s_stats.ring_discard_bytes == sizeof delimiters && uart_rx_tail == dma_produced);

    uart_rx_tail = dma_produced = UART_RX_DMA_PERIOD - 10;
    dma_feed(delimiters, 20); uart_proto_task();
    assert(uart_rx_tail == 10);
    mock_busy = false; uart_proto_task();
    assert(s_stats.dma_restarts == 1 && uart_rx_tail == 0);

    // Inject one CRC error, then two valid frames with a sequence gap.
    dma_produced = 0;
    uint8_t bad[] = {0xC0, 7, 0, 0, 0, 0xC0};
    dma_feed(bad, sizeof bad); uart_proto_task();
    assert(s_stats.crc_errors == 1);
    uint8_t raw[] = {10, PROTO_F_RESET, 0, 0}, wire[16];
    for (unsigned i = 0; i < 2; ++i) {
        raw[0] = i ? 12 : 10;
        uint16_t crc = crc16_update(0xFFFF, raw, 2);
        raw[2] = (uint8_t)crc; raw[3] = (uint8_t)(crc >> 8);
        uint16_t n = slip_encode(raw, sizeof raw, wire);
        dma_feed(wire, n); uart_proto_task();
    }
    assert(s_stats.seq_gaps == 1);
    uint8_t info_request[] = {3, PROTO_CMD_INFO, 1, 0, 0};
    proto_chan_rx(&g_uart_chan, info_request, sizeof info_request);
    drain(&g_uart_chan, true);
    assert(output_cmd == PROTO_CMD_INFO && output_len < sizeof output);
    output[output_len] = 0;
    assert(strstr((char *)output, "uart_baud: 4000000"));
    assert(strstr((char *)output, "crc_errors: 1"));
    puts("PASS: firmware/UART 32K through 8M, backpressure, PRINT ordering, RX recovery, DMA backlog/overrun/wrap/restart, diagnostics");
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="pico-uart-test-") as directory:
        tmp = Path(directory)
        (tmp / "host_hardware.h").write_text(MOCKS)
        for name in ("hardware/dma.h", "hardware/irq.h", "hardware/uart.h",
                     "pico/platform.h", "pico/time.h", "pico/critical_section.h"):
            path = tmp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "host_hardware.h"\n')
        (tmp / "test.c").write_text(TEST)
        executable = tmp / "test"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-Wall", "-Wextra", "-Werror", "-I", str(tmp),
            "-I", str(ROOT / "src"), str(tmp / "test.c"), "-lz", "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
