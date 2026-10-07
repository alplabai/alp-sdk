cmake_minimum_required(VERSION 3.20)

# The camera is opt-in: -DTR_CAMERA=ON. Default OFF because the board the game
# runs on, the E1M-AEN803 2026W36-0009 EVK, has the display panel and
# no camera on J5 -- the only camera rig is on 2026W36-0001. A camera build also costs
# a 512 KiB video pool at the base of SRAM0 (camera.conf), which is exactly the
# space the renderer's back buffer needs; without it the game double-buffers
# in SRAM0 instead of reaching for SRAM1, which is unpowered at boot.
# Which panel the game runs on. rk055 (default): the RK055HDMIPI4MA0 720x1280
# portrait MIPI-DSI panel (HX8394). rvt121: the Riverdi RVT121HVDFWCA0-B 12.1"
# 1280x800 landscape LVDS panel behind an SN65DSI83 bridge (shield
# e1m_evk_rvt121hvdfwca0). rvt121 is phase 1 only: TR_RENDER=M55, IMU steering,
# no camera, no NPU input (README, "Riverdi RVT121").
set(TR_PANEL rk055 CACHE STRING "Panel: rk055 or rvt121")
set_property(CACHE TR_PANEL PROPERTY STRINGS rk055 rvt121)
if(NOT TR_PANEL MATCHES "^(rk055|rvt121)$")
	message(FATAL_ERROR "TR_PANEL must be rk055 or rvt121, got '${TR_PANEL}'")
endif()

option(TR_CAMERA "Stack the OV9281 camera shields and the video buffer pool" OFF)

# NPU body control (docs/superpowers/specs/2026-09-24-npu-body-control-design.md):
# the HE reads the HP's pose slot (src/ipc/tr_pslot.h) instead of running a
# local camera + classical detector -- the M55-HP now owns the camera, the
# Ethos-U NPU and (separately, TR_SND_HP) sound. Exclusive with TR_CAMERA:
# that option is the OTHER, camera-on-THIS-core path (the TR_RENDER=M55
# booth fallback / 2026W36-0001 rig), and the two body-detection sources are never
# both wanted in one image.
option(TR_INPUT_NPU "Read body position from the HP's pose slot instead of a local camera" OFF)
if(TR_INPUT_NPU AND TR_CAMERA)
	message(FATAL_ERROR "TR_INPUT_NPU and TR_CAMERA are exclusive -- pick one body-detection source")
endif()
if(TR_PANEL STREQUAL "rvt121" AND TR_CAMERA)
	message(FATAL_ERROR "TR_PANEL=rvt121 does not support TR_CAMERA yet (phase 2): the 1280x800 "
			     "RGB565 frame plus the video pool is more than SRAM0")
endif()
if(TR_PANEL STREQUAL "rvt121" AND TR_INPUT_NPU)
	message(FATAL_ERROR "TR_PANEL=rvt121 does not support TR_INPUT_NPU yet (phase 2): the HP "
			     "pose path needs I2C1, which carries the bridge and the touch controller")
endif()
if(TR_INPUT_NPU)
	# The HP now owns I2C1 -- see i2c1_off.overlay's header for why the HE
	# must stop touching the same controller (its own display shield
	# enables it too, on the same pads).
	list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/i2c1_off.overlay)
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
if(TR_PANEL STREQUAL "rvt121" AND NOT TR_RENDER STREQUAL "M55")
	message(FATAL_ERROR "TR_PANEL=rvt121 needs TR_RENDER=M55 (A32 renderer on this panel is phase 2)")
endif()
if(TR_RENDER STREQUAL "A32" AND TR_CAMERA)
	# The camera pool sits at the base of SRAM0 -- on top of TR_FB_A -- until
	# plan task T-A8 moves it to SRAM1 0x02480000.
	message(FATAL_ERROR "TR_CAMERA needs TR_RENDER=M55 until the video pool moves to SRAM1 (plan T-A8)")
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

if(TR_PANEL STREQUAL "rvt121")
	# The shield's SN65DSI83 driver brings the panel up itself: there is no
	# lcd_panel node, so no deferred-init overlay and no HX8394 retry (panel.c).
	list(APPEND SHIELD e1m_evk_rvt121hvdfwca0)
else()
	list(APPEND SHIELD e1m_evk_rk055hdmipi4ma0)
	# The panel inits from main() with retries (src/platform/panel.c), not at boot.
	list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/panel_deferred.overlay)
endif()

# Panel refresh (P11a). 40: the shield's timing. 30: panel_30hz.overlay
# stretches the vertical front porch (same pixel clock and DSI link) for
# 33.3 ms of render time a frame, and the game's frame-counted constants
# scale with it (src/game/panel_hz.h). A32 render only: the M55 renderer
# runs its own TICK_MS loop.
set(TR_PANEL_HZ 40 CACHE STRING "Panel refresh, Hz: 40 or 30")
set_property(CACHE TR_PANEL_HZ PROPERTY STRINGS 40 30)
if(NOT TR_PANEL_HZ MATCHES "^(40|30)$")
	message(FATAL_ERROR "TR_PANEL_HZ must be 40 or 30, got '${TR_PANEL_HZ}'")
endif()
if(TR_PANEL STREQUAL "rvt121")
	# The Riverdi shield natively runs ~30.06 Hz (36.363636 MHz, 1440x840
	# totals); panel_30hz.overlay is RK055-specific and is never applied.
	if(NOT TR_PANEL_HZ STREQUAL "30")
		message(FATAL_ERROR "TR_PANEL=rvt121 runs ~30 Hz: pass -DTR_PANEL_HZ=30")
	endif()
elseif(TR_PANEL_HZ STREQUAL "30")
	if(NOT TR_RENDER STREQUAL "A32")
		message(FATAL_ERROR "TR_PANEL_HZ=30 needs TR_RENDER=A32")
	endif()
	list(APPEND EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/panel_30hz.overlay)
endif()

if(TR_CAMERA)
	# Carrier CSI connector + OV9281 sensor -- the shield string
	# examples/aen/aen-camera-firstlight (PR #2247) verified on the bench.
	list(APPEND SHIELD e1m_evk_rpi_csi innomaker_cam_ov9281)
	list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/camera.conf)
endif()
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
	src/platform/panel.c
	src/platform/rail5v_power.c
	src/vision/pose.c
	src/vision/track.c
)
if(TR_INPUT_NPU)
	target_sources(app PRIVATE src/ipc/tr_pslot.c)
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
	# FB B defaults to SRAM1 0x02600000 (src/ipc/tr_mbox.h). Override only to
	# pair with the old stub-table test payloads (payload-echo/-colorbar):
	# -DTR_FB_B_ADDR=0x02200000u (the shield's lcd_fb).
	set(TR_FB_B_ADDR "" CACHE STRING "Override TR_FB_B (A32 render only; empty = 0x02600000)")
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
target_compile_definitions(app PRIVATE TR_PANEL_HZ=${TR_PANEL_HZ})
if(TR_PANEL STREQUAL "rvt121")
	target_compile_definitions(app PRIVATE TR_PANEL_RVT121=1)
endif()

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
