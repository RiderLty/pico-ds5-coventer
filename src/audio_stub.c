/*
 * UAC1 音频桩实现（照搬 DS5Dongle 的 src/usb.cpp 音频部分，去副作用）。
 *
 * 目的：让设备像真实 DS5 一样带扬声器/麦克风端点。不实现实际音频功能——
 * 扬声器(OUT EP)数据从不读取（FIFO 满后驱动自动丢弃），麦克风(IN EP)
 * 从不写入（主机收到空流）。仅响应主机对 Feature Unit 的音量/静音
 * 控制请求，值存内存即可。
 */

#include <string.h>

#include "tusb.h"

#define UAC1_ENTITY_SPK_FEATURE_UNIT    0x02
#define UAC1_ENTITY_MIC_FEATURE_UNIT    0x05

// 0: 扬声器(entity 0x02) 1: 麦克风(entity 0x05)
static uint8_t  s_mute[2]   = { 0, 0 };
static int16_t  s_volume[2] = { -25600, 0 };   // 1/256 dB 单位（扬声器默认 -100dB）

// Invoked when audio class specific set request received for an entity
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request,
                                 uint8_t *pBuff)
{
    (void)rhport;

    uint8_t const ctrlSel   = TU_U16_HIGH(p_request->wValue);
    uint8_t const entityID  = TU_U16_HIGH(p_request->wIndex);
    uint8_t const index     = entityID == UAC1_ENTITY_SPK_FEATURE_UNIT ? 0 : 1;

    if (entityID != UAC1_ENTITY_SPK_FEATURE_UNIT &&
        entityID != UAC1_ENTITY_MIC_FEATURE_UNIT) {
        return false;
    }

    switch (ctrlSel) {
        case AUDIO10_FU_CTRL_MUTE:
            if (p_request->bRequest == AUDIO10_CS_REQ_SET_CUR && p_request->wLength == 1) {
                s_mute[index] = pBuff[0];
                return true;
            }
            return false;

        case AUDIO10_FU_CTRL_VOLUME:
            if (p_request->bRequest == AUDIO10_CS_REQ_SET_CUR && p_request->wLength == 2) {
                s_volume[index] = (int16_t)(pBuff[0] | (pBuff[1] << 8));
                return true;
            }
            return false;

        default:
            return false;
    }
}

// Invoked when audio class specific get request received for an entity
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    uint8_t const ctrlSel   = TU_U16_HIGH(p_request->wValue);
    uint8_t const entityID  = TU_U16_HIGH(p_request->wIndex);
    uint8_t const index     = entityID == UAC1_ENTITY_SPK_FEATURE_UNIT ? 0 : 1;

    if (entityID != UAC1_ENTITY_SPK_FEATURE_UNIT &&
        entityID != UAC1_ENTITY_MIC_FEATURE_UNIT) {
        return false;
    }

    switch (ctrlSel) {
        case AUDIO10_FU_CTRL_MUTE:
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request,
                                                              &s_mute[index], 1);

        case AUDIO10_FU_CTRL_VOLUME:
            switch (p_request->bRequest) {
                case AUDIO10_CS_REQ_GET_CUR:
                    return tud_audio_buffer_and_schedule_control_xfer(
                        rhport, p_request, &s_volume[index], sizeof(int16_t));

                case AUDIO10_CS_REQ_GET_MIN: {
                    // 扬声器 -100dB(0x9C00)~0dB；麦克风 0~+48dB(0x3000)，照搬 DS5Dongle
                    uint8_t min[2] = { 0x00, 0x00 };
                    if (index == 0) min[1] = 0x9c;
                    return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request,
                                                                      min, sizeof(min));
                }

                case AUDIO10_CS_REQ_GET_MAX: {
                    uint8_t max[2] = { 0x00, 0x00 };
                    if (index == 1) max[1] = 0x30;
                    return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request,
                                                                      max, sizeof(max));
                }

                case AUDIO10_CS_REQ_GET_RES: {
                    uint8_t res[2];
                    if (index == 0) {
                        res[0] = 0x00; res[1] = 0x01;   // 1/256 dB
                    } else {
                        res[0] = 0x7a; res[1] = 0x00;
                    }
                    return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request,
                                                                      res, sizeof(res));
                }

                default:
                    return false;
            }

        default:
            return false;
    }
}

// 主机打开/关闭音频流接口（alt setting 切换）：直接放行，
// ISO 数据不读不写即"丢弃"
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    (void)p_request;
    return true;
}
