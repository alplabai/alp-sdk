# SPDX-License-Identifier: Apache-2.0
#
# aen-isp-capture -- a real OV5647 sensor frame through the ISP
# (sensor -> csi -> cam -> isp -> memory), mirroring Alif's own sdk-alif
# samples/drivers/viewfinder recipe, with AE + AWB running through
# isp_pico.c's standard Zephyr video ctrl path. Build with the SAME two
# shields aen-camera-firstlight's OV5647 scenario uses:
#   -DSHIELD="e1m_evk_rpi_csi raspberry_pi_camera_module_1"
# See testcase.yaml for the exact scenario and README.md (if present) for the
# build command / expected results.

cmake_minimum_required(VERSION 3.20)

# Apply the board overlay explicitly -- Zephyr's automatic
# boards/<name>.overlay pickup keys off the FULLY-QUALIFIED board name
# (alp_e1m_aen80{1,3}_m55_he_ae822fa0e5597ls0_rtss_he), not the bare board
# name (see aen-camera-firstlight/CMakeLists.txt for the same note); a plain
# `west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` would
# otherwise drop the ITCM RAM-run retarget both overlays carry.
if(BOARD MATCHES "^alp_e1m_aen801_m55_he")
    set(DTC_OVERLAY_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay")
elseif(BOARD MATCHES "^alp_e1m_aen803_m55_he")
    set(DTC_OVERLAY_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he.overlay")
endif()

# Opt-in IMX296 variant (issue #2287 Stage B): -DAEN_ISP_IMX296=ON selects IMX296's ROI
# (1280x960 SRGGB10P) input instead of OV5647's native 640x480 SBGGR10P -- see src/main.c's
# FRAME_WIDTH/HEIGHT/N_FRAMES/ISP_INPUT_FOURCC #if ladder, which is the only other place this
# define is read. OFF by default: a normal build stays the OV5647 path this example was proven
# against. Needs the raspberry_pi_global_shutter_camera shield in place of
# raspberry_pi_camera_module_1 (see testcase.yaml's imx296 scenario) and hal_alif patch 0012
# (zephyr/patches/hal_alif/0012-isp-srggb10p-input.patch) for the ISP wrapper's SRGGB10P mapping.
#   -DAEN_ISP_IMX296=ON
option(AEN_ISP_IMX296 "Run IMX296's ROI crop through the ISP instead of OV5647" OFF)
if(AEN_ISP_IMX296)
    # overlay-imx296.conf: pool sizing for IMX296's larger (1280x960 vs. OV5647's 640x480)
    # buffers -- see that file's own comment and bench run 296 (a NULL video_buffer_aligned_
    # alloc() for the second buffer under the unchanged, OV5647-sized default pool). Appended,
    # not set, so it composes with any EXTRA_CONF_FILE the build command also passes (e.g.
    # overlay-no-ae.conf, testcase.yaml's imx296 scenario).
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/overlay-imx296.conf")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_isp_capture LANGUAGES C)

target_sources(app PRIVATE src/main.c)

if(AEN_ISP_IMX296)
    target_compile_definitions(app PRIVATE AEN_ISP_IMX296=1)
endif()

# Bench run 298 diagnostic: AE never moved on the IMX296 AE-on scenario. Being able to run an
# AE-OFF 60-frame IMX296 capture (rather than only the original 1-frame first-light proof) isolates
# whether the flat readback is specific to the AE loop or would also show up on a longer manual-
# exposure run -- not implied by CONFIG_ISP_LIB_AE_MODULE the way N_FRAMES's AE-on/off default is
# (src/main.c's own N_FRAMES #define), so this is a separate, explicit override:
#   -DAEN_ISP_N_FRAMES=60
# Applies to BOTH the AE-on and AE-off IMX296 builds; unset (0) leaves src/main.c's own
# IS_ENABLED(CONFIG_ISP_LIB_AE_MODULE) ? 60 : 1 default in place. No effect on the OV5647 path
# (its own N_FRAMES is a separate, unconditional 30).
set(AEN_ISP_N_FRAMES "0" CACHE STRING "Override N_FRAMES for the IMX296 variant (0 = use the AE-on/off default)")
if(AEN_ISP_IMX296 AND NOT AEN_ISP_N_FRAMES STREQUAL "0")
    target_compile_definitions(app PRIVATE AEN_ISP_N_FRAMES=${AEN_ISP_N_FRAMES})
endif()

# Bench run 303 control captures (#2287 Stage B unit 3): AE now runs (lib Frame 1..61,
# intLine/again writeback confirmed matching SHS/GAIN readback exactly) but the frame stayed pure
# noise even at SHS=4 (1114 lines, the datasheet-permitted exposure ceiling) + gain 0x1E0 (48 dB,
# the AE envelope's own ceiling) -- the SAME symptom raw (non-ISP) captures at SHS=4/gain=480
# showed earlier. SHS and gain are confounded in every failing run so far (both always moved
# together, via AE); these two options independently PIN one manual (exposure lines) + gain (0.1
# dB tenths, this driver's own IMX296 dB-tenths scale -- see isp_sns_gain_conv.h) value on the
# SENSOR directly -- not the ISP's own AE seed (isp_apply_ae(), which only applies with the AE
# library compiled in) -- so a specific SHS/gain combination can be held for a whole capture run
# independent of whatever the AE loop would have converged to. Unset (the default) leaves
# src/main.c's existing behaviour untouched. Only meaningful with AE off (overlay-no-ae.conf) --
# with AE on, isp_apply_ae()'s own per-frame writeback would immediately overwrite whatever these
# set.
#   -DAEN_ISP_IMX296_EXPOSURE_LINES=<n>  (VIDEO_CID_EXPOSURE lines, 1..1114; sentinel 0 = unset --
#                                         0 is never a legal exposure, isp_pico.h's minIntLine=1)
#   -DAEN_ISP_IMX296_GAIN=<n>            (VIDEO_CID_ANALOGUE_GAIN, 0.1 dB tenths, 0..480;
#                                         sentinel -1 = unset -- 0 dB IS a legal, meaningful gain
#                                         value this bench round explicitly wants to hold (build
#                                         C1), so it cannot double as "unset" the way 0 lines can)
set(AEN_ISP_IMX296_EXPOSURE_LINES "0" CACHE STRING "Manual IMX296 exposure (lines) control capture, AE off only (0 = unset)")
set(AEN_ISP_IMX296_GAIN "-1" CACHE STRING "Manual IMX296 gain (0.1 dB tenths) control capture, AE off only (-1 = unset)")
if(AEN_ISP_IMX296 AND NOT AEN_ISP_IMX296_EXPOSURE_LINES STREQUAL "0")
    target_compile_definitions(app PRIVATE AEN_ISP_IMX296_EXPOSURE_LINES=${AEN_ISP_IMX296_EXPOSURE_LINES})
endif()
if(AEN_ISP_IMX296 AND NOT AEN_ISP_IMX296_GAIN STREQUAL "-1")
    target_compile_definitions(app PRIVATE AEN_ISP_IMX296_GAIN=${AEN_ISP_IMX296_GAIN})
endif()
