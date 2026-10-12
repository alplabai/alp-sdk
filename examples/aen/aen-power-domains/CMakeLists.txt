# SPDX-License-Identifier: Apache-2.0
#
# aen-power-domains -- bench RAM-run proof of the SoM power-domain runtime
# (#2784, unit U5) on the E1M-AEN803 (Alif Ensemble E8, M55-HE): quiesce every
# on-module domain, hold, restore, and check each chip answers again.
#
# Plain prj.conf app (no alp_project.py board.yaml flow); Zephyr applies
# boards/<fully-qualified-board>.overlay by itself.  Build with
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_power_domains LANGUAGES C)

# The quiesce / restore pair and the chip-hook binding are SDK-internal (the
# portable surface is only the three alp_power_domain_* / boot_wake_info calls),
# so this bench app reaches the internal header directly.
get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
target_include_directories(app PRIVATE ${ALP_SDK_ROOT}/src/backends/power)

target_sources(app PRIVATE src/main.c)
# CC3501E bring-up template (cc3501e_bridge_bringup), same file the sibling
# aen-cc3501e-* examples carry.
target_sources(app PRIVATE src/cc3501e_bridge.c)
