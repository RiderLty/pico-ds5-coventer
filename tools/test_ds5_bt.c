// Drive the actual radio application with real BTstack event accessors and a
// mocked radio. Models synchronous CAN_SEND_NOW, asynchronous opens, and bonds.
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include "../src/ds5_bt.c"
#include "fixture.h"

static uint32_t clock_ms;
static int radio_failure, scans, creates, accepts, declines, mounts, unmounts, reports;
static int disconnects, drops, sends, fail_sends;
static bool bond, l2cap_initialized;
static uint16_t allocated_cid = 0x40;
static int (*connection_filter)(bd_addr_t, hci_link_type_t);

absolute_time_t get_absolute_time(void) { return clock_ms; }
uint32_t to_ms_since_boot(absolute_time_t time) { return (uint32_t)time; }
int cyw43_arch_init(void) { return radio_failure; }
void cyw43_arch_poll(void) {}
void cyw43_arch_gpio_put(unsigned pin, bool value) { (void)pin; (void)value; }
void debug_log(const char *fmt, ...) { (void)fmt; }
void input_app_bt_mount(void) { ++mounts; }
void input_app_umount(uint8_t instance) { assert(instance == 0); ++unmounts; }
bool input_app_report(uint8_t instance, const uint8_t *report, uint16_t size) {
    assert(instance == 0 && size == 64 && report[0] == 1); ++reports; return true;
}
void bd_addr_copy(bd_addr_t dest, const bd_addr_t src) { memcpy(dest, src, 6); }
void reverse_bytes(const uint8_t *src, uint8_t *dest, int len) {
    for (int i = 0; i < len; ++i) dest[len - 1 - i] = src[i];
}
uint16_t little_endian_read_16(const uint8_t *data, int pos) {
    return data[pos] | ((uint16_t)data[pos+1] << 8);
}
uint32_t little_endian_read_24(const uint8_t *data, int pos) {
    return little_endian_read_16(data, pos) | ((uint32_t)data[pos+2] << 16);
}
char *bd_addr_to_str(const bd_addr_t addr) { (void)addr; return "00:00:00:00:00:00"; }
void l2cap_init(void) { l2cap_initialized = true; }
void sdp_init(void) { assert(l2cap_initialized); }
uint8_t l2cap_register_service(btstack_packet_handler_t handler, uint16_t psm,
                             uint16_t mtu, gap_security_level_t level) {
    assert(l2cap_initialized && handler && mtu == HID_MTU && level == LEVEL_2);
    assert(psm == PSM_HID_CONTROL || psm == PSM_HID_INTERRUPT); return 0;
}
uint8_t l2cap_create_channel(btstack_packet_handler_t handler, bd_addr_t addr,
                           uint16_t psm, uint16_t mtu, uint16_t *cid) {
    (void)handler; (void)addr; (void)psm; (void)mtu; ++creates; *cid = allocated_cid++; return 0;
}
void l2cap_accept_connection(uint16_t cid) { (void)cid; ++accepts; }
void l2cap_decline_connection(uint16_t cid) { (void)cid; ++declines; }
uint8_t l2cap_send(uint16_t cid, const uint8_t *data, uint16_t len) {
    assert(mounted && control_open && interrupt_open);
    assert((cid == control_cid && len == 41 && data[0] == 0x43) ||
           (cid == interrupt_cid && len == 143 && data[0] == 0xa2));
    ++sends;
    if (fail_sends) { --fail_sends; return BTSTACK_ACL_BUFFERS_FULL; }
    return 0;
}
uint8_t l2cap_request_can_send_now_event(uint16_t cid) {
    // BTstack is allowed to deliver this event before request() returns.
    uint8_t event[] = {L2CAP_EVENT_CAN_SEND_NOW, 2, cid, cid >> 8};
    l2cap_handler(HCI_EVENT_PACKET, 0, event, sizeof(event)); return 0;
}
int gap_inquiry_start(uint8_t duration) { assert(duration == 5); ++scans; return 0; }
int gap_inquiry_stop(void) { return 0; }
uint8_t gap_disconnect(hci_con_handle_t handle) { assert(handle == acl); ++disconnects; return 0; }
bool gap_get_link_key_for_bd_addr(bd_addr_t addr, link_key_t key, link_key_type_t *type) {
    (void)addr; (void)key; (void)type; return bond;
}
void gap_drop_link_key_for_bd_addr(bd_addr_t addr) { (void)addr; bond = false; ++drops; }
int gap_pin_code_response(const bd_addr_t addr, const char *pin) { (void)addr; (void)pin; return 0; }
void gap_connectable_control(uint8_t value) { (void)value; }
void gap_discoverable_control(uint8_t value) { (void)value; }
void gap_set_page_scan_activity(uint16_t interval, uint16_t window) { (void)interval; (void)window; }
void gap_set_page_scan_type(page_scan_type_t type) { (void)type; }
void gap_set_security_level(gap_security_level_t level) { assert(level == LEVEL_2); }
void gap_ssp_set_enable(int value) { (void)value; }
void gap_secure_connections_enable(bool value) { (void)value; }
void gap_ssp_set_io_capability(int value) { (void)value; }
void gap_ssp_set_auto_accept(int value) { (void)value; }
void gap_ssp_set_authentication_requirement(int value) { (void)value; }
void gap_set_bondable_mode(int value) { (void)value; }
void gap_register_classic_connection_filter(int (*filter)(bd_addr_t, hci_link_type_t)) { connection_filter = filter; }
void gap_set_local_name(const char *name) { (void)name; }
void gap_set_class_of_device(uint32_t cod) { (void)cod; }
void hci_set_inquiry_mode(inquiry_mode_t mode) { (void)mode; }
void hci_add_event_handler(btstack_packet_callback_registration_t *handler) { assert(handler->callback == hci_handler); }
int hci_power_control(HCI_POWER_MODE mode) { (void)mode; return 0; }

