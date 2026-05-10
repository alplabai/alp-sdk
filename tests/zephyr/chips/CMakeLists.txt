# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_chips_test LANGUAGES C)

target_sources(app PRIVATE src/main.c)

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
