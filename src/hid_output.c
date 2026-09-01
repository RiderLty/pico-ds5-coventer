/*
 * 输出中间层：统一事件固定路由到 out_device（DS5 模拟）。
 */

#include "hid_output.h"

static const hid_output_ops_t *route(void)
{
    return &hid_output_device_ops;
}

void hid_output_mount(uint8_t instance, const hid_dev_info_t *info, uint8_t proto)
{
    route()->mount(instance, info, proto);
}

void hid_output_umount(uint8_t instance, const hid_dev_info_t *info)
{
    route()->umount(instance, info);
}

void hid_output_error(const char *msg)
{
    route()->error(msg);
}

void hid_output_keyboard(uint8_t instance, const hid_dev_info_t *info,
                         const uint8_t *report, uint8_t len)
{
    route()->keyboard(instance, info, report, len);
}

void hid_output_mouse(uint8_t instance, const hid_dev_info_t *info,
                      const hid_mouse_frame_t *frame)
{
    route()->mouse(instance, info, frame);
}

void hid_output_generic(uint8_t instance, const hid_dev_info_t *info,
                        const uint8_t *report, uint16_t len)
{
    route()->generic(instance, info, report, len);
}
