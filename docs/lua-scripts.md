# Lua 脚本：RAM 运行与 KV 保存

脚本源码上限为 **60 KiB（61440 字节，按 UTF-8 编码计算）**，KV 独占 Flash 尾部
**512 KiB**。Pico 2 的 4 MiB Flash 上偏移为 `0x380000..0x3fffff`；使用板级
`PICO_FLASH_SIZE_BYTES` 计算尾部地址，链接时检查固件不越入分区。

这里的 KV 是 pico-kvstore logkvs，底层 Flash blockdevice 使用 SDK 的
`flash_safe_execute`。应用只通过 KV 读写 `lua.script`，不直接擦写脚本地址。
KV 没有独立的固定 value 长度上限，受 bank 可用空间限制：分区分为两个 256 KiB bank，
每条记录还包含头、key、CRC 和对齐开销。更新需同时容纳旧、新记录；60 KiB 在当前
配置下可重复保存。小记录追加写入，同一已擦除扇区内的多次保存不必每次擦除；跨入
新扇区或 GC 时仍会擦除。512 KiB 是日志空间，当前 API 管理一个脚本，不是脚本文件列表。

## 生命周期

- 启动先读 KV，把源码复制到 RAM 后创建 VM。KV 不存在、不可读、源码超长，或脚本
  编译 / 顶层执行失败时，复制固件内的 `scripts/default.lua` 到 RAM 并运行。
- USB Device 等待启动脚本顶层全部执行完成，再应用最终 `dev_enable` 目标并连接主机。
  多次调用合并为首次枚举配置，无需断开重连；回退默认脚本前清除失败脚本的部分能力请求。
  此时尚不执行 Host 事件和 `tick`，两核等待时保留 IRQ 以支持 KV Flash 安全操作。
- BEGIN / WRITE 只上传到临时 RAM，不影响正在运行的脚本，不写 Flash。
- RUN 验证长度和 CRC 后重建 VM，运行上传源码，成功后把它作为当前 RAM 镜像。
  语法错误、顶层运行错误或顶层执行超时会恢复预制脚本；KV 中旧脚本保留。
- SAVE 保存**当前运行的 RAM 镜像**。浏览器编辑框里尚未上传运行的文字不会被保存。
- RELOAD_SAVED 丢弃未保存内容并重新从 KV 加载运行；失败同样恢复预制脚本。
- DEFAULT 在 RAM 中运行预制脚本。DELETE 只删除 KV，不切换当前运行内容。
- VM 切换前释放键盘和鼠标输出。脚本事件回调继续使用原有约 10ms 看门狗；回调错误
  走日志，不触发自动恢复（自动恢复针对加载 / 顶层执行失败）。

只有 SAVE、DELETE 以及空白 KV 首次初始化 / 后续 GC 会引起存储写入。
Flash 写入期间双核和 Host IRQ 会短暂停顿，PIO-USB 总线自愈逻辑负责恢复；仍需板上
验证保存过程中接入键鼠 / Hub 的行为。

## 前端 API 与验证页

打开 `tools/webhid-test.html`，选择 WebUSB 或 Web Serial 后使用 Lua 文本编辑框。
操作按钮支持读取、上传运行、显式保存、加载 KV、恢复预制、删除 KV 和查询状态。

```js
const source = await device.scriptRead();       // 当前 RAM 源码，返回 string
await device.scriptRun(source);                // 上传 + 在 RAM 中运行，不保存
await device.scriptSave();                     // 确认后持久化
await device.scriptReload();                   // 从 KV 重新运行
await device.scriptDefault();                  // 运行预制脚本，不修改 KV
await device.scriptDelete();                   // 删除 KV，不切换 RAM
console.log(await device.scriptStatus());
console.log(await device.scriptError());
```

各方法支持 `{ timeoutMs, signal }`，默认每条脚本请求超时 30 秒。同一 PicoDevice 的
脚本操作串行，分块事务绑定当前连接，断线重连后旧事务不能继续。两种端口支持同一
协议。WinUSB IO 未启用时，可在运行中按下再松开 BOOT 启用；若自定义脚本拦截了该默认行为，
可通过 UART 操作，或让脚本调用 `dev_enable(-1, -1, 1)` 启用 IO（`-1` = 保持其余接口不变）。
运行中改变接口布局也可能断开 WebUSB。

