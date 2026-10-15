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

# PIO-USB root-port hotplug recovery is validated against the pre-0.6.0
# receive implementation. The patch script pins that baseline and applies
# the host stall recovery plus Pico SDK 2 compatibility fixes.
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/lib/pico_pio_usb/src/pio_usb.h")
    message(FATAL_ERROR "lib/pico_pio_usb is missing; run ./scripts/setup-sdk.sh")
endif()
file(READ "${CMAKE_CURRENT_LIST_DIR}/lib/pico_pio_usb/src/pio_usb.h" PIO_USB_HEADER)
string(FIND "${PIO_USB_HEADER}" "pio_usb_host_task" PIO_USB_LEGACY_API)
if(PIO_USB_LEGACY_API EQUAL -1)
    message(FATAL_ERROR "PIO-USB must use the pre-0.6.0 hotplug baseline; run ./scripts/apply-patches.sh")
endif()
file(READ "${CMAKE_CURRENT_LIST_DIR}/lib/pico_pio_usb/src/pio_usb_host.c" PIO_USB_HOST_SOURCE)
string(FIND "${PIO_USB_HOST_SOURCE}" "pio_usb_host_stall_pending" PIO_USB_STALL_RECOVERY)
if(PIO_USB_STALL_RECOVERY EQUAL -1)
    message(FATAL_ERROR "PIO-USB host recovery patch is missing; run ./scripts/apply-patches.sh")
endif()

# TinyUSB 0.18 can permanently stop hub change polling after one failed
# transaction. Carry the upstream 0.19 retry behavior in a build copy so the
# pinned SDK source remains untouched.
set(_hub_source "${PICO_TINYUSB_PATH}/src/host/hub.c")
if(EXISTS "${_hub_source}")
    file(READ "${_hub_source}" _hub_text)
    string(FIND "${_hub_text}" "TU_VERIFY(result == XFER_RESULT_SUCCESS);" _hub_failure_check)
    if(NOT _hub_failure_check EQUAL -1)
        string(REPLACE
            "  TU_VERIFY(result == XFER_RESULT_SUCCESS);"
            "  if (result != XFER_RESULT_SUCCESS) {\n    hub_edpt_status_xfer(daddr);\n    return true;\n  }"
            _hub_text "${_hub_text}")
        string(REPLACE
            "  TU_ASSERT(xfer->result == XFER_RESULT_SUCCESS, );"
            "  if (xfer->result != XFER_RESULT_SUCCESS) {\n    hub_edpt_status_xfer(xfer->daddr);\n    return;\n  }"
            _hub_text "${_hub_text}")

        set(_hub_patched "${CMAKE_BINARY_DIR}/tusb_patch/hub.c")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tusb_patch")
        file(WRITE "${_hub_patched}" "${_hub_text}")
        get_target_property(_tuh_sources tinyusb_host_base INTERFACE_SOURCES)
        set(_tuh_sources_patched "")
        set(_hub_replaced FALSE)
        foreach(_source IN LISTS _tuh_sources)
            if(_source MATCHES "src/host/hub\\.c$")
                list(APPEND _tuh_sources_patched "${_hub_patched}")
                set(_hub_replaced TRUE)
            else()
                list(APPEND _tuh_sources_patched "${_source}")
            endif()
        endforeach()
        if(_hub_replaced)
            set_target_properties(tinyusb_host_base PROPERTIES INTERFACE_SOURCES "${_tuh_sources_patched}")
            target_include_directories(tinyusb_host_base INTERFACE "${PICO_TINYUSB_PATH}/src/host")
            message(STATUS "TinyUSB hub failure retry patch applied")
        else()
            message(FATAL_ERROR "TinyUSB hub.c was found but not in tinyusb_host_base sources")
        endif()
    else()
        message(STATUS "TinyUSB hub driver already includes failure retries")
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
