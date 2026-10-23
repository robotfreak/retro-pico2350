cmake_minimum_required(VERSION 3.13)

# Audio controller: Pico 2 + Pimoroni Pico Audio Pack, I2C slave of the main
# controller (see ../src/proto/audio_proto.h). Needs pico-extras.
set(PICO_BOARD pico2 CACHE STRING "Board type")
if(NOT PICO_EXTRAS_PATH AND DEFINED ENV{PICO_EXTRAS_PATH})
    set(PICO_EXTRAS_PATH $ENV{PICO_EXTRAS_PATH})
endif()
if(NOT PICO_EXTRAS_PATH)
    set(PICO_EXTRAS_PATH /home/peter/work/pico/pico-extras)
endif()
set(PICO_SDK_POST_LIST_DIRS ${PICO_EXTRAS_PATH})

include(${CMAKE_CURRENT_LIST_DIR}/../pico_sdk_import.cmake)
project(retro_audio C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

# synth + I2S buffer helper are shared with the audiotest demo
set(SYNTH_DIR ${CMAKE_CURRENT_LIST_DIR}/../audiotest)

add_executable(retro_audio main.cpp ${SYNTH_DIR}/synth.cpp)
target_include_directories(retro_audio PRIVATE ${SYNTH_DIR} ${CMAKE_CURRENT_LIST_DIR}/../src/proto)
target_link_libraries(retro_audio pico_stdlib pico_audio_i2s pico_i2c_slave hardware_i2c)
target_compile_definitions(retro_audio PRIVATE PICO_AUDIO_I2S_MONO_INPUT=1)
pico_enable_stdio_usb(retro_audio 0)
pico_enable_stdio_uart(retro_audio 1)
pico_add_extra_outputs(retro_audio)
