cmake_minimum_required(VERSION 3.20)

# The camera is the M55-HP's (hp_vision, TR_INPUT_NPU=ON). The old
# camera-on-the-HE path (TR_CAMERA) had no keypoints, so none of the arm
# controls (lane, jump), left the game unwinnable, and cost a 512 KiB video
# pool at the base of SRAM0, on top of the renderer's back buffer: refused.
option(TR_CAMERA "Removed: use TR_INPUT_NPU=ON (the HP owns the camera)" OFF)
if(TR_CAMERA)
	message(FATAL_ERROR "TR_CAMERA is removed: the HE-side camera and classical detector have no "
			     "keypoints, so no lane or jump control. Use -DTR_INPUT_NPU=ON with the "
			     "hp_vision image (README 'Controls').")
endif()

# NPU body control (docs/superpowers/specs/2026-09-24-npu-body-control-design.md):
# the HE reads the HP's pose slot (src/ipc/tr_pslot.h) instead of running a
# local camera + classical detector -- the M55-HP now owns the camera, the
# Ethos-U NPU and (separately, TR_SND_HP) sound.
option(TR_INPUT_NPU "Read body position from the HP's pose slot instead of a local camera" OFF)
if(TR_INPUT_NPU)
	# The HP owns I2C1 (the camera SCCB bus, 0x49011000) once the HE has
	# finished with it: the HE's display shield may configure a bridge over the
	# same controller at boot (the RVT121's SN65DSI83). The alp,i2c-handover
	# node hands it over -- the HE releases it at the end of its boot, the HP
	# (hp_vision) waits for that before it touches the bus -- instead of this
	# image switching I2C1 off. Any shield, no app code.
	list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/i2c_handover_he.overlay)
endif()

# The HP's game sound (hp_vision/ built with -DTR_HP_SOUND=ON: camera + NPU + the TAS2563 amps in
# ONE HP image, docs/2026-09-23-sound.md "Sound with the vision HP"): the HP's amp bring-up needs
# I2C2 + GPIO5, which this core also uses (INA236 power line, BMI323). With TR_HP_SOUND=ON this
# image offers I2C2 to the HP once its display is up and takes it back when the HP is done
# (src/platform/bus2_he.c, src/ipc/tr_bus2.h); the HUD shows "5V -- mW" while it is away. Needs
# TR_INPUT_NPU (the HP is the vision HP) and a shield that leaves GPIO5 alone (the RVT121).
option(TR_HP_SOUND "Lease I2C2 to the HP's game sound for its amp bring-up (hp_vision TR_HP_SOUND=ON)" OFF)
if(TR_HP_SOUND AND NOT TR_INPUT_NPU)
	message(FATAL_ERROR "TR_HP_SOUND is the vision HP's sound: it needs -DTR_INPUT_NPU=ON")
endif()
if(TR_HP_SOUND)
	# The game sound's volume (src/ipc/tr_vol.h): the EVK's rotary encoder + push switch and the
	# bench's SWD request word. volume_he.overlay maps the pads, volume_he.conf turns the input
	# subsystem on.
	list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/volume_he.overlay)
	list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/volume_he.conf)
endif()

# Who draws the frame. A32 (default): the Cortex-A32 renderer draws every
# pixel; this M55 image runs the game, flips the CDC200 and drives the SRAM1
# mailbox, and links no renderer (docs/superpowers/plans/2026-09-22-a32-renderer.md).
# M55: the original 2D sprite renderer on this core -- the booth fallback when
# no A32 chain is resident; it needs nothing in SRAM1.
set(TR_RENDER A32 CACHE STRING "Frame renderer: A32 or M55")
set_property(CACHE TR_RENDER PROPERTY STRINGS A32 M55)
if(NOT TR_RENDER MATCHES "^(A32|M55)$")
	message(FATAL_ERROR "TR_RENDER must be A32 or M55, got '${TR_RENDER}'")
endif()
if(TR_INPUT_NPU AND TR_RENDER STREQUAL "M55")
	# m55_render.conf sets CONFIG_DCACHE=y for this render path (the on-core
	# 2D renderer keeps the D-cache it was measured with); tr_pslot_read()'s
	# barrier is a DSB only, no cache clean/invalidate, matching the
	# CONFIG_DCACHE=n every other TR_INPUT_NPU/TR_RENDER combination runs
	# under -- a cached read of the HP's pose slot (SRAM0 0x0237F200) could
	# silently serve a stale line. Reviewer finding, fix round 1: block the
	# combination rather than add cache maintenance nothing has exercised.
	message(FATAL_ERROR "TR_INPUT_NPU needs TR_RENDER=A32 -- TR_RENDER=M55 runs CONFIG_DCACHE=y "
			     "(m55_render.conf) and the pose slot has no cache maintenance")
endif()
if(TR_RENDER STREQUAL "M55")
	# prj.conf pins CONFIG_DCACHE=n for the A32 link (the M55 must see the
	# mailbox uncached); the M55 renderer keeps the D-cache it was measured
	# with -- display.c cleans every line it writes.
	list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/m55_render.conf)
