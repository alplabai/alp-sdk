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
# Status: both backends build clean (arm-none-eabi-gcc 13.x).  The gd32
# backend links the GigaDevice firmware library at
# `vendors/gd32_firmware_library/` and drives the peripheral HAL plus the
# SPI1 + I2C0 slave transports; remaining HAL gaps (DSP chain upload,
# POWER_MODE_SET) and OTA degrade to STATUS_NOSUPPORT.

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
#   * gd32      -- builds against hal/bridge_hw_gd32.c +
#                  hal/transport_hw_gd32.c; requires the GigaDevice
#                  firmware library at vendors/gd32_firmware_library/.
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
    src/ota.c
)

# OTA self-flashing is gated: only -DBRIDGE_OTA_PARTITIONED=ON (paired with
# the partitioned bootloader layout + slot-linked app) arms the destructive
# flash path.  Default OFF -> ota_dispatch() returns STATUS_NOSUPPORT and the
# full-flash image can't brick itself.  HIL-REQUIRED before enabling.
option(BRIDGE_OTA_PARTITIONED "Arm the OTA Path-A flash path (HIL-gated)" OFF)

if(BRIDGE_HAL_BACKEND STREQUAL "stub")
    list(APPEND GD32_BRIDGE_SOURCES hal/bridge_hw_stub.c)
elseif(BRIDGE_HAL_BACKEND STREQUAL "gd32")
    # bridge_hw_gd32.c: the real (currently stubbed) peripheral hooks.
    # gd32_libc_stubs.c: weak `_init` / `_fini` newlib stubs needed
    # because `-nostartfiles` drops crti.o.  Only relevant to this
    # backend; the stub HAL backend never links the vendor Reset_Handler
    # that pulls in `__libc_init_array`.
    list(APPEND GD32_BRIDGE_SOURCES
        hal/bridge_hw_gd32.c
        hal/transport_hw_gd32.c
        hal/fmc_ota.c
        hal/gd32_libc_stubs.c
    )
    # Pull in the GigaDevice firmware-library wrapper.  Its CMakeLists
    # fatal-errors with a clear "drop the SDK into sdk/" instruction
    # when the unpacked archive is missing, so an unconfigured
    # workspace doesn't fall through to a confusing link error.
    set(_GD32_VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../vendors/gd32_firmware_library)
    if(NOT EXISTS ${_GD32_VENDOR_DIR}/CMakeLists.txt)
        message(FATAL_ERROR
            "BRIDGE_HAL_BACKEND=gd32 selected but "
            "${_GD32_VENDOR_DIR}/CMakeLists.txt is missing.\n"
            "Check out the alp-sdk repo's vendors/gd32_firmware_library/ "
            "tree (the wrapper README documents the manual SDK drop).")
    endif()
    add_subdirectory(${_GD32_VENDOR_DIR} ${CMAKE_BINARY_DIR}/vendors/gd32_firmware_library)
    # gd32g5x3.h #error-guards on the device-family macro; pick
    # GD32G553 since the V2N's supervisor is the GD32G553MEY7TR.
    # The vendor archive also accepts GD32G533 for the smaller
    # part-family variant.  PUBLIC so consumers of the library
    # (chiefly the gd32-bridge executable below) also see it.
    target_compile_definitions(gd32_firmware_library PUBLIC GD32G553)
    # Make the project's libopt.h selector visible to the vendor
    # library's own .c sources -- gd32g5x3.h transitively includes
    # gd32g5x3_libopt.h, and the vendor library's compile units
    # don't have `hal/` on their include path otherwise.
    target_include_directories(gd32_firmware_library PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/hal)
else()
    message(FATAL_ERROR "Unknown BRIDGE_HAL_BACKEND='${BRIDGE_HAL_BACKEND}' (expected: stub | gd32).")
endif()

add_executable(gd32-bridge ${GD32_BRIDGE_SOURCES})

target_include_directories(gd32-bridge PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/hal
)

if(BRIDGE_OTA_PARTITIONED)
    target_compile_definitions(gd32-bridge PRIVATE BRIDGE_OTA_PARTITIONED=1)
    message(STATUS "gd32-bridge: OTA Path-A flash path ARMED (BRIDGE_OTA_PARTITIONED) -- HIL-required")
endif()

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

# Emit .hex + .bin for the maintainer's flasher of choice.
#
# Use literal paths rather than $<TARGET_FILE:gd32-bridge> generator
# expressions -- on the CI runner's CMake 3.31 + arm-none-eabi-gcc
# 13.2 + Generic-system toolchain combo, $<TARGET_FILE:gd32-bridge>
# fails the GENERATE step with "No target gd32-bridge" even after
# the configure-time get_target_property() above confirms the target
# is registered as EXECUTABLE.  The target_file generator expression
# evidently can't resolve for executables on CMAKE_SYSTEM_NAME=Generic
# without a CMAKE_EXECUTABLE_SUFFIX hint; pre-computing the path
# sidesteps it entirely.
#
# Gate on BRIDGE_HAL_BACKEND=gd32 because the stub backend doesn't
# ship a startup file (no Reset_Handler symbol), so the linker
# emits an empty binary with no sections.  objcopy refuses to
# process a sectionless file and the build fails.  The stub backend
# is for protocol-layer compile validation only -- the .elf isn't
# runnable, so .hex/.bin emission is a no-op for it.
if(BRIDGE_HAL_BACKEND STREQUAL "gd32")
    set(_GD32_BRIDGE_BIN ${CMAKE_BINARY_DIR}/gd32-bridge)
    add_custom_command(TARGET gd32-bridge POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex   ${_GD32_BRIDGE_BIN} ${_GD32_BRIDGE_BIN}.hex
        COMMAND ${CMAKE_OBJCOPY} -O binary ${_GD32_BRIDGE_BIN} ${_GD32_BRIDGE_BIN}.bin
        BYPRODUCTS ${_GD32_BRIDGE_BIN}.hex ${_GD32_BRIDGE_BIN}.bin
    )
endif()

# ----------------------------------------------------------------- #
# Firmware release version + build-id stamping.                      #
# ----------------------------------------------------------------- #

# Firmware *release* version -- its own semver, tracked independently of
# the wire PROTOCOL_VERSION (src/protocol.h) and of the SDK version.
# Source of truth: firmware-version.txt.  Bump it per firmware release,
# tag to match, and name the prebuilt blob `gd32-bridge-v<ver>.bin`.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/firmware-version.txt GD32_BRIDGE_FW_VERSION LIMIT_COUNT 1)
target_compile_definitions(gd32-bridge PRIVATE GD32_BRIDGE_FW_VERSION="${GD32_BRIDGE_FW_VERSION}")
message(STATUS "gd32-bridge firmware version: ${GD32_BRIDGE_FW_VERSION}")

# GET_BUILD_ID reports "<fw-version>+<short-sha>" (e.g. "0.1.0+abc1234",
# <= GD32_BRIDGE_BUILD_ID_LEN = 20 chars): the firmware release version
# plus the exact build behind it.
# TODO: post-link hook that computes the short SHA of the .elf and
# patches the linker-emitted build-id constant.  Until that lands the
# firmware reports "<fw-version>+0000000" to keep the GET_BUILD_ID frame
# structurally valid.
