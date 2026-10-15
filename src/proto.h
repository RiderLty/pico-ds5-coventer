#ifndef PROTO_H
#define PROTO_H

#include <stdbool.h>
#include <stdint.h>

/*
 * proto — 传输无关的协议通道核心（协议 v1：INFO/PRINT/RESET）。
 *
 * 每个通道（proto_chan_t）= 4KB 消息环 + TX 单飞分块状态机 + RX 重组器 +
 * 命令分发；传输适配器（custom HID / UART 串口）实现 send_chunk/ready 并把
 * 收到的块喂回 proto_chan_rx。块格式全传输一致：
 *
 *   块 = [flags:1][data:0..61]
 *   flags: START 0x01 / END 0x02 / RESET 0x04（START|END = 单块完整消息）
 *   START 块 data 头：[cmd:1][msg_id:2 LE][varint total_len][首段 data]
 *
 * 传输适配器在块外自行加 seq 与帧封装（HID：满 63B 报文；UART：COBS+CRC16
 * 帧）——命令层对传输方式完全无感。USB HID 链路可靠无 CRC；UART 侧由
 * 适配器的 CRC16 兜底。
 *
 * 消息环：entry = [cmd:1][msg_id:2 LE][len:2 LE][data]，变长、双段回绕拷贝；
 * 生产者（core1 print + core0 应答）与消费者（core0 task）经共享临界区
 * 序列化，非阻塞，环满整条丢弃并计数。
 *
 * 路由：PRINT（cmd 0x02）广播到所有 active 通道；请求应答只回请求方通道。
 */

// ---- 协议常量（与网页端 tools/webhid-test.html 一致）----
#define PROTO_CMD_INFO    0x01
#define PROTO_CMD_PRINT   0x02
#define PROTO_CMD_RESET   0x03
// 临时压测命令（验证稳定性后整段删除）：body = [随机数据][CRC32 LE]，
// 收完校验后设备回等长随机数据 + 自身 CRC32
#define PROTO_CMD_TEST    0x04

#define PROTO_F_START     0x01
#define PROTO_F_END       0x02
#define PROTO_F_RESET     0x04

// 块 data 上限：63B 载荷 - seq(1) - flags(1)；适配器在块外加 seq
#define PROTO_CHUNK_DATA_MAX  61u
#define PROTO_RX_REASM_MAX    512u
#define PROTO_RING_SIZE       4096u   // 2 的幂
#define PROTO_RING_MASK       (PROTO_RING_SIZE - 1u)
#define PROTO_ENTRY_HDR       5u

typedef struct proto_chan proto_chan_t;

// 传输适配器操作表（实现见 usb_io.c / uart_proto.c）
typedef struct {
    const char *name;
    // 现在能否再接受一块（HID：端点空闲；UART：恒真，阻塞写）
    bool (*ready)(proto_chan_t *ch);
    // 发送一块 [flags][data≤61]；返回 true = 已排队/已落线
    bool (*send_chunk)(proto_chan_t *ch, const uint8_t *chunk, uint16_t len);
} proto_transport_t;

struct proto_chan {
    // 配置（init 后只读）
    const proto_transport_t *tr;
    bool     active;               // 传输可用（HID：实例已启用；UART：恒真）

    // 消息环（生产者 core1+core0 / 消费者 core0，共享临界区）
    uint8_t  ring[PROTO_RING_SIZE];
    uint16_t head, tail, used;
    uint32_t drops;

    // TX 单飞状态（长度/偏移用 32 位：流式压测最大 2MB，16 位会截断回绕）
    bool     tx_active;
    uint8_t  tx_cmd;
    uint16_t tx_msgid;
    uint32_t tx_len, tx_off;

    // RX 重组
    bool     rx_active;
    uint8_t  rx_cmd;
    uint16_t rx_msgid;
    uint32_t rx_total, rx_off;
    uint8_t  rx_buf[PROTO_RX_REASM_MAX];

    // 流式测试（临时，TEST 命令专用；绕过重组缓冲，任意长度零拷贝）
    bool     stream_rx;
    uint16_t rx_pend_n;            // 尾部 4B 预扣（CRC32 trailer 不进流式 CRC）
    uint8_t  rx_pend[4];
    uint32_t rxcrc;                // zlib CRC32 累计
    bool     stream_tx;
    uint32_t tx_total;             // 流式发送总长（payload + 4B CRC32）
    uint32_t txcrc, rng;           // TX 生成器：CRC 累计 + xorshift32 状态
};

// 初始化通道核心（共享临界区；必须在双核启动前调用）
void proto_init(void);

// 通道初始化：绑定传输操作表（清零全部状态）。active 由适配器按自身状态置位
void proto_chan_init(proto_chan_t *ch, const proto_transport_t *tr);

// 注册通道供 proto_log_write 广播（最多 2 个：HID + UART）
void proto_register_channel(proto_chan_t *ch);

// PRINT 消息广播：推入所有 active 通道的环（非阻塞；满则整条丢弃计数）
void proto_log_write(const uint8_t *data, uint16_t len);

// 应答消息入本通道环（rx_dispatch 用；非阻塞）
void proto_chan_submit(proto_chan_t *ch, uint8_t cmd, uint16_t msgid,
                       const uint8_t *data, uint16_t len);

// core0 主循环：TX 单飞出队分块发送（环空/未激活直接返回）
void proto_chan_task(proto_chan_t *ch);

// 传输适配器喂入收到的块 [flags][data...]（重组 → 分发）
void proto_chan_rx(proto_chan_t *ch, const uint8_t *chunk, uint16_t len);

#endif // PROTO_H
