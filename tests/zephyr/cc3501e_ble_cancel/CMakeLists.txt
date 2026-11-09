# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_cc3501e_ble_cancel)

# Issue #3103: alp_ble_disconnect() must wake an alp_ble_gatt_read() blocked
# on the CC3501E bridge.  Links the REAL driver (chips/cc3501e), the REAL
# CC3501E BLE backend, the REAL registry selector and the REAL dispatcher
# (src/ble_dispatch.c) so the test goes through the public alp_ble_* calls;
# only alp_spi_transceive() (a software slave) and the delay/gpio seams are
# faked.  Same layout as tests/zephyr/cc3501e_ble_gatt_register.
set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
file(GLOB CC3501E_DRIVER_SOURCES ${REPO_ROOT}/chips/cc3501e/cc3501e_*.c)

target_sources(app PRIVATE
    src/test_ble_cancel.c
    ${CC3501E_DRIVER_SOURCES}
    ${REPO_ROOT}/src/backends/ble/cc3501e.c
    ${REPO_ROOT}/src/backend.c
    ${REPO_ROOT}/src/ble_dispatch.c
    ${REPO_ROOT}/src/common/alp_slot_claim.c
    ${REPO_ROOT}/src/zephyr/last_error.c
)
target_include_directories(app PRIVATE
    ${REPO_ROOT}/include
    ${REPO_ROOT}/src
    ${REPO_ROOT}/src/common
    ${CMAKE_CURRENT_SOURCE_DIR}/../cc3501e_common)

# Selects the alif:ensemble:e7 row of the CC3501E BLE backend for
# ALP_SOC_REF_STR (alp_ble_open() selects by it).
target_compile_definitions(app PRIVATE
    CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=1
    CONFIG_ALP_SDK_CC3501E_AUTO_RECOVER=1)
