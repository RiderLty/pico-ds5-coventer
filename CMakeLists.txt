set(PICO_BOARD pico2 CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(pico-ds5-coventer)

pico_sdk_init()

add_subdirectory(lib/pico_pio_usb)
add_subdirectory(src)
