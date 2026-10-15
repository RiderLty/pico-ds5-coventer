#include <stdio.h>
#include <string.h>

#include "pico/critical_section.h"
#include "pico/time.h"

#include "proto.h"
#include "usb_km.h"
#include "debug.h"
#include "uart_proto.h"

// ---------------------------------------------------------------------------
// 共享临界区：两个通道的消息环都由 core1（print）+ core0（应答/消费）访问
// ---------------------------------------------------------------------------

static critical_section_t s_cs;
static proto_chan_t *s_chans[2];
static uint8_t s_chan_count;

static bool chan_push(proto_chan_t *ch, uint8_t cmd, uint16_t msgid,
                      const uint8_t *data, uint16_t len);

static void chan_error(proto_chan_t *ch, uint8_t cmd, uint16_t msgid, uint8_t code)
{
    if (msgid == PROTO_ID_RESERVED || msgid == PROTO_ID_BROADCAST) return;
    uint8_t body[] = {cmd, code};
    chan_push(ch, PROTO_CMD_ERROR, msgid, body, sizeof body);
}

// core0 会话复位；持锁清队列，避免与 core1 的日志生产者竞争。
static void chan_reset(proto_chan_t *ch)
{
    critical_section_enter_blocking(&s_cs);
    ch->head = ch->tail = ch->used = 0;
    critical_section_exit(&s_cs);
    ch->rx_active = ch->stream_rx = false;
    ch->tx_active = ch->stream_tx = false;
}

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
// 流式压测（PROTO_CMD_TEST）
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
    ch->stream_msgid = ch->rx_msgid;
    ch->txcrc = CRC32_INIT;
    ch->rng = (uint32_t)(time_us_64()) | 1u;
    ch->stream_off = 0;
}

