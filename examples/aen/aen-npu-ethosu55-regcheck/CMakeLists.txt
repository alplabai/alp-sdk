# SPDX-License-Identifier: Apache-2.0
#
# aen-npu-ethosu55-regcheck -- on-silicon presence/clock probe of the HE-core-
# local Ethos-U55 (128-MAC) on the E1M-AEN801 (Ensemble E8, M55-HE).  Sibling of
# aen-npu-ethosu-regcheck (which targets the global Ethos-U85 @ 0x49042000); the
# U85 and U55 are a single Kconfig `choice`, so they are SEPARATE builds.
#
# Build per the bench recipe with the Ethos-U core driver module added:
#   -DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>;<hal_ethos_u>
#   -DEXTRA_DTC_OVERLAY_FILE=<this app>/app.overlay
cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_npu_ethosu55_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
