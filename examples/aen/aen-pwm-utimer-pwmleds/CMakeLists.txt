# SPDX-License-Identifier: Apache-2.0
#
# aen-pwm-utimer-pwmleds -- on-silicon (scopeless) validation of
# zephyr/drivers/pwm/pwm_alif_utimer.c on the E1M-AEN801 (Ensemble E8, M55-HE).
#
# This is the CONSUMER-NODE variant of aen-pwm-utimer-regcheck.  It obtains the
# PWM controller device through a `pwm-leds` consumer node + PWM_DT_SPEC_GET,
# instead of DEVICE_DT_GET() directly on the pwm3 controller node.  Doing
# DEVICE_DT_GET() on the pwm3 child triggers a phantom `pwm3_P_pwms_IDX_0`
# codegen reference, because the "alif,pwm" binding re-declares #pwm-cells and
# the gen_defines pass then expects a `pwms` property on the controller itself.
# Resolving the device via the consumer's `pwms` phandle sidesteps that -- this
# is how the earlier board build worked.
#
# Standalone Zephyr app (no alp_project.py board.yaml codegen): the test drives
# the raw Zephyr PWM API + reads back the UTIMER registers, so it needs no
# <alp/*> portable surface.  Build per the bench recipe with
#   -DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>
# and
#   -DEXTRA_DTC_OVERLAY_FILE=<this app>/boards/alp_e1m_aen801_m55_he.overlay

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_pwm_utimer_pwmleds LANGUAGES C)

target_sources(app PRIVATE src/main.c)
