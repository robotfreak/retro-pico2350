cmake_minimum_required(VERSION 3.13)

# Standalone test for the Pimoroni Pico Audio Pack (I2S DAC, PCM5100A).
# Needs pico-extras: -DPICO_EXTRAS_PATH=... (or env PICO_EXTRAS_PATH).
set(PICO_BOARD pico2 CACHE STRING "Board type")
if(NOT PICO_EXTRAS_PATH AND DEFINED ENV{PICO_EXTRAS_PATH})
    set(PICO_EXTRAS_PATH $ENV{PICO_EXTRAS_PATH})
endif()
if(NOT PICO_EXTRAS_PATH)
    set(PICO_EXTRAS_PATH /home/peter/work/pico/pico-extras)
endif()
set(PICO_SDK_POST_LIST_DIRS ${PICO_EXTRAS_PATH})

include(pico_sdk_import.cmake)
project(audiotest C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(audiotest demo.cpp synth.cpp)
target_link_libraries(audiotest pico_stdlib pico_audio_i2s)
target_compile_definitions(audiotest PRIVATE
    PICO_AUDIO_I2S_MONO_INPUT=1
    USE_AUDIO_I2S=1
)
pico_enable_stdio_usb(audiotest 0)
pico_enable_stdio_uart(audiotest 1)
pico_add_extra_outputs(audiotest)
