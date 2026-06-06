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
    src/crc32.c
)

# OTA self-flashing is gated: only -DBRIDGE_OTA_PARTITIONED=ON (paired with
# the partitioned bootloader layout + slot-linked app) arms the destructive
# flash path.  Default OFF -> ota_dispatch() returns STATUS_NOSUPPORT and the
# full-flash image can't brick itself.  HIL-REQUIRED before enabling.
option(BRIDGE_OTA_PARTITIONED "Arm the OTA Path-A flash path (HIL-gated)" OFF)

if(BRIDGE_HAL_BACKEND STREQUAL "stub")
    list(APPEND GD32_BRIDGE_SOURCES hal/bridge_hw_stub.c)
elseif(BRIDGE_HAL_BACKEND STREQUAL "gd32")
    # hal/gd32/: the real peripheral hooks, split move-only into per-
    # peripheral translation units (fw v0.2.8 refactor).  init.c owns boot
    # bring-up + board-level reads; the rest are one TU per peripheral and
    # share tables/helpers via hal/gd32/gd32_common.h.
    # gd32_libc_stubs.c: weak `_init` / `_fini` newlib stubs needed
    # because `-nostartfiles` drops crti.o.  Only relevant to this
    # backend; the stub HAL backend never links the vendor Reset_Handler
    # that pulls in `__libc_init_array`.
    list(APPEND GD32_BRIDGE_SOURCES
        hal/gd32/init.c
        hal/gd32/adc.c
        hal/gd32/adc_stream.c
        hal/gd32/counter.c
        hal/gd32/dac.c
        hal/gd32/gpio.c
        hal/gd32/power.c
        hal/gd32/pwm.c
        hal/gd32/pwm_capture.c
        hal/gd32/qenc.c
        hal/gd32/timer_sync.c
        hal/gd32/tmu.c
        hal/gd32/trng.c
        hal/gd32/vref.c
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

# Firmware *release* version (own semver; source of truth firmware-version.txt).
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/firmware-version.txt GD32_BRIDGE_FW_VERSION LIMIT_COUNT 1)
message(STATUS "gd32-bridge firmware version: ${GD32_BRIDGE_FW_VERSION}")

# GET_BUILD_ID payload: "<fw-version>+<git-sha-prefix>", exactly 20 chars
# (protocol.c _Static_asserts the length).  Baked at BUILD time by
# cmake/gen_build_id.cmake into a generated header, regenerated on every
# build and copied only-if-different -- so a HEAD move OR a version bump
# re-bakes the id on the next incremental build with NO reconfigure.
# (The old configure-time placeholder went stale on exactly those two
# axes; an OTA bench upgrade check failed on it 2026-06-04.)
set(GD32_BRIDGE_BUILD_ID_HEADER_DIR ${CMAKE_BINARY_DIR}/generated)
add_custom_target(gd32-build-id
    COMMAND ${CMAKE_COMMAND}
        -DVERSION_FILE=${CMAKE_CURRENT_SOURCE_DIR}/firmware-version.txt
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DOUT_HEADER=${GD32_BRIDGE_BUILD_ID_HEADER_DIR}/gd32_bridge_build_id.h
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_build_id.cmake
    BYPRODUCTS ${GD32_BRIDGE_BUILD_ID_HEADER_DIR}/gd32_bridge_build_id.h
    COMMENT "Baking gd32-bridge build-id (version + git sha)")