endif()

# Release flow only: the M55 LAUNCHes the renderer at boot. The dev flow
# leaves this OFF -- the bench LAUNCHes over the debug AP (plan section 5).
option(TR_M55_AUTOLAUNCH "M55 issues LAUNCH at boot (release flow)" OFF)
set(TR_A32_LAUNCH_H "" CACHE FILEPATH
	"Header defining TR_A32_ENTRY, TR_A32_LEN, TR_A32_CRC (needed by TR_M55_AUTOLAUNCH)")
if(TR_M55_AUTOLAUNCH AND NOT EXISTS "${TR_A32_LAUNCH_H}")
	message(FATAL_ERROR "TR_M55_AUTOLAUNCH=ON needs -DTR_A32_LAUNCH_H=<header with TR_A32_ENTRY/LEN/CRC>")
endif()

# The display is a shield: -DSHIELD=<name> picks it, the RK055 panel by default.
# Nothing else in this app names a panel -- its timings (refresh), geometry,
# mount-rotation and backlight all come from the shield's devicetree, through
# the SDK's display API. A shield whose panel does not scan the game's
# 720 x 1280 portrait content as-is gets a fit overlay of the same name under
# shield-fit/ (layer-1 window), applied automatically. A different refresh is an
# extra overlay (panel_30hz.overlay for the RK055), passed with
# -DEXTRA_DTC_OVERLAY_FILE=.
if(NOT SHIELD)
	set(SHIELD e1m_evk_rk055hdmipi4ma0)
endif()
foreach(shield IN LISTS SHIELD)
	if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/shield-fit/${shield}.overlay)
		list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/shield-fit/${shield}.overlay)
	endif()
	# The shield's bridge recovery recipe, only where the HE hands I2C1 to the HP (TR_INPUT_NPU).
	if(TR_INPUT_NPU AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/shield-fit-npu/${shield}.overlay)
		list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/shield-fit-npu/${shield}.overlay)
	endif()
endforeach()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(trace_runner LANGUAGES C)

target_sources(app PRIVATE
	src/main.c
	src/game/attract.c
	src/game/hiscore.c
	src/game/tilt.c
	src/game/mode.c
	src/game/react.c
	src/game/score.c
	src/game/state.c
	src/game/step.c
	src/platform/imu.c
	src/platform/rail5v_power.c
	src/vision/arms.c
	src/vision/pose.c
	src/vision/track.c
)
# Why did the HE restart? A fatal error record + the console tail of the previous boot, kept in the
# shared SRAM0 page and printed at the next boot (src/ipc/tr_he_fault.h, src/platform/he_fault.c).
option(TR_HE_FAULT_LOG "Keep the HE's last fatal error and console tail across a restart, print them at boot" ON)
if(TR_HE_FAULT_LOG)
	target_sources(app PRIVATE src/platform/he_fault.c src/ipc/tr_he_fault.c)
endif()
# After a fatal error the HE asks the Secure Enclave to reset the whole SoC (bounded, direct MHUv2 send,
# src/platform/he_fault.c) instead of halting; the third quick fatal error in a row still halts
# (src/ipc/tr_reset_guard.h). OFF: a fatal error halts the HE, as Zephyr's default does. Needs TR_HE_FAULT_LOG.
option(TR_HE_FAULT_SOC_RESET "HE fatal error -> SE whole-SoC reset (loop-guarded) instead of a halt" ON)
if(TR_HE_FAULT_LOG)
	target_compile_definitions(app PRIVATE TR_HE_FAULT_SOC_RESET=$<BOOL:${TR_HE_FAULT_SOC_RESET}>)
endif()
# BENCH ONLY: the HE panics on demand once the bench writes tr_fault_inject_t over SWD to
# TR_MEM_FAULT_INJECT (src/ipc/tr_fault_inject.h). Inert until the magic is written. Default ON for the
# exhibition build so the SoC-reset loop guard can be tested on the real unit; turn OFF for a
# customer-facing build.
option(TR_BENCH_FAULT_INJECT "BENCH ONLY: SWD-armed fatal-error injector (inert until armed)" ON)
target_compile_definitions(app PRIVATE TR_BENCH_FAULT_INJECT=$<BOOL:${TR_BENCH_FAULT_INJECT}>)
if(TR_HP_SOUND)
	target_sources(app PRIVATE src/platform/bus2_he.c src/ipc/tr_bus2.c
		src/platform/volume_he.c src/ipc/tr_vol.c src/ipc/tr_knob.c)
	# src/platform/tr_i2c2_rearm.h resets the i2c_dw driver's sync semaphore (struct
	# i2c_dw_dev_config in drivers/i2c/i2c_dw.h, not a public header).
	target_include_directories(app PRIVATE ${ZEPHYR_BASE}/drivers/i2c)
	target_compile_definitions(app PRIVATE TR_HP_SOUND=1)
