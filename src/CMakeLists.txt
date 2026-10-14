set(target_name pico-ds5-coventer)
add_executable(${target_name})

target_sources(${target_name} PRIVATE
 main.c
 usb_descriptors.c
 usb_km.c
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
 ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/dcd_pio_usb.c
 ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
 )

# print memory usage, enable all warnings
target_link_options(${target_name} PRIVATE -Xlinker --print-memory-usage)
target_compile_options(${target_name} PRIVATE -Wall -Wextra)

# use tinyusb implementation
target_compile_definitions(${target_name} PRIVATE PIO_USB_USE_TINYUSB)

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
)

# 可选：调试串口波特率覆盖（默认 4000000，须适配器支持；可 -DDEBUG_BAUDRATE=115200 降速）
set(DEBUG_BAUDRATE "" CACHE STRING "Debug UART baudrate override (default 4000000)")
if(DEBUG_BAUDRATE)
  target_compile_definitions(${target_name} PRIVATE DEBUG_BAUDRATE=${DEBUG_BAUDRATE})
endif()

pico_add_extra_outputs(${target_name})
