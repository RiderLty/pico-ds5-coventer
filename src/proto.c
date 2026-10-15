#include <stdio.h>
#include <string.h>

#include "pico/critical_section.h"
#include "pico/time.h"

#include "proto.h"
#include "usb_km.h"
#include "debug.h"

// ---------------------------------------------------------------------------
// 共享临界区：两个通道的消息环都由 core1（print）+ core0（应答/消费）访问
// ---------------------------------------------------------------------------

static critical_section_t s_cs;
static proto_chan_t *s_chans[2];
static uint8_t s_chan_count;

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

// ---------------------------------------------------------------------------
// 消息环（变长 entry [cmd][msgid:2][len:2][data]，双段回绕拷贝；持锁调用）
// ---------------------------------------------------------------------------

static void ring_read(proto_chan_t *ch, uint16_t pos, uint8_t *dst, uint16_t n)
{
    pos &= PROTO_RING_MASK;
    uint16_t first = PROTO_RING_SIZE - pos;
    if (first > n) first = n;
    memcpy(dst, ch->ring + pos, first);
    memcpy(dst + first, ch->ring, (size_t)(n - first));
}

static void ring_write(proto_chan_t *ch, uint16_t pos, const uint8_t *src, uint16_t n)
{
    pos &= PROTO_RING_MASK;
    uint16_t first = PROTO_RING_SIZE - pos;
    if (first > n) first = n;
    memcpy(ch->ring + pos, src, first);
    memcpy(ch->ring, src + first, (size_t)(n - first));
}

static bool chan_push(proto_chan_t *ch, uint8_t cmd, uint16_t msgid,
                      const uint8_t *data, uint16_t len)
{
    if (len > PROTO_RING_SIZE - PROTO_ENTRY_HDR) {
        critical_section_enter_blocking(&s_cs);
        ch->drops++;
        critical_section_exit(&s_cs);
        return false;
    }

    bool ok = false;
    critical_section_enter_blocking(&s_cs);
    uint16_t need = (uint16_t)(PROTO_ENTRY_HDR + len);
    if (PROTO_RING_SIZE - ch->used >= need) {
        uint8_t hdr[PROTO_ENTRY_HDR] = { cmd, (uint8_t)msgid, (uint8_t)(msgid >> 8),
                                         (uint8_t)len, (uint8_t)(len >> 8) };
        ring_write(ch, ch->head, hdr, PROTO_ENTRY_HDR);
        ring_write(ch, (uint16_t)(ch->head + PROTO_ENTRY_HDR), data, len);
        ch->head = (uint16_t)((ch->head + need) & PROTO_RING_MASK);
        ch->used = (uint16_t)(ch->used + need);
        ok = true;
    } else {
        ch->drops++;
    }
    critical_section_exit(&s_cs);
    return ok;
}

static bool chan_pop_header(proto_chan_t *ch)
{
    uint8_t hdr[PROTO_ENTRY_HDR];
    critical_section_enter_blocking(&s_cs);
    if (ch->used < PROTO_ENTRY_HDR) {
        critical_section_exit(&s_cs);
        return false;
    }
    ring_read(ch, ch->tail, hdr, PROTO_ENTRY_HDR);
    ch->tail = (uint16_t)((ch->tail + PROTO_ENTRY_HDR) & PROTO_RING_MASK);
    ch->used = (uint16_t)(ch->used - PROTO_ENTRY_HDR);
    critical_section_exit(&s_cs);

    ch->tx_active = true;
    ch->tx_cmd    = hdr[0];
    ch->tx_msgid  = (uint16_t)(hdr[1] | (hdr[2] << 8));
    ch->tx_len    = (uint16_t)(hdr[3] | (hdr[4] << 8));
    ch->tx_off    = 0;
    return true;
}

// 从环内数据区（已弹出 5B 头）就地读一段数据，不整条拷贝
static void chan_read_data(proto_chan_t *ch, uint16_t off, uint8_t *dst, uint16_t n)
{
    critical_section_enter_blocking(&s_cs);
    ring_read(ch, (uint16_t)(ch->tail + off), dst, n);
    critical_section_exit(&s_cs);
}

static void chan_commit(proto_chan_t *ch, uint16_t n)
{
    critical_section_enter_blocking(&s_cs);
    ch->tail = (uint16_t)((ch->tail + n) & PROTO_RING_MASK);
    ch->used = (uint16_t)(ch->used - n);
    critical_section_exit(&s_cs);
}

// ---------------------------------------------------------------------------
// 命令分发（共用；应答只回请求方通道）
// ---------------------------------------------------------------------------

static void chan_dispatch(proto_chan_t *ch, uint8_t cmd, uint16_t msgid,
                          const uint8_t *body, uint16_t len)
{
    debug_printf("[DS5C][IO] dispatch(%s): cmd=0x%02x msgid=%u len=%u\n",
                 ch->tr->name, cmd, msgid, len);
    (void)body;
    (void)len;

    switch (cmd) {
    case PROTO_CMD_INFO: {
        char info[192];
        uint32_t uptime_ms = to_ms_since_boot(get_absolute_time());
        int n = snprintf(info, sizeof info,
                         "pico-ds5-coventer\n"
                         "transport: %s\n"
                         "build: " __DATE__ " " __TIME__ "\n"
                         "uptime_ms: %lu\n"
                         "kbd:%d mouse:%d io:%d\n"
                         "log_drops: %lu",
                         ch->tr->name,
                         (unsigned long)uptime_ms,
                         (int)usb_km_kbd_enabled(), (int)usb_km_mouse_enabled(),
                         (int)usb_km_io_enabled(),
                         (unsigned long)ch->drops);
        if (n > 0) {
            chan_push(ch, PROTO_CMD_INFO, msgid, (const uint8_t *)info, (uint16_t)n);
        }
        break;
    }
    case PROTO_CMD_RESET:
        ch->rx_active = false;
        debug_printf("[DS5C][IO] session reset via %s\n", ch->tr->name);
        break;
    default:
        debug_printf("[DS5C][IO] unknown cmd 0x%02x via %s\n", cmd, ch->tr->name);
        break;
    }
}

