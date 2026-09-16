# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)

# PROBE_UNDERRUN (#2149 bench verification) and PROBE_RX_TEARDOWN (#2171,
# phase 1 of #2150) both need CONFIG_LOG on -- see underrun.conf's own
# header; PROBE_RX_TEARDOWN reuses it rather than adding a near-duplicate
# file, since its own step 2 exercises the SAME alp_i2s_zephyr
# I2S_STATE_ERROR recovery LOG_WRN_RATELIMIT() as PROBE_UNDERRUN's step 2.
# OVERLAY_CONFIG has to be set BEFORE find_package(Zephyr...) below for
# Zephyr's own Kconfig merge to pick it up; every other PROBE_* mode leaves
# both undefined, so this is a no-op for them.
if(DEFINED PROBE_UNDERRUN OR DEFINED PROBE_RX_TEARDOWN)
  list(APPEND OVERLAY_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/underrun.conf")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_i2s_tas2563_probe LANGUAGES C)
target_sources(app PRIVATE src/main.c)

# BENCH-TEST (test/u46-i2s-tas2563-on-reworked-mux), not upstream: the
# CC3501E bridge bring-up helper + this app's two-entry GPIO-proxy route
# table, needed to drive the I2S0 mux SELECT + ENABLE (E1M IO13/IO8) before
# touching i2s3 on the reworked e1m-aen-evk-03 bench board -- see the board
# overlay and src/main.c.
target_sources(app PRIVATE src/cc3501e_bridge.c src/cc3501e_gpio_routes.c)

# PROBE_LISTEN (test/u46-i2s-tas2563-on-reworked-mux): compile-time switch to
# src/main.c's attended-listening variant (long continuous tone + a page-0
# register dump, no PASS/FAIL verdict) instead of the normal automated probe.
# Opt in with `west build ... -- -DPROBE_LISTEN=1`; unset (the default),
# this app is unchanged.
if(DEFINED PROBE_LISTEN)
  target_compile_definitions(app PRIVATE PROBE_LISTEN=${PROBE_LISTEN})
endif()

# PROBE_MELODY (test/u46-i2s-tas2563-on-reworked-mux): compile-time switch to
# src/main.c's short-tune variant (Ode to Joy, public domain) instead of the
# normal automated probe or PROBE_LISTEN's sustained tone. Opt in with
# `west build ... -- -DPROBE_MELODY=1`; unset (the default), this app is
# unchanged, same as PROBE_LISTEN unset.
if(DEFINED PROBE_MELODY)
  target_compile_definitions(app PRIVATE PROBE_MELODY=${PROBE_MELODY})
endif()

# PROBE_LOOPBACK (test/u46-i2s-tas2563-on-reworked-mux): compile-time switch
# to src/main.c's speaker-to-mic loopback variant (six tone/silence windows,
# integer-only per-sample stats + fixed-point Goertzel, a LIVE/DEAD/
# INCONCLUSIVE verdict) instead of the normal automated probe or the
# PROBE_LISTEN/PROBE_MELODY playback-only variants. Opt in with
# `west build ... -- -DPROBE_LOOPBACK=1`; unset (the default), this app is
# unchanged, same as PROBE_LISTEN/PROBE_MELODY unset.
if(DEFINED PROBE_LOOPBACK)
  target_compile_definitions(app PRIVATE PROBE_LOOPBACK=${PROBE_LOOPBACK})
endif()

# PROBE_RESUME (test/u46-i2s-tas2563-on-reworked-mux): compile-time switch to
# src/main.c's #2146 bench-verification variant (tas2563_resume() ordered
# start, a halt-time measurement, a restart-without-resume that reproduces
# the bug on purpose, a resume-after-restart that proves the fix, and a
# short-gap self-heal sweep) instead of the normal automated probe or the
# other three PROBE_* variants. Opt in with `west build ... --
# -DPROBE_RESUME=1`; unset (the default), this app is unchanged, same as
# the other three PROBE_* switches unset.
if(DEFINED PROBE_RESUME)
  target_compile_definitions(app PRIVATE PROBE_RESUME=${PROBE_RESUME})
endif()

# PROBE_RESUME2 (test/u46-i2s-tas2563-on-reworked-mux): compile-time switch
# to src/main.c's PROBE_RESUME + mic-measured-pitch variant -- the same A-F
# #2146 sequence, but with >=3 s of silence before every tone and an
# objective PDM-mic Goertzel pitch scan (200-4000 Hz) per tone/silence
# window, instead of trusting a listener's ear alone. Opt in with
# `west build ... -- -DPROBE_RESUME2=1`; unset (the default), this app is
# unchanged, same as the other four PROBE_* switches unset.
if(DEFINED PROBE_RESUME2)
  target_compile_definitions(app PRIVATE PROBE_RESUME2=${PROBE_RESUME2})
endif()

# PROBE_UNDERRUN (test/2149-underrun-bench): compile-time switch to
# src/main.c's #2149 speaker-safe silicon verification variant -- proves the
# DesignWare I2S bit clock keeps running across a TX underrun (never
# stopped by alp_audio_out_stop()) so the TAS2563 amps never see a clock-
# loss shutdown for an underrun, with a real stop()/start()-without-resume
# negative control (step 5) to prove the check itself can fail. Opt in with
# `west build ... -- -DPROBE_UNDERRUN=1`; unset (the default), this app is
# unchanged, same as every other PROBE_* switch unset. Also pulls in
# underrun.conf (see the OVERLAY_CONFIG block above) for CONFIG_LOG.
if(DEFINED PROBE_UNDERRUN)
  target_compile_definitions(app PRIVATE PROBE_UNDERRUN=${PROBE_UNDERRUN})
endif()

# PROBE_RX_TEARDOWN (test/2171-rx-teardown-arm): compile-time switch to
# src/main.c's #2171 (phase 1 of #2150) variant -- reuses PROBE_UNDERRUN's
# bring-up + TX-underrun-arm pattern, then configures/starts/stops I2S3 in
# RX (<alp/i2s.h>, ALP_I2S_DIR_RX) to exercise rx_stream_disable() while TX
# is still depending on the shared bit clock, and re-reads CER/PWR_CTL
# afterward. Opt in with `west build ... -- -DPROBE_RX_TEARDOWN=1`; unset
# (the default), this app is unchanged, same as every other PROBE_* switch
# unset. Also pulls in underrun.conf (see the OVERLAY_CONFIG block above)
# for CONFIG_LOG.
if(DEFINED PROBE_RX_TEARDOWN)
  target_compile_definitions(app PRIVATE PROBE_RX_TEARDOWN=${PROBE_RX_TEARDOWN})
endif()
