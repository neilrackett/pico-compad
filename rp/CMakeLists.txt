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
# "Usable" means "contains pico_sdk_init.cmake", the same test the error
# below uses. Checking only that the directory exists would accept a
# stale path that still exists but is no longer an SDK, and then fail
# later with a message blaming the submodule for the environment.
if(NOT DEFINED PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH} AND
       EXISTS $ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    else()
        if(DEFINED ENV{PICO_SDK_PATH})
            message(WARNING
                "Ignoring PICO_SDK_PATH=$ENV{PICO_SDK_PATH}: no Pico SDK "
                "there. Using the submodule instead.")
        endif()
        set(PICO_SDK_PATH ${COMPAD_ROOT}/lib/pico-sdk)
        set(ENV{PICO_SDK_PATH} ${PICO_SDK_PATH})
    endif()
endif()

if(NOT EXISTS ${PICO_SDK_PATH}/pico_sdk_init.cmake)
    message(FATAL_ERROR
        "No Pico SDK at ${PICO_SDK_PATH}. If you passed -DPICO_SDK_PATH, "
        "check it. Otherwise run: "
        "git submodule update --init --recursive lib/pico-sdk")
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

# One version, from the same file the ST side reads, so the UF2 can say
# which build it is. CONFIGURE_DEPENDS is what stops a stale configure
# quietly shipping the old number after version.txt changes.
file(READ ${COMPAD_ROOT}/version.txt COMPAD_VERSION)
string(STRIP "${COMPAD_VERSION}" COMPAD_VERSION)
set_property(DIRECTORY APPEND PROPERTY
             CMAKE_CONFIGURE_DEPENDS ${COMPAD_ROOT}/version.txt)
target_compile_definitions(compad PRIVATE COMPAD_VERSION="${COMPAD_VERSION}")

# encode.h includes the real xpad, Bluepad32 and protocol headers rather
# than copying their constants. protocol.h is the shared wire contract
# that the ST target also compiles, not ST code: it is portable C.
target_include_directories(compad PRIVATE
    src
    ${BLUEPAD32_ROOT}/src/components/bluepad32/include
    ${COMPAD_ROOT}/lib/xpad/src
    ${COMPAD_ROOT}/target/atarist/src
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

# USB stdio installs a repeating alarm to pump TinyUSB, 1000 times a
# second by default. This device's actual job is a 50 Hz tick next to a
# latency-sensitive radio, so 20x that rate in timer interrupts buys
# nothing. 10 ms keeps the console usable for bring-up at a hundredth
# of the interrupt load.
target_compile_definitions(compad PRIVATE
    PICO_STDIO_USB_TASK_INTERVAL_US=10000
)

pico_add_extra_outputs(compad)
