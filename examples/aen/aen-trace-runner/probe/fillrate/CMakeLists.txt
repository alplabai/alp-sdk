# SPDX-License-Identifier: Apache-2.0
#
# Stage-0 fill-rate probe (docs/2026-09-22-core-allocation.md). Standalone
# Zephyr app -- does NOT touch the game under src/, tests/, art/, tools/ or
# the top-level CMakeLists.txt. Answers: can a Cortex-M55 sustain a
# full-screen 720x1280 RGB565 repaint, and where does it saturate? Measures
# raw SRAM0/SRAM1/DTCM bandwidth, the renderer's real inner loops (indexed
# sprite blit, horizontally-scaled scanline copy), and full-frame repaint
# bandwidth with the CDC200 panel blanked vs live. Built for both M55 cores;
# see README.md for the exact invocation this was verified with.

cmake_minimum_required(VERSION 3.20)

# Display only -- the camera shields the game stacks are not needed here.
list(APPEND SHIELD e1m_evk_rk055hdmipi4ma0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(trace_runner_fillrate_probe LANGUAGES C)

target_sources(app PRIVATE src/main.c)
