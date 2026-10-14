/*
 * DMO 鼠标报文队列实现（见 mouse_queue.h 的语义说明）
 *
 * 生产者：core1 的映射引擎（经 core_mouse_cb → mouse_queue_push）
 * 消费者：core0 主循环（mouse_hid_service → mouse_queue_pop → USB 端点）
 * 两侧不同核，索引/计数用临界区保护（与 touch_queue 同做法）。
 */

#include "mouse_queue.h"
#include "pico/sync.h"

static mouse_report_t queue[MOUSE_QUEUE_SIZE];   // BSS 零初始化，无需运行期 init
static uint16_t head = 0;    // 出队索引
static uint16_t tail = 0;    // 入队索引
static uint16_t count = 0;

// 固定自旋锁号 19（编译期静态绑定，理由见 kbd_out.c）
#define MOUSE_QUEUE_SPINLOCK_ID 19u
#define KM_SPIN_LOCK_INSTANCE(id) \
    ((spin_lock_t *)(SIO_BASE + SIO_SPINLOCK0_OFFSET + (id) * 4u))

static critical_section_t cs = {
    .spin_lock = KM_SPIN_LOCK_INSTANCE(MOUSE_QUEUE_SPINLOCK_ID),
};

// dx/dy 用 ±32767（与描述符逻辑范围一致）；wheel 用 ±127
static inline int16_t clamp_i16(int32_t v)
{
  if (v > 32767) return 32767;
  if (v < -32767) return -32767;
  return (int16_t)v;
}

static inline int8_t clamp_i8(int32_t v)
{
  if (v > 127) return 127;
  if (v < -127) return -127;
  return (int8_t)v;
}

void mouse_queue_push(int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons)
{
  mouse_report_t rep = {
    .buttons = (uint8_t)(buttons & MOUSE_HID_BIT_MASK),
    .dx = dx,
    .dy = dy,
    .wheel = wheel,
  };

  critical_section_enter_blocking(&cs);

  if (count > 0) {
    mouse_report_t *last = &queue[(tail - 1) & MOUSE_QUEUE_MASK];
    if (last->buttons == rep.buttons) {
      // 按键状态相同 → 可以合并（含"纯位移"和"重复的按键抖动"两种情况）
      if (rep.dx == 0 && rep.dy == 0 && rep.wheel == 0) {
        // 无位移、按键也没变：没有任何新信息，丢弃
        critical_section_exit(&cs);
        return;
      }
      last->dx = clamp_i16((int32_t)last->dx + rep.dx);
      last->dy = clamp_i16((int32_t)last->dy + rep.dy);
      last->wheel = clamp_i8((int32_t)last->wheel + rep.wheel);
      critical_section_exit(&cs);
      return;
    }
  }

  if (count == MOUSE_QUEUE_SIZE) {
    // 满：只可能发生在按键状态频繁跳变时。丢最旧（位移损失不可避免），
    // 但绝不丢"状态跳变"本身，否则宿主的按键会卡住。
    head = (head + 1) & MOUSE_QUEUE_MASK;
    count--;
  }
  queue[tail] = rep;
  tail = (tail + 1) & MOUSE_QUEUE_MASK;
  count++;

  critical_section_exit(&cs);
}

bool mouse_queue_pop(mouse_report_t *out)
{
  bool ok = false;
  critical_section_enter_blocking(&cs);
  if (count > 0) {
    *out = queue[head];
    head = (head + 1) & MOUSE_QUEUE_MASK;
    count--;
    ok = true;
  }
  critical_section_exit(&cs);
  return ok;
}

uint16_t mouse_queue_count(void)
{
  critical_section_enter_blocking(&cs);
  uint16_t n = count;
  critical_section_exit(&cs);
  return n;
}

// 清空队列（接口关闭时调用，防陈旧位移发给下次启用后的主机）
void mouse_queue_clear(void)
{
  critical_section_enter_blocking(&cs);
  head = 0;
  tail = 0;
  count = 0;
  critical_section_exit(&cs);
}
