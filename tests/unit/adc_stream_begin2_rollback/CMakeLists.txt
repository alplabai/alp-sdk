# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_adc_stream_begin2_rollback)

# Same harness as tests/unit/adc_stream_close_slot (the real peripheral_adc.c
# against the scripted supervisor / GD32 model in that test's src/stubs.c),
# in its OWN binary because the slot bookkeeping in peripheral_adc.c is
# process-global: the BEGIN2-rollback test leaves slot 0 reserved for good
# (that is the behaviour under test) and must not share a process with tests
# that need both slots.  CONFIG_ALP_SDK_DSP=1 additionally enables the
# spectrum path so the discontinuity reset is reachable.
set(SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(SHARED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../adc_stream_close_slot/src)

target_sources(app PRIVATE
    src/test_adc_stream_begin2_rollback.c
    ${SHARED_DIR}/stubs.c
    ${SDK_DIR}/src/zephyr/peripheral_adc.c
    ${SDK_DIR}/src/zephyr/handles.c
    ${SDK_DIR}/src/common/alp_slot_claim.c
)
target_compile_definitions(app PRIVATE
    CONFIG_ALP_SDK_V2N_SUPERVISOR=1
    CONFIG_ALP_SDK_DSP=1
    CONFIG_ALP_SDK_MAX_ADC_STREAM_HANDLES=1
)
target_include_directories(app PRIVATE
    ${SDK_DIR}/include
    ${SDK_DIR}/src/zephyr
    ${SDK_DIR}/src/common
    ${SHARED_DIR}
)