## 首次枚举与运行期接口启用

希望插上主机就包含键盘、鼠标和 WinUSB，可在脚本顶层直接写：

```lua
dev_enable(1, 1, 1)
```

通过网页上传运行并保存到设备后，重新上电即可。第一次枚举直接使用该配置，不会先以
DS5 基线枚举再断开。运行中上传脚本仍按运行期处理，改变配置会重枚举；只有保存后
下次上电才影响下一次首次枚举。不需要添加固定启动延时。

顶层 `dev_enable` 只是提交目标，顶层全部结束后才应用；`*_is_on()` 此时仍是已应用状态，
不要在顶层循环等待其变化。`tick`、输入回调和 BOOT 回调在 USB 启动后执行，所产生的
配置变更仍走热重载。相同目标不会重枚举。

## BOOT 按键与接口启用

预制脚本保留了“启动约 5 秒后启用接口”的完整代码，但已整段注释，默认不会运行。
该示例累计 `tick(dt_us)` 时间后调用 `dev_enable(1, 1, 1)`，同时开启键盘、鼠标和 WinUSB，
这是运行期延时示例；计时从脚本 tick 开始，不从主机首次挂载开始。启用会重枚举 USB，断开等待约 0.5 秒，
期间 DS5 上报暂停。如需恢复，取消脚本文末整个代码块的注释。

默认只需在设备运行中按下再松开 BOOT（BOOTSEL），即可启用 WinUSB。
板载 `on_boot_button(pressed)` 回调先于默认行为执行，按下为 `true`，松开为 `false`。
返回 `true` 会拦截本次事件的默认行为；返回 `false`、`nil`、不返回值或未定义时，
松开按键会请求 `dev_enable(-1, -1, 1)`，键盘和鼠标设置保持不变。
仅在按下事件返回 `true` 不会拦截之后的松开事件；要自定义接口，松开时也必须返回 `true`。

例如，将默认的 `on_boot_button` 替换为以下回调，松开时开启键盘、保持鼠标不变，并关闭 WinUSB：

```lua
function on_boot_button(pressed)
    if not pressed then
        dev_enable(1, -1, 0)
    end
    return true
end
```

三个参数可按需改为 `-1`（保持）、`0`（关闭）、`1`（开启）。返回 `true` 只拦截默认动作，
不会自动启用任何接口；由脚本的 `dev_enable` 决定。此例关闭 WinUSB，后续连接可改用 UART。
目标接口与当前状态相同时不会重枚举。更新预制脚本不会覆盖 KV 中已有的用户脚本；
要使用新版预制脚本，需恢复预制并加载到内存，需断电保留时再写入存储。

## 通信协议

命令 `SCRIPT=0x20`，普通消息 body 最多 512B；请求首字节为操作码。
所有整数为 little endian。成功回复同命令、同 message ID；未指定的成功 body 为空。

| 操作 | 请求 body | 成功回复 |
| --- | --- | --- |
| STATUS `0` | `[0]` | UTF-8 JSON，字段见下表 |
| BEGIN `1` | `[1][size:u32][crc32:u32]` | 分配 1..61440B 上传缓冲；同会话重新 BEGIN 替换未完成上传 |
| WRITE `2` | `[2][offset:u32][data:1..507B]` | offset 必须等于已接收字节数 |
| RUN `3` | `[3]` | 长度 / CRC 校验通过后运行 RAM 脚本 |
| SAVE `4` | `[4]` | 保存当前运行镜像到 KV |
| READ `5` | `[5][offset:u32][count:u16]` | 当前 RAM 原始字节，count 为 0..512 |
| DEFAULT `6` | `[6]` | 运行预制脚本，丢弃临时上传 |
| DELETE `7` | `[7]` | 删除 KV；不存在也成功 |
| ABORT `8` | `[8]` | 丢弃当前会话的临时上传 |
| RELOAD_SAVED `9` | `[9]` | 丢弃临时上传，从 KV 加载运行 |
| ERROR `10` | `[10]` | 最近一次操作错误的 UTF-8 文本，最多 159B；无错误时为空 |
| AI_STATUS `11` | `[11]` | KV `ai_cfg` 的 JSON 元信息：size / crc32 / maxSize（4096）；未保存时 size=0 |
| AI_READ `12` | `[12][offset:u32][count:u16]` | AI 配置原始字节，count 为 0..512 |
| AI_BEGIN `13` | `[13][size:u32][crc32:u32]` | 分配 1..4096B AI 上传缓冲，后续复用 WRITE `2` |
| AI_SAVE `14` | `[14]` | 完整长度和 CRC 通过后写入 `ai_cfg`，不执行 Lua |

