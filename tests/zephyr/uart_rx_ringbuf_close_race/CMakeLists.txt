# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_uart_rx_ringbuf_close_race)

target_sources(app PRIVATE
    src/test_uart_rx_ringbuf_close_race.c
)

# handles.h (struct alp_uart_rx_ringbuf + alp_z_uart_rx_ringbuf_pool_acquire/
# release) is an alp-sdk-INTERNAL header -- src/zephyr is only exposed to
# the module's own zephyr_library() target, not to a downstream app, so
# this test adds the include path itself.  alp_slot_claim.h (src/common)
# and lwrb/lwrb.h (vendors/lwrb) are already exposed globally by the
# module's own zephyr/CMakeLists.txt.
set(ALP_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
target_include_directories(app PRIVATE
    ${ALP_SDK_DIR}/include
    ${ALP_SDK_DIR}/src/zephyr
)