endif()
if(TR_INPUT_NPU)
	target_sources(app PRIVATE src/ipc/tr_pslot.c src/ipc/tr_cam_view.c)
	target_compile_definitions(app PRIVATE TR_INPUT_NPU=1)
	# The camera's mounting rotation (src/vision/cam_rot.h; empty = its
	# default). The HE only needs 0 vs 90/270 (the upright frame's
	# dimensions): keep it the same as the HP build's.
	set(TR_CAM_ROTATE "" CACHE STRING "Camera rotation to upright, clockwise degrees: 0, 90 or 270 (empty = cam_rot.h default)")
	if(NOT TR_CAM_ROTATE STREQUAL "") # not if(TR_CAM_ROTATE): "0" is CMake-false
		if(NOT TR_CAM_ROTATE MATCHES "^(0|90|270)$")
			message(FATAL_ERROR "TR_CAM_ROTATE must be 0, 90 or 270, got '${TR_CAM_ROTATE}'")
		endif()
		target_compile_definitions(app PRIVATE TR_CAM_ROTATE=${TR_CAM_ROTATE})
	endif()
	# Whether the HP mirrors the upright view like a selfie (cam_rot.h
	# TR_CAM_MIRROR; the HP's own option of the same name). The arm controls
	# need it: with the mirror the arm on the screen's left is the player's
	# LEFT arm, without it their RIGHT (src/vision/pose.c). Set it exactly as
	# the HP build -- build-release.sh refuses a pair that disagrees.
	option(TR_CAM_MIRROR "The HP mirrors the upright camera view like a selfie (match the hp_vision build)" ON)
	if(TR_CAM_MIRROR)
		target_compile_definitions(app PRIVATE TR_CAM_MIRROR=1)
	else()
		target_compile_definitions(app PRIVATE TR_CAM_MIRROR=0)
	endif()
else()
	target_sources(app PRIVATE src/platform/camera.c src/vision/detect.c)
	target_compile_definitions(app PRIVATE TR_INPUT_NPU=0)
endif()
# Bench/dev only: a deliberate IMU tilt takes over camera-less attract
# (src/game/tilt.h). OFF for exhibition builds -- the board is mounted and
# nobody tilts it (docs/2026-09-22-exhibition-requirements.md).
option(TR_TILT_TAKEOVER "IMU tilt takes over camera-less attract (bench/dev)" OFF)
if(TR_TILT_TAKEOVER)
	target_compile_definitions(app PRIVATE TR_TILT_TAKEOVER=1)
endif()
if(TR_RENDER STREQUAL "A32")
	target_sources(app PRIVATE
		src/game/sfx.c
		src/ipc/tr_aring.c
		src/ipc/tr_flip.c
		src/ipc/tr_wd.c
		src/ipc/tr_mbox.c
		src/platform/a32.c
		src/platform/display_a32.c
		src/platform/hud_l2.c
		src/hud/hud.c
	)
	target_compile_definitions(app PRIVATE TR_RENDER_A32=1)
	# FB B defaults to SRAM1 0x025EA000 (src/ipc/tr_mbox.h). Override only to
	# pair with the old stub-table test payloads (payload-echo/-colorbar):
	# -DTR_FB_B_ADDR=0x02200000u (the shield's lcd_fb).
	set(TR_FB_B_ADDR "" CACHE STRING "Override TR_FB_B (A32 render only; empty = 0x025EA000)")
	if(TR_FB_B_ADDR)
		target_compile_definitions(app PRIVATE TR_FB_B_ADDR=${TR_FB_B_ADDR})
	endif()
	if(TR_M55_AUTOLAUNCH)
		target_compile_definitions(app PRIVATE TR_M55_AUTOLAUNCH=1 TR_A32_LAUNCH_H="${TR_A32_LAUNCH_H}")
	endif()
else()
	target_sources(app PRIVATE
		src/platform/display.c
		src/render/render.c
		src/render/sprite.c
	)
	target_compile_definitions(app PRIVATE TR_RENDER_A32=0)
endif()
target_include_directories(app PRIVATE src)

# No fused multiply-add: float code must compute the same bits as the host
# tests (tests/host/runner.sh passes the same flag; see the plan's section 5).
target_compile_options(app PRIVATE -ffp-contract=off)

# SDK GAP (see src/platform/display.c's header comment): <alp/display.h> has no
# flip/page-swap op, so display.c calls the CDC200 driver's own device-specific
# cdc200_swap_fb()/cdc200_get_framebuffer()/restore_fb() extension.  Those are
# declared in the driver's private header, which the module does not export.
# Including the real header rather than re-declaring the prototypes here means a
# driver-side change breaks the BUILD instead of silently corrupting a swap
# descriptor.  Delete this line when the alp-sdk surface grows a flip op.
target_include_directories(app PRIVATE ${ZEPHYR_ALP_SDK_MODULE_DIR}/zephyr/drivers/display)
