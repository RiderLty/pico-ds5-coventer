#include <stdio.h>
#include <string.h>

#include "pico/critical_section.h"
#include "pico/time.h"
#include "tusb.h"

#include "usb_io.h"
#include "usb_km.h"
#include "debug.h"

// ---------------------------------------------------------------------------
// 跨核消息环（4KB）：entry = [cmd:1][msg_id:2 LE][len:2 LE][data]。
// 生产者 core1（Lua print）+ core0（应答），消费者 core0 usb_io_task，
// 全部经同一 critical_section（striped 锁，双核启动前初始化）。非阻塞：
// 环满整条丢弃并计数。写入允许跨越环绕点（两段 memcpy）。
// ---------------------------------------------------------------------------

#define LOG_RING_SIZE  4096u                  // 2 的幂，索引用掩码回绕
#define LOG_RING_MASK  (LOG_RING_SIZE - 1u)
#define LOG_ENTRY_HDR  5u
#define RX_REASM_MAX   512u                   // 主机→设备重组上限（v1 全是小命令）

static critical_section_t s_cs;
static uint8_t  s_ring[LOG_RING_SIZE];
static uint16_t s_head, s_tail, s_used;       // BSS 零 = 空环
static uint32_t s_drops;

// 环内逻辑偏移读写（调用方持有临界区），处理回绕两段拷贝
static void ring_read(uint16_t pos, uint8_t *dst, uint16_t n)
{
    pos &= LOG_RING_MASK;
    uint16_t first = LOG_RING_SIZE - pos;
    if (first > n) first = n;
    memcpy(dst, s_ring + pos, first);
    memcpy(dst + first, s_ring, (size_t)(n - first));
}

static void ring_write(uint16_t pos, const uint8_t *src, uint16_t n)
{
    pos &= LOG_RING_MASK;
    uint16_t first = LOG_RING_SIZE - pos;
    if (first > n) first = n;
    memcpy(s_ring + pos, src, first);
    memcpy(s_ring, src + first, (size_t)(n - first));
}

// 入队一条消息（整条或整弃）
static bool ring_push(uint8_t cmd, uint16_t msgid, const uint8_t *data, uint16_t len)
{
    bool ok = false;
    critical_section_enter_blocking(&s_cs);
    uint16_t need = (uint16_t)(LOG_ENTRY_HDR + len);
    if (LOG_RING_SIZE - s_used >= need) {
        uint8_t hdr[LOG_ENTRY_HDR] = { cmd, (uint8_t)msgid, (uint8_t)(msgid >> 8),
                                       (uint8_t)len, (uint8_t)(len >> 8) };
        ring_write(s_head, hdr, LOG_ENTRY_HDR);
        ring_write((uint16_t)(s_head + LOG_ENTRY_HDR), data, len);
        s_head = (uint16_t)((s_head + need) & LOG_RING_MASK);
        s_used = (uint16_t)(s_used + need);
        ok = true;
    } else {
        s_drops++;
    }
    critical_section_exit(&s_cs);
    return ok;
}

void usb_io_init(void)
{
    critical_section_init_with_lock_num(&s_cs, next_striped_spin_lock_num());
    debug_printf("[DS5C][IO] proto ring initialized\n");
}

// ---------------------------------------------------------------------------
// TX 分块状态机（core0）：同方向单飞 —— 一条消息发完才取下一条。
// 出队时只弹 5B 头，数据留在环内按块就地读，绝不整条拷贝（任意长度无感）。
// ---------------------------------------------------------------------------

static uint8_t s_instance = USB_IO_INST_NONE;

static struct {
    bool     active;
    uint8_t  cmd;
    uint16_t msgid;
    uint16_t len;      // 消息总长
    uint16_t off;      // 已发送的数据偏移
} s_tx;

static uint8_t s_tx_seq;

// LEB128 / protobuf varint（长度字段编码）
static uint8_t *varint_put(uint8_t *p, uint32_t v)
{
    while (v >= 0x80u) {
        *p++ = (uint8_t)(v | 0x80u);
        v >>= 7;
    }
    *p++ = (uint8_t)v;
    return p;
}

static const uint8_t *varint_get(const uint8_t *p, const uint8_t *end, uint32_t *out)
{
    uint32_t v = 0;
    int shift = 0;
    while (p < end && shift < 28) {
        uint8_t b = *p++;
        v |= (uint32_t)(b & 0x7Fu) << shift;
        if (!(b & 0x80u)) {
            *out = v;
            return p;
        }
        shift += 7;
    }
    return NULL;   // 截断/非法
}

