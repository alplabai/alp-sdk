# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_chips_test LANGUAGES C)

# da9292_internal.h (chips/da9292/) is deliberately NOT installed / not
# part of <alp/chips/da9292.h>'s public surface -- it exists only so
# test_power.c can directly exercise da9292_poll_budget_step(), the
# #757 poll-budget boundary logic the I2C test double's address-keyed
# (not per-register) canned-response model can't reach end-to-end.
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../chips/da9292)

target_sources(app PRIVATE
    src/main.c
    src/test_accelerators.c
    src/test_audio.c
    src/test_blocks.c
    src/test_cameras.c
    src/test_cc3501e.c
    src/test_connectivity.c
    src/test_core_surfaces.c
    src/test_displays.c
    src/test_gd32_bridge.c
    src/test_industrial.c
    src/test_platform_ics.c
    src/test_power.c
    src/test_security.c
    src/test_sensors.c
)

# Fake i2c-emul fixtures (fake_lsm6dso / fake_ssd1306 / fake_bme280)
# are temporarily disabled -- the EMUL_DT_INST_DEFINE pattern they
# use needs a paired DEVICE_DT_INST_DEFINE for Zephyr 3.7's
# native_sim x86 link to resolve `__device_dts_ord_<N>` external
# references for the parent i2c-emul controller's child list.
# Re-enable after sorting that out (tracked as a v0.2 follow-up;
# the fake-using ZTESTs are gated on CONFIG_TEST_FAKES_ENABLED).
#
# target_sources(app PRIVATE
#     src/fake_lsm6dso.c
#     src/fake_ssd1306.c
#     src/fake_bme280.c
# )
