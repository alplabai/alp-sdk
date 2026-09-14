# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_cc3501e_host_driver)

# Link the REAL Alif-side host driver (chips/cc3501e/cc3501e_*.c, split by
# subsystem -- issue #461), so the request encoding + reply decoding under
# test are the production code paths.  The test supplies its own
# alp_spi_transceive() (a software model of the firmware slave) plus inert
# alp_delay_*/alp_gpio_* seams, so nothing but the driver + the wire
# contract is exercised -- no SDK backend, no silicon.
set(CHIPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../chips)
file(GLOB CC3501E_DRIVER_SOURCES ${CHIPS_DIR}/cc3501e/cc3501e_*.c)

target_sources(app PRIVATE
    src/test_host_driver.c
    ${CC3501E_DRIVER_SOURCES}
)
# The repo-root include/ gives the driver the CANONICAL public headers
# (<alp/chips/cc3501e.h>, <alp/protocol/cc3501e.h>, <alp/peripheral.h>).
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../cc3501e_common)

# issue #2126: the alp_sdk.cc3501e.host_driver.auto_recover_off twister
# scenario (testcase.yaml) passes -DCC3501E_AUTO_RECOVER_OFF=1 (a plain
# CMake cache var via extra_args, NOT a Kconfig symbol) to force
# CONFIG_ALP_SDK_CC3501E_AUTO_RECOVER=0 for this build. A REAL Kconfig
# override (extra_configs: CONFIG_ALP_SDK_CC3501E_AUTO_RECOVER=n) can't work
# here: that symbol lives under `if ALP_SDK` / `if ALP_SDK_CHIP_CC3501E`
# (zephyr/Kconfig, zephyr/kconfigs/chips.kconfig), and enabling ALP_SDK ALSO
# makes zephyr/CMakeLists.txt build+link the real alp-sdk Zephyr library --
# which duplicates chips/cc3501e/cc3501e_*.c (globbed directly, above) and
# the alp_spi_transceive/alp_gpio_* fakes this test supplies, and the build
# fails with `multiple definition of ...`. A compile definition on THIS
# target alone gets the one macro cc3501e_core.c's own #ifndef fallback
# reads, without pulling in the rest of the SDK.
if(CC3501E_AUTO_RECOVER_OFF)
    target_compile_definitions(app PRIVATE CONFIG_ALP_SDK_CC3501E_AUTO_RECOVER=0)
endif()
