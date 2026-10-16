/*
 * 跨核传输层实现：SPSC 字节块队列，按 kind 分发到 DS5 映射引擎。
 *
 * 数据通路：core1（input_app）入队
 *           → 原子索引发布的 SPSC 队列（无需关中断或等锁）
 *           → core0 主循环 input_queue_flush() 批量出队 → ds5_pad_feed_*
 */

#include <string.h>
#include <stdio.h>

#include "input_queue.h"
#include "ds5_pad.h"
#include "debug.h"

#define INQ_QUEUE_DEPTH  128u   // 2 的幂（掩码索引）
#define INQ_FLUSH_BUDGET 32u    // core0 每轮最多出队块数，防止饿死 tud_task()

typedef struct {
    uint8_t len;
    uint8_t kind;
    uint8_t data[INQ_REC_MAX];
} inq_rec_t;

static inq_rec_t      s_queue[INQ_QUEUE_DEPTH];
static uint32_t       s_head, s_tail;
static uint32_t       s_dropped;   // core1 写入累计值，core0 读取差值
static uint32_t       s_reported_drops; // 仅 core0 访问

_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0), "SPSC requires lock-free 32-bit atomics");

void input_queue_init(void)
{
    __atomic_store_n(&s_head, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_tail, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_dropped, 0u, __ATOMIC_RELAXED);
    s_reported_drops = 0;
}

// 队列满则丢最新：保住更早的事件，
// 且被丢的多为重复率最高的移动帧。
void input_queue_send(uint8_t kind, const void *data, uint8_t len)
{
    if (!data || !len) return;
    if (len > INQ_REC_MAX) len = INQ_REC_MAX;

    uint32_t tail = __atomic_load_n(&s_tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
    if ((uint32_t)(tail - head) == INQ_QUEUE_DEPTH) {
        uint32_t dropped = __atomic_load_n(&s_dropped, __ATOMIC_RELAXED);
        __atomic_store_n(&s_dropped, dropped + 1u, __ATOMIC_RELAXED);
        return;
    }
    inq_rec_t *r = &s_queue[tail & (INQ_QUEUE_DEPTH - 1u)];
    r->len = len;
    r->kind = kind;
    memcpy(r->data, data, len);
    // 内容写完后才发布；收到消费者的 head 后才复用旧槽位。
    __atomic_store_n(&s_tail, tail + 1u, __ATOMIC_RELEASE);
}

void input_queue_flush(void)
{
    uint32_t dropped = 0;
    for (uint8_t budget = INQ_FLUSH_BUDGET; budget; budget--) {
        uint32_t head = __atomic_load_n(&s_head, __ATOMIC_RELAXED);
        if (head == __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE)) {
            uint32_t total = __atomic_load_n(&s_dropped, __ATOMIC_RELAXED);
            dropped = total - s_reported_drops;
            s_reported_drops = total;
            break;
        }

        inq_rec_t *r = &s_queue[head & (INQ_QUEUE_DEPTH - 1u)];
        uint8_t len = r->len;
        uint8_t kind = r->kind;
        uint8_t data[INQ_REC_MAX];
        memcpy(data, r->data, len);
        // 先复制到本核栈，再交还槽位；映射引擎不持有队列里的指针。
        __atomic_store_n(&s_head, head + 1u, __ATOMIC_RELEASE);

        if (kind == INQ_KIND_PAD) {
            ds5_pad_feed_pad(data, len);
        } else if (kind == INQ_KIND_TOUCH) {
            ds5_pad_feed_touch(data, len);
        } else if (kind == INQ_KIND_MOTION) {
            ds5_pad_feed_motion(data, len);
        }
    }

    // 队列本身不取日志锁；只在排空后报告本窗口的丢弃差值。
    if (dropped) {
        DEBUG("[DS5C][Q] dropped %lu record(s)\n", (unsigned long)dropped);
    }
}
