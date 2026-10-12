# SPDX-License-Identifier: Apache-2.0
#
# aen-power-stop -- bench proof of the Alif SE STOP backend (#2784, unit U7) on
# the E1M-AEN803 (Alif Ensemble E8, M55-HE; the M55-HP builds too, not bench-verified).  Built as an MRAM image: STOP wakes
# through a cold boot, so the image has to be the one the Secure Enclave boots
# (a RAM-run image would not come back).
#
# Plain prj.conf app (no alp_project.py board.yaml flow); Zephyr applies
# boards/<fully-qualified-board>.overlay by itself; the overlay only enables the GPIO
# controllers of the pads the power layer drives (GPIO5, GPIO11, LPGPIO) -- BRD_I2C and
# the LPTIMER come from the board layer, and it has no ITCM retarget (MRAM image).
# Build with
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_power_stop LANGUAGES C)

# The retention-proof counter and the RV-3028 hook binding are SDK-internal (the
# portable surface is the <alp/power.h> calls this app also makes), so this bench
# app reaches the internal headers directly, like aen-power-domains.
get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
target_include_directories(app PRIVATE ${ALP_SDK_ROOT}/src/backends/power)

target_sources(app PRIVATE src/main.c)
