# SPDX-License-Identifier: Apache-2.0
#
# cc3501e-bridge firmware top-level CMake.
#
# Builds the Cortex-M33 firmware that runs on the TI CC3501E Wi-Fi 6 +
# BLE 5.4 coprocessor on the E1M-AEN module family.  This is a SEPARATE
# COMPILE ARTIFACT from the Zephyr-side alp-sdk library (the Zephyr
# build never descends here) -- embedded in alp-sdk for single-source
# protocol + atomic cross-side changes, the same way firmware/gd32-bridge/
# is.  Invoke directly:
#
#   # Host-side / CI compile smoke (no TI SDK needed):
#   cmake -B build/stub -S firmware/cc3501e \
#     -DCMAKE_TOOLCHAIN_FILE=firmware/cc3501e/toolchain/arm-none-eabi.cmake \
#     -DCC3501E_HAL_BACKEND=stub
#   cmake --build build/stub
#
#   # Production image (bench: needs TI ticlang + SimpleLink CC33xx SDK):
#   cmake -B build/ti -S firmware/cc3501e \
#     -DCMAKE_TOOLCHAIN_FILE=firmware/cc3501e/toolchain/ticlang.cmake \
#     -DCC3501E_HAL_BACKEND=ti
#   cmake --build build/ti
#
# The output image lives at build/<dir>/cc3501e-bridge(.hex/.bin on ti).

cmake_minimum_required(VERSION 3.20)
project(cc3501e-bridge LANGUAGES C ASM)

# ----------------------------------------------------------------- #
# Backend selection                                                  #
# ----------------------------------------------------------------- #
#
# CC3501E_HAL_BACKEND controls which cc3501e_hw_* implementation links:
#   * stub -- hal/cc3501e_hw_stub.c only; every HW-touching op reports
#             CC3501E_HW_ERR_NOTIMPL.  Silicon-free; this is what CI and
#             the host unit test build.
#   * ti   -- hal/ti/* against TI's SimpleLink CC33xx SDK (vendored as a
#             submodule).  Built on the bench with ticlang.
set(CC3501E_HAL_BACKEND "stub" CACHE STRING "CC3501E HAL backend (stub | ti)")

# ----------------------------------------------------------------- #
# Host-control transport selection (customer-selectable)             #
# ----------------------------------------------------------------- #
#
# CC3501E_CONTROL_TRANSPORT picks which slave link main() brings up:
#   * spi  -- DEFAULT + always-available baseline/fallback.
#   * sdio -- OPTIONAL, higher throughput.  Only for boards that route
#             the Alif's single SDIO controller to the CC3501E (no SD
#             card).  See docs/cc3501e-bridge.md.
# Both transports always COMPILE (cheap; the framing is shared); this
# only selects which one main() starts.  The choice is sourced from the
# customer board.yaml in a full studio build; the firmware default is spi.
set(CC3501E_CONTROL_TRANSPORT "spi" CACHE STRING "Active host-control transport (spi | sdio)")

# ----------------------------------------------------------------- #
# Source layout                                                      #
# ----------------------------------------------------------------- #

set(CC3501E_BRIDGE_SOURCES
    src/main.c
    src/protocol.c
    src/protocol_meta.c
    src/protocol_gpio.c
    src/protocol_camera.c
    src/protocol_wifi.c
    src/protocol_sockets.c
    src/protocol_ble.c
    src/protocol_power.c
    src/protocol_diag.c
    src/protocol_ota.c
    src/worker.c
    src/event_ring.c
    src/transport_spi.c
    src/transport_sdio.c
)

if(CC3501E_HAL_BACKEND STREQUAL "stub")
    list(APPEND CC3501E_BRIDGE_SOURCES hal/cc3501e_hw_stub.c)