static bool tx_pop_header(void)
{
    uint8_t hdr[LOG_ENTRY_HDR];
    critical_section_enter_blocking(&s_cs);
    if (s_used < LOG_ENTRY_HDR) {
        critical_section_exit(&s_cs);
        return false;
    }
    ring_read(s_tail, hdr, LOG_ENTRY_HDR);
    s_tail = (uint16_t)((s_tail + LOG_ENTRY_HDR) & LOG_RING_MASK);
    s_used = (uint16_t)(s_used - LOG_ENTRY_HDR);
    critical_section_exit(&s_cs);

    s_tx.active = true;
    s_tx.cmd    = hdr[0];
    s_tx.msgid  = (uint16_t)(hdr[1] | (hdr[2] << 8));
    s_tx.len    = (uint16_t)(hdr[3] | (hdr[4] << 8));
    s_tx.off    = 0;
    debug_printf("[DS5C][IO] tx start: cmd=0x%02x msgid=%u len=%u used=%u\n",
                 s_tx.cmd, s_tx.msgid, s_tx.len, s_used);
    return true;
}

// 从环内数据区（已弹出头的消息）就地读一块数据
static void tx_read_data(uint16_t off, uint8_t *dst, uint16_t n)
{
    critical_section_enter_blocking(&s_cs);
    ring_read((uint16_t)(s_tail + off), dst, n);
    critical_section_exit(&s_cs);
}

static void tx_commit(uint16_t n)
{
    critical_section_enter_blocking(&s_cs);
    s_tail = (uint16_t)((s_tail + n) & LOG_RING_MASK);
    s_used = (uint16_t)(s_used - n);
    critical_section_exit(&s_cs);
}

void usb_io_task(void)
{
    if (s_instance == USB_IO_INST_NONE) return;

    if (!s_tx.active && !tx_pop_header()) return;
    if (!tud_hid_n_ready(s_instance)) return;

    uint8_t payload[IO_PAYLOAD_SIZE];
    uint16_t pos = 0;
    payload[pos++] = s_tx_seq++;
    uint8_t flags_pos = pos;
    payload[pos++] = 0;

    bool first = (s_tx.off == 0);
    if (first) {
        payload[pos++] = s_tx.cmd;
        payload[pos++] = (uint8_t)(s_tx.msgid & 0xFFu);
        payload[pos++] = (uint8_t)(s_tx.msgid >> 8);
        pos = (uint16_t)(varint_put(payload + pos, s_tx.len) - payload);
    }

    uint16_t cap = (uint16_t)(IO_PAYLOAD_SIZE - pos);
    uint16_t n = (uint16_t)(s_tx.len - s_tx.off);
    if (n > cap) n = cap;

    // 环内数据偏移从 0 计（数据在环内紧跟已弹出的 5B 头之后）
    tx_read_data(s_tx.off, payload + pos, n);
    pos = (uint16_t)(pos + n);

    bool last = (uint16_t)(s_tx.off + n) == s_tx.len;
    payload[flags_pos] = (uint8_t)((first ? PROTO_F_START : 0u) |
                                   (last  ? PROTO_F_END   : 0u));

    // 补齐到满 63B 载荷（+1B Report ID = 64B 线上包）：短 IN 包在
    // Windows→Chrome 链路上会被丢弃，长度恒定也简化两端解析
    memset(payload + pos, 0, (size_t)(IO_PAYLOAD_SIZE - pos));
    pos = IO_PAYLOAD_SIZE;

    if (tud_hid_n_report(s_instance, IO_REPORT_ID_IN, payload, pos)) {
        if (last) {
            debug_printf("[DS5C][IO] tx end: cmd=0x%02x msgid=%u off=%u\n",
                         s_tx.cmd, s_tx.msgid, s_tx.off + n);
        }
        tx_commit(n);
        s_tx.off = (uint16_t)(s_tx.off + n);
        if (last) s_tx.active = false;
    } else {
        debug_printf("[DS5C][IO] tx report busy\n");
    }
}

// ---------------------------------------------------------------------------
// RX 重组（core0，tud_task 上下文）：START..END 拼装 → 查表分发。
// 主机→设备 v1 全是小命令（INFO/RESET），超过上限直接丢弃复位。
// ---------------------------------------------------------------------------

static struct {
    bool     active;
    uint8_t  cmd;
    uint16_t msgid;
    uint16_t total;
    uint16_t off;
    uint8_t  buf[RX_REASM_MAX];
} s_rx;

