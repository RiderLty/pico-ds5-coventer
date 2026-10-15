# USB 描述符热重载

本功能通过 Lua `ability_enable(kbd, mouse, io)` 运行期追加或移除键鼠 HID 接口
与 custom IO 通讯接口。
DS5 + UAC1 基线不变，设备身份始终保持 Sony `054c:0ce6`。
实现参考 `../pico-hid-mapper` 的配置描述符追加和断开/重连流程。

## 故障根因

旧实现给键盘、鼠标队列静态绑定编号 18/19，手工构造
`SIO_BASE + SIO_SPINLOCK0_OFFSET + id * 4` 地址作为 `spin_lock_t *`。
RP2350 的 Pico SDK 因 errata E2 默认启用 `PICO_USE_SW_SPIN_LOCKS=1`，
`spin_lock_t` 指向软件锁状态，不能直接使用 SIO 硬件锁寄存器地址。
首次进入队列临界区就可能卡死：启动预装键鼠验证会在设备栈初始化前停止；
热重载则在断开、描述符组装之后的键盘队列切换处停止，无法执行重连。

描述符日志只证明组装完成，不证明后续队列操作与 USB 连接已经执行。
循环出现 `USBD Xfer Complete` 是正常 HID 轮询日志，不能据此判定枚举配置错误。

修复方式：

```c
critical_section_init_with_lock_num(&cs, next_striped_spin_lock_num());
```

`kbd_out_init()` 与 `mouse_queue_init()` 通过 SDK 选择正确的锁地址。
`main()` 在启动 core1 前调用 `usb_km_init()`，保证生产者访问队列前锁已就绪。
两队列使用 striped 池，临界区内不嵌套其他锁，不额外消耗 TinyUSB 所需的独占锁池。
不要恢复手工 SIO 指针、依赖 BSS 零初始化临界区，或为此禁用 RP2350 软件锁。

另一个必要配置是 `CFG_TUD_HID=4`：新配置最多有 DS5、鼠标、键盘、IO 四个 HID 实例。
只配置一个实例不能打开全部接口。无须为热重载修改 TinyUSB 源码。

## 切换流程与约束

1. core1 的 Lua 提交目标状态，由原子变量传给 core0。调用是异步的。
2. core0 `usb_km_task()` 检测到目标不同；相同状态不重枚举。
3. 设备栈已初始化时调用 `tud_disconnect()`，等待 500ms 让主机观察断开。
   core0 暂停 USB 上报；core1 仍处理输入。
4. `usb_km_apply(mouse, kbd)` 复制 227B 基线，按鼠标、键盘顺序追加接口，
   回填 `wTotalLength`、`bNumInterfaces` 和键盘接口编号。
5. 更新键盘 HID 实例号、键盘队列开关、关闭侧队列与生效状态。
6. 调用 `tud_connect()`，恢复主循环，等待主机复位与配置新设备。
   真正完成挂载由 `tud_mount_cb()` 的 `MOUNTED` 日志确认。

注意 Lua API 的参数顺序为 **键盘、鼠标**，内部描述符组装函数为 **鼠标、键盘**。
只在 USB 断开窗口内替换配置描述符，报告描述符按 HID 实例号分发。
不要在 Lua/core1 直接调用 `tud_*` 或改写设备描述符。

当前采用已验证的断开 → 500ms → apply → 连接流程，没有引入设备栈反复 deinit/init。
`tud_disconnect()` 后 `tud_mounted()` 可能仍为 true，软件状态尚未因主机总线事件更新；
它不能单独证明物理断开失败。`tud_connect()` 也不意味着主机已经完成枚举。

## 配置组合

| `ability_enable(kbd, mouse, io)` | 配置长度 | 接口数 | HID 实例布局 |
|---|---|---|---|
| `false, false, false` | 227B | 4 | DS5=0 |
| `false, true, false` | 252B | 5 | DS5=0，鼠标=1 |
| `true, false, false` | 252B | 5 | DS5=0，键盘=1 |
| `true, true, false` | 277B | 6 | DS5=0，鼠标=1，键盘=2 |
| `false, false, true` | 259B | 5 | DS5=0，IO=1 |
| `false, true, true` | 284B | 6 | DS5=0，鼠标=1，IO=2 |
| `true, false, true` | 284B | 6 | DS5=0，键盘=1，IO=2 |
| `true, true, true` | 309B | 7 | DS5=0，鼠标=1，键盘=2，IO=3 |

音频占接口 0..2，DS5 固定为接口 3、IN `0x84` / OUT `0x03`。
鼠标启用时为接口 4、IN `0x86`、MPS 8B；键盘为接口 4 或 5、IN `0x87`、MPS 16B；
IO 接口紧随键盘、IN `0x88` / OUT `0x08`、MPS 64B、1ms 间隔。
所有 HID IN 端点均为 1ms 间隔。接口号连续，端点互不冲突。

报告描述符长度分别为 DS5 321B、鼠标 59B、键盘 89B、IO 38B。
鼠标报文 6B、无 Report ID；键盘实际发送 ID 2、1B 修饰键 + 14B 位图，
总长度含 ID 为 16B。键盘描述符另有 ID 1 的 6KRO 与 LED output。
键鼠接口协议均为 None，不是 Boot 键鼠接口。

### IO 通讯接口（custom HID，协议 v1）

单接口双中断端点（IN `0x88` / OUT `0x08`）、vendor usage page `0xFF00`、
**无 IAD**。Report ID 1 = 设备→主机，Report ID 2 = 主机→设备；每报文
1B Report ID + ≤63B 协议块。分块格式：`[seq:1][flags:1][data:0..61]`，
flags START/END/RESET（START|END = 单块完整消息），START 块 data 头为
`[cmd:1][msg_id:2 LE][varint total_len]`。USB HID 链路可靠有序，故无逐包
CRC、无重传，流控靠端点 NAK 背压。

