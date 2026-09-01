/**
 * @file hid_parser.c
 * @brief HID 报告描述符解析 + dispatch 实现
 *
 * 参考 HID 1.11 Device Class Definition §6.2.2 描述符格式。
 */

#include "hid_parser.h"
#include <string.h>     // memset

/*--------------------------------------------------------------------+
 * 辅助宏
 *--------------------------------------------------------------------*/

// 短 item 的数据长度（字节）
// bSize: 0=0B, 1=1B, 2=2B, 3=4B
#define ITEM_DATA_SIZE(prefix) \
    (((prefix) & 0x03) < 3 ? ((prefix) & 0x03) : 4)

/*--------------------------------------------------------------------+
 * 辅助函数
 *--------------------------------------------------------------------*/

// 从小端数据中读取有符号整数
static int32_t read_signed(const uint8_t *data, uint8_t size)
{
    uint32_t raw = 0;
    for (uint8_t i = 0; i < size && i < 4; i++)
        raw |= (uint32_t)data[i] << (8 * i);
    switch (size) {
    case 1: return (int32_t)(int8_t)raw;
    case 2: return (int32_t)(int16_t)raw;
    default: return (int32_t)raw;
    }
}

// 从小端数据中读取无符号整数
static uint32_t read_unsigned(const uint8_t *data, uint8_t size)
{
    uint32_t value = 0;
    for (uint8_t i = 0; i < size && i < 4; i++)
        value |= (uint32_t)data[i] << (8 * i);
    return value;
}

/*--------------------------------------------------------------------+
 * 通用描述符字段解析（共享 — mouse/gamepad 共用）
 *--------------------------------------------------------------------*/

