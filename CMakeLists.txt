cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico_w CACHE STRING "Target board")
set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/external/pico-sdk)

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(macropad_3 C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

pico_sdk_init()

add_executable(macropad_3
    src/main.c
)

target_link_libraries(macropad_3
    pico_stdlib
    pico_cyw43_arch_none
)

pico_enable_stdio_usb(macropad_3 1)
pico_enable_stdio_uart(macropad_3 0)
pico_add_extra_outputs(macropad_3)
