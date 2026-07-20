# SPDX-License-Identifier: Apache-2.0
#
# aen-analog-validate -- DAC0 -> ADC loopback on-silicon analog validation for
# the E1M-AEN801 (Ensemble E8, M55-HE), driven through the SoM-portable
# <alp/dac.h> + <alp/adc.h> surface.
#
# board.yaml-driven app (like aen-eeprom-manifest): scripts/alp_project.py
# derives the CONFIG_* set (CONFIG_ADC, CONFIG_ALP_SOC_ALIF_ENSEMBLE_E8, ...)
# from board.yaml and layers it over prj.conf via OVERLAY_CONFIG.  The
# device-only DT wiring (dac0 + adc12_0 + the alp-dac0/alp-adc0 aliases) lives in
# boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay, which Zephyr
# auto-applies by the FULLY-QUALIFIED board name (bare-name overlays are
# silently dropped by `west build -b alp_e1m_aen801_m55_he/.../rtss_he`).

cmake_minimum_required(VERSION 3.20)

if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_alp_generated ${CMAKE_BINARY_DIR}/generated/alp.conf)
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${ALP_SDK_ROOT}/scripts/alp_project.py
            --input ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
            --emit zephyr-conf --core m55_he
            --output ${_alp_generated}
    RESULT_VARIABLE _alp_rv
    OUTPUT_VARIABLE _alp_stdout
    ERROR_VARIABLE  _alp_stderr
)
if(NOT _alp_rv EQUAL 0)
    message(FATAL_ERROR
        "alp_project.py failed (rv=${_alp_rv}); "
        "check ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml.\n"
        "stderr: ${_alp_stderr}")
endif()

list(APPEND OVERLAY_CONFIG ${_alp_generated})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_analog_validate LANGUAGES C)

target_sources(app PRIVATE src/main.c)
