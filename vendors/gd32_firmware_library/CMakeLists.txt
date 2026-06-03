# SPDX-License-Identifier: Apache-2.0
# Thin CMake wrapper for the GD32G5x3 firmware library.
#
# Consumed by firmware/gd32-bridge/CMakeLists.txt when BRIDGE_HAL_BACKEND=gd32.
# The actual library sources live in this directory's `upstream/`
# git-submodule pointing at https://github.com/alplabai/gd32g5x3-firmware-library
# (a verbatim mirror of GigaDevice's v1.5.0 archive, redistributed
# under SLA-GD0001 v1.1 -- see that repo's README for the licence
# discussion).

cmake_minimum_required(VERSION 3.20)

set(_GD32_UPSTREAM ${CMAKE_CURRENT_SOURCE_DIR}/upstream)
set(_GD32_FW_ROOT  ${_GD32_UPSTREAM}/Firmware)
set(_GD32_CMSIS    ${_GD32_FW_ROOT}/CMSIS)
set(_GD32_STDPER   ${_GD32_FW_ROOT}/GD32G5x3_standard_peripheral)
set(_GD32_DEVICE_H ${_GD32_CMSIS}/GD/GD32G5x3/Include/gd32g5x3.h)

if(NOT EXISTS ${_GD32_DEVICE_H})
    message(FATAL_ERROR
        "GD32 firmware library not found.\n"
        "Expected gd32g5x3.h under:\n"
        "  ${_GD32_CMSIS}/GD/GD32G5x3/Include/\n"
        "Run:  git submodule update --init --recursive vendors/gd32_firmware_library/upstream\n"
        "(submodule points at https://github.com/alplabai/gd32g5x3-firmware-library)")
endif()

message(STATUS "vendors/gd32_firmware_library/upstream: found GD32G5x3 firmware library "
               "(${_GD32_DEVICE_H})")

# Collect every peripheral source the standard-peripheral library
# publishes.  CMake's file(GLOB) is acceptable here because the
# vendor archive's source list is fixed per release; refreshing the
# submodule + re-running CMake picks up additions / removals.
file(GLOB _GD32_STDPER_SOURCES
    ${_GD32_STDPER}/Source/gd32g5x3_*.c)

# Vendor reset + clock-tree setup.  The GCC startup ships as
# startup_gd32g5x3.S (no `_gd32g553.S` -- the suffix matches the
# silicon family, not the specific MPN).  system_gd32g5x3.c carries
# SystemInit().
#
# system_gd32g5x3.c is taken from an alp-sdk OVERRIDE (overrides/) instead of the
# submodule copy: the stock file selects the 216M-PLL-HXTAL clock, but on the
# E1M-V2M101 SoM the GD32 HXTAL (5L35023B SE2) never starts, so SystemInit()
# hangs forever before main().  The override selects 216M-PLL-IRC8M; keeping it
# here leaves the submodule pristine (clean `git submodule update` diffs),
# mirroring the vendored FSP r_sci_b_spi module on the Zephyr side.  Drop or
# refresh the override if the submodule is bumped or SE2 is brought up.
set(_GD32_SYSTEM_OVERRIDE ${CMAKE_CURRENT_SOURCE_DIR}/overrides/system_gd32g5x3.c)
if(EXISTS ${_GD32_SYSTEM_OVERRIDE})
    set(_GD32_SYSTEM_C ${_GD32_SYSTEM_OVERRIDE})
    message(STATUS "vendors/gd32_firmware_library: using alp-sdk clock override "
                   "(216M-PLL-IRC8M; submodule kept pristine)")
else()
    set(_GD32_SYSTEM_C ${_GD32_CMSIS}/GD/GD32G5x3/Source/system_gd32g5x3.c)
endif()
set(_GD32_SYSTEM_SOURCES
    ${_GD32_SYSTEM_C}
    ${_GD32_CMSIS}/GD/GD32G5x3/Source/GCC/startup_gd32g5x3.S)

add_library(gd32_firmware_library STATIC
    ${_GD32_SYSTEM_SOURCES}
    ${_GD32_STDPER_SOURCES})

# CMSIS core headers live directly under Firmware/CMSIS/, NOT under a
# Core/Include subdir like Zephyr's hal_*.  Confirmed against the
# v1.5.0 layout.  Per-device headers in Firmware/CMSIS/GD/GD32G5x3/Include/.
target_include_directories(gd32_firmware_library PUBLIC
    ${_GD32_CMSIS}
    ${_GD32_CMSIS}/GD/GD32G5x3/Include
    ${_GD32_STDPER}/Include)

target_compile_options(gd32_firmware_library PRIVATE
    -mcpu=cortex-m33
    -mthumb
    -mfloat-abi=soft
    -ffunction-sections
    -fdata-sections
    -Wno-unused-parameter
    -Wno-misleading-indentation)
