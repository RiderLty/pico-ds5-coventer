// dev_enable 三态合并逻辑的主机侧单测（不依赖 USB 栈 / Pico SDK）。
//
// 为什么要单独测：三态合并是 dev_enable 的全部语义（-1 保持 / 0 关 / 1 开），
// 但它藏在 core1 的 Lua 调用路径里，真机只能靠"接不接得上"间接判断。
// 这里直接 include 工程真实的 usb_km.h，把合并函数拉出来穷举验证。
#include <stdio.h>
#include <assert.h>
#include "usb_km.h"

#define K USB_KM_BIT_KBD
#define M USB_KM_BIT_MOUSE
#define I USB_KM_BIT_IO

static int fails;

static void eq(uint32_t got, uint32_t want, const char *name)
{
    if (got == want) { printf("  ok   %s\n", name); return; }
    printf("  FAIL %s: got 0b%d%d%d want 0b%d%d%d\n", name,
           !!(got & K), !!(got & M), !!(got & I),
           !!(want & K), !!(want & M), !!(want & I));
    fails++;
}

// 一次完整请求：从 cur 出发按 (kbd, mouse, io) 三个参数合并
static uint32_t req(uint32_t cur, int kbd, int mouse, int io)
{
    cur = usb_km_merge_tri(cur, kbd, K);
    cur = usb_km_merge_tri(cur, mouse, M);
    cur = usb_km_merge_tri(cur, io, I);
    return cur;
}

int main(void)
{
    printf("单参数三态（当前全关 0b000）：\n");
    eq(usb_km_merge_tri(0, -1, K), 0, "-1 保持 → 不变");
    eq(usb_km_merge_tri(0,  0, K), 0, " 0 关闭 → 仍关");
    eq(usb_km_merge_tri(0,  1, K), K, " 1 开启 → 打开");

    printf("单参数三态（当前全开 0b111）：\n");
    eq(usb_km_merge_tri(K|M|I, -1, K), K|M|I, "-1 保持 → 不变");
    eq(usb_km_merge_tri(K|M|I,  0, K), M|I,   " 0 关闭 → 关掉该位");
    eq(usb_km_merge_tri(K|M|I,  1, K), K|M|I, " 1 开启 → 已开不变");

    printf("组合请求（从全关出发）：\n");
    eq(req(0, 1, 1, 1), K|M|I, "dev_enable(1,1,1) → 全开");
    eq(req(0, 1, -1, -1), K, "dev_enable(1) → 只开键盘（省略按 -1）");
    eq(req(0, -1, 1, -1), M, "dev_enable(-1,1) → 只开鼠标");
    eq(req(0, -1, -1, 1), I, "dev_enable(-1,-1,1) → 只开 IO");

    printf("组合请求（从全开出发，只改一项）：\n");
    eq(req(K|M|I, -1, -1, 0), K|M, "关 IO，键盘鼠标不动");
    eq(req(K|M|I, 0, -1, -1), M|I, "关键盘，其余不动");
    eq(req(K|M|I, -1, 0, -1), K|I, "关鼠标，其余不动");
    eq(req(K|M|I, -1, -1, -1), K|M|I, "全 -1 → 完全不变");

    printf("幂等 / 边界：\n");
    eq(req(K|M|I, 1, 1, 1), K|M|I, "已全开再请求全开 → 不变（不触发重枚举）");
    eq(req(0, 0, 0, 0), 0, "已全关再请求全关 → 不变");
    eq(usb_km_merge_tri(0, 5, K), K, "非法正值 5 → 按开启");
    eq(usb_km_merge_tri(K, -5, K), K, "非法负值 -5 → 按保持");

    if (fails) { printf("\n失败 %d 项\n", fails); return 1; }
    printf("\nPASS: dev_enable 三态合并 16 项断言全部通过\n");
    return 0;
}
