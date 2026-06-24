# SPDX-License-Identifier: Apache-2.0
#
# aen-can-regcheck -- scopeless on-silicon validation of the Alif CAN-FD
# controller staging (zephyr/drivers/can/can_cast.c "cast,can") on the
# E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run + RAM-console flow.
#
# Plain prj.conf app (no alp_project.py board.yaml emit): the driver Kconfig
# (CONFIG_CAN_ALIF_CAST, default y under DT_HAS_CAST_CAN_ENABLED) is sourced
# under CONFIG_ALP_SDK, which prj.conf sets.  The driver TU is the fork's
# can_cast.c, vendored Tier-2 (Apache-2.0); upstream Zephyr's only CAN driver is
# the Bosch M_CAN (can_mcan.c), whose register map does NOT match the CAST IP.
# The overlay enables can0 + defines the alp_can0 alias + supplies pinctrl_can0.
# Live bus traffic is HW-blocked on this batch (no CAN transceiver wired).
# See src/main.c + the board overlay.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_can_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