static void hci_event(uint8_t *packet, size_t size) { hci_handler(HCI_EVENT_PACKET, 0, packet, size); }
static void channel_open(uint16_t psm, uint16_t cid, uint8_t status) {
    uint8_t event[26] = {L2CAP_EVENT_CHANNEL_OPENED, 24, status};
    event[11] = psm; event[12] = psm >> 8;
    event[13] = cid; event[14] = cid >> 8;
    l2cap_handler(HCI_EVENT_PACKET, 0, event, sizeof(event));
}
static void incoming(uint16_t psm, uint16_t cid, bool wrong_peer) {
    uint8_t event[16] = {L2CAP_EVENT_INCOMING_CONNECTION, 14};
    reverse_bytes(peer, event+2, 6); if (wrong_peer) event[2] ^= 1;
    event[10] = psm; event[11] = psm >> 8; event[12] = cid; event[13] = cid >> 8;
    l2cap_handler(HCI_EVENT_PACKET, 0, event, sizeof(event));
}
static void acl_connected(void) {
    uint8_t event[13] = {HCI_EVENT_CONNECTION_COMPLETE, 11, 0, 1, 0};
    reverse_bytes(peer, event+5, 6); event[11] = HCI_LINK_TYPE_ACL; hci_event(event, sizeof(event));
}
static void acl_disconnected(void) {
    uint8_t event[] = {HCI_EVENT_DISCONNECTION_COMPLETE, 4, 0, 1, 0, 0x13};
    hci_event(event, sizeof(event));
}
static void inquiry_result(uint32_t cod, const char *name) {
    uint8_t event[80] = {GAP_EVENT_INQUIRY_RESULT, 78};
    event[2] = 0x11; event[9] = cod; event[10] = cod >> 8; event[11] = cod >> 16;
    if (name) { event[25] = 1; event[26] = strlen(name); memcpy(event+27, name, strlen(name)); }
    hci_event(event, sizeof(event));
}
int main(void)
{
    // Failure must leave configuration/Lua usable and never poll an uninitialized radio.
    radio_failure = 1; ds5_bt_init(); assert(!radio_ready); ds5_bt_task();
    radio_failure = 0; ds5_bt_init(); assert(radio_ready && connection_filter);
    assert(ds5_bt_flash_storage_offset() ==
           (PICO_FLASH_SIZE_BYTES == 16777216u ? 0xf7e000u : 0x37e000u));
    uint8_t state[] = {BTSTACK_EVENT_STATE, 1, HCI_STATE_WORKING};
    hci_event(state, sizeof(state)); ds5_bt_task(); assert(scans == 1);
    uint8_t done[] = {GAP_EVENT_INQUIRY_COMPLETE, 1, 0};
    hci_event(done, sizeof(done)); clock_ms += 1600; ds5_bt_task(); assert(scans == 2);
    inquiry_result(0x2540, "Wireless Controller"); assert(!selected); // keyboard
    inquiry_result(0x2508, "Other gamepad"); assert(!selected);
    inquiry_result(0x2508, "Wireless Controller"); assert(selected && outgoing);
    hci_event(done, sizeof(done)); assert(creates == 1 && control_cid);
    acl_connected();
    channel_open(PSM_HID_CONTROL, control_cid, 0);
    assert(creates == 2 && interrupt_cid && !mounted && !mounts && !sends);
    channel_open(PSM_HID_INTERRUPT, interrupt_cid, 0); assert(mounted && mounts == 1);
    fail_sends = 2; ds5_bt_task(); assert(control_pending && output_pending && sends == 2);
    clock_ms += 21; ds5_bt_task(); assert(!control_pending && !output_pending && sends == 4);
    l2cap_handler(L2CAP_DATA_PACKET, interrupt_cid, (uint8_t *)fixture, 79); assert(reports == 1);
    l2cap_handler(L2CAP_DATA_PACKET, control_cid, (uint8_t *)fixture, 79); assert(reports == 1);
    uint16_t old_cid = interrupt_cid;
    uint8_t closed[] = {L2CAP_EVENT_CHANNEL_CLOSED, 2, old_cid, old_cid >> 8};
    l2cap_handler(HCI_EVENT_PACKET, 0, closed, sizeof(closed));
    assert(!mounted && unmounts == 1 && disconnects == 1 && !output_pending);
    acl_disconnected(); assert(!selected && !control_cid && !interrupt_cid);
    assert(!connection_filter(peer, HCI_LINK_TYPE_ACL)); bond = true;
    assert(connection_filter(peer, HCI_LINK_TYPE_ACL));
    assert(!connection_filter(peer, HCI_LINK_TYPE_SCO));
    // PS reconnect: controller initiates both services; no outgoing create.
    uint8_t request[12] = {HCI_EVENT_CONNECTION_REQUEST, 10};
    reverse_bytes(peer, request+2, 6); request[11] = HCI_LINK_TYPE_ACL;
    hci_event(request, sizeof(request)); acl_connected(); assert(!outgoing);
    incoming(PSM_HID_CONTROL, 0x70, true); assert(declines == 1);
    incoming(PSM_HID_CONTROL, 0x70, false); assert(accepts == 1);
    incoming(PSM_HID_CONTROL, 0x71, false); assert(declines == 2);
    channel_open(PSM_HID_CONTROL, 0x70, 0); assert(!mounted && creates == 2);
    incoming(PSM_HID_INTERRUPT, 0x71, false); channel_open(PSM_HID_INTERRUPT, 0x71, 0);
    assert(mounted && mounts == 2); ds5_bt_task();
    // No valid input after mode init: clean disconnect instead of stuck paired state.
    clock_ms += INPUT_TIMEOUT_MS + 1; ds5_bt_task(); assert(!mounted && disconnects == 2);
    acl_disconnected();
    // Authentication failure drops stale bond; pairing remains available.
    selected = true; acl_connected();
    uint8_t auth[] = {HCI_EVENT_AUTHENTICATION_COMPLETE, 3, 5, 1, 0};
    hci_event(auth, sizeof(auth)); assert(drops == 1 && !bond && disconnects == 3);
    acl_disconnected(); clock_ms += 1600; ds5_bt_task(); assert(inquiring);
    // Pending channel allocation must never count as an open channel.
    selected = outgoing = true; channel_open(PSM_HID_CONTROL, 0x90, 0x77);
    assert(!mounted && !selected);
    // Timeout before ACL completion: keep the old attempt isolated; late ACL
    // success must disconnect, never mount it or mix it with the next scan.
    selected = outgoing = true; control_cid = 0xa0;
    deadline = clock_ms + 1; clock_ms += 2; ds5_bt_task();
    assert(closing && selected && !mounted);
    acl_connected(); assert(disconnects == 4 && !mounted);
    acl_disconnected();
    // Late failed-open event from the completed attempt is harmless; slots reusable.
    channel_open(PSM_HID_CONTROL, 0xa0, 0x04);
    assert(!selected && !control_cid && !interrupt_cid);
    puts("PASS: init failure, scan retry/filter, pair, async open, send retry, input, disconnect, bonded reconnect, auth failure, timeout, partition");
}