uint8_t hid_parse_report_fields(hid_field_t *fields, uint8_t max_fields,
                                 uint8_t *report_id,
                                 const uint8_t *data, uint16_t len)
{
    if (!fields || !report_id || !data || len == 0 || max_fields == 0) return 0;

    memset(fields, 0, (size_t)max_fields * sizeof(hid_field_t));
    *report_id = 0;
    uint8_t num_fields = 0;

    // ---- 全局状态（Global items：Main item 之后保持）----
    uint16_t glb_usage_page  = 0;
    int32_t  glb_log_min     = 0;
    int32_t  glb_log_max     = 0;
    uint8_t  glb_report_size = 0;
    uint8_t  glb_report_count = 0;
    uint16_t bit_offset       = 0;   // 累计位偏移（不含 Report ID 字节）
    uint8_t  cur_report_id    = 0;   // 当前生效的 Report ID（0 = 描述符无 Report ID）

    // PUSH/POP 保存的全局项快照栈（部分多集合鼠标描述符用它保存/恢复全局状态；
    // 规范还包含物理范围/Unit，本解析器不使用故不入栈）
    #define _GLB_STACK_DEPTH 4
    struct {
        uint16_t usage_page;
        int32_t  log_min, log_max;
        uint8_t  report_size, report_count;
    } glb_stack[_GLB_STACK_DEPTH];
    uint8_t glb_depth = 0;

    // ---- 局部状态（Local items：Main item 之后复位）----
    #define _LOC_MAX_USAGES 8
    uint16_t loc_usages[_LOC_MAX_USAGES];
    uint8_t  loc_usage_count = 0;
    uint32_t loc_usage_min   = 0;
    uint32_t loc_usage_max   = 0;
    bool     loc_has_usage_range = false;

    // Collection 嵌套计数（仅用于跳过，不解析 usage 继承）
    int collection_depth = 0;

    for (uint16_t i = 0; i < len; ) {
        uint8_t prefix = data[i++];

        // 长 item（0xFE）：跳过
        if (prefix == 0xFE) {
            if (i >= len) break;
            uint8_t sz = data[i++];
            if (i + sz > len) break;
            i += sz;
            continue;
        }

        uint8_t sz = ITEM_DATA_SIZE(prefix);
        if (i + sz > len) break;

        switch (prefix) {

        /*--- Global items ---*/

        case 0x05:  // Usage Page (1B)
            glb_usage_page = data[i++];
            break;

        case 0x15: case 0x16: case 0x17:  // Logical Minimum
            glb_log_min = read_signed(&data[i], sz);
            i += sz;
            break;

        case 0x25: case 0x26: case 0x27:  // Logical Maximum
            glb_log_max = read_signed(&data[i], sz);
            i += sz;
            break;

        case 0x75:  // Report Size (1B)
            glb_report_size = data[i++];
            break;

        case 0x85:  // Report ID (1B)
            *report_id = data[i++];
            cur_report_id = *report_id;   // 后续字段归属此 ID（复合设备各集合各不相同）
            bit_offset = 0;               // 各报告的位偏移独立计数
            break;

        case 0x95:  // Report Count (1B)
            glb_report_count = data[i++];
            break;

        /*--- Local items ---*/

        case 0x09: {  // Usage (1B)
            uint8_t usage = data[i++];
            if (loc_usage_count < _LOC_MAX_USAGES) {
                loc_usages[loc_usage_count++] = usage;
            }
            break;
        }

        case 0x19: case 0x1A: case 0x1B:  // Usage Minimum
            loc_usage_min = read_unsigned(&data[i], sz);
            loc_has_usage_range = true;
            i += sz;
            break;

        case 0x29: case 0x2A: case 0x2B:  // Usage Maximum
            loc_usage_max = read_unsigned(&data[i], sz);
            loc_has_usage_range = true;
            i += sz;
            break;

        /*--- Main items ---*/

        case 0x81: {  // Input
            if (i >= len) break;
            uint8_t flags = data[i++];

            uint8_t count = glb_report_count;
            if (count == 0) count = 1;

            bool is_const   = (flags & 0x01) != 0;
            bool is_rel     = (flags & 0x04) != 0;

            if (is_const) {
                bit_offset += (uint16_t)glb_report_size * count;
            } else if (loc_usage_count > 0) {
                for (uint8_t j = 0; j < count && num_fields < max_fields; j++) {
                    hid_field_t *f = &fields[num_fields++];
                    f->usage_page  = glb_usage_page;
                    f->usage_id    = loc_usages[j % loc_usage_count];
                    f->report_id   = cur_report_id;
                    f->bit_offset  = bit_offset;
                    f->bit_size    = glb_report_size;
                    f->logical_min = glb_log_min;
                    f->logical_max = glb_log_max;
                    f->is_relative = is_rel;
                    f->is_constant = false;
                    bit_offset += glb_report_size;
                }
            } else if (loc_has_usage_range) {
                uint32_t range = loc_usage_max - loc_usage_min + 1;
                for (uint32_t j = 0; j < range && num_fields < max_fields; j++) {
                    hid_field_t *f = &fields[num_fields++];
                    f->usage_page  = glb_usage_page;
                    f->usage_id    = (uint16_t)(loc_usage_min + j);
                    f->report_id   = cur_report_id;
                    f->bit_offset  = bit_offset;
                    f->bit_size    = glb_report_size;
                    f->logical_min = glb_log_min;
                    f->logical_max = glb_log_max;
                    f->is_relative = is_rel;
                    f->is_constant = false;
                    bit_offset += glb_report_size;
                }
                if ((uint32_t)count > range) {
                    bit_offset += (uint16_t)glb_report_size * (count - (uint8_t)range);
                }
            } else {
                bit_offset += (uint16_t)glb_report_size * count;
            }

            loc_usage_count = 0;
            loc_has_usage_range = false;
            break;
        }

        case 0xA1:  // Collection
            i++;
            collection_depth++;
            break;

        case 0xC0:  // End Collection
            if (collection_depth > 0) collection_depth--;
            break;

        case 0xA0:  // PUSH：保存全局项
            if (glb_depth < _GLB_STACK_DEPTH) {
                glb_stack[glb_depth].usage_page   = glb_usage_page;
                glb_stack[glb_depth].log_min      = glb_log_min;
                glb_stack[glb_depth].log_max      = glb_log_max;
                glb_stack[glb_depth].report_size  = glb_report_size;
                glb_stack[glb_depth].report_count = glb_report_count;
                glb_depth++;
            }
            break;

        case 0xB2:  // POP：恢复全局项
            if (glb_depth > 0) {
                glb_depth--;
                glb_usage_page   = glb_stack[glb_depth].usage_page;
                glb_log_min      = glb_stack[glb_depth].log_min;
                glb_log_max      = glb_stack[glb_depth].log_max;
                glb_report_size  = glb_stack[glb_depth].report_size;
                glb_report_count = glb_stack[glb_depth].report_count;
            }
            break;

        default:
            i += sz;
            break;
        }
    }

    return num_fields;
}

