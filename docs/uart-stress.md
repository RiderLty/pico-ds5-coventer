# 串口压测排查

UART0 默认 GPIO2 TX、GPIO3 RX、4000000 baud、8N1，无硬件流控。
USB-TTL 的 RX 接 GPIO2、TX 接 GPIO3，并共地，使用 3.3V 电平。
固件和网页必须同时更新；只改网页波特率不会改变固件配置。

## 已复现的软件问题

| 位置 | 原因和影响 | 修复 |
| --- | --- | --- |
| 网页 CRC32 | 用位运算读取尾部校验码得到有符号 int32，与无符号计算结果比较。最高位为 1 的正确 CRC 会被误判，随机数据约一半受影响 | 用 DataView 按小端读取 uint32 |
| 消息环发送 | tail 每发一块已前进，下一块读取时又加累计偏移，从第二块起跳读。长 PRINT、较长 INFO 会损坏，可能连带破坏后续队列 | 从当前 tail 读取，块发送成功才提交 |
| UART DMA 接收 | 4096B 环积压到 2048B 就丢弃；4M 8N1 下约 5.12ms 的调度延迟即可触发。只比较写地址也无法发现整圈覆盖 | 使用长周期 DMA 传输计数追踪生产进度，只在积压达到整环时丢弃并重新同步 |
| 网页请求 | 发送完才登记应答，快速回复会被漏接；并发 INFO/TEST 会交错消息块 | 先登记应答，再发送；按完整消息串行发送 |
| 流式回复 | TEST 与普通消息共用发送状态，会打断未完成的 PRINT；发送被 FIFO 拒绝时仍前进 CRC/随机数/偏移，重试会漏数据（主要影响 WinUSB） | 独立流式状态，先完成在途普通消息，拒收时回滚生成状态 |
| 收包恢复 | TEST 完成后残留普通 RX 状态；丢失 END 后把下一条 START 当旧数据；小于 CRC 长度的 TEST 可下溢 | 清理状态、允许新 START 恢复、检查和裁剪长度 |

网页分块大小也已统一为 62B（flags + 最多 61B data），并关闭逐块 console
日志，避免大包压测产生数万条日志拖慢处理。RESET 现在有确认回复，连接时由 PicoDevice 自动完成会话复位。

以上均有代码复现或回归检查，但尚未用用户的 USB-TTL 完成真机压测，不能据此
断言硬件链路已经稳定。

## 真机复测

1. 刷入 `build/src/pico-ds5-coventer.uf2`，等待约 10 秒，更新
   `tools/webhid-test.html` 和同目录 `pico-device.js`，在 Chrome/Edge 强制刷新（Ctrl+F5）。
2. 连接串口，波特率选 4000000；点击 INFO，保存完整结果。
3. 依次运行 513B、32K、64K、256K、512K、1M、2M、4M、8M。
   INFO 会排队等待压测完成，超时从出队开始计算。
4. 再查 INFO，对比前后计数增量。也可断开串口后用 WinUSB 重复压测作为对照；
   WinUSB 查询 INFO 也能读到 UART 计数。

| INFO 字段 | 增长时的含义 |
| --- | --- |
| `uart_ring_discard_bytes` | 固件来不及消费 DMA 环，整批丢弃的积压字节数 |
| `uart_errors` | 检测到 UART 帧错、校验错、break 或 FIFO overrun 的轮次，并非精确坏字节数 |
| `crc_errors` | 设备收到的 SLIP 帧 CRC16 不匹配 |
| `frame_errors` | 设备收到短帧、非法转义、超长帧或半帧超时 |
| `uart_seq_gaps` | CRC 合法帧的序号不连续事件；主机重连后序号从零开始也可能增长一次 |
| `dma_restarts` | 接收任务发现 DMA 异常停止并重装的次数，首次启动不计 |
| `log_drops` | 当前传输通道的消息环满、整条消息被丢弃的次数；压测回复期间日志积压也可导致增长 |

这些计数为设备启动后的累计值。设备计数主要观察主机到设备方向，设备到主机的
CRC16 错误会在浏览器控制台显示 `RX CRC 失败`。CRC16 只检错，没有重传；丢一帧
便可能导致整次压测校验失败或超时。

如果仍失败，保留失败原文、浏览器控制台日志、压测前后 INFO、USB-TTL 型号和
驱动版本，以及同样大小的 WinUSB 压测结果。4M 下无 RTS/CTS，仍需真机确认
适配器吞吐、接线及系统调度能否持续承载。

## 本地回归

```bash
cmake --build build -j8
node tools/test_web_protocol.mjs
node tools/test_pico_device.mjs
python3 tools/test_uart_protocol.py
python3 tools/test_usb_descriptors.py
```

网页测试直接加载实际 pico-device.js，在 Node 中运行 CRC、快速应答、消息串行化、
错误恢复和 32K 至 8M 的 SLIP 往返。API 测试验证公开接口、取消、超时、重连及浏览器传输资源清理。固件测试编译实际协议层及 UART 源码，
模拟 DMA/UART 寄存器，使用 ASan/UBSan 和独立 zlib CRC32 检查大包、发送拒收重试、
消息环回绕、收包恢复、DMA 积压/溢出/计数回绕/重启及诊断输出。
模拟测试不能验证真实 DMA 总线时序、Windows 串口驱动或 USB-TTL 电气行为。