CRC32 使用 IEEE / zlib（多项式 `0xedb88320`，初始值和最终异或 `0xffffffff`），覆盖
完整源码或 AI 配置 JSON，无结尾 NUL。上传 30 秒无活动、所属通道 RESET 或 USB 断开后临时镜像失效。
一次仅允许一个上传方；其他通道可查询 STATUS / READ / ERROR，修改操作返回 BUSY。
AI 与脚本共用一个上传缓冲和同样的会话/超时限制，RUN 不能执行 AI 上传，AI_SAVE 不能保存脚本上传。
AI_READ 每块重新读取 KV，客户端对完整结果做 CRC 校验；读取期间变更须重试。
DELETE 仅删除 `lua.script`，AI 配置保持原样。所有 KV 和脚本操作在 core1 执行，回复交回 core0 发送；RESET 会丢弃尚未执行的旧会话操作和旧回复，
已经开始的操作不会回滚。

跨核 mailbox 用 acquire/release 原子状态交接数据所有权：core0 提交请求和发送回复，
core1 执行脚本/KV 操作。没有任务时只检查状态，不取锁、不关中断；回复出口暂忙时
保留当前回复，期间新请求仍返回 BUSY。

| STATUS 字段 | 含义 |
| --- | --- |
| `maxSize` / `partitionSize` | 61440 / 524288 字节 |
| `size` / `crc32` | 当前 RAM 源码长度和 CRC |
| `saved` | 最近成功读取或保存过 KV 源码，且此后未删除；不是“当前源码已保存”的标志 |
| `dirty` | 当前 RAM 镜像尚未通过 SAVE / 成功 RELOAD 与 KV 同步；RUN / DEFAULT / 回退均置 true |
| `default` | 当前镜像来自预制脚本 |
| `kvReady` | KV 初始化是否成功 |
| `uploadSize` / `received` | 临时上传声明长度 / 已接收长度，无上传时为 0 |

`saved=true, dirty=true, default=true` 可表示 KV 有脚本但加载失败，已恢复预制。
同一份源码重复 RUN 仍会置 dirty；它是操作状态，不是逐字节相等比较。

错误回复 `ERROR=0x7f`：`[0x20][errorCode][optional UTF-8 detail]`。
沿用 `1=未知操作, 2=长度/偏移错误, 3=CRC错误, 4=忙`，新增 `5=脚本操作失败,
6=KV操作失败`。上传分配失败或长度 / CRC 校验失败时当前 VM 保留；RUN 的 Lua 加载失败
才切换预制脚本。浏览器 `PicoDeviceError.detail` 保留诊断文字。

## 验证

```bash
python3 tools/test_lua_scripts.py
python3 tools/test_core_queues.py
python3 tools/test_ds5_touch.py
node tools/test_pico_device.mjs
node tools/test_web_protocol.mjs
python3 tools/test_uart_protocol.py
python3 tools/test_usb_descriptors.py
cmake --build build -j8
```

主机测试使用真实 Lua、脚本状态机、logkvs 和模拟 NOR Flash，覆盖 RAM-only 运行、
保存 / 删除 / 回退、60 KiB、CRC、GC、记录写入中断、会话隔离和无限循环。
浏览器模拟测试覆盖 UTF-8、分块上传 / 读取、操作顺序和连接关闭。

板上验证顺序：首次无 KV 启动 → 上传运行但不保存 → 断电确认恢复原脚本 → 上传并保存
→ 断电确认自动加载 → 60 KiB 上传保存 → 错误源码回退 → 删除 KV 后重启。WebUSB 与
UART 各跑一遍，并在保存时保持 Host 端设备连接。主机测试不替代这些硬件验证。