/*--------------------------------------------------------------------+
 * 鼠标描述符解析
 *--------------------------------------------------------------------*/

bool hid_mouse_parse(hid_mouse_desc_t *desc, const uint8_t *data, uint16_t len)
{
    if (!desc || !data || len == 0) return false;

    memset(desc, 0, sizeof(*desc));
    desc->idx_x       = 0xFF;
    desc->idx_y       = 0xFF;
    desc->idx_wheel   = 0xFF;
    desc->idx_buttons = 0xFF;

    // 调用共享字段解析器（复合设备的所有集合都展开在 fields[] 里，
    // 每个字段带自己的 report_id）
    uint8_t last_report_id = 0;
    desc->num_fields = hid_parse_report_fields(
        desc->fields, HID_MOUSE_MAX_FIELDS, &last_report_id, data, len);
    if (desc->num_fields == 0) return false;

    // ---- 锁定鼠标集合的 Report ID：取第一根相对 X/Y 轴所在集合 ----
    // 复合设备（鼠标 + 多媒体键/系统控制等）各集合 Report ID 不同，
    // 报文按 ID 区分；若取错 ID，dispatch 会把每帧报文静默丢弃。
    uint8_t target_id = 0xFF;
    for (uint8_t i = 0; i < desc->num_fields; i++) {
        const hid_field_t *f = &desc->fields[i];
        if (f->usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP &&
            f->is_relative &&
            (f->usage_id == HID_USAGE_X_AXIS || f->usage_id == HID_USAGE_Y_AXIS)) {
            target_id = f->report_id;
            break;
        }
    }

    if (target_id == 0xFF) return false;   // 无相对轴集合 → 非鼠标

    // ---- 只在目标集合内回填预查索引 ----
    for (uint8_t i = 0; i < desc->num_fields; i++) {
        const hid_field_t *f = &desc->fields[i];
        if (f->report_id != target_id) continue;

        if (f->usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
            switch (f->usage_id) {
            case HID_USAGE_X_AXIS:
                if (desc->idx_x == 0xFF) desc->idx_x = i;
                break;
            case HID_USAGE_Y_AXIS:
                if (desc->idx_y == 0xFF) desc->idx_y = i;
                break;
            case HID_USAGE_WHEEL:
                if (desc->idx_wheel == 0xFF) desc->idx_wheel = i;
                break;
            }
        }
        // 按键页字段（跳过常量填充位）；多组按键不必连续，dispatch 按序扫描
        if (f->usage_page == HID_USAGE_PAGE_BUTTON && !f->is_constant) {
            if (desc->idx_buttons == 0xFF) desc->idx_buttons = i;
            desc->button_count++;
        }
    }

    // 判定成功：至少一根相对轴 + 至少一个按键。
    // 手柄摇杆/触摸板为绝对轴设备，会被排除；因此本解析也可用于识别
    // bInterfaceProtocol 不规范（=None）的鼠标。
    bool has_rel_axis =
        (desc->idx_x != 0xFF && desc->fields[desc->idx_x].is_relative) ||
        (desc->idx_y != 0xFF && desc->fields[desc->idx_y].is_relative);
    if (!has_rel_axis || desc->button_count == 0) return false;

    desc->report_id = target_id;
    return true;
}
/*--------------------------------------------------------------------+
 * 字段值提取
 *--------------------------------------------------------------------*/

