# SPDX-License-Identifier: Apache-2.0
#
# aen-npu-ethosu-regcheck -- scopeless on-silicon presence/clock validation of the
# Arm Ethos-U85 NPU (compatible "arm,ethos-u", ethosu85 @ 0x49042000) on the
# E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run + RAM-console flow.
#
# Plain prj.conf app: the Ethos-U core driver (CONFIG_ETHOS_U) and the arm
# frontend (CONFIG_ETHOS_U_ARM, default y when the arm,ethos-u node is enabled)
# are upstream Zephyr + the hal_ethos_u module -- NOT gated under CONFIG_ALP_SDK.
#
# The NPU node is supplied by the app overlay (app.overlay), passed at build time
# via -DEXTRA_DTC_OVERLAY_FILE.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_npu_ethosu_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
