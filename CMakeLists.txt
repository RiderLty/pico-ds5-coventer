set(PICO_BOARD pico2 CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)

# 使用项目内置的 SDK 副本（与 DS5Dongle CI 环境一致：pico-sdk 2.3.0 +
# tinyusb@2d56dc53），不依赖环境变量。在 pico_sdk_import.cmake 之前设置。
set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/lib/pico-sdk")

include(pico_sdk_import.cmake)
project(pico-ds5-coventer)

pico_sdk_init()

# 校验 tinyusb 是否已切到 DS5Dongle CI 指定的 commit（未切换则提示运行 setup 脚本）
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C ${PICO_SDK_PATH}/lib/tinyusb rev-parse HEAD
        OUTPUT_VARIABLE TINYUSB_HEAD
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT TINYUSB_HEAD STREQUAL "2d56dc533e45e4e91b15e93fdab5e22e964f328d")
        message(WARNING
            "lib/pico-sdk/lib/tinyusb 不在要求的 commit (2d56dc53，UAC1 支持)。\n"
            "请运行: ./scripts/setup-sdk.sh")
    endif()
endif()

# ── hidkit 解析库（HID/XInput 语义解析）与适配器：git 子模块 ──────────────
#   lib/hidkit                  —— 纯 C、零平台依赖的解析 core（不碰 USB 栈，只吃
#                                  描述符与报文，吐标准事件；src/input_app.c 是接线层）
#   lib/hidkit-tusb-xinput      —— XInput 类驱动 + 归一化接线（Xbox 没有 HID 接口，
#                                  走独立类驱动；驱动经 input_app.c 的
#                                  usbh_app_driver_get_cb 注册）
#   lib/hidkit-tusb-switch-pro  —— Switch Pro USB 握手（枚举后主机不握手完全静默，
#                                  手柄需子命令使能上报/读 SPI 校准）；发送走
#                                  tuh_hid_send_report（标准 HID host API，非类驱动）
# ── Lua 5.4.6（引擎脚本；vendored 拷贝自 pico-hid-mapper/lib/lua-5.4.6/src，
#    非 submodule —— 与 mapper 的管理方式一致）────────────────────────────
add_subdirectory(lib/lua-5.4.6/src)
add_subdirectory(lib/hidkit)
add_subdirectory(lib/hidkit-tusb-xinput)
add_subdirectory(lib/hidkit-tusb-switch-pro)
add_subdirectory(lib/pico_pio_usb)
add_subdirectory(src)
