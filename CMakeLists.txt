cmake_minimum_required(VERSION 3.13)

# Target board: Raspberry Pi Pico 2 W (RP2350 + CYW43439 Wi-Fi/BT).
# Use "pico2" instead if you're building for a plain Pico 2 without wireless
# (and drop the WiFi/Telnet sources + pico_cyw43_arch_lwip_* link below).
set(PICO_BOARD pico2_w CACHE STRING "Board type")

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
)

target_link_libraries(retro_pico2350
    pico_stdlib
    hardware_dma
    hardware_pio
    hardware_irq
    hardware_clocks
    hardware_spi
    pico_cyw43_arch_lwip_threadsafe_background
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

# The pico-sdk default core0 stack is only 2KB (PICO_STACK_SIZE). That's
# tight even without WiFi; combined with the cyw43/lwIP background IRQ
# (threadsafe_background) calling back into lwIP/DHCP/TCP processing on top
# of whatever our own BASIC interpreter's call depth happens to be, it can
# overflow and hard-fault a few seconds into a WiFi session.
#
# NOTE: core0's stack is hard-linked into the SCRATCH_Y memory bank, which
# is a separate, fixed 4KB region on RP2350 (0x20081000, length 4k) - NOT
# part of the main 512KB SRAM pool. 0x1000 (4096 bytes) is therefore the
# maximum PICO_STACK_SIZE that fits without a custom linker script; going
# any higher fails to link ("stack doesn't fit"). If 4KB still isn't enough,
# the next step is a linker-script override that relocates .stack into the
# main RAM region instead (there is plenty of room there), but that needs
# care to not overlap the heap, so it's intentionally not done speculatively.
target_compile_definitions(retro_pico2350 PRIVATE
    PICO_STACK_SIZE=0x1000
)

# USB stdio off (we drive our own PS/2 keyboard), UART stdio off by default too.
# Enable UART stdio for debug printf over the Pico's UART0 (GP0/GP1).
pico_enable_stdio_usb(retro_pico2350 0)
pico_enable_stdio_uart(retro_pico2350 1)

pico_add_extra_outputs(retro_pico2350)
