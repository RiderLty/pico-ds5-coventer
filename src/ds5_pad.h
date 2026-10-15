#ifndef DS5_PAD_H
#define DS5_PAD_H

#include <stdint.h>

/*
 * DS5 映射引擎（core0 侧）—— 手柄/触摸/六轴全 Lua 驱动版。
 *
 * 本模块只消费 input_queue 的三种帧（均由 Lua pad_* 输出函数入队）：
 *   INQ_KIND_PAD   — 手柄轴/扳机/按键
 *   INQ_KIND_TOUCH — 触摸板坐标（2 触点）
 *   INQ_KIND_MOTION — 六轴陀螺/加速度
 * 组装 63 字节 0x01 输入报告（Report ID 1 之后，布局按内核 hid-playstation.c：
 *   0-3 LX/LY/RX/RY, 4-5 L2/R2, 6 seq, 7 hat|面键, 8 肩键/扳机/摇杆键,
 *   9 PS/触摸板/静音, 11-14 'RC0A', 15-26 陀螺仪/加速度,
 *   32-39 触摸板, 52-54 status）。
 * 手柄未接入时报告保持空闲态（摇杆回中/按键全零）。
 *
 * 仅允许 core0 调用。
 */

// 输入报告数据长度（Report ID 1 之后）
#define DS_REPORT_LEN 63u

// 初始化（在 tud_init 之后、主循环之前调用）
void ds5_pad_init(void);

// 喂入手柄帧（INQ_KIND_PAD 负载，15 字节，布局见 input_queue.h）。
// flags bit0=0 视为断连：清手柄状态，报告回落空闲态。
void ds5_pad_feed_pad(const uint8_t *data, uint8_t len);

// 喂入触摸帧（INQ_KIND_TOUCH 负载，9 字节，布局见 input_queue.h）。
void ds5_pad_feed_touch(const uint8_t *data, uint8_t len);

// 喂入六轴帧（INQ_KIND_MOTION 负载，12 字节，布局见 input_queue.h）。
void ds5_pad_feed_motion(const uint8_t *data, uint8_t len);

// 周期处理（主循环每轮调用）：手柄已连接时应用手柄状态，否则维持空闲态
void ds5_pad_tick(void);

// 组装输入报告到 out（至少 63 字节）
void ds5_pad_build_report(uint8_t *out);

#endif // DS5_PAD_H
