# PicoDevice 网页通信 API

`PicoDevice.isWinUsbDevice(usbDevice)` 仅检查描述符，筛选本项目 VID/PID 且具有厂商类
bulk IN/OUT 的设备，不打开设备。连接后 `device.usbDevice` 返回底层 USBDevice（串口或
断开时为 null）。通过 `connect('winusb', { device: usbDevice })` 可连接已授权设备，
不弹选择框；未传设备时默认选择器按 VID/PID 和厂商类筛选。工作台另有自动发现、重连、
退避和手动暂停控制，详见 [WinUSB 重连](../frontend/README.md#winusb-自动重连)。

`tools/pico-device.js` 封装 WinUSB（WebUSB）和串口（Web Serial），不依赖第三方库。
测试页 `tools/webhid-test.html` 直接使用这套 API。Chrome/Edge 顶层安全页面可用：
localhost、HTTPS，或通过普通 `<script>` 从 file:// 打开；ES module 请通过 HTTP(S) 加载。

```html
<script src="pico-device.js"></script>
<script>
const device = new PicoDevice();
const stopLog = device.onLog((text) => console.log("Pico:", text));
device.onError((error) => console.error(error.code, error.message));

// 必须从用户点击事件直接调用 connect，浏览器才允许弹出选择框。
connectButton.onclick = async () => {
  try {
    await device.connect("winusb");
    console.log(await device.info());
    console.log(await device.test(513));
  } catch (error) {
    console.error(error);
  }
};
disconnectButton.onclick = () => device.disconnect();
</script>
```

ES module 入口：

```js
import { PicoDevice, PicoProtocol, PicoDeviceError } from "./tools/pico-device.mjs";

const device = new PicoDevice({ timeoutMs: 2000, maxQueue: 32 });
await device.connect("serial", { baudRate: 4000000 });
const body = await device.call(PicoProtocol.CMD.INFO);
console.log(new TextDecoder().decode(body));
await device.disconnect();
```

## 连接与状态

| API | 行为 |
| --- | --- |
| `connect("winusb", options)` | 选择 USB 设备，找到 vendor 类 bulk IN/OUT 接口，打开读取循环 |
| `connect("serial", options)` | 选择串口，默认 4000000 baud、8N1、无流控 |
| `disconnect()` | 立即拒绝在途及排队调用，释放读写锁并关闭设备；可重复调用 |
| `connected` | 是否可以发起调用 |
| `state` | `disconnected` / `connecting` / `connected` / `disconnecting` |
| `transportType` | `winusb` / `serial` / `null` |
| `PicoDevice.supported` | 当前浏览器是否提供 `{ winusb, serial }` API |

连接默认执行并等待 RESET，清理固件残留的 RX/TX 状态。RESET 重置通信会话，不重启板子。
`connect()` 失败会关闭已经打开的资源。此版本需配合有 RESET 确认和 ERROR 回复的固件。

WinUSB options 支持 `device`（已授权的 USBDevice）、`filters`（默认 vendorId 0x054c）、
`interfaceNumber` 和 `configurationValue`（默认 1）。串口支持 `port`（已授权的 SerialPort）、
`filters` 和 `baudRate`。传入已授权设备时不弹出选择框。
高级选项 `reset: false` 可跳过握手，但调用者需确保设备状态干净。

支持重连；监听器在同一个实例重连后保留。设备选择框不能由脚本主动关闭：选择框显示期间调用
`disconnect()`，随后即使用户选了设备也会立即关闭，原 `connect()` 拒绝；选择流程结束后才能再次连接。
不自动重试命令，避免重复执行有副作用的操作。

## 请求与发送

```js
const reply = await device.call(command, buffer, {
  timeoutMs: 3000,
  signal: abortController.signal,
});
```

`command` 是 0..255 整数，`buffer` 默认空字节，接受 ArrayBuffer、TypedArray 或 DataView。
`call()` 返回回复 body 的 Uint8Array，不含帧头。命令与内容明确分开，固件需实现该命令才能执行。
当前固件支持 INFO、RESET、TEST、SCRIPT；PRINT 是设备日志，ERROR 是设备拒绝请求的回复。

- 普通消息最大 512B；TEST 是 4B..8MiB，包含末尾 CRC32。入队时复制 body，后续修改原缓冲不会影响发送。
- `call()` 按「完整发送 + 收完回复」顺序执行；可同时发起多个 Promise 调用，但线上同一时刻执行一个请求。
  这与固件单个 RX 重组器和单个 TEST 回复状态匹配，避免交错或 BUSY。
- 默认最多容纳 32 个在途及排队操作，满时抛出 `QUEUE_FULL`，防止无限堆积。
- 超时从请求出队开始，包含发送及接收，不包括等待前一条请求的时间。
- ID 0 保留、65535 广播；1..65534 匹配请求回复。同一连接内已发出的 ID 不复用，耗尽需重连。
- `send(command, buffer, options)` 返回分配的 messageId，只等写入完成；设备回复通过 onMessage 观察，
  不负责判断设备是否接受命令。RESET/TEST 必须使用 call；SCRIPT 请使用下述脚本方法等待完成，防止交错或 BUSY。
- `__send_message()` 和 `__on_message()` 是内部扩展钩子，业务代码使用 call/send 和订阅方法。

```js
console.log(await device.info()); // UTF-8 文本
await device.reset();             // 等待会话复位确认，不重置主机编号
const result = await device.test(8 * 1024 * 1024);
console.log(result.size, result.durationMs, result.mibPerSecond);
console.log(result.uploadCrc, result.downloadCrc, result.body);
```

`test(size)` 填充随机数据、追加上行 CRC32，并验证回复长度和下行 CRC32；设备回复是新生成的随机数据，
不是上行内容的原样回显。默认超时随大小增长，可通过 options 覆盖。
`durationMs` 是调用耗时（含排队），吞吐为双向 MiB/s。size 包含 4B CRC trailer。

## Lua 脚本

编辑和运行只修改 RAM，`scriptSave()` 才写 KV。源码按 UTF-8 计，限制 1..61440 字节；
持久化分区为 Flash 尾部 512 KiB。启动无 KV 或加载失败时自动运行预制脚本。

| API | 返回与行为 |
| --- | --- |
| `scriptStatus(options)` | 状态对象：size/crc32、saved/dirty/default、kvReady、上传进度等 |
| `scriptRead(options)` | 当前 RAM 源码 string，分块读取后校验 CRC |
| `scriptRun(source, options)` | 上传并运行，返回状态对象；source 为 string 或字节缓冲，不保存 |
| `scriptSave(options)` | 保存当前运行源码到 KV，不读取浏览器编辑框 |
| `scriptReload(options)` | 从 KV 重新加载运行；加载失败恢复预制脚本 |
| `scriptDefault(options)` | 在 RAM 运行预制脚本，不修改 KV |
| `scriptDelete(options)` | 删除 KV 脚本，不切换当前运行内容 |
| `aiCfgGet(options)` | 读取设备 KV 的 AI JSON；从未保存返回 null；分块并校验 CRC |
| `aiCfgSet(config, options)` | JSON UTF-8 限制 1..4096B，上传并保存到 `ai_cfg`；成功返回 true，不修改 Lua |
| `scriptError(options)` | 最近操作错误 string；错误详情也在 PicoDeviceError.detail 中 |

```js
await device.scriptRun(editor.value);
// 验证运行正常后，由保存按钮单独调用：
await device.scriptSave();
```

脚本方法支持 timeoutMs/signal，默认每条请求 30 秒；同实例的脚本事务串行且绑定当前
连接。重连不会继续旧上传。RUN 的编译或顶层执行失败会恢复预制源码并拒绝 Promise，
可再次 `scriptRead()` 读取恢复后的内容。完整协议与状态含义见 [Lua 脚本说明](lua-scripts.md)。

WinUSB 固件每次写入协议帧后都会主动 flush TinyUSB 的发送 FIFO，短确认包与多包回复的
尾部无需等待后续日志凑满 64 字节。旧固件可能出现 Lua 已运行、页面仍等待“加载到内存”
确认的现象；需更新设备 UF2 才能修复，刷新网页不会改变设备的发送行为。

## 取消与错误

```js
const controller = new AbortController();
const task = device.info({ signal: controller.signal });
controller.abort();
try { await task; } catch (error) {
  if (error.name === "AbortError") console.log("已取消");
}
```

排队中的取消不影响连接。当前协议没有线上 CANCEL，取消在途操作、请求超时或传输/协议错误会关闭连接，
拒绝剩余调用，避免半条消息或仍在发送的 TEST 回复污染后续交互；需要重新 connect。
CRC16 坏帧会丢弃并触发 error 事件，如无法完成回复则按请求超时处理。
有效 ERROR 回复只拒绝对应调用，连接仍可用。参数错误不会发包。

| 错误 | 判别 |
| --- | --- |
| 设备拒绝 | `PicoDeviceError`，code=`DEVICE`，提供 command/deviceCode/messageId/detail；deviceCode 1/2/3/4/5/6 = 不支持/长度非法/CRC 失败/忙/脚本失败/KV 失败 |
| 超时 / 取消 | code=`TIMEOUT` / `ABORTED`（取消的 name 是 AbortError） |
| 未连接 / 状态冲突 | code=`DISCONNECTED` / `STATE` |
| 队列满 | code=`QUEUE_FULL` |
| 解析 / 链路 CRC | code=`PROTOCOL` / `FRAME_CRC` |
| USB 传输 / 接口 / 浏览器不支持 | code=`TRANSFER` / `INTERFACE` / `UNSUPPORTED` |
| API 参数错误 | TypeError / RangeError |

浏览器原始异常（如用户取消选择框、权限失败、拔线）也可能直接抛出 DOMException。

## 事件订阅

所有订阅返回 `unsubscribe()`，监听器异常会传给 error 事件，不打断读取循环或请求匹配。
异步监听器被调用但不等待，耗时业务应自行节流。

```js
const off = device.onLog((text, message) => console.log(text));
device.onMessage(({ command, body, messageId, broadcast, matched }) => {
  // broadcast: ID 为 65535；matched: 这条消息匹配了一个等待中的 call。
  // 包括请求回复、设备 ERROR、广播、send 的回复和未匹配/迟到消息。
});
device.onStateChange((state) => console.log(state));
device.onClose((error) => console.log(error ?? "主动断开"));
device.onError((error) => console.error(error));
device.onChunk(({ direction, bytes }) => { /* direction: rx / tx */ });
off();
```

`onLog` 仅分发 PRINT 广播；迟到回复不会被当成日志。
`on_log`、`on_message` 是对应方法的别名。也支持 `on("log" | "message" | "statechange" | "close" | "error" | "chunk", fn)`。
日志回调收到的是设备的一条 PRINT 消息文本，不保证恰好是一整行。
message 的 body 与 call 返回的字节共享引用，监听器应只读；需修改时先 `.slice()`。
`PicoProtocol` 暴露 CMD、ID_RESERVED、ID_BROADCAST、MAX_BODY、MAX_TEST 和 CRC 辅助函数。

## 回归

```bash
node tools/test_pico_device.mjs
node tools/test_web_protocol.mjs
python3 tools/test_uart_protocol.py
python3 tools/test_usb_descriptors.py
python3 tools/test_lua_scripts.py
cmake --build build -j8
```

公开 API 测试包含模拟浏览器的真实 WebUSB/Web Serial 适配器路径、资源释放、请求队列、取消、断线重连、
设备错误、监听器隔离、脚本 UTF-8/60 KiB/会话隔离及 4B..513B 的边界。协议测试覆盖 32KiB..8MiB 的真实串口编码/SLIP 往返。
模拟测试不代替 Windows 与板子的实机测试。
