# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(u8g2_oled_draw LANGUAGES C)

# u8g2's minimal core draw pipeline (u8g2_SetupBuffer/hvline/box/font +
# the u8x8 HAL it rides on) is vendored in-tree at vendors/u8g2/csrc --
# see that directory's README.md for the file list + why. The alp-sdk
# Zephyr module (zephyr/CMakeLists.txt) compiles it into this build
# automatically because board.yaml's `libraries: [u8g2]` sets
# CONFIG_ALP_SDK_U8G2_VENDORED_CORE=y; nothing to wire here beyond the
# app's own sources. That Kconfig line is unconditional for every
# `libraries: [u8g2]` slice (metadata/libraries/u8g2.yaml's
# integration.zephyr.kconfig:) -- there is no SDK-level switch to swap
# in a FULL upstream u8g2 checkout (real panel drivers, bundled fonts)
# instead; a customer who wants one vendors/builds it as their own
# Zephyr module rather than through this SDK's `libraries: [u8g2]` knob.
# Not needed for this example, which only exercises the vendored core.
target_sources(app PRIVATE
    src/main.c
    src/u8g2_ram_null.c
    src/u8g2_font_6x10_tr.c
)
