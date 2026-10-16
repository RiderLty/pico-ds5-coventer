/* DualSense classic Bluetooth input, Pico 2 W, core1 only.
 * Protocol reference: RiderLty/pico-hid-mapper and awalol/DS5Dongle (MIT).
 * GAP/L2CAP own ACL creation, command scheduling, authentication and stored-key
 * replies. Sending raw HCI replies here would duplicate BTstack's own replies.
 * Native USB and its descriptors remain exclusively on core0.
 */
#include "ds5_bt.h"
#include "ds5_bt_report.h"
#include "pico/cyw43_arch.h"
#include "btstack_config.h"
#include "btstack_event.h"
#include "gap.h"
#include "l2cap.h"
#include "classic/sdp_server.h"
#include "pico/btstack_flash_bank.h"
#include "hardware/flash.h"
#include "lua_store.h"
#include "input_app.h"
#include "debug.h"
#include <string.h>

#define HID_MTU 672
#define CONNECT_TIMEOUT_MS 20000u
#define INPUT_TIMEOUT_MS 10000u
static btstack_packet_callback_registration_t hci_events;
static bd_addr_t peer;
static hci_con_handle_t acl = HCI_CON_HANDLE_INVALID;
static uint16_t control_cid, interrupt_cid;
static bool control_open, interrupt_open, closing;
static bool radio_ready, stack_ready, inquiring, selected, outgoing, mounted;
static bool control_pending, output_pending, control_requested, output_requested;
static uint32_t deadline, next_scan, next_send;
static bool led;
static uint32_t led_changed;
static uint8_t init_output[143];
static void l2cap_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size);

// Keep existing script/AI KV at the exact same offset in both variants.
_Static_assert(PICO_FLASH_BANK_TOTAL_SIZE == 8192, "BT partition size changed");
_Static_assert((LUA_KV_SIZE + PICO_FLASH_BANK_TOTAL_SIZE) % FLASH_SECTOR_SIZE == 0,
               "persistent storage must be sector aligned");
uint32_t ds5_bt_flash_storage_offset(void)
{
    return PICO_FLASH_SIZE_BYTES - LUA_KV_SIZE - PICO_FLASH_BANK_TOTAL_SIZE;
}

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
static bool due(uint32_t now, uint32_t time) { return (int32_t)(now - time) >= 0; }

static void release_input(void)
{
    if (mounted) input_app_umount(0);
    mounted = false;
    control_pending = output_pending = false;
    control_requested = output_requested = false;
}

static void reset_link(void)
{
    release_input();
    acl = HCI_CON_HANDLE_INVALID;
    control_cid = interrupt_cid = 0;
    control_open = interrupt_open = false;
    selected = outgoing = closing = false;
    deadline = 0;
    next_scan = now_ms() + 1500;
    gap_connectable_control(1);
    gap_discoverable_control(1);
    DEBUG("[DS5C][BT] waiting for PS reconnect or PS+Create pairing\n");
}

static void disconnect_link(void)
{
    release_input();
    closing = true;
    deadline = 0;
    if (acl != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(acl); // BTstack schedules command when the controller is ready
    } else if (!control_cid && !interrupt_cid) {
        reset_link();
    }
    // Before ACL completion, let BTstack/controller page timeout finish the
    // pending classic connection. gap_connect_cancel() is BLE-only. Keep the
    // selected peer until CHANNEL_OPENED(failed) so a late event cannot attach
    // a previous attempt to a newly selected controller.
}

static bool is_gamepad(uint32_t cod)
{
    // Peripheral major class + gamepad minor class (not keyboards/mice).
    return (cod & 0x001f3cu) == 0x000508u;
}

static int accept_connection(bd_addr_t addr, hci_link_type_t type)
{
    if (type != HCI_LINK_TYPE_ACL) return 0;
    if (selected) return memcmp(peer, addr, 6) == 0;
    link_key_t key;
    link_key_type_t key_type;
    // New pairing is selected by inquiry. PS reconnect is restricted to bonds.
    return gap_get_link_key_for_bd_addr(addr, key, &key_type);
}

static void request_sends(void)
{
    // request_can_send_now may invoke our callback synchronously. Set the flag
    // first, and never request again inside a failed send callback (recursion).
    if (control_pending && control_open && !control_requested) {
        control_requested = true;
        l2cap_request_can_send_now_event(control_cid);
    }
    if (output_pending && interrupt_open && !output_requested) {
        output_requested = true;
        l2cap_request_can_send_now_event(interrupt_cid);
    }
}

static void channels_ready(void)
{
    if (!control_open || !interrupt_open || mounted || closing) return;
    input_app_bt_mount();
    mounted = true;
    control_pending = output_pending = true;
    ds5_bt_make_init_output(init_output);
    next_send = now_ms();
    deadline = now_ms() + INPUT_TIMEOUT_MS;
    gap_connectable_control(0);
    gap_discoverable_control(0);
    DEBUG("[DS5C][BT] HID ready; initializing full reports\n");
}

