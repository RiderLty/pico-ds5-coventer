/*
 * DMO 鼠标报文队列（core1 引擎 → core0 USB）
 *
 * 与 touch_queue 同范式（临界区保护的 SPSC 环），但语义有一条关键差别：
 * 触点事件可以"合并/丢弃旧位置"，鼠标是**相对增量**——丢一条就永久丢位移，
 * 所以队列满时**不丢最旧**，而是把新报文合并进队尾（仅当按键状态相同，
 * 否则必须保留各自的状态跳变）。
 */
#ifndef MOUSE_QUEUE_H
#define MOUSE_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include "mouse_hid.h"   // mouse_report_t

#define MOUSE_QUEUE_SIZE  32
#define MOUSE_QUEUE_MASK  (MOUSE_QUEUE_SIZE - 1)

// 初始化（main 中调用一次，core1 启动前）
void mouse_queue_init(void);

// 入队（引擎侧调用；队列满则与队尾合并，不丢位移）
void mouse_queue_push(int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons);

// 出队（core0 主循环调用，每次最多 1 条）
bool mouse_queue_pop(mouse_report_t *out);

// 清空队列（接口关闭时调用，防陈旧位移）
void mouse_queue_clear(void);

// 当前积压条数（诊断用）
uint16_t mouse_queue_count(void);

#endif // MOUSE_QUEUE_H
