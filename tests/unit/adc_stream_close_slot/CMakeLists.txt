# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_adc_stream_close_slot)

# Links the REAL src/zephyr/peripheral_adc.c (+ its handle pool and slot-claim
# helpers) WITHOUT the alp-sdk Zephyr module -- same pattern as
# tests/unit/pwm_gd32_bridge_period.  Linking the module would pull in the
# real v2n_supervisor.c, whose acquire() needs a live GD32 handshake that
# native_sim cannot provide; src/stubs.c instead scripts the supervisor
# acquire result and models the GD32's per-slot stream state, which is
# exactly what the close / open-rollback slot bookkeeping depends on.
#
# The two CONFIG_ symbols are normally Kconfig-driven by the module; set
# them here as plain defines since the module (and its Kconfig) is absent.
# A pool of ONE handle makes the open-rollback (handle pool exhausted after
# STREAM_BEGIN succeeded) reachable deterministically.
set(SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

target_sources(app PRIVATE
    src/test_adc_stream_close_slot.c
    src/stubs.c
    ${SDK_DIR}/src/zephyr/peripheral_adc.c
    ${SDK_DIR}/src/zephyr/handles.c
    ${SDK_DIR}/src/common/alp_slot_claim.c
)
target_compile_definitions(app PRIVATE
    CONFIG_ALP_SDK_V2N_SUPERVISOR=1
    CONFIG_ALP_SDK_MAX_ADC_STREAM_HANDLES=1
)
target_include_directories(app PRIVATE
    ${SDK_DIR}/include
    ${SDK_DIR}/src/zephyr
    ${SDK_DIR}/src/common
)
