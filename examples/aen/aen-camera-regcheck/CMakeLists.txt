# SPDX-License-Identifier: Apache-2.0
#
# aen-camera-regcheck -- scopeless on-silicon validation of the Alif CAMERA
# CAPTURE stack staging (zephyr/drivers/video/video_alif.c "alif,cam" + the
# DesignWare CSI-2 bridge + D-PHY + the ON Semi ARX3A0 sensor) on the
# E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run + RAM-console flow.
#
# Plain prj.conf app (no alp_project.py board.yaml emit): the driver Kconfigs
# (CONFIG_VIDEO_ALIF_CAM et al., default y under DT_HAS_*_ENABLED) are sourced
# under CONFIG_ALP_SDK, which prj.conf sets.  The driver TUs are now PORTED to
# the upstream v4.4 video API (the former CONFIG_ALP_VIDEO_ALIF_BROKEN gate is
# RETIRED), so they build in when their DT node is enabled.  The overlay enables
# csi/dphy/arx3a0 (which instantiate on v4.4); cam compiles but stays disabled
# (instantiation needs the fork-only global-base + a cam->csi graph -- task #21).
# Live frame capture is HW-blocked on this batch (no camera sensor wired).
# See src/main.c + the board overlay.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_camera_regcheck LANGUAGES C)

target_sources(app PRIVATE src/main.c)
