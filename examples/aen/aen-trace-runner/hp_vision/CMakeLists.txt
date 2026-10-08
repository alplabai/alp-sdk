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

# The combined HP unit (docs/2026-09-23-sound.md "Sound with the vision HP"): TR_HP_SOUND=ON
# links the game sound -- sound/src/main.c as the GAME firmware (TR_SND_EMBED: its own
# cooperative thread above the vision's), the tr_audio synth, the ring pop, I2S3 -> both TAS2563
# amps and the proven bring-up -- into THIS image, so the Riverdi build plays its music and SFX
# beside the camera + NPU. Only with -DTR_SND_REWORKED_U46=ON (sound_gate.cmake: the i2s3 node
# contends on a stock 74LVC157 U46). Opt-in: without TR_HP_SOUND this image is unchanged. The HE
# is built with the same -DTR_HP_SOUND=ON (../CMakeLists.txt): it leases I2C2 + GPIO5 to this
# core for the amp bring-up and takes them back (src/ipc/tr_bus2.h).
# Release: a32/release/build-release.sh TR_HP_VISION=ON TR_SND_HP=ON (hp_vision_check.sh and
# snd_hp_check.sh run on this one image).
option(TR_SND_REWORKED_U46 "Carrier's U46 I2S mux is a reworked 3257-type switch on +3V3 (sound/CMakeLists.txt)" OFF)
option(TR_HP_SOUND "Game sound (I2S3 + TAS2563) in this image: REWORKED-U46 carriers only (needs the clockctrl-patched ZEPHYR_BASE)" OFF)
include(${CMAKE_CURRENT_LIST_DIR}/sound_gate.cmake)
if(TR_HP_SOUND)
	# set(), not list(APPEND): defining DTC_OVERLAY_FILE switches off Zephyr's pick-up of
	# boards/<board>.overlay, so it is named here. Order matters: the board overlay disables
	# i2c2 and lpgpio, sound.overlay turns gpio5 / lpgpio / i2s3 / SPI1 on, sound_hp.overlay
	# turns i2c2 back on (deferred) and orders the IRQ priorities.
	set(DTC_OVERLAY_FILE
		${CMAKE_CURRENT_LIST_DIR}/boards/alp_e1m_aen803_m55_hp_ae822fa0e5597ls0_rtss_hp.overlay
		${CMAKE_CURRENT_LIST_DIR}/../sound/sound.overlay
		${CMAKE_CURRENT_LIST_DIR}/sound_hp.overlay)
	list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/sound.conf)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(trace_runner_hp_vision LANGUAGES C CXX)

if(TR_HP_SOUND)
	# sound/CMakeLists.txt's refusal, in effect: I2S3 runs off the CGU 76.8 MHz audio source only
	# alp-sdk's clockctrl patch turns on; unpatched, the build is clean and nothing ever clocks out.
	file(READ ${ZEPHYR_BASE}/drivers/clock_control/clock_control_alif.c _tr_clkctl)
	string(FIND "${_tr_clkctl}" "ALIF_EXPMST0_CTRL_REG_OFF" _tr_patched)
	if(_tr_patched EQUAL -1)
		message(FATAL_ERROR "${ZEPHYR_BASE}/drivers/clock_control/clock_control_alif.c lacks alp-sdk's "
			"clockctrl patch (no 76.8 MHz audio clock for I2S3). TR_HP_SOUND=ON needs a patched tree "
			"(zephyr/patches/zephyr/0001-clock_control_alif-*), docs/2026-09-23-sound.md.")
	endif()
	target_sources(app PRIVATE
		../sound/src/main.c
		../sound/src/cc3501e_bridge.c
		../sound/src/cc3501e_gpio_routes.c
		../src/audio/tr_audio.c
		../src/audio/tr_amp_ready.c
		../src/ipc/tr_aring.c
		../src/ipc/tr_bus2.c
	)
	target_include_directories(app PRIVATE ../sound/src)
	# The game's sound, as the standalone GAME image builds it by default (sound/CMakeLists.txt:
	# V2 + V3 voicing, 16 kHz, round-4 amp config = no override writes): the synth defines are
	# the same bits the host goldens pin.
	set(TR_SND_VOLUME "" CACHE STRING "Digital volume 0..128 (empty = 128; sound/src/main.c caps it)")
	set(TR_SND_AMP_REGS_FILE NONE CACHE STRING "Header defining TR_SND_AMP_REGS (extra TAS2563 writes), or NONE")
	target_compile_definitions(app PRIVATE TR_HP_SOUND=1 TR_SND_EMBED=1 TR_SND_TEST=0 TR_SND_MIC=0
		TR_AUDIO_V2=1 TR_AUDIO_V3=1 TR_AUDIO_RATE=16000u)
	if(TR_SND_VOLUME)
		target_compile_definitions(app PRIVATE TR_SND_VOLUME=${TR_SND_VOLUME}u)
	endif()
	if(TR_SND_AMP_REGS_FILE AND NOT TR_SND_AMP_REGS_FILE STREQUAL "NONE")
		target_compile_definitions(app PRIVATE TR_SND_AMP_REGS_FILE="${TR_SND_AMP_REGS_FILE}")
	endif()
endif()

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
