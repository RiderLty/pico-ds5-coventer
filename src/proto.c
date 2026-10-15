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
// 流式压测（临时，PROTO_CMD_TEST；验证稳定性后整段删除）
// 消息体 = [随机数据...][CRC32 LE]（zlib），收发两侧同构。RX 绕过重组
// 缓冲逐字节喂 CRC（尾 4B 预扣作校验码），TX 用 xorshift32 现场生成。
// ---------------------------------------------------------------------------

#define CRC32_INIT 0xFFFFFFFFu

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

// 逐字节入流：保持最后 4B 在 pend（不进 CRC），其余喂 CRC
static void test_rx_feed(proto_chan_t *ch, const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        if (ch->rx_pend_n == 4u) {
            ch->rxcrc = crc32_update(ch->rxcrc, ch->rx_pend, 1u);
            memmove(ch->rx_pend, ch->rx_pend + 1, 3u);
            ch->rx_pend_n = 3u;
        }
        ch->rx_pend[ch->rx_pend_n++] = data[i];
        ch->rx_off++;
    }
}

static void test_tx_start(proto_chan_t *ch)
{
    ch->stream_tx = true;
    ch->tx_total  = ch->rx_total;
    ch->tx_cmd    = ch->rx_cmd;      // 回显：cmd 与 msg_id 必须与请求一致，
    ch->tx_msgid  = ch->rx_msgid;    // 否则上位机 pending 匹配不上 → 超时
    ch->txcrc = CRC32_INIT;
    ch->rng = (uint32_t)(time_us_64()) | 1u;
    ch->tx_off = 0;
}

static void test_rx_end(proto_chan_t *ch)
{
    ch->stream_rx = false;

    if (ch->rx_off != ch->rx_total || ch->rx_pend_n != 4u) {
        DEBUG("[test] rx FAIL: got %lu/%lu B\n",
              (unsigned long)ch->rx_off, (unsigned long)ch->rx_total);
        return;
    }
    uint32_t crc_recv = (uint32_t)ch->rx_pend[0] | ((uint32_t)ch->rx_pend[1] << 8) |
                        ((uint32_t)ch->rx_pend[2] << 16) | ((uint32_t)ch->rx_pend[3] << 24);
    if ((ch->rxcrc ^ 0xFFFFFFFFu) != crc_recv) {
        DEBUG("[test] rx FAIL: crc %08lx != %08lx\n",
              (unsigned long)(ch->rxcrc ^ 0xFFFFFFFFu), (unsigned long)crc_recv);
        return;
    }

    DEBUG("[test] rx ok %lu B via %s, echoing...\n",
          (unsigned long)ch->rx_total, ch->tr->name);
    test_tx_start(ch);   // 回发等长随机数据（同通道，msg_id 回显）
}

// 生成随机数据并累计 CRC；返回写入字节数
static uint16_t test_gen(proto_chan_t *ch, uint8_t *dst, uint16_t len)
{
    for (uint16_t i = 0; i < len; i += 4) {
        uint32_t x = ch->rng;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        ch->rng = x;
        uint16_t k = (uint16_t)(len - i);
        if (k > 4u) k = 4u;
        memcpy(dst + i, &x, k);
    }
    ch->txcrc = crc32_update(ch->txcrc, dst, len);
    return len;
}

