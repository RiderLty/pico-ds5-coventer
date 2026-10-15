set(target_name pico-ds5-coventer)
add_executable(${target_name})

target_sources(${target_name} PRIVATE
 main.c
 usb_descriptors.c
 usb_km.c
 usb_io.c
 uart_proto.c
 proto.c
 kbd_hid.c
 kbd_out.c
 mouse_hid.c
 mouse_queue.c
 input_app.c
 debug.c
 input_queue.c
 ds5_pad.c
 audio_stub.c
 lua_core.c
 lua_binder.c
 lua_store.c
 lua_script.c
 ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/dcd_pio_usb.c
 ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
 )

# ── 预制 Lua 脚本自动嵌入（防脚本/固件不同步）────────────────────────────
# configure 期比对时间戳：default.lua 更新则重新生成 lua_default_script.h
set(LUA_SCRIPT_SRC "${CMAKE_CURRENT_LIST_DIR}/../scripts/default.lua")
set(LUA_SCRIPT_HDR "${CMAKE_CURRENT_LIST_DIR}/lua_default_script.h")
add_custom_command(
  OUTPUT ${LUA_SCRIPT_HDR}
  COMMAND python3 ${CMAKE_CURRENT_LIST_DIR}/../tools/embed_lua.py
          ${LUA_SCRIPT_SRC} ${LUA_SCRIPT_HDR}
  DEPENDS ${LUA_SCRIPT_SRC}
  COMMENT "embed_lua: ${LUA_SCRIPT_SRC} -> lua_default_script.h"
)
add_custom_target(embed_lua_script DEPENDS ${LUA_SCRIPT_HDR})
add_dependencies(${target_name} embed_lua_script)

# print memory usage, enable all warnings
target_link_options(${target_name} PRIVATE -Xlinker --print-memory-usage)
target_compile_options(${target_name} PRIVATE -Wall -Wextra)

# use tinyusb implementation
target_compile_definitions(${target_name} PRIVATE PIO_USB_USE_TINYUSB)

# Pico SDK 的 TinyUSB interface 默认会注入 CFG_TUSB_DEBUG=0；用编译选项先取消
# 再重定义，避免同一命令行出现两个 -D 导致大量 redefined warning。输出函数由
# tusb_config.h 绑定到 debug.c 的 tu_printf()，走现有 UART。
set(TUSB_DEBUG_LEVEL 0 CACHE STRING "TinyUSB debug log level (0..3)")
target_compile_options(${target_name} PRIVATE
  -UCFG_TUSB_DEBUG
  -DCFG_TUSB_DEBUG=${TUSB_DEBUG_LEVEL}
)

# Silence recovery threshold inherited from pico-hid-mapper's host fix.
set(PIO_USB_STALL_RECOVER_US 2500 CACHE STRING "PIO-USB host silence recovery threshold (us), 0 disables")
target_compile_definitions(${target_name} PRIVATE
  PIO_USB_STALL_RECOVER_US=${PIO_USB_STALL_RECOVER_US}
)

# ── hidkit 解析库（INTERFACE 库，源码编进本 target）────────────────────────
# 容量：转换器场景单槽位足够；HIDKIT_HOT 把库内每报文热路径放进 RAM ——
# 宿主侧 hidkit_input_* 出口已在 input_app.c 同样标注，整条输入链才真正不停摆
target_compile_definitions(${target_name} PRIVATE
  HIDKIT_MAX_KEYBOARDS=2
  HIDKIT_MAX_MICE=2
  HIDKIT_MAX_GAMEPADS=2
  HIDKIT_MAX_MOUSE_BUTTONS=8   # 描述符解析上限本就是 8（uint8_t 位掩码宽度），boot 固定格式默认 5
  HIDKIT_ENABLE_ADAPTER_AZERON=0
  # HIDKIT_HOT 在 src/hidkit_user_config.h 里定义 —— 带括号的宏定义会被
  # target_compile_definitions 静默丢弃，必须走官方 HIDKIT_USER_CONFIG 扩展点
  HIDKIT_USER_CONFIG=\"hidkit_user_config.h\"
)

# needed so tinyusb can find tusb_config.h
target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(${target_name} PRIVATE
 pico_stdlib
 pico_pio_usb
 tinyusb_device
 tinyusb_host
 pico_time
 hardware_uart
 hardware_gpio
 hidkit
 hidkit_tusb_xinput
 hidkit_tusb_switch_pro
 lua
 kvstore
)

# Tail 512 KiB belongs exclusively to KV. Fail the link before firmware can
# overlap it, while retaining the board's actual PICO_FLASH_SIZE_BYTES.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/kv_reserve.ld
  "ASSERT(__flash_binary_end <= ORIGIN(FLASH) + LENGTH(FLASH) - 524288, \"firmware overlaps tail KV partition\")\n")
target_link_options(${target_name} PRIVATE "LINKER:-T,${CMAKE_CURRENT_BINARY_DIR}/kv_reserve.ld")
pico_add_link_depend(${target_name} ${CMAKE_CURRENT_BINARY_DIR}/kv_reserve.ld)

# 可选：调试串口波特率覆盖（默认 4000000，须适配器支持；可 -DDEBUG_BAUDRATE=115200 降速）
set(DEBUG_BAUDRATE "" CACHE STRING "Debug UART baudrate override (default 4000000)")
if(DEBUG_BAUDRATE)
  target_compile_definitions(${target_name} PRIVATE DEBUG_BAUDRATE=${DEBUG_BAUDRATE})
endif()

pico_add_extra_outputs(${target_name})
