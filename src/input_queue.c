/*
 * 跨核传输层实现：SPSC 字节块队列，按 kind 分发到 DS5 映射引擎。
 *
 * 数据通路：core1（input_app）入队
 *           → critical_section 保护的 SPSC 队列
 *           → core0 主循环 input_queue_flush() 批量出队 → ds5_pad_feed_*
 */

#include <string.h>
#include <stdio.h>

#include "pico/sync.h"

#include "input_queue.h"
#include "ds5_pad.h"
#include "debug.h"

#define INQ_QUEUE_DEPTH  128u   // 2 的幂（掩码索引）
#define INQ_FLUSH_BUDGET 32u    // core0 每轮最多出队块数，防止饿死 tud_task()

typedef struct {
    uint8_t len;                    // 含 kind 标签的总长
    uint8_t data[INQ_REC_MAX];
} inq_rec_t;

static inq_rec_t      s_queue[INQ_QUEUE_DEPTH];
static uint16_t       s_head, s_tail, s_count;
static critical_section_t s_cs;
static uint32_t       s_dropped;   // 溢出丢弃块数（临界区内读写）

void input_queue_init(void)
{
    critical_section_init(&s_cs);
    s_head = s_tail = s_count = 0;
    s_dropped = 0;
}

// 入队核心（须持有临界区）。队列满则丢最新：保住更早的事件，
// 且被丢的多为重复率最高的移动帧。
static void queue_push_locked(uint8_t kind, const uint8_t *data, uint8_t len)
{
    if (s_count == INQ_QUEUE_DEPTH) {
        s_dropped++;
        return;
    }
    inq_rec_t *r = &s_queue[s_tail];
    r->len = (uint8_t)(len + 1u);
    r->data[0] = kind;
    memcpy(&r->data[1], data, len);
    s_tail = (uint16_t)((s_tail + 1u) & (INQ_QUEUE_DEPTH - 1u));
    s_count++;
}

void input_queue_send(uint8_t kind, const void *data, uint8_t len)
{
    if (!data || !len) return;
    if (len > INQ_REC_MAX) len = INQ_REC_MAX;

    critical_section_enter_blocking(&s_cs);
    queue_push_locked(kind, (const uint8_t *)data, len);
    critical_section_exit(&s_cs);
}

void input_queue_flush(void)
{
    uint32_t dropped = 0;
    for (uint8_t budget = INQ_FLUSH_BUDGET; budget; budget--) {
        critical_section_enter_blocking(&s_cs);
        if (s_count == 0) {
            dropped = s_dropped;
            s_dropped = 0;
            critical_section_exit(&s_cs);
            break;
        }

        inq_rec_t *r = &s_queue[s_head];
        uint8_t len = (uint8_t)(r->len - 1u);
        uint8_t kind = r->data[0];
        uint8_t data[INQ_REC_MAX];
        memcpy(data, &r->data[1], len);
        s_head = (uint16_t)((s_head + 1u) & (INQ_QUEUE_DEPTH - 1u));
        s_count--;
        critical_section_exit(&s_cs);

        if (kind == INQ_KIND_KB) {
            ds5_pad_feed_kb(data, len);
        } else if (kind == INQ_KIND_MS) {
            ds5_pad_feed_ms(data, len);
        } else if (kind == INQ_KIND_PAD) {
            ds5_pad_feed_pad(data, len);
        }
    }

    // 丢弃计数在临界区外打日志：日志锁与临界区锁分离，临界区内不再取第二把锁
    if (dropped) {
        debug_printf("[DS5C][Q] dropped %lu record(s)\n", (unsigned long)dropped);
    }
}
