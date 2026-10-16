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
// 手柄帧（PAD）+ 触摸帧（TOUCH）+ 六轴帧（MOTION）—— 全部由 Lua 脚本
// 的 pad_* 输出函数入队，core0 的 ds5_pad 消费。
#define INQ_KIND_PAD    4u   // 手柄帧（15B，布局见下）
#define INQ_KIND_TOUCH  5u   // 触摸帧（9B，布局见下）
#define INQ_KIND_MOTION 6u   // 六轴帧（12B，布局见下）

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

// 触摸帧布局（INQ_KIND_TOUCH 之后，小端）：
//   [0..1]   x0  u16（0..4095，DS5 原生坐标，不缩放）
//   [2..3]   y0  u16
//   [4..5]   x1  u16
//   [6..7]   y1  u16
//   [8]      flags：bit0 = 触点 0 在位 / bit1 = 触点 1 在位
#define INQ_TOUCH_PAYLOAD 9u

// 六轴帧布局（INQ_KIND_MOTION 之后，小端）：
//   [0..1]  gx i16（陀螺 X，±32767 = ±2000 dps）
//   [2..3]  gy i16
//   [4..5]  gz i16
//   [6..7]  ax i16（加速度 X，±32767 = ±4 g）
//   [8..9]  ay i16
//   [10..11] az i16
#define INQ_MOTION_PAYLOAD 12u

// 单条记录负载上限（字节）
#define INQ_REC_MAX   16u

// 初始化原子 SPSC 索引，必须在 multicore_launch_core1() 之前调用。
void input_queue_init(void);

// 入队一块带 kind 标签的数据（负载 ≤ INQ_REC_MAX，超限截断）。
// 队列满则丢弃最新并计数（排空后累计在丢弃计数里）。
// 仅允许 core1 调用。
void input_queue_send(uint8_t kind, const void *data, uint8_t len);

// core0 主循环调用：批量出队，按 kind 分发到 DS5 映射引擎。
void input_queue_flush(void);

#endif // INPUT_QUEUE_H