# Common per-target config: Cortex-M33 + thumb + soft-float, the build-id
# header, includes, the vendor lib + .hex/.bin emission (gd32 backend only;
# the stub backend has no Reset_Handler so objcopy of a sectionless ELF
# fails).  Literal output paths sidestep a $<TARGET_FILE:> GENERATE-step
# failure on Generic-system toolchains.
function(gd32_firmware_target tgt linker_script)
    target_include_directories(${tgt} PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/hal
                               ${GD32_BRIDGE_BUILD_ID_HEADER_DIR})
    # The generated gd32_bridge_build_id.h carries GD32_BRIDGE_BUILD_ID;
    # protocol.c includes it when this flag is set (and keeps its
    # hermetic placeholder fallback for non-CMake consumers like the
    # SDK-side transport unit tests).  The old per-target
    # GD32_BRIDGE_FW_VERSION define was consumed by no source file --
    # deleted; the version rides inside the build-id.
    target_compile_definitions(${tgt} PRIVATE GD32_BRIDGE_HAVE_BUILD_ID_HEADER)
    add_dependencies(${tgt} gd32-build-id)
    target_compile_options(${tgt} PRIVATE
        -mcpu=cortex-m33 -mthumb -mfloat-abi=soft
        -ffunction-sections -fdata-sections
        -Wall -Wextra -Wshadow -Wpedantic)
    target_link_options(${tgt} PRIVATE
        -mcpu=cortex-m33 -mthumb -mfloat-abi=soft
        -Wl,--gc-sections -nostartfiles -T${linker_script})
    if(BRIDGE_HAL_BACKEND STREQUAL "gd32")
        target_link_libraries(${tgt} PRIVATE gd32_firmware_library)
        set(_bin ${CMAKE_BINARY_DIR}/${tgt})
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} -O ihex   ${_bin} ${_bin}.hex
            COMMAND ${CMAKE_OBJCOPY} -O binary ${_bin} ${_bin}.bin
            BYPRODUCTS ${_bin}.hex ${_bin}.bin)
    endif()
endfunction()

if(NOT BRIDGE_OTA_PARTITIONED)
    # Default: a single full-flash image (linked at 0x08000000).  OTA is
    # inert (ota_dispatch -> STATUS_NOSUPPORT); this is the bench/test image.
    add_executable(gd32-bridge ${GD32_BRIDGE_SOURCES})
    gd32_firmware_target(gd32-bridge ${CMAKE_SOURCE_DIR}/toolchain/gd32g553_flash.ld)
else()
    # OTA Path-A armed: a bootloader at flash base + the app linked for each
    # A/B slot (vector table relocated via SCB->VTOR at boot).  HIL-REQUIRED:
    # the boot/validate/jump + slot-relocation are NOT validated on silicon
    # (a bug bricks the part -- no host SWD reflash this HW rev).
    if(NOT BRIDGE_HAL_BACKEND STREQUAL "gd32")
        message(FATAL_ERROR "BRIDGE_OTA_PARTITIONED requires BRIDGE_HAL_BACKEND=gd32.")
    endif()
    message(STATUS "gd32-bridge: OTA Path-A ARMED (bootloader + A/B slots) -- HIL-required, do not field-flash unvalidated")

    set(APP_SLOT_LEN 0x0003B000)   # 236 KB per slot (ota_layout.h)
    set(APP_SLOT_BASE 0x0800A000)  # slot A
    configure_file(${CMAKE_SOURCE_DIR}/toolchain/gd32g553_app_slot.ld.in
                   ${CMAKE_BINARY_DIR}/gd32g553_slot_a.ld @ONLY)
    set(APP_SLOT_BASE 0x08045000)  # slot B
    configure_file(${CMAKE_SOURCE_DIR}/toolchain/gd32g553_app_slot.ld.in
                   ${CMAKE_BINARY_DIR}/gd32g553_slot_b.ld @ONLY)

    add_executable(gd32-bridge-slot-a ${GD32_BRIDGE_SOURCES})
    gd32_firmware_target(gd32-bridge-slot-a ${CMAKE_BINARY_DIR}/gd32g553_slot_a.ld)
    target_compile_definitions(gd32-bridge-slot-a PRIVATE
        BRIDGE_OTA_PARTITIONED=1 BRIDGE_APP_SLOT_BASE=0x0800A000u)

    add_executable(gd32-bridge-slot-b ${GD32_BRIDGE_SOURCES})
    gd32_firmware_target(gd32-bridge-slot-b ${CMAKE_BINARY_DIR}/gd32g553_slot_b.ld)
    target_compile_definitions(gd32-bridge-slot-b PRIVATE
        BRIDGE_OTA_PARTITIONED=1 BRIDGE_APP_SLOT_BASE=0x08045000u)

    add_executable(gd32-bootloader src/boot/boot_main.c src/crc32.c hal/gd32_libc_stubs.c)
    gd32_firmware_target(gd32-bootloader ${CMAKE_SOURCE_DIR}/toolchain/gd32g553_bootloader.ld)
endif()

# GET_BUILD_ID reports "<fw-version>+<short-sha>"; the post-link SHA hook is
# still TODO (firmware reports "<fw-version>+0000000" until then).
