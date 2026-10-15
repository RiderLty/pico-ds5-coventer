# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概览

RP2350 (Pico 2) 固件：USB 输入（键鼠 / Xbox XInput / Switch Pro / HID 手柄）→ Sony
DualSense (DS5) 转换器。输入侧 host 跑在 core1（PIO-USB，GPIO12/13），输出侧
device 跑在 core0（原生 USB，模拟 `054c:0ce6`）。设备端描述符基线取自
awalol/DS5Dongle（UAC1 音频接口 0-2 + DS5 接口 3）。

`README.md` 是功能与 Lua API 的权威说明；`docs/usb-hot-reload.md` 是热重载、
协议通道与日志路径的深度文档。两者都已很详尽，**不要在此重复其内容**，改代码
前先读对应那份。

## 构建

依赖通过 git submodule + vendored 源码管理，首次克隆后必须运行 setup：

```bash
./scripts/setup-sdk.sh                  # submodule init + 把 SDK 内嵌 tinyusb 切到 2d56dc53
cmake -S . -B build -DTUSB_DEBUG_LEVEL=0
cmake --build build -j8
# 产物：build/src/pico-ds5-coventer.uf2
```

- 回归命令见 `docs/pico-device.md` 与 `docs/lua-scripts.md`；`src/CMakeLists.txt` 开 `-Wall -Wextra`。
- 烧录：按住 BOOTSEL 插 USB，拷贝 uf2。
- 调试构建：`cmake -S . -B build-debug -DTUSB_DEBUG_LEVEL=2`（级别 0..3；2 会打印
  枚举与每个端点传输，刷屏且影响时序，定位完必须回 0）。
- 串口适配器不支持 4Mbaud 时加 `-DDEBUG_BAUDRATE=115200`（默认 4000000）。

**CMake 缓存陷阱**：`TUSB_DEBUG_LEVEL` 与 `DEBUG_BAUDRATE` 是 CACHE 变量，复用已有
构建目录时旧值会静默生效（实测踩过：残留的 115200 让"默认 4M"实际按 115200 发，
4M 终端收全 0）。改回默认需显式传入或 `-U DEBUG_BAUDRATE`。

## 上位机验证工具

设备日志不在串口终端里——UART 线上只有 SLIP+CRC16 二进制帧。日志走协议 PRINT
通道，必须用网页控制台看：

```bash
python3 tools/serve_https.py           # 自签 HTTPS（WebHID/Web Serial 需要 secure context）
# 打开 https://localhost:8443/webhid-test.html —— WebUSB / Web Serial 双入口
```

`tools/webhid-test.html` 是协议对端参考实现：`PicoIO` 消息层 + `Transport` 抽象
（`WinUsbTransport` / `SerialTransport`），常量表与 `src/proto.h` 必须保持一致，
改协议要两边同步。

## 架构

### 双核分工（关键约束：`tud_*` 只在 core0 调用）

| | core0 (`main.c: main`) | core1 (`main.c: core1_main`) |
|---|---|---|
| 角色 | USB Device + DS5 映射 | USB Host (PIO-USB) + Lua VM |
| 主循环 | `tud_task` / `input_queue_flush` / `ds5_pad_tick` / `usb_km_task` / `usb_km_service` / `usb_io_task` / `uart_proto_task` / `lua_script_task` | `tuh_task` / `input_app_task` / `lua_script_process` / `lua_binder_process` |
| 独占 | 全部 `tud_*`、描述符组装、重枚举 | hidkit 回调、Lua VM、Switch Pro 握手 |

手柄帧经 `input_queue.c` 的 SPSC 队列（`INQ_KIND_PAD`）交给 core0 的 `ds5_pad.c`；
键鼠有各自队列，脚本命令通过 `lua_script.c` 的有界 mailbox 往返两核。
所有 `tud_*` 调用集中在 core0 是为了从结构上消除双核直写
Device FIFO 的竞态——**不要在 core1 或 Lua 里调 `tud_*` 或改写描述符**。

