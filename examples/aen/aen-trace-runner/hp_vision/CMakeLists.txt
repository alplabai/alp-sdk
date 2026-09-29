# SPDX-License-Identifier: Apache-2.0
#
# M55-HP vision app: camera -> letterbox -> Ethos-U55-256 MoveNet -> decode ->
# pose slot (docs/superpowers/specs/2026-09-24-npu-body-control-design.md).
# The production successor to probe/npu (which proves the NPU alone, no
# camera) and probe/camera (which proves the camera alone, on the HE) --
# this app is both together, on the HP, publishing into the same tr_pslot.h
# slot the HE's TR_INPUT_NPU build (../CMakeLists.txt) reads.
#
# alp-sdk-lcd's e1m_evk_rpi_csi shield ships a boards/<qualified-board>.overlay
# for rtss_he only, not rtss_hp -- boards/shields/tr_hp_csi_complete/ is a
# local ordering fix (ponytail; its own header explains why an app-level
# board overlay alone cannot substitute) that supplies the same content
# BEFORE innomaker_cam_ov9281 needs it. Delete tr_hp_csi_complete once
# alp-sdk-lcd ships the real per-board overlay (see that file's header for
# the exact upstream path) and this SHIELD list reverts to just the two
# lines design sec 2 originally called for.
cmake_minimum_required(VERSION 3.20)

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}) # shields live under <root>/boards/shields/ (list_shields.py --board-root)
list(APPEND SHIELD e1m_evk_rpi_csi tr_hp_csi_complete innomaker_cam_ov9281)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(trace_runner_hp_vision LANGUAGES C CXX)

target_sources(app PRIVATE
	src/main.c
	../src/platform/camera.c
	../src/vision/movenet.c
	../src/vision/pose.c
	../src/vision/camera_ae.c
	../src/vision/kp_smooth.c
	../src/ipc/tr_pslot.c
	../src/ipc/tr_cam_view.c
)
target_include_directories(app PRIVATE ../src)
target_compile_options(app PRIVATE -ffp-contract=off)
# The camera's mounting rotation (../src/vision/cam_rot.h; empty = its
# default): this image applies it (pre-process + keypoints) and publishes it
# to the A32 in tr_cam_view_t. 90 <-> 270 is an HP-only rebuild.
set(TR_CAM_ROTATE "" CACHE STRING "Camera rotation to upright, clockwise degrees: 0, 90 or 270 (empty = cam_rot.h default)")
if(NOT TR_CAM_ROTATE STREQUAL "") # not if(TR_CAM_ROTATE): "0" is CMake-false
	if(NOT TR_CAM_ROTATE MATCHES "^(0|90|270)$")
		message(FATAL_ERROR "TR_CAM_ROTATE must be 0, 90 or 270, got '${TR_CAM_ROTATE}'")
	endif()
	target_compile_definitions(app PRIVATE TR_CAM_ROTATE=${TR_CAM_ROTATE})
endif()
# Selfie mirror of the upright view (../src/vision/cam_rot.h TR_CAM_MIRROR):
# a sensor flip bit set after every camera open, published in tr_cam_view_t.
# HP-only: the A32 and HE follow the mirrored pixels/keypoints unchanged.
option(TR_CAM_MIRROR "Mirror the upright camera view left/right like a selfie (sensor flip bit)" ON)
if(TR_CAM_MIRROR)
	target_compile_definitions(app PRIVATE TR_CAM_MIRROR=1)
else()
	target_compile_definitions(app PRIVATE TR_CAM_MIRROR=0)
endif()
