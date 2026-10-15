#ifndef PROTO_H
#define PROTO_H

#include <stdbool.h>
#include <stdint.h>

/*
 * proto — 传输无关的协议通道核心（协议 v1：INFO/PRINT/RESET/TEST）。
 *
 * 每个通道（proto_chan_t）= 4KB 消息环 + TX 单飞分块状态机 + RX 重组器 +
 * 命令分发；传输适配器（WinUSB / UART 串口）实现 send_chunk/ready 并把
 * 收到的块喂回 proto_chan_rx。块格式全传输一致：
 *
 *   块 = [flags:1][data:0..61]
 *   flags: START 0x01 / END 0x02 / RESET 0x04（START|END = 单块完整消息）
 *   START 块 data 头：[cmd:1][msg_id:2 LE][varint total_len][首段 data]
 *   msg_id: 0 保留；1..65534 请求/应答配对；65535 设备主动广播。
 *
 * 两种传输适配器在块外都加 seq 与 SLIP+CRC16 帧封装；命令层对传输方式
 * 完全无感。USB bulk 自带链路完整性保证；UART 依赖适配器的 CRC16 检错。
 *
 * 消息环：entry = [cmd:1][msg_id:2 LE][len:2 LE][data]，变长、双段回绕拷贝；
 * 生产者（core1 print + core0 应答）与消费者（core0 task）经共享临界区
 * 序列化，非阻塞，环满整条丢弃并计数。
 *
 * 路由：PRINT（cmd 0x02）广播到所有 active 通道；请求应答只回请求方通道。
 */

// ---- 协议常量（与网页端 tools/pico-device.js 一致）----
#define PROTO_CMD_INFO    0x01
#define PROTO_CMD_PRINT   0x02
#define PROTO_CMD_RESET   0x03
// 流式压测命令：body = [随机数据][CRC32 LE]，
// 收完校验后设备回等长随机数据 + 自身 CRC32
#define PROTO_CMD_TEST    0x04
#define PROTO_CMD_SCRIPT  0x20
#define PROTO_CMD_ERROR   0x7F  // body = [原 cmd][错误码][可选 UTF-8 详情]，msg_id 回显请求

#define PROTO_ID_RESERVED  0x0000u
#define PROTO_ID_BROADCAST 0xFFFFu

#define PROTO_ERR_COMMAND  1u
#define PROTO_ERR_LENGTH   2u
#define PROTO_ERR_CRC      3u
#define PROTO_ERR_BUSY     4u
#define PROTO_ERR_SCRIPT   5u
#define PROTO_ERR_STORAGE  6u

#define PROTO_F_START     0x01
#define PROTO_F_END       0x02
#define PROTO_F_RESET     0x04

// 块 data 上限：63B 载荷 - seq(1) - flags(1)；适配器在块外加 seq
#define PROTO_CHUNK_DATA_MAX  61u
#define PROTO_RX_REASM_MAX    512u
#define PROTO_TEST_MAX        (8u * 1024u * 1024u)
#define PROTO_RING_SIZE       4096u   // 2 的幂
#define PROTO_RING_MASK       (PROTO_RING_SIZE - 1u)
#define PROTO_ENTRY_HDR       5u
#define PROTO_REPLY_RESERVE   768u    // 日志不能耗尽应答空间

typedef struct proto_chan proto_chan_t;

// 传输适配器操作表（实现见 usb_io.c / uart_proto.c）
typedef struct {
    const char *name;
    // 现在能否再接受一块（WinUSB：FIFO 可写；UART：恒真，阻塞写）
    bool (*ready)(proto_chan_t *ch);
    // 发送一块 [flags][data≤61]；返回 true = 已排队/已落线
    bool (*send_chunk)(proto_chan_t *ch, const uint8_t *chunk, uint16_t len);
} proto_transport_t;

struct proto_chan {
    // 配置（init 后只读）
    const proto_transport_t *tr;
    bool     active;               // 传输可用（WinUSB：接口已启用；UART：恒真）
    uint32_t generation;           // invalidates asynchronous replies on reset

    // 消息环（生产者 core1+core0 / 消费者 core0，共享临界区）
    uint8_t  ring[PROTO_RING_SIZE];
    uint16_t head, tail, used;
    uint32_t drops;

    // TX 单飞状态（长度/偏移用 32 位，支持多 MB 流式压测）
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

    // 流式测试（TEST 命令专用；绕过重组缓冲，逐块处理）
    bool     stream_rx;
    uint16_t rx_pend_n;            // 尾部 4B 预扣（CRC32 trailer 不进流式 CRC）
    uint8_t  rx_pend[4];
    uint32_t rxcrc;                // zlib CRC32 累计
    bool     stream_tx;
    uint16_t stream_msgid;
    uint32_t stream_off;           // 独立于普通消息 TX，避免打断 PRINT 后破坏其偏移
    uint32_t tx_total;             // 流式发送总长（payload + 4B CRC32）
    uint32_t txcrc, rng;           // TX 生成器：CRC 累计 + xorshift32 状态
};

// 初始化通道核心（共享临界区；必须在双核启动前调用）
void proto_init(void);

// 通道初始化：绑定传输操作表（清零全部状态）。active 由适配器按自身状态置位
void proto_chan_init(proto_chan_t *ch, const proto_transport_t *tr);
// core0: invalidate asynchronous jobs/replies and clear RX/TX on session changes.
void proto_chan_reset(proto_chan_t *ch);

// 注册通道供 proto_log_write 广播（最多 2 个：WinUSB + UART）
void proto_register_channel(proto_chan_t *ch);

// PRINT 消息广播：推入所有 active 通道的环（非阻塞；满则整条丢弃计数）
void proto_log_write(const uint8_t *data, uint16_t len);

// 应答/推送入本通道环（非阻塞）；返回 false 表示编号/长度非法或队列已满。
bool proto_chan_submit(proto_chan_t *ch, uint8_t cmd, uint16_t msgid,
                       const uint8_t *data, uint16_t len);

// core0 主循环：TX 单飞出队分块发送（环空/未激活直接返回）
void proto_chan_task(proto_chan_t *ch);

// 传输适配器喂入收到的块 [flags][data...]（重组 → 分发）
void proto_chan_rx(proto_chan_t *ch, const uint8_t *chunk, uint16_t len);

#endif // PROTO_H
