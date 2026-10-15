add_library(kvstore INTERFACE)
target_sources(kvstore INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/kvstore.c
    ${CMAKE_CURRENT_LIST_DIR}/src/kvstore_logkvs.c
    ${CMAKE_CURRENT_LIST_DIR}/src/blockdevice_stage.c
    ${CMAKE_CURRENT_LIST_DIR}/src/crc32_ansi.c
    ${CMAKE_CURRENT_LIST_DIR}/src/flash.c)
target_include_directories(kvstore INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(kvstore INTERFACE pico_flash pico_multicore hardware_flash)
