cmake_minimum_required(VERSION 3.13)

# Backplane controller firmware: Raspberry Pi Pico 2 W doing WiFi + TCP/IP
# for the main Retro-Pico controller. Built separately from the main project:
#   cd backplane && mkdir build && cd build && cmake -G Ninja .. && ninja
set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(${CMAKE_CURRENT_LIST_DIR}/../pico_sdk_import.cmake)

project(retro_backplane C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(retro_backplane
    main.c
    ../src/proto/bp_link.c
)

target_include_directories(retro_backplane PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}        # lwipopts.h
    ${CMAKE_CURRENT_LIST_DIR}/../src/proto
)

target_link_libraries(retro_backplane
    pico_stdlib
    hardware_uart
    hardware_irq
    pico_cyw43_arch_lwip_threadsafe_background
)

# The UART is the link to the main controller; debug goes out over USB.
pico_enable_stdio_usb(retro_backplane 1)
pico_enable_stdio_uart(retro_backplane 0)

pico_add_extra_outputs(retro_backplane)