static void hci_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel;
    if (type != HCI_EVENT_PACKET || size < 2) return;
    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        stack_ready = btstack_event_state_get_state(packet) == HCI_STATE_WORKING;
        if (stack_ready) {
            gap_set_page_scan_activity(0x12, 0x12);
            gap_set_page_scan_type(PAGE_SCAN_MODE_INTERLACED);
            next_scan = now_ms();
            DEBUG("[DS5C][BT] radio ready\n");
        }
        break;
    case GAP_EVENT_INQUIRY_RESULT: {
        if (selected || acl != HCI_CON_HANDLE_INVALID || !inquiring) break;
        if (!is_gamepad(gap_event_inquiry_result_get_class_of_device(packet))) break;
        // DS5 and Edge both advertise Wireless Controller. A known bond also
        // qualifies if a firmware omits the extended inquiry local name.
        bd_addr_t addr;
        gap_event_inquiry_result_get_bd_addr(packet, addr);
        link_key_t key;
        link_key_type_t key_type;
        bool known = gap_get_link_key_for_bd_addr(addr, key, &key_type);
        bool named = false;
        if (gap_event_inquiry_result_get_name_available(packet)) {
            uint8_t len = gap_event_inquiry_result_get_name_len(packet);
            const uint8_t *name = gap_event_inquiry_result_get_name(packet);
            static const char ds5_name[] = "Wireless Controller";
            static const char edge_name[] = "DualSense";
            named = (len >= sizeof(ds5_name)-1 &&
                     !memcmp(name, ds5_name, sizeof(ds5_name)-1)) ||
                    (len >= sizeof(edge_name)-1 &&
                     !memcmp(name, edge_name, sizeof(edge_name)-1));
        }
        if (!known && !named) break;
        bd_addr_copy(peer, addr);
        selected = outgoing = true;
        deadline = now_ms() + CONNECT_TIMEOUT_MS;
        gap_inquiry_stop();
        DEBUG("[DS5C][BT] selected %s\n", bd_addr_to_str(peer));
        break;
    }
    case GAP_EVENT_INQUIRY_COMPLETE:
        inquiring = false;
        if (selected && outgoing && acl == HCI_CON_HANDLE_INVALID && !control_cid) {
            uint8_t status = l2cap_create_channel(l2cap_handler, peer, PSM_HID_CONTROL,
                                                  HID_MTU, &control_cid);
            if (status) { DEBUG("[DS5C][BT] create failed=%u\n", status); disconnect_link(); }
        } else if (!selected) next_scan = now_ms() + 1500;
        break;
    case HCI_EVENT_CONNECTION_REQUEST:
        // BTstack has already applied accept_connection and schedules accept.
        if (size >= 12) {
            bd_addr_t addr;
            hci_event_connection_request_get_bd_addr(packet, addr);
            if (accept_connection(addr, (hci_link_type_t)hci_event_connection_request_get_link_type(packet))) {
                bd_addr_copy(peer, addr);
                selected = true;
                if (inquiring) gap_inquiry_stop();
                deadline = now_ms() + CONNECT_TIMEOUT_MS;
            }
        }
        break;
    case HCI_EVENT_CONNECTION_COMPLETE:
        if (hci_event_connection_complete_get_link_type(packet) != HCI_LINK_TYPE_ACL) break;
        if (hci_event_connection_complete_get_status(packet)) {
            reset_link();
        } else {
            hci_event_connection_complete_get_bd_addr(packet, peer);
            acl = hci_event_connection_complete_get_connection_handle(packet);
            selected = true;
            if (inquiring) gap_inquiry_stop();
            if (closing) { gap_disconnect(acl); break; }
            deadline = now_ms() + CONNECT_TIMEOUT_MS;
            DEBUG("[DS5C][BT] ACL connected %s\n", bd_addr_to_str(peer));
        }
        break;
    case HCI_EVENT_PIN_CODE_REQUEST: {
        bd_addr_t addr;
        hci_event_pin_code_request_get_bd_addr(packet, addr);
        gap_pin_code_response(addr, "0000");
        break;
    }
    case HCI_EVENT_AUTHENTICATION_COMPLETE:
        if (hci_event_authentication_complete_get_status(packet) && selected) {
            gap_drop_link_key_for_bd_addr(peer);
            DEBUG("[DS5C][BT] authentication failed; use PS+Create to pair again\n");
            disconnect_link();
        }
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        if (hci_event_disconnection_complete_get_connection_handle(packet) == acl) {
            DEBUG("[DS5C][BT] disconnected reason=%u\n",
                  hci_event_disconnection_complete_get_reason(packet));
            reset_link();
        }
        break;
    default: break;
    }
}

