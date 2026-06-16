# SPDX-License-Identifier: Apache-2.0
#
# aen-pwm-utimer-regcheck -- on-silicon (scopeless) validation of
# zephyr/drivers/pwm/pwm_alif_utimer.c.  Standalone Zephyr app (no alp_project.py
# board.yaml codegen): the test drives the raw Zephyr PWM API + reads back the
# UTIMER registers, so it needs no <alp/*> portable surface.  Build per the bench
# recipe with -DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>
# and -DEXTRA_DTC_OVERLAY_FILE=<this app>/boards/alp_e1m_aen801_m55_he.overlay.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_pwm_utimer_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
