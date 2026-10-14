#ifndef DS5_PAD_H
#define DS5_PAD_H

#include <stdint.h>

/*
 * DS5 映射引擎（core0 侧）：
 *   input_queue 出队的键盘报文 / 鼠标帧喂进来，维护手柄状态，
 *   ds5_pad_build_report() 组装 63 字节 0x31 输入报告数据
 *   （Report ID 1 之后的部分，字节布局按内核 hid-playstation.c：
 *     0-3 LX/LY/RX/RY, 4-5 L2/R2, 6 seq, 7 hat|面键, 8 肩键/扳机/摇杆键,
 *     9 PS/触摸板/静音, 11-14 'RC0A', 15-26 陀螺仪/加速度(静止),
 *     32-39 触摸板, 52-54 status）。
 *
 * 仅允许 core0 调用。
 */

// 输入报告数据长度（Report ID 1 之后）
#define DS_REPORT_LEN 63u

// 初始化（在 tud_init 之后、主循环之前调用）
void ds5_pad_init(void);

// 喂入原始键盘报文（mod + reserved + keys[6]，最长 8 字节）
void ds5_pad_feed_kb(const uint8_t *report, uint8_t len);

// 喂入打包鼠标帧：buttons u8, wheel i8, x i16, y i16（小端，共 7 字节）
void ds5_pad_feed_ms(const uint8_t *data, uint8_t len);

// 喂入手柄帧（INQ_KIND_PAD 负载，15 字节，布局见 input_queue.h）。
// flags bit0=0 视为断连：清手柄状态并回落键鼠映射。
void ds5_pad_feed_pad(const uint8_t *data, uint8_t len);

// 周期处理：摇杆回中衰减、触摸板超时松开（主循环每轮调用）
void ds5_pad_tick(void);

// 组装输入报告到 out（至少 63 字节）
void ds5_pad_build_report(uint8_t *out);

#endif // DS5_PAD_H