static void rx_dispatch(uint8_t cmd, uint16_t msgid, const uint8_t *body, uint16_t len)
{
    debug_printf("[DS5C][IO] dispatch: cmd=0x%02x msgid=%u len=%u\n", cmd, msgid, len);
    (void)body;   // v1 命令无请求体（INFO/RESET）；带参数的命令接入时使用
    (void)len;
    switch (cmd) {
    case PROTO_CMD_INFO: {
        char info[192];
        uint32_t uptime_ms = to_ms_since_boot(get_absolute_time());
        int n = snprintf(info, sizeof info,
                         "pico-ds5-coventer\n"
                         "build: " __DATE__ " " __TIME__ "\n"
                         "uptime_ms: %lu\n"
                         "kbd:%d mouse:%d io:%d\n"
                         "log_drops: %lu",
                         (unsigned long)uptime_ms,
                         (int)usb_km_kbd_enabled(), (int)usb_km_mouse_enabled(),
                         (int)usb_km_io_enabled(),
                         (unsigned long)s_drops);
        if (n > 0) ring_push(PROTO_CMD_INFO, msgid, (const uint8_t *)info, (uint16_t)n);
        break;
    }
    case PROTO_CMD_RESET:
        s_rx.active = false;
        debug_printf("[DS5C][IO] session reset by host\n");
        break;
    default:
        debug_printf("[DS5C][IO] unknown cmd 0x%02x\n", cmd);
        break;
    }
}

void usb_io_rx(const uint8_t *payload, uint16_t len)
{
    if (s_instance == USB_IO_INST_NONE || len < 2u) {
        debug_printf("[DS5C][IO] rx drop: inst=%d len=%u\n", s_instance, len);
        return;
    }
    debug_printf("[DS5C][IO] rx chunk: len=%u flags=0x%02x d0=%02x d1=%02x d2=%02x\n",
                 len, payload[1],
                 len > 2 ? payload[2] : 0, len > 3 ? payload[3] : 0,
                 len > 4 ? payload[4] : 0);

    uint8_t flags = payload[1];
    const uint8_t *data = payload + 2;
    uint16_t dlen = (uint16_t)(len - 2u);

    if (flags & PROTO_F_RESET) {
        s_rx.active = false;
        return;
    }

    if (flags & PROTO_F_START) {
        if (dlen < 3u) return;
        uint32_t total = 0;
        const uint8_t *q = varint_get(data + 3, data + dlen, &total);
        if (!q || total > RX_REASM_MAX) {
            s_rx.active = false;
            return;
        }
        s_rx.active = true;
        s_rx.cmd    = data[0];
        s_rx.msgid  = (uint16_t)(data[1] | (data[2] << 8));
        s_rx.total  = (uint16_t)total;
        s_rx.off    = (uint16_t)(dlen - (uint16_t)(q - data));
        if (s_rx.off > s_rx.total) s_rx.off = s_rx.total;   // 主机侧可能补零到 64B
        memcpy(s_rx.buf, q, s_rx.off);
        if (flags & PROTO_F_END) {
            s_rx.active = false;
            rx_dispatch(s_rx.cmd, s_rx.msgid, s_rx.buf, s_rx.off);
        }
        return;
    }

    if (!s_rx.active) return;   // CONT/END 无在途消息，丢弃

    uint16_t space = (uint16_t)(s_rx.total - s_rx.off);
    if (dlen > space) dlen = space;
    memcpy(s_rx.buf + s_rx.off, data, dlen);
    s_rx.off = (uint16_t)(s_rx.off + dlen);

    if (flags & PROTO_F_END) {
        s_rx.active = false;
        rx_dispatch(s_rx.cmd, s_rx.msgid, s_rx.buf, s_rx.off);
    }
}

// ---------------------------------------------------------------------------
// 对外 API
// ---------------------------------------------------------------------------

void usb_io_set_instance(uint8_t instance)
{
    s_instance = instance;
    debug_printf("[DS5C][IO] instance=%d (0xFF = off)\n", instance);
}

uint8_t usb_io_instance(void)
{
    return s_instance;
}

bool usb_io_enabled(void)
{
    return s_instance != USB_IO_INST_NONE;
}

void usb_log_write(const uint8_t *data, uint16_t len)
{
    if (len > LOG_RING_SIZE - LOG_ENTRY_HDR) {
        critical_section_enter_blocking(&s_cs);
        s_drops++;
        critical_section_exit(&s_cs);
        return;
    }
    ring_push(PROTO_CMD_PRINT, 0u, data, len);
}
