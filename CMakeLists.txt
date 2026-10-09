cmake_minimum_required(VERSION 3.13)

# Main controller: plain Raspberry Pi Pico 2 (RP2350). WiFi/TCP lives on a
# separate Pico 2 W "backplane controller" (see backplane/), connected via UART.
set(PICO_BOARD pico2 CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(retro_pico2350 C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# SD card support (FatFs + SPI driver) is an external dependency - see
# README.md "SD card library" for how to fetch it into third_party/.
set(FATFS_SD_DIR ${CMAKE_CURRENT_LIST_DIR}/third_party/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)
if(NOT EXISTS ${FATFS_SD_DIR}/src/CMakeLists.txt)
    message(FATAL_ERROR
        "SD card library not found at ${FATFS_SD_DIR}.\n"
        "Fetch it first, e.g.:\n"
        "  git clone https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico.git ${FATFS_SD_DIR}\n"
        "See README.md for details.")
endif()
add_subdirectory(${FATFS_SD_DIR}/src fatfs_sd_build)

add_executable(retro_pico2350
    src/main.c
    src/video/dvi.c
    src/input/ps2kbd.c
    src/input/ps2mouse.c
    src/basic/basic.c
    src/net/net.c
    src/proto/bp_link.c
    src/storage/sdcard.c
    src/storage/hw_config.c
)

pico_generate_pio_header(retro_pico2350
    ${CMAKE_CURRENT_LIST_DIR}/src/input/ps2.pio
)

target_include_directories(retro_pico2350 PRIVATE
    src
    src/video
    src/input
    src/basic
    src/net
    src/storage
    src/proto
)

target_link_libraries(retro_pico2350
    pico_stdlib
    hardware_dma
    hardware_pio
    hardware_irq
    hardware_clocks
    hardware_spi
    hardware_uart
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

# The pico-sdk default core0 stack is only 2KB. core0's stack lives in the
# fixed 4KB SCRATCH_Y bank, so 0x1000 is the maximum without a custom linker
# script.
target_compile_definitions(retro_pico2350 PRIVATE
    PICO_STACK_SIZE=0x1000
)

# USB stdio off (we drive our own PS/2 keyboard), UART stdio off by default too.
# Enable UART stdio for debug printf over the Pico's UART0 (GP0/GP1).
pico_enable_stdio_usb(retro_pico2350 0)
pico_enable_stdio_uart(retro_pico2350 1)

pico_add_extra_outputs(retro_pico2350)
