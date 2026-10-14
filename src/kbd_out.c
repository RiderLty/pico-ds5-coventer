/*
 * kbd_out 实现 — 宿主键盘状态队列（core1 生产 / core0 消费）
 *
 * 状态是**绝对值**（整份修饰键 + 位图），所以队列满时丢最旧即可：
 * 最新一条总是最终真相，宿主拿到它就能对齐。
 */
#include "kbd_out.h"
#include "pico/sync.h"
#include <string.h>

#define KBD_OUT_QUEUE_SIZE  16
#define KBD_OUT_QUEUE_MASK  (KBD_OUT_QUEUE_SIZE - 1)

static kbd_out_state_t queue[KBD_OUT_QUEUE_SIZE];
static uint16_t head = 0;
static uint16_t tail = 0;
static uint16_t count = 0;
static critical_section_t cs;

// 当前累积状态（只在临界区内读写，故 core0/core1 都可安全调用下面的接口）
static kbd_out_state_t s_state;

static bool s_enabled = false;

//--------------------------------------------------------------------+
// 生命周期
//--------------------------------------------------------------------+
void kbd_out_init(bool enabled)
{
  critical_section_init(&cs);
  head = tail = count = 0;
  memset(queue, 0, sizeof(queue));
  memset(&s_state, 0, sizeof(s_state));
  s_enabled = enabled;
}

void kbd_out_set_enabled(bool on)
{
  critical_section_enter_blocking(&cs);
  if (s_enabled != on) {
    head = tail = count = 0;
    memset(&s_state, 0, sizeof(s_state));
  }
  s_enabled = on;
  critical_section_exit(&cs);
}

bool kbd_out_enabled(void)
{
  return s_enabled;
}

//--------------------------------------------------------------------+
// core1：状态维护 + 入队
//--------------------------------------------------------------------+
// 把 keycode 折算成「修饰键位 / 位图位」；返回 false 表示该键不在可转发范围
static bool kbd_bit_of(uint8_t keycode, uint8_t *byte_idx, uint8_t *bit_mask, bool *is_modifier)
{
  if (keycode >= KBD_HID_USAGE_MIN && keycode <= KBD_HID_USAGE_MAX) {
    uint8_t bit = (uint8_t)(keycode - KBD_HID_USAGE_MIN);   // 0..111
    *byte_idx = (uint8_t)(bit >> 3);
    *bit_mask = (uint8_t)(1u << (bit & 7));
    *is_modifier = false;
    return true;
  }
  if (keycode >= KBD_HID_MOD_MIN && keycode <= KBD_HID_MOD_MAX) {
    *byte_idx = 0;
    *bit_mask = (uint8_t)(1u << (keycode - KBD_HID_MOD_MIN));
    *is_modifier = true;
    return true;
  }
  return false;   // 媒体键等：位图装不下，忽略
}

void kbd_out_key(uint8_t keycode, bool down)
{
  if (!s_enabled) return;

  uint8_t byte_idx, bit_mask;
  bool is_modifier;
  if (!kbd_bit_of(keycode, &byte_idx, &bit_mask, &is_modifier)) return;

  // 状态更新与入队在同一个临界区：s_state 可能被任一核的调用者改动
  critical_section_enter_blocking(&cs);
  if (is_modifier) {
    if (down) s_state.modifier |= bit_mask;
    else      s_state.modifier &= (uint8_t)~bit_mask;
  } else {
    if (down) s_state.bitmap[byte_idx] |= bit_mask;
    else      s_state.bitmap[byte_idx] &= (uint8_t)~bit_mask;
  }
  if (count == KBD_OUT_QUEUE_SIZE) {
    head = (head + 1) & KBD_OUT_QUEUE_MASK;
    count--;
  }
  queue[tail] = s_state;
  tail = (tail + 1) & KBD_OUT_QUEUE_MASK;
  count++;
  critical_section_exit(&cs);
}

void kbd_out_release_all(void)
{
  if (!s_enabled) return;

  critical_section_enter_blocking(&cs);
  memset(&s_state, 0, sizeof(s_state));
  if (count == KBD_OUT_QUEUE_SIZE) {
    head = (head + 1) & KBD_OUT_QUEUE_MASK;
    count--;
  }
  queue[tail] = s_state;
  tail = (tail + 1) & KBD_OUT_QUEUE_MASK;
  count++;
  critical_section_exit(&cs);
}

//--------------------------------------------------------------------+
// 查询（Lua 的 dkb_state / dkb_is_down）
//--------------------------------------------------------------------+
void kbd_out_state(kbd_out_state_t *out)
{
  critical_section_enter_blocking(&cs);
  *out = s_state;
  critical_section_exit(&cs);
}

bool kbd_out_is_down(uint8_t keycode)
{
  uint8_t byte_idx, bit_mask;
  bool is_modifier;
  if (!kbd_bit_of(keycode, &byte_idx, &bit_mask, &is_modifier)) return false;

  critical_section_enter_blocking(&cs);
  uint8_t v = is_modifier ? s_state.modifier : s_state.bitmap[byte_idx];
  critical_section_exit(&cs);
  return (v & bit_mask) != 0;
}

void kbd_out_state_bytes(uint8_t out[16])
{
  kbd_out_state_t st;
  kbd_out_state(&st);
  memset(out, 0, 16);
  out[0] = st.modifier;
  memcpy(out + 1, st.bitmap, KBD_HID_NKRO_BYTES);
}

void kbd_out_state_words(uint32_t out[4])
{
  uint8_t buf[16];
  kbd_out_state_bytes(buf);
  for (int i = 0; i < 4; i++) {
    out[i] = (uint32_t)buf[i * 4]
           | ((uint32_t)buf[i * 4 + 1] << 8)
           | ((uint32_t)buf[i * 4 + 2] << 16)
           | ((uint32_t)buf[i * 4 + 3] << 24);
  }
}

//--------------------------------------------------------------------+
// core0：消费
//--------------------------------------------------------------------+
bool kbd_out_peek(kbd_out_state_t *out)
{
  bool ok = false;
  critical_section_enter_blocking(&cs);
  if (count > 0) {
    *out = queue[head];
    ok = true;
  }
  critical_section_exit(&cs);
  return ok;
}

void kbd_out_commit(void)
{
  critical_section_enter_blocking(&cs);
  if (count > 0) {
    head = (head + 1) & KBD_OUT_QUEUE_MASK;
    count--;
  }
  critical_section_exit(&cs);
}

uint16_t kbd_out_count(void)
{
  critical_section_enter_blocking(&cs);
  uint16_t n = count;
  critical_section_exit(&cs);
  return n;
}
