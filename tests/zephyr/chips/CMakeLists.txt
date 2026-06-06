# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_chips_test LANGUAGES C)

target_sources(app PRIVATE src/main.c)

# Fake i2c-emul fixtures.  Each fake_<chip>.c registers an
# EMUL_DT_INST_DEFINE target under the i2c0_emul controller (see
# boards/native_sim_native_64.overlay) so the chip drivers exercise
# real register transfers on native_sim.
target_sources(app PRIVATE
    src/fake_lsm6dso.c
    src/fake_ssd1306.c
    src/fake_bme280.c
)