int32_t hid_field_read(const hid_field_t *f,
                       const uint8_t *report, uint16_t report_len)
{
    if (!f || !report || f->bit_size == 0 || f->bit_size > 32)
        return 0;

    // 检查字段是否在报告范围内（uint32 防止大报告 bit_offset 回绕）
    uint32_t end_bit = (uint32_t)f->bit_offset + f->bit_size;
    uint32_t end_byte = (end_bit + 7) / 8;
    if (end_byte > report_len)
        return 0;

    uint16_t byte_idx = f->bit_offset / 8;
    uint8_t  bit_pos  = f->bit_offset % 8;

    // 逐位读取，避免跨多字节时的大移位操作
    uint32_t raw = 0;
    uint8_t bits_read = 0;
    while (bits_read < f->bit_size) {
        uint8_t bits_in_this_byte = 8 - bit_pos;
        if (bits_in_this_byte > f->bit_size - bits_read)
            bits_in_this_byte = f->bit_size - bits_read;
        if (byte_idx >= report_len)
            break;

        uint8_t mask = (uint8_t)((1u << bits_in_this_byte) - 1u);
        uint8_t val = (report[byte_idx] >> bit_pos) & mask;
        raw |= (uint32_t)val << bits_read;

        bits_read += bits_in_this_byte;
        byte_idx++;
        bit_pos = 0;
    }

    // 符号扩展：如果 logical_min < 0 且最高位为 1
    if (f->logical_min < 0) {
        uint32_t sign_bit = (uint32_t)1 << (f->bit_size - 1);
        if (raw & sign_bit) {
            // 高位补 1；bit_size==32 时字段已满 32 位，无需扩展（<<32 是 UB）
            uint32_t sign_ext = (f->bit_size >= 32) ? 0u : (~((uint32_t)0) << f->bit_size);
            raw |= sign_ext;
        }
    }

    return (int32_t)raw;
}

/*--------------------------------------------------------------------+
 * 鼠标报文纯解析（host 侧）：提取归一化帧，无状态、无副作用
 *--------------------------------------------------------------------*/

bool hid_mouse_parse_frame(const hid_mouse_desc_t *desc,
                           const uint8_t *report, uint16_t len,
                           hid_mouse_frame_t *out)
{
    if (!desc || !report || len == 0 || !out) return false;
    out->buttons = 0;
    out->wheel = 0;
    out->x = 0;
    out->y = 0;

    // Report ID 校验：复合设备的消费者/系统控制等报文在此被拒
    const uint8_t *field_data = report;
    uint16_t field_len = len;
    if (desc->report_id != 0) {
        if (len < 1 || report[0] != desc->report_id) return false;
        field_data = report + 1;
        field_len = len - 1;
    }

    // ---- 按键掩码 ----
    // 按键字段不一定在 fields[] 里连续（多组按键可被 X/Y 等隔开），
    // 故从首个按键字段向后顺序扫描，按扫描序编号 bit0..7
    if (desc->idx_buttons != 0xFF && desc->button_count > 0) {
        uint8_t bit = 0;
        for (uint8_t i = desc->idx_buttons; i < desc->num_fields && bit < 8; i++) {
            const hid_field_t *f = &desc->fields[i];
            if (f->usage_page != HID_USAGE_PAGE_BUTTON || f->is_constant) continue;
            if (hid_field_read(f, field_data, field_len)) {
                out->buttons |= (uint8_t)(1u << bit);
            }
            bit++;
        }
    }

    // ---- 轴与滚轮 ----
    if (desc->idx_x != 0xFF)
        out->x = (int16_t)hid_field_read(&desc->fields[desc->idx_x], field_data, field_len);
    if (desc->idx_y != 0xFF)
        out->y = (int16_t)hid_field_read(&desc->fields[desc->idx_y], field_data, field_len);
    if (desc->idx_wheel != 0xFF)
        out->wheel = (int8_t)hid_field_read(&desc->fields[desc->idx_wheel], field_data, field_len);

    return true;
}