// ---------------------------------------------------------------------------
// RX 重组：块 [flags][data...] → 消息 → 分发
// ---------------------------------------------------------------------------

void proto_chan_rx(proto_chan_t *ch, const uint8_t *chunk, uint16_t len)
{
    if (!ch->active || len < 1u) return;

    uint8_t flags = chunk[0];
    const uint8_t *data = chunk + 1;
    uint16_t dlen = (uint16_t)(len - 1u);

    if (flags & PROTO_F_RESET) {
        ch->rx_active = false;
        return;
    }

    if (flags & PROTO_F_START) {
        if (dlen < 3u) return;
        uint32_t total = 0;
        const uint8_t *q = varint_get(data + 3, data + dlen, &total);
        if (!q || total > PROTO_RX_REASM_MAX) {
            ch->rx_active = false;
            return;
        }
        ch->rx_active = true;
        ch->rx_cmd    = data[0];
        ch->rx_msgid  = (uint16_t)(data[1] | (data[2] << 8));
        ch->rx_total  = (uint16_t)total;
        ch->rx_off    = (uint16_t)(dlen - (uint16_t)(q - data));
        if (ch->rx_off > ch->rx_total) ch->rx_off = ch->rx_total;   // 传输侧补零裁剪
        memcpy(ch->rx_buf, q, ch->rx_off);
        if (flags & PROTO_F_END) {
            ch->rx_active = false;
            chan_dispatch(ch, ch->rx_cmd, ch->rx_msgid, ch->rx_buf, ch->rx_off);
        }
        return;
    }

    if (!ch->rx_active) return;   // CONT/END 无在途消息，丢弃

    uint16_t space = (uint16_t)(ch->rx_total - ch->rx_off);
    if (dlen > space) dlen = space;
    memcpy(ch->rx_buf + ch->rx_off, data, dlen);
    ch->rx_off = (uint16_t)(ch->rx_off + dlen);

    if (flags & PROTO_F_END) {
        ch->rx_active = false;
        chan_dispatch(ch, ch->rx_cmd, ch->rx_msgid, ch->rx_buf, ch->rx_off);
    }
}

// ---------------------------------------------------------------------------
// TX 单飞分块：出队只弹 5B 头，数据留环内就地按块读（任意长度无感）
// ---------------------------------------------------------------------------

void proto_chan_task(proto_chan_t *ch)
{
    if (!ch->active) return;

    if (!ch->tx_active && !chan_pop_header(ch)) return;
    if (!ch->tr->ready(ch)) return;

    uint8_t chunk[1u + PROTO_CHUNK_DATA_MAX];
    uint16_t pos = 0;
    chunk[pos++] = 0;              // flags 占位

    bool first = (ch->tx_off == 0);
    if (first) {
        chunk[pos++] = ch->tx_cmd;
        chunk[pos++] = (uint8_t)(ch->tx_msgid & 0xFFu);
        chunk[pos++] = (uint8_t)(ch->tx_msgid >> 8);
        pos = (uint16_t)(varint_put(chunk + pos, ch->tx_len) - chunk);
    }

    uint16_t cap = (uint16_t)((1u + PROTO_CHUNK_DATA_MAX) - pos);
    uint16_t n = (uint16_t)(ch->tx_len - ch->tx_off);
    if (n > cap) n = cap;

    chan_read_data(ch, ch->tx_off, chunk + pos, n);
    pos = (uint16_t)(pos + n);

    bool last = (uint16_t)(ch->tx_off + n) == ch->tx_len;
    chunk[0] = (uint8_t)((first ? PROTO_F_START : 0u) | (last ? PROTO_F_END : 0u));

    if (ch->tr->send_chunk(ch, chunk, pos)) {
        chan_commit(ch, n);
        ch->tx_off = (uint16_t)(ch->tx_off + n);
        if (last) ch->tx_active = false;
    }
}

// ---------------------------------------------------------------------------
// 对外 API
// ---------------------------------------------------------------------------

void proto_init(void)
{
    critical_section_init_with_lock_num(&s_cs, next_striped_spin_lock_num());
    debug_printf("[DS5C][IO] proto core initialized\n");
}

void proto_chan_init(proto_chan_t *ch, const proto_transport_t *tr)
{
    memset(ch, 0, sizeof *ch);
    ch->tr = tr;
}

void proto_register_channel(proto_chan_t *ch)
{
    if (s_chan_count < 2) s_chans[s_chan_count++] = ch;
}

void proto_log_write(const uint8_t *data, uint16_t len)
{
    for (uint8_t i = 0; i < s_chan_count; i++) {
        proto_chan_t *ch = s_chans[i];
        if (!ch->active) continue;
        chan_push(ch, PROTO_CMD_PRINT, 0u, data, len);
    }
}

void proto_chan_submit(proto_chan_t *ch, uint8_t cmd, uint16_t msgid,
                       const uint8_t *data, uint16_t len)
{
    chan_push(ch, cmd, msgid, data, len);
}
