#ifndef HID_PARSER_H
#define HID_PARSER_H

/**
 * @file hid_parser.h
 * @brief HID 报告描述符解析器（host 侧专用）
 *
 * 解析 USB HID Report Descriptor，提取字段的偏移/位宽/用法，
 * 并提供按字段元数据解释原始报告的能力。
 *
 * 两个解析路径：
 *   - hid_mouse_parse/hid_mouse_dispatch: 鼠标描述符解析 + dispatch
 *   - gamepad_parser.c:                  手柄描述符解析 + dispatch（通过 hid_parse_report_fields）
 *   - hid_dispatch_mouse/hid_dispatch_keyboard: 硬编码固定格式（PIO device 侧）
 */

#include <stdint.h>
#include <stdbool.h>

/*--------------------------------------------------------------------+
 * 配置常量
 *--------------------------------------------------------------------*/

#define HID_MOUSE_MAX_FIELDS   40   // 鼠标描述符最多字段数（展开后；多 Report ID 复合设备的全部集合都展开在此）
#define HID_MOUSE_MAX_USAGES   8    // 单个 Input item 最多 usage 数
#define HID_PARSE_MAX_FIELDS   64   // 通用描述符解析器最多字段数

/*--------------------------------------------------------------------+
 * HID Usage 定义
 *--------------------------------------------------------------------*/

// Usage Page
#define HID_USAGE_PAGE_GENERIC_DESKTOP  0x01
#define HID_USAGE_PAGE_BUTTON           0x09

// Generic Desktop Usage ID
#define HID_USAGE_X_AXIS      0x30
#define HID_USAGE_Y_AXIS      0x31
#define HID_USAGE_Z_AXIS      0x32
#define HID_USAGE_RX_AXIS     0x33
#define HID_USAGE_RY_AXIS     0x34
#define HID_USAGE_RZ_AXIS     0x35
#define HID_USAGE_SLIDER      0x36
#define HID_USAGE_DIAL        0x37
#define HID_USAGE_WHEEL       0x38
#define HID_USAGE_HAT_SWITCH  0x39
#define HID_USAGE_DPAD_UP     0x90
#define HID_USAGE_DPAD_DOWN   0x91
#define HID_USAGE_DPAD_LEFT   0x92
#define HID_USAGE_DPAD_RIGHT  0x93

// Generic Desktop top-level usage
#define HID_USAGE_DESKTOP_POINTER  0x01
#define HID_USAGE_DESKTOP_MOUSE    0x02
#define HID_USAGE_DESKTOP_JOYSTICK 0x04
#define HID_USAGE_DESKTOP_GAMEPAD  0x05
#define HID_USAGE_DESKTOP_KEYBOARD 0x06
#define HID_USAGE_DESKTOP_MULTI_AXIS 0x08

// Collection types (0xA1 的参数)
#define HID_COLLECTION_PHYSICAL    0x00
#define HID_COLLECTION_APPLICATION 0x01

// Input flags (0x81 的参数)
#define HID_INPUT_CONSTANT  0x01
#define HID_INPUT_VARIABLE  0x02
#define HID_INPUT_RELATIVE  0x04

/*--------------------------------------------------------------------+
 * 数据结构
 *--------------------------------------------------------------------*/

// 展开后的单个 HID 字段（一个 usage → 一个字段）
typedef struct {
    uint16_t usage_page;    // Usage Page（如 0x01 = Generic Desktop）
    uint16_t usage_id;      // Usage ID（如 0x30 = X 轴）
    uint16_t bit_offset;    // 在报告中的位偏移（不含 Report ID 字节，按所属 Report ID 独立计）
    uint8_t  bit_size;      // 位宽（1~32）
    int32_t  logical_min;   // 逻辑最小值
    int32_t  logical_max;   // 逻辑最大值
    uint8_t  report_id;     // 字段所属的 Report ID（复合设备各集合各不相同）
    bool     is_relative : 1;  // 相对值（Relative）
    bool     is_constant : 1;  // 常量（Constant，通常为 padding）
} hid_field_t;