启动顺序有硬性依赖：`debug_init()` → `input_queue_init()` → `usb_km_init()` →
`lua_script_init()` → 两核各自 `flash_safe_execute_core_init()`（core0 在启动 core1 前完成）。
队列临界区必须经 SDK 初始化且在 core1 启动前就绪
（生产者随时可能入队）；core1 栈用 `multicore_launch_core1_with_stack` 扩到 16KB
（默认 2KB 不够 Lua 解析器）。

### 运行期接口热重载（`usb_km.c` + `usb_descriptors.c: usb_km_apply`）

Lua `ability_enable(kbd, mouse, io)` 异步提交目标状态（原子变量），core0
`usb_km_task()` 检测差异后：`tud_disconnect()` → 500ms → `usb_km_apply()` 组装配置
描述符 → `tud_connect()`。相同状态不重枚举。flash 基线 227B/4 接口，全开
300B/7 接口。

- 描述符组装方式：拷贝 flash 基线到 RAM，按 **鼠标 → 键盘 → IO** 顺序追加接口块，
  回填 `wTotalLength`、`bNumInterfaces` 和键盘/IO 的 `bInterfaceNumber`（追加顺序
  必须连续）。函数参数顺序与 Lua 不同（Lua 是 kbd, mouse, io）。
- 内部有自检：三项都不追加时 RAM 副本必须与 flash 基线逐字节一致，否则打 BUG 日志。
- HID 实例号不是常量：DS5=0，鼠标=1，键盘=1+鼠标有无，IO 紧随键盘。
  `CFG_TUD_HID=3` + `CFG_TUD_VENDOR=1` 是热重载的必要配置。
- **所有追加接口都是单接口 HID、无 IAD**。带 IAD 的 USB 类（CDC/NCM/RNDIS）会让
  Windows usbccgp 放弃 legacy 音频分组、拆散 DS5 音频接口，与音频基线互斥。这是
  已实测的硬约束，探索记录在 `exp/cdc-serial` 分支——不要重新尝试。
- 只在 USB 断开窗口内替换描述符；报告描述符按 HID 实例号在
  `tud_hid_descriptor_report_cb` 分发。

### 协议通道核心（`proto.c`，传输无关）

`proto_chan_t` = 4KB 消息环 + TX 单飞分块 + RX 重组 + 命令分发。两个平级适配器
实现 `proto_transport_t` 操作表：

- `usb_io.c` — WinUSB vendor 接口（class 0xFF 单接口 + bulk，MS OS 1.0 描述符）
- `uart_proto.c` — UART0 4Mbaud GPIO2/3，SLIP 转义 + CRC16-CCITT，DMA 环形 RX

块格式全传输一致 `[flags][data≤61]`（flags START/END/RESET）；适配器在块外自行加
seq 与帧封装（HID 满 63B 报文；UART 加 CRC + SLIP 边界）。命令层对传输无感。
路由规则：PRINT 广播到所有 active 通道，请求应答只回请求方。

加新命令要动三处：`proto.h` 常量 → `proto.c` 的 `rx_dispatch` → 网页端 `CMD` 表。

### 日志

唯一出口是 `DEBUG(fmt, ...)`（`debug.h`），格式化后作为 PRINT 子命令广播到所有
激活传输。Lua `print` 经此接入（`[lua]` 前缀）。TinyUSB 日志经 `tu_printf` 同样
路由。proto 通道就绪前的 boot 极早期日志直接丢弃。`debug_write_raw()` 只给协议帧
TX 用，持自旋锁防双核撕裂。

### Lua 接线（`lua_core.c` / `lua_binder.c` / `input_app.c`）

