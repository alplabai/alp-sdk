# SPDX-License-Identifier: Apache-2.0
#
# aen-analog-validate -- DAC0 -> ADC loopback on-silicon analog validation for
# the E1M-AEN801 (Ensemble E8, M55-HE), driven through the SoM-portable
# <alp/dac.h> + <alp/adc.h> surface.
#
# board.yaml-driven app (like aen-eeprom-manifest): `alp_sdk_zephyr_conf()`
# derives the CONFIG_* set (CONFIG_ADC, CONFIG_ALP_SOC_ALIF_ENSEMBLE_E8, ...)
# from board.yaml and layers it over prj.conf via EXTRA_CONF_FILE.  The
# device-only DT wiring (dac0 + adc12_0 + the alp-dac0/alp-adc0 aliases) lives in
# boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay, which Zephyr
# auto-applies by the FULLY-QUALIFIED board name (bare-name overlays are
# silently dropped by `west build -b alp_e1m_aen801_m55_he/.../rtss_he`).

cmake_minimum_required(VERSION 3.20)

# board.yaml -> build/generated/alp.conf at configure time, layered
# over prj.conf via EXTRA_CONF_FILE.  Both the emit and the append
# MUST happen before find_package(Zephyr) -- cmake/alp.cmake documents
# why, and the ALP_SDK_ROOT contract a copied-out project relies on.
if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
endif()
include(${ALP_SDK_ROOT}/cmake/alp.cmake)

alp_sdk_zephyr_conf(m55_he)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_analog_validate LANGUAGES C)

target_sources(app PRIVATE src/main.c)