// TX 流式块：随机数据填满块，末块追加 4B CRC32 trailer
static bool test_tx_chunk(proto_chan_t *ch)
{
    if (!ch->tr->ready(ch)) return false;

    uint8_t chunk[1u + PROTO_CHUNK_DATA_MAX];
    uint16_t pos = 0;
    chunk[pos++] = 0;

    bool first = (ch->tx_off == 0);
    if (first) {
        chunk[pos++] = ch->tx_cmd;
        chunk[pos++] = (uint8_t)(ch->tx_msgid & 0xFFu);
        chunk[pos++] = (uint8_t)(ch->tx_msgid >> 8);
        pos = (uint16_t)(varint_put(chunk + pos, ch->tx_total) - chunk);
    }

    uint32_t payload_left = (uint32_t)(ch->tx_total - 4u - ch->tx_off);
    uint16_t cap = (uint16_t)((1u + PROTO_CHUNK_DATA_MAX) - pos);
    uint16_t n = (payload_left > cap) ? cap : (uint16_t)payload_left;

    bool last;
    if (payload_left == 0u) {
        n = 0;                                   // 纯 trailer 块
        last = true;
    } else {
        test_gen(ch, chunk + pos, n);
        pos = (uint16_t)(pos + n);
        ch->tx_off += n;
        last = (ch->tx_off == ch->tx_total - 4u) &&
               ((uint16_t)(pos + 4u) <= (1u + PROTO_CHUNK_DATA_MAX));
    }
    if (last) {
        uint32_t crc = ch->txcrc ^ 0xFFFFFFFFu;
        chunk[pos++] = (uint8_t)crc;
        chunk[pos++] = (uint8_t)(crc >> 8);
        chunk[pos++] = (uint8_t)(crc >> 16);
        chunk[pos++] = (uint8_t)(crc >> 24);
    }
    chunk[0] = (uint8_t)((first ? PROTO_F_START : 0u) | (last ? PROTO_F_END : 0u));

    if (ch->tr->send_chunk(ch, chunk, pos)) {
        if (last) {
            ch->stream_tx = false;
            DEBUG("[test] tx ok %lu B via %s\n",
                  (unsigned long)ch->tx_total, ch->tr->name);
        }
        return true;
    }
    return false;
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
    DEBUG("[DS5C][IO] dispatch(%s): cmd=0x%02x msgid=%u len=%u\n",
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
        DEBUG("[DS5C][IO] session reset via %s\n", ch->tr->name);
        break;
    default:
        DEBUG("[DS5C][IO] unknown cmd 0x%02x via %s\n", cmd, ch->tr->name);
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
        ch->stream_rx = false;
        return;
    }

    // 流式测试收包中：喂 CRC，END 出结果。HID 适配器的块恒满长（补零），
    // 超出 total 的补零不进流
    if (ch->stream_rx) {
        uint32_t space = ch->rx_total - ch->rx_off;
        uint16_t feed = (dlen > space) ? (uint16_t)space : dlen;
        test_rx_feed(ch, data, feed);
        if (flags & PROTO_F_END) test_rx_end(ch);
        return;
    }

    if (flags & PROTO_F_START) {
        if (dlen < 3u) return;
        uint32_t total = 0;
        const uint8_t *q = varint_get(data + 3, data + dlen, &total);
        if (!q) {
            ch->rx_active = false;
            return;
        }
        ch->rx_active = true;
        ch->rx_cmd    = data[0];
        ch->rx_msgid  = (uint16_t)(data[1] | (data[2] << 8));
        ch->rx_total  = total;
        ch->rx_off    = 0;

        if (ch->rx_cmd == PROTO_CMD_TEST) {
            // 流式模式（任意长度，绕过重组缓冲）
            ch->stream_rx = true;
            ch->rx_pend_n = 0u;
            ch->rxcrc = CRC32_INIT;
            test_rx_feed(ch, q, (uint16_t)(dlen - (uint16_t)(q - data)));
            if (flags & PROTO_F_END) test_rx_end(ch);
            return;
        }

        if (total > PROTO_RX_REASM_MAX) {
            ch->rx_active = false;
            return;
        }
        ch->rx_off = (uint16_t)(dlen - (uint16_t)(q - data));
        if (ch->rx_off > ch->rx_total) ch->rx_off = ch->rx_total;   // 传输侧补零裁剪
        memcpy(ch->rx_buf, q, ch->rx_off);
        if (flags & PROTO_F_END) {
            ch->rx_active = false;
            chan_dispatch(ch, ch->rx_cmd, ch->rx_msgid, ch->rx_buf, ch->rx_off);
        }
        return;
    }

    if (!ch->rx_active) return;   // CONT/END 无在途消息，丢弃

    uint32_t space = ch->rx_total - ch->rx_off;
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

    // 流式压测发送优先（占用通道期间环内消息排队等待）
    if (ch->stream_tx) {
        test_tx_chunk(ch);
        return;
    }

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

    bool last = (ch->tx_off + n) == ch->tx_len;
    chunk[0] = (uint8_t)((first ? PROTO_F_START : 0u) | (last ? PROTO_F_END : 0u));

    if (ch->tr->send_chunk(ch, chunk, pos)) {
        chan_commit(ch, n);
        ch->tx_off += n;
        if (last) ch->tx_active = false;
    }
}

// ---------------------------------------------------------------------------
// 对外 API
// ---------------------------------------------------------------------------

void proto_init(void)
{
    critical_section_init_with_lock_num(&s_cs, next_striped_spin_lock_num());
    DEBUG("[DS5C][IO] proto core initialized\n");
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