static void test_rx_end(proto_chan_t *ch)
{
    ch->stream_rx = false;
    ch->rx_active = false;

    if (ch->rx_off != ch->rx_total || ch->rx_pend_n != 4u) {
        chan_error(ch, ch->rx_cmd, ch->rx_msgid, PROTO_ERR_LENGTH);
        DEBUG("[test] rx FAIL: got %lu/%lu B\n",
              (unsigned long)ch->rx_off, (unsigned long)ch->rx_total);
        return;
    }
    uint32_t crc_recv = (uint32_t)ch->rx_pend[0] | ((uint32_t)ch->rx_pend[1] << 8) |
                        ((uint32_t)ch->rx_pend[2] << 16) | ((uint32_t)ch->rx_pend[3] << 24);
    if ((ch->rxcrc ^ 0xFFFFFFFFu) != crc_recv) {
        chan_error(ch, ch->rx_cmd, ch->rx_msgid, PROTO_ERR_CRC);
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

    // 传输 ready 只表示可能可发，send_chunk 仍可能因 FIFO 空间不足拒收。
    // 在整块被接受前，生成器、CRC 和偏移都必须保持可重试。
    uint32_t saved_off = ch->stream_off;
    uint32_t saved_crc = ch->txcrc;
    uint32_t saved_rng = ch->rng;

    uint8_t chunk[1u + PROTO_CHUNK_DATA_MAX];
    uint16_t pos = 0;
    chunk[pos++] = 0;

    bool first = (ch->stream_off == 0);
    if (first) {
        chunk[pos++] = PROTO_CMD_TEST;
        chunk[pos++] = (uint8_t)(ch->stream_msgid & 0xFFu);
        chunk[pos++] = (uint8_t)(ch->stream_msgid >> 8);
        pos = (uint16_t)(varint_put(chunk + pos, ch->tx_total) - chunk);
    }

    uint32_t payload_left = (uint32_t)(ch->tx_total - 4u - ch->stream_off);
    uint16_t cap = (uint16_t)((1u + PROTO_CHUNK_DATA_MAX) - pos);
    uint16_t n = (payload_left > cap) ? cap : (uint16_t)payload_left;

    bool last;
    if (payload_left == 0u) {
        n = 0;                                   // 纯 trailer 块
        last = true;
    } else {
        test_gen(ch, chunk + pos, n);
        pos = (uint16_t)(pos + n);
        ch->stream_off += n;
        last = (ch->stream_off == ch->tx_total - 4u) &&
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
    ch->stream_off = saved_off;
    ch->txcrc = saved_crc;
    ch->rng = saved_rng;
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
    if (msgid == PROTO_ID_RESERVED) return false;
    if (len > PROTO_RX_REASM_MAX) {
        critical_section_enter_blocking(&s_cs);
        ch->drops++;
        critical_section_exit(&s_cs);
        return false;
    }

    bool ok = false;
    critical_section_enter_blocking(&s_cs);
    uint16_t need = (uint16_t)(PROTO_ENTRY_HDR + len);
    uint16_t reserve = msgid == PROTO_ID_BROADCAST ? PROTO_REPLY_RESERVE : 0u;
    if (PROTO_RING_SIZE - ch->used >= need + reserve) {
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
static void chan_read_data(proto_chan_t *ch, uint8_t *dst, uint16_t n)
{
    critical_section_enter_blocking(&s_cs);
    // chan_commit 已推进 tail；不能再加累计 tx_off，否则第二块起会跳读。
    ring_read(ch, ch->tail, dst, n);
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

    if (len != 0u && (cmd == PROTO_CMD_INFO || cmd == PROTO_CMD_RESET)) {
        chan_error(ch, cmd, msgid, PROTO_ERR_LENGTH);
        return;
    }

    switch (cmd) {
    case PROTO_CMD_INFO: {
        char info[512];
        uart_proto_stats_t uart = uart_proto_stats();
        uint32_t uptime_ms = to_ms_since_boot(get_absolute_time());
        int n = snprintf(info, sizeof info,
                         "pico-ds5-coventer\n"
                         "transport: %s\n"
                         "protocol: 1 ids:1..65534 broadcast:65535\n"
                         "max_request: 512 max_test: 8388608\n"
                         "build: " __DATE__ " " __TIME__ "\n"
                         "uptime_ms: %lu\n"
                         "kbd:%d mouse:%d io:%d\n"
                         "log_drops: %lu\n"
                         "uart_baud: %lu\n"
                         "uart_ring_discard_bytes: %lu\n"
                         "uart_errors: %lu crc_errors: %lu frame_errors: %lu\n"
                         "uart_seq_gaps: %lu dma_restarts: %lu",
                         ch->tr->name,
                         (unsigned long)uptime_ms,
                         (int)usb_km_kbd_enabled(), (int)usb_km_mouse_enabled(),
                         (int)usb_km_io_enabled(),
                         (unsigned long)ch->drops,
                         (unsigned long)DEBUG_BAUDRATE,
                         (unsigned long)uart.ring_discard_bytes,
                         (unsigned long)uart.uart_errors,
                         (unsigned long)uart.crc_errors,
                         (unsigned long)uart.frame_errors,
                         (unsigned long)uart.seq_gaps,
                         (unsigned long)uart.dma_restarts);
        if (n > 0) {
            if (n >= (int)sizeof info) n = (int)sizeof info - 1;
            chan_push(ch, PROTO_CMD_INFO, msgid, (const uint8_t *)info, (uint16_t)n);
        }
        break;
    }
    case PROTO_CMD_RESET:
        chan_reset(ch);
        chan_push(ch, PROTO_CMD_RESET, msgid, (const uint8_t *)"", 0);
        DEBUG("[DS5C][IO] session reset via %s\n", ch->tr->name);
        break;
    default:
        chan_error(ch, cmd, msgid, PROTO_ERR_COMMAND);
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

    if (len > 1u + PROTO_CHUNK_DATA_MAX ||
        (flags & ~(PROTO_F_START | PROTO_F_END | PROTO_F_RESET))) {
        ch->rx_active = ch->stream_rx = false;
        return;
    }
    if (flags & PROTO_F_RESET) {
        if (flags == PROTO_F_RESET && dlen == 0u) chan_reset(ch);
        return;
    }

    // 流式测试收包中：喂 CRC，END 出结果；变长帧不允许额外填充。
    if (ch->stream_rx && !(flags & PROTO_F_START)) {
        uint32_t space = ch->rx_total - ch->rx_off;
        if (dlen > space) {
            ch->rx_active = ch->stream_rx = false;
            chan_error(ch, ch->rx_cmd, ch->rx_msgid, PROTO_ERR_LENGTH);
            return;
        }
        test_rx_feed(ch, data, dlen);
        if (flags & PROTO_F_END) test_rx_end(ch);
        return;
    }

    if (flags & PROTO_F_START) {
        // 新消息可恢复丢失 END 后的会话，不能把 START 头当作旧压测数据。
        ch->rx_active = false;
        ch->stream_rx = false;
        if (dlen < 3u) return;
        uint16_t msgid = (uint16_t)(data[1] | (data[2] << 8));
        if (msgid == PROTO_ID_RESERVED || msgid == PROTO_ID_BROADCAST) return;
        uint32_t total = 0;
        const uint8_t *q = varint_get(data + 3, data + dlen, &total);
        if (!q) {
            chan_error(ch, data[0], msgid, PROTO_ERR_LENGTH);
            return;
        }
        if ((uint32_t)(data + dlen - q) > total) {
            chan_error(ch, data[0], msgid, PROTO_ERR_LENGTH);
            return;
        }
        ch->rx_active = true;
        ch->rx_cmd    = data[0];
        ch->rx_msgid  = msgid;
        ch->rx_total  = total;
        ch->rx_off    = 0;

        if (ch->rx_cmd == PROTO_CMD_TEST) {
            if (total < 4u || total > PROTO_TEST_MAX || ch->stream_tx) {
                ch->rx_active = false;
                chan_error(ch, data[0], msgid,
                           ch->stream_tx ? PROTO_ERR_BUSY : PROTO_ERR_LENGTH);
                return;
            }
            // 流式模式（不超过 PROTO_TEST_MAX，绕过重组缓冲）
            ch->stream_rx = true;
            ch->rx_pend_n = 0u;
            ch->rxcrc = CRC32_INIT;
            uint16_t feed = (uint16_t)(dlen - (uint16_t)(q - data));
            test_rx_feed(ch, q, feed);
            if (flags & PROTO_F_END) test_rx_end(ch);
            return;
        }

        if (total > PROTO_RX_REASM_MAX) {
            ch->rx_active = false;
            chan_error(ch, data[0], msgid, PROTO_ERR_LENGTH);
            return;
        }
        ch->rx_off = (uint16_t)(dlen - (uint16_t)(q - data));
        memcpy(ch->rx_buf, q, ch->rx_off);
        if (flags & PROTO_F_END) {
            ch->rx_active = false;
            if (ch->rx_off == ch->rx_total)
                chan_dispatch(ch, ch->rx_cmd, ch->rx_msgid, ch->rx_buf, ch->rx_off);
            else chan_error(ch, ch->rx_cmd, ch->rx_msgid, PROTO_ERR_LENGTH);
        }
        return;
    }

    if (!ch->rx_active) return;   // CONT/END 无在途消息，丢弃

    uint32_t space = ch->rx_total - ch->rx_off;
    if (dlen > space) {
        ch->rx_active = false;
        chan_error(ch, ch->rx_cmd, ch->rx_msgid, PROTO_ERR_LENGTH);
        return;
    }
    memcpy(ch->rx_buf + ch->rx_off, data, dlen);
    ch->rx_off = (uint16_t)(ch->rx_off + dlen);

    if (flags & PROTO_F_END) {
        ch->rx_active = false;
        if (ch->rx_off == ch->rx_total)
            chan_dispatch(ch, ch->rx_cmd, ch->rx_msgid, ch->rx_buf, ch->rx_off);
        else chan_error(ch, ch->rx_cmd, ch->rx_msgid, PROTO_ERR_LENGTH);
    }
}

// ---------------------------------------------------------------------------
// TX 单飞分块：出队只弹 5B 头，数据留环内就地按块读（任意长度无感）
// ---------------------------------------------------------------------------

void proto_chan_task(proto_chan_t *ch)
{
    if (!ch->active) return;

    // 先完成已开始的普通消息，避免 TEST 的 START 插进 PRINT 的 CONT/END。
    // 流式压测期间新的环内消息等待，流式状态与普通消息状态互不覆盖。
    if (ch->stream_tx && !ch->tx_active) {
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

    chan_read_data(ch, chunk + pos, n);
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
        chan_push(ch, PROTO_CMD_PRINT, PROTO_ID_BROADCAST, data, len);
    }
}

bool proto_chan_submit(proto_chan_t *ch, uint8_t cmd, uint16_t msgid,
                       const uint8_t *data, uint16_t len)
{
    return chan_push(ch, cmd, msgid, data, len);
}
