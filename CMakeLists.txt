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
    ${PICO_SDK_PATH}/lib/tinyusb/lib/networking/dhserver.c
    src/app_config.c
    src/button.c
    src/main.c
    src/network.c
    src/rest_client.c
)

target_include_directories(macropad_3 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${PICO_SDK_PATH}/lib/tinyusb/lib/networking
)

target_compile_definitions(macropad_3 PRIVATE
    CYW43_DEFAULT_IP_AP_ADDRESS=0xAC100401
    CYW43_DEFAULT_IP_AP_GATEWAY=0xAC100401
    CYW43_DEFAULT_IP_MASK=0xFFFFFF00
    CYW43_DEFAULT_IP_DNS=0x00000000
    PICO_FLASH_ASSUME_CORE1_SAFE=1
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
    PICO_DEFAULT_UART_BAUD_RATE=115200
)

target_link_libraries(macropad_3
    hardware_flash
    pico_flash
    pico_lwip_mdns
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
)

pico_enable_stdio_usb(macropad_3 1)
pico_enable_stdio_uart(macropad_3 1)
pico_add_extra_outputs(macropad_3)