// 鼠标描述符解析结果
typedef struct {
    hid_field_t fields[HID_MOUSE_MAX_FIELDS];
    uint8_t     num_fields;         // 实际字段数
    uint8_t     report_id;          // 鼠标集合的 Report ID（0 = 无 Report ID；
                                    // 复合设备含多个集合时取 X/Y 所在集合的 ID）

    // 预查索引（0xFF = 不存在），指向 fields[] 数组下标
    uint8_t     idx_x;              // X 轴字段
    uint8_t     idx_y;              // Y 轴字段
    uint8_t     idx_wheel;          // 滚轮字段
    uint8_t     idx_buttons;        // 按键组起始字段
    uint8_t     button_count;       // 按键个数
} hid_mouse_desc_t;

// 归一化鼠标帧：一份报文解析出的统一数据（按键掩码 bit0..7 = L R M B F 5 6 7）
typedef struct {
    uint8_t buttons;   // 当前按住的按键位掩码
    int8_t  wheel;     // 滚轮增量
    int16_t x, y;      // 相对位移
} hid_mouse_frame_t;

// 鼠标设备句柄 = 描述符 + 解析成功标志（边沿检测由输出层负责，此处无状态）
typedef struct {
    hid_mouse_desc_t desc;
    bool             parsed;         // 挂载时描述符解析成功标志（失败则报告走原始 HEX 兜底）
} hid_mouse_dev_t;

/*--------------------------------------------------------------------+
 * API
 *--------------------------------------------------------------------*/

/**
 * @brief 通用 HID 报告描述符字段解析器（共享）
 *
 * 遍历 HID Report Descriptor，展开所有 Input item 中的字段到 hid_field_t[] 数组。
 * 不关心 usage 含义，只记录字段元数据（偏移/位宽/逻辑范围/usage 等）。
 * 调用者自行对返回的 fields[] 做后处理索引。
 *
 * @param fields      输出：展开后的字段数组
 * @param max_fields  数组容量
 * @param report_id   输出：报告 ID（0 = 无 Report ID 字节）
 * @param data        输入的 HID Report Descriptor 二进制数据
 * @param len         描述符长度（字节）
 * @return 字段数（0 表示解析失败或无双字段）
 */
uint8_t hid_parse_report_fields(hid_field_t *fields, uint8_t max_fields,
                                 uint8_t *report_id,
                                 const uint8_t *data, uint16_t len);

/**
 * @brief 解析鼠标 HID 报告描述符
 *
 * 支持多 Report ID 复合设备（鼠标 + 多媒体键/系统控制等集合共存于一个接口）：
 * 以 X/Y 轴所在集合的 Report ID 为准，只索引该集合的字段。
 * 判定成功需找到相对轴（X 或 Y，Relative）且按键数 ≥ 1——因此也可用于
 * 识别 bInterfaceProtocol 不规范（=None）的鼠标；手柄摇杆/触摸板为绝对轴，
 * 会被此规则排除。
 *
 * @param desc  输出：解析结果
 * @param data  输入的 HID Report Descriptor 二进制数据
 * @param len   描述符长度（字节）
 * @return true 解析成功，false 失败（无效参数、字段溢出或不含鼠标集合）
 */
bool hid_mouse_parse(hid_mouse_desc_t *desc, const uint8_t *data, uint16_t len);

/**
 * @brief 从报告数据中按字段元数据提取值
 *
 * 处理跨字节位域对齐和符号扩展（当 logical_min < 0 时）。
 *
 * @param f          字段元数据
 * @param report     原始 HID 报告数据
 * @param report_len 报告长度（字节）
 * @return 提取的整数值（已做符号扩展）
 */
int32_t hid_field_read(const hid_field_t *f,
                       const uint8_t *report, uint16_t report_len);

/**
 * @brief 鼠标报文纯解析（host 侧）：按描述符从 report 提取归一化帧
 *
 * 无状态、不产生边沿事件——按键边沿检测由输出层（out_text）负责，
 * 二进制输出则直接使用帧数据。含 Report ID 校验：复合设备的
 * 消费者/系统控制等非鼠标报文返回 false（调用方跳过即可）。
 *
 * @param desc     解析好的鼠标描述符
 * @param report   原始 HID 报告数据
 * @param len      报告长度（字节）
 * @param out      输出：归一化后的按键/滚轮/X/Y
 * @return true 提取成功，false 报文不属于鼠标集合（Report ID 不匹配或参数无效）
 */
bool hid_mouse_parse_frame(const hid_mouse_desc_t *desc,
                           const uint8_t *report, uint16_t len,
                           hid_mouse_frame_t *out);

#endif // HID_PARSER_H
