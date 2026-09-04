# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_audio_volume_guard)

# The guard under test is the dependency-free static-inline
# alp_audio_volume_settable() in src/backends/audio/audio_volume_guard.h
# -- shared by the Yocto (ALSA) and Zephyr (I2S) audio-out backends'
# out_set_volume -- so the test needs only <alp/audio.h> for the
# alp_audio_format_t enum, no I2S/ALSA device.
target_sources(app PRIVATE src/test_audio_volume_guard.c)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../../src/backends/audio
	${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
