# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_cc3501e_wifi_backend_security)

# Link the REAL Alif-side host driver (chips/cc3501e/cc3501e_*.c), the REAL
# backend registry selector (src/backend.c), and the CC3501E backend under
# test (src/backends/wifi/cc3501e.c) -- issue #742.  Deliberately does NOT
# link src/wifi_dispatch.c or the CONFIG_ALP_SDK zephyr_library (which pulls
# in a real Zephyr SPI backend that would conflict with this test's fake
# alp_spi_transceive): the test instantiates ALP_BACKEND_DEFINE_CLASS(wifi)
# itself (see test_wifi_backend_security.c) and drives the backend's ops
# vtable directly via alp_backend_select(), the same production code path
# alp_wifi_open()/alp_wifi_connect() use, minus the handle-pool bookkeeping
# that isn't this test's concern.  The test supplies its own
# alp_spi_transceive() (a software model of the firmware slave) plus inert
# alp_delay_*/alp_gpio_* seams -- no SDK backend registration via Kconfig,
# no silicon.
set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(CHIPS_DIR ${REPO_ROOT}/chips)
file(GLOB CC3501E_DRIVER_SOURCES ${CHIPS_DIR}/cc3501e/cc3501e_*.c)

target_sources(app PRIVATE
    src/test_wifi_backend_security.c
    ${CC3501E_DRIVER_SOURCES}
    ${REPO_ROOT}/src/backends/wifi/cc3501e.c
    ${REPO_ROOT}/src/backend.c
)
# The repo-root include/ gives the driver + backend the CANONICAL public
# headers; src/ gives the backend its private src/backends/wifi/wifi_ops.h.
target_include_directories(app PRIVATE
    ${REPO_ROOT}/include
    ${REPO_ROOT}/src)
