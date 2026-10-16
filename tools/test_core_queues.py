#!/usr/bin/env python3
"""用真实双线程验证两个 SPSC 环的内容、满队列策略和 32 位索引回绕。"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
COMMON = r"""
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "debug.h"
#define N 200000u
void debug_log(const char *fmt, ...) { (void)fmt; }
"""
INPUT = COMMON + r"""
#include "input_queue.c"
static unsigned received;
static void accept(const uint8_t *p, uint8_t n, uint8_t kind) {
    uint32_t words[4]; assert(n==16); memcpy(words,p,16);
    assert(words[0]==received && words[1]==~words[0]);
    assert(words[2]==(words[0]^0x12345678u) && words[3]==0xabcdef98u);
    assert(kind==INQ_KIND_PAD+received%3u);
    ++received;
}
void ds5_pad_feed_pad(const uint8_t *p,uint8_t n) { accept(p,n,INQ_KIND_PAD); }
void ds5_pad_feed_touch(const uint8_t *p,uint8_t n) { accept(p,n,INQ_KIND_TOUCH); }
void ds5_pad_feed_motion(const uint8_t *p,uint8_t n) { accept(p,n,INQ_KIND_MOTION); }
static void send_record(unsigned i) {
    uint32_t words[]={i,~i,i^0x12345678u,0xabcdef98u,0};
    input_queue_send(INQ_KIND_PAD+i%3u,words,sizeof words); // 超限截断至 16 字节
}
static void *producer(void *arg) {
    (void)arg;
    for (unsigned i=0;i<N;++i) {
        while ((uint32_t)(__atomic_load_n(&s_tail,__ATOMIC_RELAXED)-
                         __atomic_load_n(&s_head,__ATOMIC_ACQUIRE))==INQ_QUEUE_DEPTH)
            sched_yield();
        send_record(i);
    }
    return NULL;
}
int main(void) {
    input_queue_init(); input_queue_flush();
    // 计数在填满时跨过 UINT32_MAX，保持所有 128 个槽位可用。
    __atomic_store_n(&s_head,UINT32_MAX-64u,__ATOMIC_RELAXED);
    __atomic_store_n(&s_tail,UINT32_MAX-64u,__ATOMIC_RELAXED);
    for(unsigned i=0;i<INQ_QUEUE_DEPTH;++i) send_record(i);
    send_record(9999); assert(s_dropped==1);
    input_queue_flush(); assert(received==INQ_FLUSH_BUDGET);
    while(received<INQ_QUEUE_DEPTH) input_queue_flush();
    input_queue_flush(); assert(s_reported_drops==1);
    input_queue_send(INQ_KIND_PAD,NULL,16); input_queue_send(INQ_KIND_PAD,"",0);
    assert(s_head==s_tail);
    input_queue_init(); received=0;
    pthread_t thread; assert(!pthread_create(&thread,NULL,producer,NULL));
    while(received<N) { input_queue_flush(); sched_yield(); }
    assert(!pthread_join(thread,NULL)); assert(!s_dropped && s_head==s_tail);
    puts("PASS: input SPSC full/drop-newest, 16-byte boundary, wraparound, 200000 concurrent records");
}
"""
BOOT = COMMON + r"""
#include "boot_button.c"
static unsigned received, enabled;
bool lua_binder_on_boot_button(bool pressed) {
    assert(pressed==(bool)(received%2u)); ++received; return false;
}
void usb_km_request(int k,int m,int io) { assert(k==-1 && m==-1 && io==1); ++enabled; }
static void *producer(void *arg) {
    (void)arg;
    for(unsigned i=0;i<N;++i) {
        while((uint32_t)(__atomic_load_n(&s_tail,__ATOMIC_RELAXED)-
                        __atomic_load_n(&s_head,__ATOMIC_ACQUIRE))==BOOT_EVENT_DEPTH)
            sched_yield();
        push_event(i%2u);
    }
    return NULL;
}
int main(void) {
    boot_button_init(); boot_button_process();
    __atomic_store_n(&s_head,UINT32_MAX-3u,__ATOMIC_RELAXED);
    __atomic_store_n(&s_tail,UINT32_MAX-3u,__ATOMIC_RELAXED);
    for(unsigned i=0;i<BOOT_EVENT_DEPTH;++i) push_event(i%2u);
    push_event(true); boot_button_process();
    assert(received==8 && enabled==4 && s_head==s_tail);
    boot_button_init(); received=enabled=0;
    pthread_t thread; assert(!pthread_create(&thread,NULL,producer,NULL));
    while(received<N) { boot_button_process(); sched_yield(); }
    assert(!pthread_join(thread,NULL)); assert(enabled==N/2u && s_head==s_tail);
    puts("PASS: BOOT SPSC full/drop-newest, wraparound, default release action, 200000 concurrent events");
}
"""
HARDWARE = r"""
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define __no_inline_not_in_flash_func(x) x
typedef unsigned uint;
static inline uint32_t save_and_disable_interrupts(void) { return 0; }
static inline void restore_interrupts(uint32_t flags) { (void)flags; }
static inline void hw_write_masked(volatile uint32_t *p,uint32_t v,uint32_t m) { *p=(*p&~m)|v; }
static inline bool multicore_lockout_start_timeout_us(uint64_t t) { (void)t; return true; }
static inline void multicore_lockout_end_blocking(void) {}
static inline uint64_t time_us_64(void) { return 0; }
static struct { struct { uint32_t ctrl; } io[2]; } test_ioqspi;
static struct { uint32_t gpio_hi_in; } test_sio;
#define ioqspi_hw (&test_ioqspi)
#define sio_hw (&test_sio)
#define GPIO_OVERRIDE_LOW 0u
#define GPIO_OVERRIDE_NORMAL 1u
#define IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB 0u
#define IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS 3u
#define SIO_GPIO_HI_IN_QSPI_CSN_BITS 1u
"""


def main():
    with tempfile.TemporaryDirectory(prefix="pico-spsc-test-") as directory:
        tmp = Path(directory)
        (tmp / "hardware_mock.h").write_text(HARDWARE)
        for name in ("hardware/gpio.h", "hardware/structs/ioqspi.h",
                     "hardware/structs/sio.h", "hardware/sync.h",
                     "pico/multicore.h", "pico/time.h"):
            path = tmp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "hardware_mock.h"\n')
        for name, source in (("input", INPUT), ("boot", BOOT)):
            path = tmp / f"{name}.c"
            path.write_text(source)
            executable = tmp / name
            subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O2", "-g",
                            "-Wall", "-Wextra", "-pthread", "-fsanitize=address,undefined",
                            "-I", str(tmp), "-I", str(ROOT / "src"),
                            str(path), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=60,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0",
                                "UBSAN_OPTIONS": "halt_on_error=1"})


if __name__ == "__main__":
    main()
