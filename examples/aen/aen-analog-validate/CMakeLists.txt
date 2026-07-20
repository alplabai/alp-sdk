# SPDX-License-Identifier: Apache-2.0
#
# aen-analog-validate -- DAC0 -> ADC loopback on-silicon analog validation for
# the E1M-AEN801 (Ensemble E8, M55-HE), driven through the SoM-portable
# <alp/dac.h> + <alp/adc.h> surface.
#
# board.yaml-driven app (like aen-eeprom-manifest): the planner derives the
# CONFIG_* set (CONFIG_ADC, CONFIG_ALP_SOC_ALIF_ENSEMBLE_E8, ...) from
# board.yaml and wires it into `west build` via `-DEXTRA_CONF_FILE=...`
# (see docs/adr/0020-sdk-owns-build-execution.md). The
# device-only DT wiring (dac0 + adc12_0 + the alp-dac0/alp-adc0 aliases) lives in
# boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay, which Zephyr
# auto-applies by the FULLY-QUALIFIED board name (bare-name overlays are
# silently dropped by `west build -b alp_e1m_aen801_m55_he/.../rtss_he`).

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_analog_validate LANGUAGES C)

target_sources(app PRIVATE src/main.c)
