# SPDX-License-Identifier: Apache-2.0
#
# aen-tilt-arcade -- an original tilt-steered arcade game ("Riftrunner") on
# the RK055HDMIPI4MA0 720x1280 portrait panel (E1M-EVK connector J6), for an
# E1M-AEN SoM (M55-HE).  Renders through the portable <alp/display.h> surface
# (alp_display_blit / alp_display_clear) with small dirty-rect blits only --
# never a full-frame redraw -- and steers from the carrier BMI323 (U13,
# soldered on the E1M-EVK, not the SoM) tilt sensor via
# <alp/chips/bmi323.h> + <alp/peripheral.h> (portable I2C).
#
# See src/main.c for the game rules and the dirty-rect / tilt-mapping
# rationale.  Like aen-dsi-display (the display bring-up this rides on,
# #2199), the shield is appended here so a plain `west build` of this
# directory needs no -DSHIELD -- the shield's Kconfig.defconfig enables the
# display-chain drivers, and this app's own prj.conf turns on CONFIG_ALP_SDK
# for the alp-sdk backends (display + I2C + the BMI323 chip driver).

cmake_minimum_required(VERSION 3.20)

list(APPEND SHIELD e1m_evk_rk055hdmipi4ma0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_tilt_arcade LANGUAGES C)

target_sources(app PRIVATE src/main.c)