static void l2cap_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    if (type == L2CAP_DATA_PACKET) {
        if (mounted && channel == interrupt_cid) {
            uint8_t usb[64];
            if (ds5_bt_report_to_usb(packet, size, usb)) {
                deadline = now_ms() + INPUT_TIMEOUT_MS;
                input_app_report(0, usb, sizeof(usb));
            }
        }
        return;
    }
    if (type != HCI_EVENT_PACKET || size < 2) return;
    switch (hci_event_packet_get_type(packet)) {
    case L2CAP_EVENT_INCOMING_CONNECTION: {
        uint16_t cid = l2cap_event_incoming_connection_get_local_cid(packet);
        uint16_t psm = l2cap_event_incoming_connection_get_psm(packet);
        bd_addr_t addr;
        l2cap_event_incoming_connection_get_address(packet, addr);
        uint16_t *destination = psm == PSM_HID_CONTROL ? &control_cid :
                                psm == PSM_HID_INTERRUPT ? &interrupt_cid : NULL;
        if (!selected || memcmp(peer, addr, 6) || !destination || *destination) {
            l2cap_decline_connection(cid);
        } else {
            *destination = cid;
            l2cap_accept_connection(cid);
        }
        break;
    }
    case L2CAP_EVENT_CHANNEL_OPENED: {
        uint16_t cid = l2cap_event_channel_opened_get_local_cid(packet);
        uint16_t psm = l2cap_event_channel_opened_get_psm(packet);
        if (l2cap_event_channel_opened_get_status(packet)) {
            DEBUG("[DS5C][BT] channel failed psm=%u status=%u\n", psm,
                  l2cap_event_channel_opened_get_status(packet));
            if (acl == HCI_CON_HANDLE_INVALID) reset_link();
            else disconnect_link();
            break;
        }
        if (closing) { disconnect_link(); break; }
        if (psm == PSM_HID_CONTROL) {
            control_cid = cid;
            control_open = true;
            if (outgoing && !interrupt_cid) {
                uint8_t status = l2cap_create_channel(l2cap_handler, peer, PSM_HID_INTERRUPT,
                                                      HID_MTU, &interrupt_cid);
                if (status) { disconnect_link(); break; }
            }
        } else if (psm == PSM_HID_INTERRUPT) {
            interrupt_cid = cid;
            interrupt_open = true;
        }
        channels_ready();
        break;
    }
    case L2CAP_EVENT_CHANNEL_CLOSED: {
        uint16_t cid = l2cap_event_channel_closed_get_local_cid(packet);
        if (cid == control_cid || cid == interrupt_cid) {
            if (cid == control_cid) { control_cid = 0; control_open = false; }
            if (cid == interrupt_cid) { interrupt_cid = 0; interrupt_open = false; }
            disconnect_link();
        }
        break;
    }
    case L2CAP_EVENT_CAN_SEND_NOW: {
        uint16_t cid = l2cap_event_can_send_now_get_local_cid(packet);
        if (cid == control_cid) {
            control_requested = false;
            if (control_pending) {
                // Match mapper's full-mode feature request; keep pending on ACL backpressure.
                uint8_t feature[41] = {0x43, 0x05};
                if (!l2cap_send(cid, feature, sizeof(feature))) control_pending = false;
            }
        } else if (cid == interrupt_cid) {
            output_requested = false;
            if (output_pending && !l2cap_send(cid, init_output, sizeof(init_output)))
                output_pending = false;
        }
        break;
    }
    default: break;
    }
}

void ds5_bt_init(void)
{
    // Radio startup is after the script/Device handshake. Never block first USB
    // enumeration on controller pairing or CYW43 firmware download.
    if (cyw43_arch_init()) {
        DEBUG("[DS5C][BT] CYW43 initialization failed; USB configuration still available\n");
        return;
    }
    radio_ready = true;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
    l2cap_init(); // Must precede service registration (not follow it).
    sdp_init();
    l2cap_register_service(l2cap_handler, PSM_HID_CONTROL, HID_MTU, LEVEL_2);
    l2cap_register_service(l2cap_handler, PSM_HID_INTERRUPT, HID_MTU, LEVEL_2);
    gap_set_security_level(LEVEL_2);
    gap_ssp_set_enable(true);
    gap_secure_connections_enable(true);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
    gap_ssp_set_auto_accept(true);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_set_bondable_mode(1);
    gap_register_classic_connection_filter(accept_connection);
    gap_set_local_name("Pico DS5 Converter");
    gap_set_class_of_device(0x000100);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    gap_connectable_control(1);
    gap_discoverable_control(1);
    hci_events.callback = hci_handler;
    hci_add_event_handler(&hci_events);
    hci_power_control(HCI_POWER_ON);
}

void ds5_bt_task(void)
{
    if (!radio_ready) return;
    cyw43_arch_poll();
    uint32_t now = now_ms();
    if (!stack_ready) return;
    if (deadline && due(now, deadline)) {
        DEBUG("[DS5C][BT] connection/input timed out\n");
        disconnect_link();
    }
    if (!selected && !inquiring && due(now, next_scan)) {
        // Set before calling GAP: callbacks may be synchronous.
        inquiring = true;
        if (gap_inquiry_start(5)) { inquiring = false; next_scan = now + 1500; }
    }
    if ((control_pending || output_pending) && due(now, next_send)) {
        next_send = now + 20;
        request_sends();
    }
    bool desired = mounted || (inquiring && ((now / 200u) & 1u));
    if (desired != led && (uint32_t)(now - led_changed) >= 20) {
        led = desired;
        led_changed = now;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
    }
}
