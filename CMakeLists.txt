cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico_w CACHE STRING "Target board")
set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/external/pico-sdk)

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(alien_macropad_firmware C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

pico_sdk_init()

set(FIRMWARE_TARGET alien_macropad_firmware)

add_executable(${FIRMWARE_TARGET}
    ${PICO_SDK_PATH}/lib/tinyusb/lib/networking/dhserver.c
    src/app_config.c
    src/button.c
    src/main.c
    src/network.c
    src/rest_client.c
)

set_target_properties(${FIRMWARE_TARGET} PROPERTIES
    LINKER_LANGUAGE C
)

target_include_directories(${FIRMWARE_TARGET} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${PICO_SDK_PATH}/lib/tinyusb/lib/networking
)

target_compile_definitions(${FIRMWARE_TARGET} PRIVATE
    CYW43_DEFAULT_IP_AP_ADDRESS=0xAC100401
    CYW43_DEFAULT_IP_AP_GATEWAY=0xAC100401
    CYW43_DEFAULT_IP_MASK=0xFFFFFF00
    CYW43_DEFAULT_IP_DNS=0x00000000
    PICO_FLASH_ASSUME_CORE1_SAFE=1
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
    PICO_DEFAULT_UART_BAUD_RATE=115200
    # This firmware is C-only. pico_runtime still pulls in pico_cxx_options;
    # disable the SDK new/delete overrides so CI does not require Arm libstdc++.
    PICO_CXX_DISABLE_ALLOCATION_OVERRIDES=1
)

target_link_libraries(${FIRMWARE_TARGET}
    hardware_flash
    pico_flash
    pico_lwip_mdns
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
)

pico_enable_stdio_usb(${FIRMWARE_TARGET} 1)
pico_enable_stdio_uart(${FIRMWARE_TARGET} 1)
pico_add_extra_outputs(${FIRMWARE_TARGET})
