# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Neil Rackett

# COMpad adapter firmware for the Pico W.
#
#   cmake -B build -S . && cmake --build build
#
# Produces build/compad.uf2. PICO_SDK_PATH and the Bluepad32 root both
# point at this repository's submodules, so a fresh clone needs nothing
# installed but CMake and arm-none-eabi-gcc.

cmake_minimum_required(VERSION 3.13)

# pico_w or pico2_w. Both have the CYW43 that Bluepad32 needs; a plain
# Pico has no radio and cannot run this at all.
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD "pico_w")
endif()

set(COMPAD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(BLUEPAD32_ROOT ${COMPAD_ROOT}/lib/bluepad32)

# The submodule, unless something usable is already pointed at. An
# explicit -DPICO_SDK_PATH always wins; an environment variable wins
# only if the directory it names actually exists, because a stale
# PICO_SDK_PATH left over from another project is common and the
# failure it causes here reads as a broken checkout.
if(NOT DEFINED PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH} AND IS_DIRECTORY $ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    else()
        if(DEFINED ENV{PICO_SDK_PATH})
            message(STATUS
                "Ignoring PICO_SDK_PATH=$ENV{PICO_SDK_PATH}: not a directory")
        endif()
        set(PICO_SDK_PATH ${COMPAD_ROOT}/lib/pico-sdk)
        set(ENV{PICO_SDK_PATH} ${PICO_SDK_PATH})
    endif()
endif()

if(NOT EXISTS ${PICO_SDK_PATH}/pico_sdk_init.cmake)
    message(FATAL_ERROR
        "No Pico SDK at ${PICO_SDK_PATH}. "
        "Run: git submodule update --init --recursive lib/pico-sdk")
endif()

# BTstack from the Pico SDK rather than from Bluepad32's own copy, which
# is what the upstream Pico W example does by default and needs no
# patching of the SDK.
set(BTSTACK_ROOT ${PICO_SDK_PATH}/lib/btstack)

include(pico_sdk_import.cmake)

project(compad C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(compad
    src/main.c
    src/compad_platform.c
)

target_include_directories(compad PRIVATE
    src
    ${BLUEPAD32_ROOT}/src/components/bluepad32/include
)

# Needed for btstack_config.h and sdkconfig.h to be found by libbluepad32
include_directories(src)
include_directories(${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/include)
include_directories(${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/include)
include_directories(${BTSTACK_ROOT}/src)

target_link_libraries(compad PUBLIC
    pico_stdlib
    pico_cyw43_arch_none
    pico_btstack_classic
    pico_btstack_cyw43
    bluepad32
)

add_subdirectory(${BLUEPAD32_ROOT}/src/components/bluepad32 libbluepad32)

# USB only, never UART. UART0 carries the wire, and one log line landing
# mid frame would corrupt it.
pico_enable_stdio_usb(compad 1)
pico_enable_stdio_uart(compad 0)

pico_add_extra_outputs(compad)
