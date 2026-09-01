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

add_subdirectory(lib/pico_pio_usb)
add_subdirectory(src)
