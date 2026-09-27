# SPDX-License-Identifier: Apache-2.0
#
# aen-isp-regcheck -- scopeless on-silicon validation of the Alif ISP-Pico
# (Verisilicon ISP Nano "Pico", zephyr/drivers/video/isp_pico.c, compatible
# "vsi,isp-pico") staging on the E1M-AEN801 (Ensemble E8, M55-HE), via the bench
# RAM-run + RAM-console flow.
#
# Plain prj.conf app (no alp_project.py board.yaml emit).  This regcheck enables
# CONFIG_VIDEO_ISP_VSI=y -- the isp_pico.c driver TU COMPILES + LINKS against
# hal_alif v2.3.0 (isp-vsi.h + isp_ctrl_params.h vendored VERBATIM into
# zephyr/include/zephyr/drivers/video/, see the driver file header) -- and
# (stage 1, on real hardware) streams one frame off the ISP's self-contained
# Test-Pattern Generator through the MI DMA: no camera sensor is wired on
# this hardware batch, but the TPG needs none.  Live SENSOR capture
# (camera->csi->isp->memory) stays BENCH-BLOCKED.  See src/main.c + the
# board overlays.

cmake_minimum_required(VERSION 3.20)

# No DTC_OVERLAY_FILE: Zephyr applies boards/<fully-qualified-board>.overlay
# by itself, so the AEN801 and AEN803 targets each read their own file.

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_isp_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