`lua_binder` 只做**单向事件通知**（C → Lua，无返回值、无拦截语义）。脚本用
`enable_listen_*` 声明关注的事件，C 侧 bitmap/标志 O(1) 过滤，未监听的事件跳过
VM。输出由脚本调 C 注册函数（`dkb_*` / `dmo_*` / `ability_enable`）。

- 新增 Lua 函数：在 `input_app.c` 的 `lua_binder_rebind()` 里 `lua_binder_bind()`。
  rebind 在 VM 重建后、脚本加载前调用。
- 脚本由 `lua_script.c` 在 core1 管理：RAM 镜像最多 60 KiB，显式保存才写 KV；
  `lua_store.c` 独占 Flash 尾部 512 KiB，默认脚本在 KV 缺失或加载失败时兜底。
  协议/API/状态语义见 `docs/lua-scripts.md`。双核 Flash 安全初始化必须保留，不能
  直接在 core0 通讯回调操作 Lua 或写 Flash。
- 预制脚本：`scripts/default.lua` → `tools/embed_lua.py` → `src/lua_default_script.h`
  （**自动生成，勿手改**；CMake 自动重新生成）。
- 手柄输入目前是 C 直通（`ds5_pad.c`），不走 Lua；`pad.*` 接口落地后移交脚本。

## 平台坑（均已实测踩过，勿重蹈）

- **RP2350 软件自旋锁**：不能用 `SIO_BASE + SIO_SPINLOCK0_OFFSET + id*4` 手工构造
  `spin_lock_t*`——RP2350 因 errata E2 默认 `PICO_USE_SW_SPIN_LOCKS=1`，该地址指向
  软件锁状态，首次进临界区即死锁。队列用
  `critical_section_init_with_lock_num(&cs, next_striped_spin_lock_num())`。
- **锁号撞号**：`spin_lock_init(固定 ID)` 不入池登记，会与后续 `critical_section_init`
  领到的锁撞号并自锁。须用 `spin_lock_claim_unused(true)` 正式领取（见 `debug.c`）。
- **GPIO 复用 FUNCSEL**：RP2350 上 GPIO2/3 的 UART0 功能在 FUNCSEL 11
  （`GPIO_FUNC_UART_AUX`），写 RP2040 时代的 `GPIO_FUNC_UART` 引脚根本不接 UART。
  必须用 `UART_FUNCSEL_NUM(UART, pin)`。
- **clk_peri**：`set_sys_clock_khz()` 会把 clk_peri 切到 48MHz USB PLL，而 UART 时钟
  源正是 clk_peri——必须 `clock_configure_undivided()` 挂回 clk_sys。系统时钟须为
  12MHz 整数倍（PIO-USB 依赖），当前 240MHz。
- **RX 悬空**：RP2350 pad 复位无上下拉，UART RX 需 `gpio_pull_up()`。
- **`HIDKIT_HOT` 必须走 `HIDKIT_USER_CONFIG`**（`src/hidkit_user_config.h`）：带括号
  的宏定义会被 CMake `target_compile_definitions` 静默丢弃（flags.make 里根本不
  出现），热路径就悄悄留在 flash。
- **`hidkit` include 顺序**：先 `tusb.h` 再 `hidkit.h`——hidkit 的 `MOUSE_BUTTON_*`
  与 TinyUSB 的 HID 枚举名重叠。
- **工具链 15.x**：Lua `os` 库拉入 `remove()/rename()` → `_unlink_r/_link_r`，SDK 未
  提供桩，`main.c` 里自行定义返回错误。
- **ARM 工具链/依赖版本锁定**：`lib/pico-sdk` 内嵌 tinyusb 固定在 `2d56dc53`（该版本
  才有 UAC1 支持）；`lib/pico_pio_usb` 固定 `9510f795` 热插拔基线并应用本地补丁。
  SDK 子模块显示 TinyUSB 指针差异是 setup 脚本的正常结果，**不要修改依赖源码**。
  这些固定由 `CMakeLists.txt` 顶部校验并警告。