elseif(CC3501E_HAL_BACKEND STREQUAL "ti")
    # The real TI SimpleLink / driverlib hooks (bench build).
    list(APPEND CC3501E_BRIDGE_SOURCES
        hal/ti/cc3501e_hw_ti.c
        hal/ti/transport_hw_ti_spi.c
        hal/ti/transport_hw_ti_sdio.c
    )
    # TI's SimpleLink CC33xx SDK is vendored as a submodule.  It is
    # BSD-3-licensed and obtained from TI (alp-sdk does not redistribute
    # it).  Fatal-error with a clear instruction when it's missing so an
    # unconfigured workspace doesn't fall through to a confusing error.
    set(_TI_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/simplelink-cc33xx)
    if(NOT EXISTS ${_TI_SDK_DIR}/CMakeLists.txt)
        message(FATAL_ERROR
            "CC3501E_HAL_BACKEND=ti selected but "
            "${_TI_SDK_DIR}/CMakeLists.txt is missing.\n"
            "Check out the TI SimpleLink CC33xx SDK submodule under "
            "firmware/cc3501e/vendor/simplelink-cc33xx/ (see README.md "
            "\"Build\").  CI builds the stub backend instead.")
    endif()
    add_subdirectory(${_TI_SDK_DIR} ${CMAKE_BINARY_DIR}/vendor/simplelink-cc33xx)
else()
    message(FATAL_ERROR "Unknown CC3501E_HAL_BACKEND='${CC3501E_HAL_BACKEND}' (expected: stub | ti).")
endif()

if(CC3501E_CONTROL_TRANSPORT STREQUAL "sdio")
    add_compile_definitions(CC3501E_CONTROL_TRANSPORT_SDIO=1)
elseif(NOT CC3501E_CONTROL_TRANSPORT STREQUAL "spi")
    message(FATAL_ERROR "Unknown CC3501E_CONTROL_TRANSPORT='${CC3501E_CONTROL_TRANSPORT}' (expected: spi | sdio).")
endif()

# Firmware *release* version (own semver; source of truth firmware-version.txt).
# Parse the SemVer and pack MINOR/PATCH into the u16 marker reported by
# GET_DIAG_INFO.fw_version, so the runtime value is DERIVED from this file and
# can never drift (see the CC3501E_BRIDGE_FW_VERSION_U16 comment in src/protocol.c).
# Pre-1.0 packing: (MINOR << 8) | PATCH -> 0.2.0 = 0x0200.  ti/build_ti.sh mirrors
# this for the TI build (that path does not go through CMake).
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/firmware-version.txt CC3501E_BRIDGE_FW_VERSION LIMIT_COUNT 1)
if(NOT CC3501E_BRIDGE_FW_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "firmware-version.txt not SemVer major.minor.patch: '${CC3501E_BRIDGE_FW_VERSION}'")
endif()
math(EXPR CC3501E_BRIDGE_FW_VERSION_U16 "(${CMAKE_MATCH_2} << 8) | ${CMAKE_MATCH_3}" OUTPUT_FORMAT HEXADECIMAL)
message(STATUS "cc3501e-bridge firmware version: ${CC3501E_BRIDGE_FW_VERSION} (fw_version marker ${CC3501E_BRIDGE_FW_VERSION_U16})")
message(STATUS "cc3501e-bridge HAL backend: ${CC3501E_HAL_BACKEND}; transport: ${CC3501E_CONTROL_TRANSPORT}")

add_executable(cc3501e-bridge ${CC3501E_BRIDGE_SOURCES})

# Feed the derived fw_version marker to protocol.c (overrides its #ifndef fallback).
target_compile_definitions(cc3501e-bridge PRIVATE
    CC3501E_BRIDGE_FW_VERSION_U16=${CC3501E_BRIDGE_FW_VERSION_U16})

# Include paths: firmware src + hal, and the repo-root include/ so the
# firmware includes the CANONICAL <alp/protocol/cc3501e.h> directly --
# no mirrored header, no drift (the payoff of embedding in alp-sdk).
target_include_directories(cc3501e-bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/hal
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include)

target_compile_options(cc3501e-bridge PRIVATE
    -mcpu=cortex-m33 -mthumb -mfloat-abi=soft
    -ffunction-sections -fdata-sections
    -Wall -Wextra -Wshadow -Wpedantic)
target_link_options(cc3501e-bridge PRIVATE
    -mcpu=cortex-m33 -mthumb -mfloat-abi=soft
    -Wl,--gc-sections -nostartfiles)

if(CC3501E_HAL_BACKEND STREQUAL "ti")
    target_link_libraries(cc3501e-bridge PRIVATE simplelink_cc33xx)
    set(_bin ${CMAKE_BINARY_DIR}/cc3501e-bridge)
    add_custom_command(TARGET cc3501e-bridge POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex   ${_bin} ${_bin}.hex
        COMMAND ${CMAKE_OBJCOPY} -O binary ${_bin} ${_bin}.bin
        BYPRODUCTS ${_bin}.hex ${_bin}.bin)
endif()
