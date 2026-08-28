# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_pwm_gd32_bridge_period_refusal)

# Issue #1730: an over-long PWM period must be REFUSED by the bridge, not
# silently clamped to 65.536 ms while reporting success -- and the host-side
# cache (src/backends/pwm/gd32_bridge.c's gd32_pwm_state_t::period_ns) must
# not record a period that was never actually programmed.
#
# Same technique as tests/unit/pwm_gd32_bridge_period (issue #1624): link
# the REAL backend file under test directly (br_open/br_set_period/
# br_set_duty are `static`, so alp_backend_select walks the real registry),
# WITHOUT the alp-sdk Zephyr module -- linking the module would pull in the
# real src/zephyr/v2n_supervisor.c, whose acquire() performs an actual GD32
# SPI/I2C handshake and can only ever return ALP_ERR_NOT_READY on
# native_sim. The supervisor acquire/release + gd32g553_pwm_* HAL seam are
# stubbed in src/stubs.c; the stubbed gd32g553_pwm_set simulates the
# corrected gd32-bridge firmware's refusal (ALP_ERR_NOSUPPORT) for a period
# beyond what the 16-bit prescaled counter can express.
set(SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

target_sources(app PRIVATE
    src/test_period_refusal.c
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