命令：`INFO`（主机查询，设备回文本：构建时间/运行时长/接口开关/丢包计数）、
`PRINT`（设备→主机日志，任意长度自动分块）、`RESET`（会话复位）。
Lua `print` 双发：UART 照旧先行，再非阻塞入 **4KB 跨核消息环**
（core1 生产 + core0 应答生产、core0 task 消费，striped 锁；环满整条丢弃
并计数），usb_io_task 从环就地分块发送 —— 生产者与主机均对分块无感。
主机侧用 `tools/webhid-test.html`（Chrome/Edge，WebHID，协议封装类
`PicoIO`）验证 INFO 对话与 print 日志流；连接过滤器
`{vendorId: 0x054c, usagePage: 0xFF00}`。

**无 IAD 是有意的**：Windows usbccgp 只要配置描述符里出现任何 IAD 就只用
IAD 分组、放弃 legacy 音频分组，未覆盖的多接口功能会被拆散 —— 带 IAD 的
CDC/NCM 类与 DS5 音频基线在 Windows 上互斥（见 `exp/cdc-serial` 分支的
探索记录）。HID 单接口不需要 IAD，音频不受影响。

### 传输抽象（proto 通道核心）

协议 v1 的命令层与传输解耦（`src/proto.c`）：通道实例 = 4KB 消息环 + TX
单飞分块 + RX 重组 + 分发，块格式全传输一致（`[flags][data≤61]`，seq 归
传输适配器）。两个平级适配器：

- **custom HID**（`usb_io.c`）：块 ↔ 满 63B 报文（seq 前缀 + 补零，短包
  在 Windows→Chrome 链路上不可靠）；USB 链路可靠，无 CRC
- **调试串口**（`uart_proto.c`，UART0 4Mbaud，GPIO2/3 + USB-TTL）：帧 =
  `0xC0 <SLIP 转义([seq][flags][data] + CRC16-CCITT)> 0xC0`（RFC 1055）。
  RX 为 DMA 环形接收（移植 pico-hid-mapper，零 RX 中断，溢出/错误守护 +
  50ms 半帧超时）；TX 持 debug 锁与日志文本共写 UART——日志为 ASCII，与
  帧混流互不干扰（解码器按 0xC0 重同步，垃圾帧被 CRC 淘汰）

路由：PRINT 广播到所有激活传输；请求应答只回请求方传输。网页
`tools/webhid-test.html` 的 `PicoIO` 消息层对两者无感，`Transport` 抽象
（HidTransport / SerialTransport）各自处理 seq 与帧封装；串口连接默认
4000000（适配器须支持，可改）。

## 主线默认行为

- 上电仅返回 flash 基线，键鼠与 IO 开关均为 false。
- 默认脚本累计 `tick(dt_us)` 约 5 秒后请求 `ability_enable(true, true, true)`。
  计时从 Lua tick 启动开始，不从主机首次挂载开始；主机晚连接时可能直接看到键鼠配置。
- `USB_KM_BOOT`、`USB_KM_HOT_TEST` 和首次挂载后 15 秒等待仅用于故障隔离，已删除。
- TinyUSB 默认 debug=0；保留 UART 生命周期与简短 KM 状态日志。
- 关闭键盘时清按键状态，关闭鼠标时清队列，Lua 请求关闭鼠标也会清按键掩码。
  未启用接口的输出不转发，切换期间输入不保证逐事件重放。

## 日志与排查

正常请求启用后应看到以下关键日志（启动时也有 `queues initialized`）：

```text
[DS5C][KM] reload: old(kbd=0 mouse=0 io=0) -> new(kbd=1 mouse=1 io=1), ...
[DS5C][KM] config desc: 309 B, 7 itf, mouse=1 kbd=1 io=1
[DS5C][KM] reconnect requested; waiting for host enumeration
[DS5C][USB] MOUNTED: ... hid(kbd=1 mouse=1)
```

主机事件还可能产生 SUSPEND、RESUME 或 UMOUNTED 日志，不以每次断开必须出现
UMOUNTED 为成功条件。若停在 config desc 后没有 reconnect，先检查队列临界区；
若有 reconnect 没有新 MOUNTED，再查看总线复位、控制请求、SET_CONFIGURATION
与类驱动打开流程。

```bash
cmake -S . -B build-debug -DTUSB_DEBUG_LEVEL=2
cmake --build build-debug -j8
```

日志走 UART0 GPIO2 TX / GPIO3 RX，默认 4000000 8N1；必要时加
`-DDEBUG_BAUDRATE=115200` 并同步串口设置。级别 2 会输出每个端点传输，
可能大量刷屏并影响时序。定位完毕用 `-DTUSB_DEBUG_LEVEL=0` 重新构建。

## 验证记录

本轮在 Pico 2 上由用户实测确认：

- 初始描述符直接含键鼠能够枚举，排除了“追加后描述符本身必然无法枚举”的判断。
- 基线 DS5 首次挂载后等待 15 秒，再放行 Lua 启用请求，热重载后工作正常。

上述第二项使用临时验证版，以确保先观察基线挂载再切换；主线恢复默认脚本的 5 秒计时。
清理后的主线分别做 TinyUSB debug=2 与 debug=0 编译验证，未新增硬件实测。
反复开关、单独键盘/鼠标与跨操作系统兼容性尚未逐项实测。
