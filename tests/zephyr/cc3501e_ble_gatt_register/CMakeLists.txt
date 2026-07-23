# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_cc3501e_ble_gatt_register)

# Link the REAL Alif-side host driver (chips/cc3501e/cc3501e_*.c), the REAL
# backend registry selector (src/backend.c), and the CC3501E BLE backend
# under test (src/backends/ble/cc3501e.c) -- issue #480, same pattern as
# tests/zephyr/cc3501e_wifi_backend_security (issue #742).  Deliberately does
# NOT link src/ble_dispatch.c or the CONFIG_ALP_SDK zephyr_library (which
# would pull in a real Zephyr SPI backend that conflicts with this test's
# fake alp_spi_transceive): the test instantiates ALP_BACKEND_DEFINE_CLASS(ble)
# itself and drives the backend's ops vtable directly via alp_backend_select(),
# the same production code path alp_ble_open()/alp_ble_gatt_register_service()
# use, minus the handle-pool bookkeeping that isn't this test's concern.  The
# test supplies its own alp_spi_transceive() (a software model of the firmware
# slave) plus inert alp_delay_*/alp_gpio_* seams -- no SDK backend
# registration via Kconfig, no silicon, no BLE stack.
set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(CHIPS_DIR ${REPO_ROOT}/chips)
file(GLOB CC3501E_DRIVER_SOURCES ${CHIPS_DIR}/cc3501e/cc3501e_*.c)

target_sources(app PRIVATE
    src/test_ble_gatt_register.c
    ${CC3501E_DRIVER_SOURCES}
    ${REPO_ROOT}/src/backends/ble/cc3501e.c
    ${REPO_ROOT}/src/backend.c
)
# The repo-root include/ gives the driver + backend the CANONICAL public
# headers; src/ gives the backend its private src/backends/ble/ble_ops.h.
target_include_directories(app PRIVATE
    ${REPO_ROOT}/include
    ${REPO_ROOT}/src)
