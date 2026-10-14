#ifndef INPUT_QUEUE_H
#define INPUT_QUEUE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * 跨核传输层：SPSC 字节块队列（由 pico-hid-forward 的 cdc_output 改造，
 * 去掉串口路径）。
 *
 * core1（input_app）把统一事件打上 kind 标签入队；
 * core0 的 input_queue_flush() 批量出队喂给 DS5 映射引擎——
 * 所有 tud_* 调用只发生在 core0，从结构上消除双核直写 Device FIFO 的竞态。
 */

// 记录 kind 标签（每条记录第 1 字节）。
// 手柄直通帧（PAD）是唯一记录类型 —— 键鼠→手柄的固定管线已移除
// （键鼠输入经 Lua 转发到宿主键鼠接口，见 input_app.c）
#define INQ_KIND_PAD  4u   // 其余字节 = 手柄帧（15B，布局见下）

// 手柄帧布局（INQ_KIND_PAD 之后，小端）：
//   [0]      flags：bit0 = 手柄已连接（0 = 断连标记，其余字节应全零）
//   [1..2]   ls_x  i16（hidkit 归一化 -32767..32767）
//   [3..4]   ls_y  i16
//   [5..6]   rs_x  i16
//   [7..8]   rs_y  i16
//   [9]      lt    u8（0..255，由 0..32767 折算）
//   [10]     rt    u8
//   [11..14] btnmask u32（bit n = hidkit BTN_* n 按住；BTN_* ∈ 0x01..0x1B）
#define INQ_PAD_PAYLOAD 15u

// 单条记录负载上限（字节）
#define INQ_REC_MAX   16u

// 初始化队列与临界区。必须在 multicore_launch_core1() 之前调用：
// 生产者（core1）随时可能入队，自旋锁必须先就绪。
void input_queue_init(void);

// 入队一块带 kind 标签的数据（负载 ≤ INQ_REC_MAX，超限截断）。
// 队列满则丢弃最新并计数（排空后累计在丢弃计数里）。
// 仅允许 core1 调用。
void input_queue_send(uint8_t kind, const void *data, uint8_t len);

// core0 主循环调用：批量出队，按 kind 分发到 DS5 映射引擎。
void input_queue_flush(void);

#endif // INPUT_QUEUE_H
