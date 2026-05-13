# SPDX-License-Identifier: Apache-2.0
#
# gd32-bridge firmware top-level CMake.
#
# Builds the Cortex-M33 firmware for the GD32G553MEY7TR on E1M-X V2N.
# This is a SEPARATE COMPILE ARTIFACT from the Zephyr-side alp-sdk
# library; the Zephyr build never descends here.  Invoke directly:
#
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/arm-none-eabi.cmake
#   cmake --build build
#
# The output binary lives at build/gd32-bridge.elf (+ .hex + .bin).
#
# Status (2026-05-12): the toolchain file + HAL stub + protocol layer
# compile standalone; the GigaDevice firmware library hook-up at
# `vendors/gd32_firmware_library/` is TBD.

cmake_minimum_required(VERSION 3.20)
project(gd32-bridge LANGUAGES C ASM)

# ----------------------------------------------------------------- #
# Backend selection                                                  #
# ----------------------------------------------------------------- #

# BRIDGE_HAL_BACKEND controls which bridge_hw_* implementation is
# linked.  Two modes:
#   * stub      -- builds against hal/bridge_hw_stub.c only; every
#                  HW-touching opcode reports BRIDGE_HW_ERR_NOTIMPL.
#                  Useful for protocol round-trip testing in
#                  hardware-less unit-test environments.
#   * gd32      -- builds against hal/bridge_hw_gd32.c; requires the
#                  GigaDevice firmware library (TBD location).
set(BRIDGE_HAL_BACKEND "stub" CACHE STRING "Bridge HAL backend (stub | gd32)")

# ----------------------------------------------------------------- #
# Source layout                                                      #
# ----------------------------------------------------------------- #

set(GD32_BRIDGE_SOURCES
    src/main.c
    src/protocol.c
    src/transport_spi.c
    src/transport_i2c.c
    src/bootloader/bootloader.c
)

if(BRIDGE_HAL_BACKEND STREQUAL "stub")
    list(APPEND GD32_BRIDGE_SOURCES hal/bridge_hw_stub.c)
elseif(BRIDGE_HAL_BACKEND STREQUAL "gd32")
    list(APPEND GD32_BRIDGE_SOURCES hal/bridge_hw_gd32.c)
    # Pull in the GigaDevice firmware-library wrapper.  Its CMakeLists
    # fatal-errors with a clear "drop the SDK into sdk/" instruction
    # when the unpacked archive is missing, so an unconfigured
    # workspace doesn't fall through to a confusing link error.
    set(_GD32_VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vendors/gd32_firmware_library)
    if(NOT EXISTS ${_GD32_VENDOR_DIR}/CMakeLists.txt)
        message(FATAL_ERROR
            "BRIDGE_HAL_BACKEND=gd32 selected but "
            "${_GD32_VENDOR_DIR}/CMakeLists.txt is missing.\n"
            "Check out the alp-sdk repo's vendors/gd32_firmware_library/ "
            "tree (the wrapper README documents the manual SDK drop).")
    endif()
    add_subdirectory(${_GD32_VENDOR_DIR} ${CMAKE_BINARY_DIR}/vendors/gd32_firmware_library)
else()
    message(FATAL_ERROR "Unknown BRIDGE_HAL_BACKEND='${BRIDGE_HAL_BACKEND}' (expected: stub | gd32).")
endif()

add_executable(gd32-bridge ${GD32_BRIDGE_SOURCES})

# Sanity-print: if the target didn't register (CI hit "No target
# gd32-bridge" during generation on 79b1ffc+127f18f), this prints
# `bridge target type: <empty>` and any subsequent target_* call
# below would also error.  Once the build is reliable on CI for a
# few commits this can come out.
get_target_property(_BRIDGE_TYPE gd32-bridge TYPE)
message(STATUS "bridge target type: ${_BRIDGE_TYPE}")
message(STATUS "bridge sources: ${GD32_BRIDGE_SOURCES}")

target_include_directories(gd32-bridge PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/hal
)

if(BRIDGE_HAL_BACKEND STREQUAL "gd32")
    target_link_libraries(gd32-bridge PRIVATE gd32_firmware_library)
endif()

# Cortex-M33 + thumb + soft-float (no FPU on GD32G5 baseline; revisit
# if the part variant has the FPU enabled and the linker / runtime
# wants to use it).
target_compile_options(gd32-bridge PRIVATE
    -mcpu=cortex-m33
    -mthumb
    -mfloat-abi=soft
    -ffunction-sections
    -fdata-sections
    -Wall -Wextra -Wshadow -Wpedantic
)
target_link_options(gd32-bridge PRIVATE
    -mcpu=cortex-m33
    -mthumb
    -mfloat-abi=soft
    -Wl,--gc-sections
    -nostartfiles
    -T${CMAKE_SOURCE_DIR}/toolchain/gd32g553_flash.ld
)

# Emit .hex + .bin for the maintainer's flasher of choice.  Guarded
# behind `if(TARGET ...)` because the previous unconditional form
# fatal-errored on CI with "No target gd32-bridge" -- whatever causes
# the target to not register reliably in the CI runner's CMake
# 3.31 + Generic-system toolchain combo, the build still finishes
# (we just skip the .hex/.bin emission).  HIL turn-on can carry the
# .elf without those; xPack OpenOCD + GDB accept .elf directly.
if(TARGET gd32-bridge)
    add_custom_command(TARGET gd32-bridge POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:gd32-bridge> $<TARGET_FILE:gd32-bridge>.hex
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:gd32-bridge> $<TARGET_FILE:gd32-bridge>.bin
        BYPRODUCTS $<TARGET_FILE:gd32-bridge>.hex $<TARGET_FILE:gd32-bridge>.bin
    )
else()
    message(WARNING "gd32-bridge target not registered -- skipping .hex/.bin POST_BUILD emission")
endif()

# ----------------------------------------------------------------- #
# Build-id stamping (truncated SHA-1 of the ELF).                    #
# ----------------------------------------------------------------- #

# TODO: post-link hook that computes sha1(.elf) and patches the
# linker-emitted build-id constant.  Until that lands the firmware
# reports the placeholder string '0000000000000000abcd' to keep the
# GET_BUILD_ID frame structurally valid.
