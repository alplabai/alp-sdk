# SPDX-License-Identifier: Apache-2.0
#
# aen-dac-regcheck -- scopeless on-silicon validation of the Alif DAC driver
# (zephyr/drivers/dac/dac_alif.c, compatible "alif,dac") on the E1M-AEN801
# (Ensemble E8, M55-HE), via the bench RAM-run + RAM-console flow.
#
# Plain prj.conf app (no alp_project.py board.yaml emit): the driver Kconfig
# (CONFIG_DAC_ALIF, default y, depends on DT_HAS_ALIF_DAC_ENABLED) is sourced
# under CONFIG_ALP_SDK, which prj.conf sets.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_dac_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
