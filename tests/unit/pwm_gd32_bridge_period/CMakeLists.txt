# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_pwm_gd32_bridge_period)

# Issue #1624: src/backends/pwm/gd32_bridge.c's br_open() never wrote the
# dispatcher-owned struct alp_pwm::period_ns, so alp_pwm_set_duty() always
# bounds-checked pulse_ns against 0 and returned ALP_ERR_INVAL.
#
# This test links the REAL backend file under test directly, WITHOUT the
# alp-sdk Zephyr module (this prj.conf deliberately omits CONFIG_ALP_SDK --
# see the gd32_bridge_ota/transport tests for the same pattern). Linking the
# module instead would pull in the real src/zephyr/v2n_supervisor.c, whose
# acquire() performs an actual GD32 SPI/I2C handshake and can only ever
# return ALP_ERR_NOT_READY on native_sim (no bus/GD32 emulation) -- exactly
# the vacuous-skip trap noted for the existing
# alp_sdk.peripheral.v2n_supervisor scenario. Stubbing the supervisor
# acquire/release + gd32g553_pwm_* HAL seam (src/stubs.c) lets the real
# br_open() run to completion so the dispatcher-owned field write is
# actually exercised.
set(SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

target_sources(app PRIVATE
    src/test_period.c
    src/stubs.c
    ${SDK_DIR}/src/backend.c
    ${SDK_DIR}/src/backends/pwm/gd32_bridge.c
)
target_include_directories(app PRIVATE
    ${SDK_DIR}/include
    ${SDK_DIR}/src
    ${SDK_DIR}/src/common
    ${SDK_DIR}/src/backends/pwm
    ${SDK_DIR}/src/zephyr
)
