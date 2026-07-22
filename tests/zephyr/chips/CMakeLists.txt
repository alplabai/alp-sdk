# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_chips_test LANGUAGES C)

# da9292_internal.h (chips/da9292/) is deliberately NOT installed / not
# part of <alp/chips/da9292.h>'s public surface -- it exists only so
# test_power.c can directly exercise da9292_poll_budget_step(), the
# #757 poll-budget boundary logic the I2C test double's address-keyed
# (not per-register) canned-response model can't reach end-to-end.
#
# bme280_internal.h (chips/bme280/) is the same pattern -- it exists
# only so test_sensors.c can directly exercise bme280_unpack_h4_h5(),
# the #797 H4/H5 shared-nibble unpack the disabled i2c-emul fake can't
# reach (see the fake-fixtures note below).
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../chips/da9292
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../chips/bme280)

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
# are compiled out by maintainer decision (commit 4b0b33d5): driver
# register-protocol validation happens on real silicon via
# examples/v2n/v2n-brd-i2c-bringup, not against these doubles (see
# CHANGELOG.md, "Removed -- chips: fake-based register tests"). The
# fake-using ZTESTs stay in tree but are gated on
# DT_NODE_EXISTS(DT_NODELABEL(fake_lsm6dso)) so they compile to
# no-ops while the fake_* nodes below stay commented out.
#
# target_sources(app PRIVATE
#     src/fake_lsm6dso.c
#     src/fake_ssd1306.c
#     src/fake_bme280.c
# )
