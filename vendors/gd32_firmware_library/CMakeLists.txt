# SPDX-License-Identifier: Apache-2.0
# Thin CMake wrapper for the GD32G5x3 firmware library.
#
# Consumed by gd32-bridge/CMakeLists.txt when BRIDGE_HAL_BACKEND=gd32.
# The SDK itself is NOT vendored in git (see README.md); this file
# only exposes the header search paths + the source-file glob for the
# vendor's CMSIS + standard-peripheral library once the unpacked
# archive has been dropped under `sdk/`.

cmake_minimum_required(VERSION 3.20)

# Resolve the canonical drop layout described in README.md.
set(_GD32_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
set(_GD32_FW_ROOT  ${_GD32_SDK_ROOT}/Firmware)
set(_GD32_CMSIS    ${_GD32_FW_ROOT}/CMSIS)
set(_GD32_STDPER   ${_GD32_FW_ROOT}/GD32G5x3_standard_peripheral)
set(_GD32_DEVICE_H ${_GD32_CMSIS}/GD/GD32G5x3/Include/gd32g5x3.h)

if(NOT EXISTS ${_GD32_DEVICE_H})
    message(FATAL_ERROR
        "GD32 firmware library not found.\n"
        "Expected gd32g5x3.h under:\n"
        "  ${_GD32_CMSIS}/GD/GD32G5x3/Include/\n"
        "Drop the GigaDevice 'GD32G5x3 Firmware Library' archive into "
        "vendors/gd32_firmware_library/sdk/ following the layout "
        "documented in vendors/gd32_firmware_library/README.md.")
endif()

message(STATUS "vendors/gd32_firmware_library/sdk: found GD32G5x3 firmware library "
               "(${_GD32_DEVICE_H})")

# Collect every peripheral source the standard-peripheral library
# publishes.  CMake's file(GLOB) is acceptable here because adding /
# removing a peripheral C file in the vendor pack is a manual drop
# and the next CMake re-run picks it up.
file(GLOB _GD32_STDPER_SOURCES
    ${_GD32_STDPER}/Source/gd32g5x3_*.c)

# Vendor reset + clock-tree setup.  startup_gd32g553.S provides the
# Cortex-M33 reset handler that the linker script wires to the FMC
# flash base (0x08000000).  system_gd32g5x3.c carries SystemInit().
set(_GD32_SYSTEM_SOURCES
    ${_GD32_CMSIS}/GD/GD32G5x3/Source/system_gd32g5x3.c
    ${_GD32_CMSIS}/GD/GD32G5x3/Source/GCC/startup_gd32g553.S)

add_library(gd32_firmware_library STATIC
    ${_GD32_SYSTEM_SOURCES}
    ${_GD32_STDPER_SOURCES})

target_include_directories(gd32_firmware_library PUBLIC
    ${_GD32_CMSIS}/Core/Include
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
