# SPDX-License-Identifier: Apache-2.0
#
# aen-cc3501e-socket-throughput -- measures CC3501E end-to-end TCP socket
# throughput over a correctly-sized window on the E1M-AEN801 (Ensemble E8,
# M55-HE). See src/main.c for why the window has to be this big.
#
# Standalone Zephyr app (no alp_project.py board.yaml flow), same shape as
# its closest sibling aen-cc3501e-command-sweep: build with
#   ZEPHYR_BASE=<zephyr-base> west build \
#     -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <this-dir> -- \
#     "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
#     -DEXTRA_DTC_OVERLAY_FILE=<this-dir>/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_cc3501e_socket_throughput LANGUAGES C)

target_sources(app PRIVATE src/main.c)
# The SoM bring-up TEMPLATE (see its own header comment): opens the
# inter-chip SPI1 bridge + WIFI_EN/nRESET, enables the LP-pad output
# drivers pinctrl does not reach, binds the READY line, and runs the
# power + reset sequence. Copied verbatim from aen-cc3501e-command-sweep --
# byte-identical to the copies in every other aen-cc3501e-* sibling, same
# reasoning: a copyable template, not a cross-example reach-in.
target_sources(app PRIVATE src/cc3501e_bridge.c)
