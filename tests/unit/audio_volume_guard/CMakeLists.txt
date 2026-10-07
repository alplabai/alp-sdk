# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_audio_volume_guard)

# Covers both the dependency-free static-inline alp_audio_volume_settable()
# guard (src/backends/audio/audio_volume_guard.h, shared by the Yocto/ALSA
# and Zephyr/I2S audio-out backends' out_set_volume) AND the sw_fallback
# backend's real out_set_volume entry point, fetched via the audio backend
# registry (src/backends/audio/audio_ops.h) -- issue #2002. No I2S/ALSA
# device is touched either way.
target_sources(app PRIVATE src/test_audio_volume_guard.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../src/backends/audio
	${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
