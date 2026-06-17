# SPDX-License-Identifier: Apache-2.0
#
# aen-analog-validate -- DAC0 -> ADC loopback on-silicon analog validation for
# the E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run + RAM-console flow.
#
# Drives a known code on dac0 (alif,dac, 0.750 V VREF) and reads it back on an
# adc12 instance (alif,adc, 1.8 V VREF), asserting the ADC raw matches the
# VREF-ratio-scaled DAC setpoint within tolerance.  See src/main.c + README.md.
#
# Plain prj.conf app (no alp_project.py board.yaml emit): the driver Kconfigs
# (CONFIG_DAC_ALIF + CONFIG_ADC_ALIF, default y on DT_HAS_*_ENABLED) are sourced
# under CONFIG_ALP_SDK, which prj.conf sets.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_analog_validate LANGUAGES C)

target_sources(app PRIVATE src/main.c)
