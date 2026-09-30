# SPDX-License-Identifier: Apache-2.0
#
# Camera bring-up diagnostic probe (see README.md). Standalone Zephyr app --
# does NOT touch the game under src/, tests/, art/, tools/ or the top-level
# CMakeLists.txt. Answers: which step of camera open fails on the
# E1M-AEN803 2026W36-0009 bench, and with what raw errno -- <alp/camera.h>'s
# zephyr_video.c backend collapses every failure into ALP_ERR_IO, so this
# calls the same Zephyr video/I2C calls directly to see through that.

cmake_minimum_required(VERSION 3.20)

# Carrier connector shield + sensor shield only -- no display shield. This
# is the simplest configuration that can still reproduce the failure.
list(APPEND SHIELD e1m_evk_rpi_csi innomaker_cam_ov9281)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(trace_runner_camera_probe LANGUAGES C)

target_sources(app PRIVATE src/main.c)
