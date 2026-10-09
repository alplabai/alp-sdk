# sound/ -- Trace Runner game sound: src/audio's synth on I2S3 -> both
# TAS2563 amps. docs/2026-09-23-sound.md has the build/bench recipes.
#
#   TR_SND_MODE=GAME  (default) the M55-HP firmware: drains the HE's event
#                     ring (src/ipc/tr_aring.h, SRAM0 0x0237F000) into the synth.
#   TR_SND_MODE=TEST  standalone sound test: music + a scripted SFX run, a
#                     1 kHz reference tone, and a PDM-mic loopback capture to
#                     SRAM0 0x02000000 with an in-band PASS/FAIL.
cmake_minimum_required(VERSION 3.20)

set(TR_SND_MODE GAME CACHE STRING "GAME (HP, reads the HE ring) or TEST (standalone loopback test)")
set_property(CACHE TR_SND_MODE PROPERTY STRINGS GAME TEST)
if(NOT TR_SND_MODE MATCHES "^(GAME|TEST)$")
	message(FATAL_ERROR "TR_SND_MODE must be GAME or TEST, got '${TR_SND_MODE}'")
endif()

# Enabling i2s3 drives P9_3/4/5. On an EVK 2626-R2 carrier whose U46 is the
# stock 74LVC157 that contends with U46's outputs (no Hi-Z) the moment the
# pins are muxed -- only a carrier reworked to a 3257-type bus switch (VCC on
# +3V3, E1M-AEN803 serial 2026W36-0002's carrier) may run this image.
option(TR_SND_REWORKED_U46 "Carrier's U46 I2S mux is a reworked 3257-type switch on +3V3" OFF)
if(NOT TR_SND_REWORKED_U46)
	message(FATAL_ERROR "sound/ drives I2S3 through U46: build only for a carrier with the "
		"reworked U46 (-DTR_SND_REWORKED_U46=ON). Stock 74LVC157 carriers contend on P9_3/4/5.")
endif()

list(APPEND DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/sound.overlay)
# TEST only: OFF = play-only (no PDM node, no DMIC driver, no capture) -- the
# quickest bench check that I2S3 streams, and a split if PDM + I2S3 together
# ever misbehave.
option(TR_SND_MIC "TEST image also captures the PDM mics (loopback verdict)" ON)
if(TR_SND_MODE STREQUAL "TEST")
	if(TR_SND_MIC)
		list(APPEND DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/pdm.overlay)
		list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/mic.conf)
	endif()
	list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/test.conf)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(trace_runner_sound LANGUAGES C)

# I2S3 and the PDM run off the CGU's 76.8 MHz audio source, which only
# alp-sdk's clockctrl patch (zephyr/patches/zephyr/0001-clock_control_alif-
# master-source-expmst-i2s-setrate.patch) turns on. Unpatched, i2s_dw
# tolerates the missing set_rate silently: the build is clean, the first
# write queues, and nothing ever clocks out (bench, 2026-09-23). Refuse.
file(READ ${ZEPHYR_BASE}/drivers/clock_control/clock_control_alif.c _tr_clkctl)
string(FIND "${_tr_clkctl}" "ALIF_EXPMST0_CTRL_REG_OFF" _tr_patched)
if(_tr_patched EQUAL -1)
	message(FATAL_ERROR "${ZEPHYR_BASE}/drivers/clock_control/clock_control_alif.c lacks alp-sdk's "
		"clockctrl patch (no 76.8 MHz audio clock for I2S3/PDM). Build against a patched tree, "
		"e.g. ZEPHYR_BASE=$ZEPHYR_BASE (docs/2026-09-23-sound.md).")
endif()

target_sources(app PRIVATE
	src/main.c
	src/cc3501e_bridge.c
	src/cc3501e_gpio_routes.c
	../src/audio/tr_audio.c
	../src/audio/tr_amp_ready.c
	../src/ipc/tr_aring.c
)
target_include_directories(app PRIVATE ../src)
# Synth build (bench A/B): TR_SND_V2=OFF renders the first synth version;
# TR_SND_RATE=48000 needs V2 (16 kHz and 48 kHz divide 76.8 MHz exactly).
option(TR_SND_V2 "Band-limited synth + soft limiter + level trims (TR_AUDIO_V2)" ON)
option(TR_SND_V3 "Small-speaker voicing: 4th-order 450 Hz master high-pass + everything voiced from ~500 Hz (TR_AUDIO_V3, needs V2)" ON)
set(TR_SND_RATE 16000 CACHE STRING "Synth and I2S3 rate: 16000 or 48000")
set(TR_SND_VOLUME "" CACHE STRING "Digital volume 0..128 (empty = 128)")
# Extra TAS2563 register writes for bench A/B, e.g. amp_regs_dvc_minus6db.h
# (DVC -6 dB). Default NONE = the amps' POR DVC 0 dB: round 3 showed no amp
# fault latch in any window with or without it, and it cost 6 dB at the mic.
set(TR_SND_AMP_REGS_FILE NONE CACHE STRING
	"Header defining TR_SND_AMP_REGS (extra TAS2563 writes), or NONE")
target_compile_definitions(app PRIVATE TR_AUDIO_V2=$<BOOL:${TR_SND_V2}> TR_AUDIO_RATE=${TR_SND_RATE}u
	TR_AUDIO_V3=$<AND:$<BOOL:${TR_SND_V2}>,$<BOOL:${TR_SND_V3}>>)
if(TR_SND_VOLUME)
	target_compile_definitions(app PRIVATE TR_SND_VOLUME=${TR_SND_VOLUME}u)
endif()
if(TR_SND_AMP_REGS_FILE AND NOT TR_SND_AMP_REGS_FILE STREQUAL "NONE")
	target_compile_definitions(app PRIVATE TR_SND_AMP_REGS_FILE="${TR_SND_AMP_REGS_FILE}")
endif()
target_compile_definitions(app PRIVATE TR_SND_TEST=$<IF:$<STREQUAL:${TR_SND_MODE},TEST>,1,0>
	TR_SND_MIC=$<IF:$<AND:$<STREQUAL:${TR_SND_MODE},TEST>,$<BOOL:${TR_SND_MIC}>>,1,0>)
# Same bits as the host tests (the synth is integer-only, but keep the rule).
target_compile_options(app PRIVATE -ffp-contract=off)
